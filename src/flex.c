#define _POSIX_C_SOURCE 200809L

#include "ag_flex.h"

#include "ag_log.h"
#include "ag_policy.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define AG_LINE_BUFFER 8192
#define AG_COMMAND_SIZE 1024
#define AG_RESPONSE_BODY 256
#define AG_COMMAND_TIMEOUT_MS 4000
#define AG_CONNECT_TIMEOUT_MS 5000
#define AG_SAVED_RESPONSES 8

typedef struct {
    bool used;
    unsigned sequence;
    unsigned long code;
    char body[AG_RESPONSE_BODY];
} ag_saved_response;

typedef struct {
    const ag_config *config;
    volatile sig_atomic_t *stop_requested;
    int socket_fd;
    unsigned sequence;
    char receive_buffer[AG_LINE_BUFFER];
    size_t receive_length;
    char interlock_id[64];
    bool observe_only;
    bool has_frequency;
    double frequency_mhz;
    char tx_antenna[AG_MAX_ANTENNA_ID + 1];
    bool command_in_progress;
    bool interlock_update_pending;
    bool pending_interlock_ready;
    ag_saved_response saved_responses[AG_SAVED_RESPONSES];
} ag_connection;

static int handle_status(ag_connection *connection, const char *line);

static long long monotonic_milliseconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static int connect_socket(const ag_config *config, volatile sig_atomic_t *stop_requested)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    char port[16];
    int socket_fd = -1;
    int lookup_result;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof(port), "%u", config->port);
    lookup_result = getaddrinfo(config->host, port, &hints, &addresses);
    if (lookup_result != 0) {
        ag_log(AG_LOG_ERROR, "Cannot resolve %s: %s", config->host,
               gai_strerror(lookup_result));
        return -1;
    }

    for (address = addresses; address != NULL && !*stop_requested; address = address->ai_next) {
        int flags;
        int connect_result;
        int socket_error = 0;
        socklen_t socket_error_size = sizeof(socket_error);
        struct pollfd descriptor;

        socket_fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_fd < 0) {
            continue;
        }
        flags = fcntl(socket_fd, F_GETFL, 0);
        if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(socket_fd);
            socket_fd = -1;
            continue;
        }
        connect_result = connect(socket_fd, address->ai_addr, address->ai_addrlen);
        if (connect_result < 0 && errno != EINPROGRESS) {
            close(socket_fd);
            socket_fd = -1;
            continue;
        }
        descriptor = (struct pollfd){socket_fd, POLLOUT, 0};
        if (connect_result < 0 && poll(&descriptor, 1, AG_CONNECT_TIMEOUT_MS) <= 0) {
            close(socket_fd);
            socket_fd = -1;
            continue;
        }
        if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) < 0
            || socket_error != 0 || fcntl(socket_fd, F_SETFL, flags) < 0) {
            close(socket_fd);
            socket_fd = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(addresses);

    if (socket_fd >= 0) {
        int enabled = 1;
        struct timeval send_timeout = {AG_COMMAND_TIMEOUT_MS / 1000, 0};
        setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
        setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
        setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
    }
    return socket_fd;
}

static int send_all(int socket_fd, const char *data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        ssize_t result = send(socket_fd, data + sent, length - sent, MSG_NOSIGNAL);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return -1;
        }
        sent += (size_t)result;
    }
    return 0;
}

static int extract_line(ag_connection *connection, char *line, size_t line_size)
{
    char *newline = memchr(connection->receive_buffer, '\n', connection->receive_length);
    size_t length;
    if (newline == NULL) {
        return 0;
    }
    length = (size_t)(newline - connection->receive_buffer);
    if (length > 0u && connection->receive_buffer[length - 1u] == '\r') {
        --length;
    }
    if (length + 1u > line_size) {
        ag_log(AG_LOG_ERROR, "Radio sent a line larger than the protocol limit");
        return -1;
    }
    memcpy(line, connection->receive_buffer, length);
    line[length] = '\0';
    length = (size_t)(newline - connection->receive_buffer) + 1u;
    memmove(connection->receive_buffer, connection->receive_buffer + length,
            connection->receive_length - length);
    connection->receive_length -= length;
    return 1;
}

static int read_line(ag_connection *connection, char *line, size_t line_size, int timeout_ms)
{
    int extracted;
    struct pollfd descriptor;

    extracted = extract_line(connection, line, line_size);
    if (extracted != 0) {
        return extracted;
    }
    if (connection->receive_length == sizeof(connection->receive_buffer)) {
        ag_log(AG_LOG_ERROR, "Radio sent an unterminated line larger than %d bytes",
               AG_LINE_BUFFER);
        return -1;
    }
    descriptor = (struct pollfd){connection->socket_fd, POLLIN, 0};
    for (;;) {
        int poll_result = poll(&descriptor, 1, timeout_ms);
        if (poll_result < 0 && errno == EINTR) {
            return 0;
        }
        if (poll_result <= 0) {
            return poll_result;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return -1;
        }
        if ((descriptor.revents & POLLIN) != 0) {
            ssize_t received;
            if (connection->receive_length == sizeof(connection->receive_buffer)) {
                ag_log(AG_LOG_ERROR, "Radio receive buffer exceeded %d bytes", AG_LINE_BUFFER);
                return -1;
            }
            received = recv(connection->socket_fd,
                            connection->receive_buffer + connection->receive_length,
                            sizeof(connection->receive_buffer) - connection->receive_length, 0);
            if (received < 0 && errno == EINTR) {
                return 0;
            }
            if (received <= 0) {
                return -1;
            }
            connection->receive_length += (size_t)received;
            return extract_line(connection, line, line_size);
        }
    }
}

static bool parse_response(
    const char *line,
    unsigned *sequence,
    unsigned long *code,
    const char **body)
{
    char *end;
    const char *cursor;
    if (line[0] != 'R') {
        return false;
    }
    errno = 0;
    *sequence = (unsigned)strtoul(line + 1, &end, 10);
    if (errno != 0 || end == line + 1 || *end != '|') {
        return false;
    }
    cursor = end + 1;
    errno = 0;
    *code = strtoul(cursor, &end, 16);
    if (errno != 0 || end == cursor || *end != '|') {
        return false;
    }
    *body = end + 1;
    return true;
}

static void save_response(
    ag_connection *connection,
    unsigned sequence,
    unsigned long code,
    const char *body)
{
    size_t index;
    for (index = 0; index < AG_SAVED_RESPONSES; ++index) {
        if (!connection->saved_responses[index].used) {
            ag_saved_response *saved = &connection->saved_responses[index];
            saved->used = true;
            saved->sequence = sequence;
            saved->code = code;
            snprintf(saved->body, sizeof(saved->body), "%s", body);
            return;
        }
    }
    ag_log(AG_LOG_WARNING, "Discarded an unmatched Flex response because the response cache is full");
}

static bool take_response(
    ag_connection *connection,
    unsigned sequence,
    unsigned long *code,
    const char **body)
{
    size_t index;
    for (index = 0; index < AG_SAVED_RESPONSES; ++index) {
        ag_saved_response *saved = &connection->saved_responses[index];
        if (saved->used && saved->sequence == sequence) {
            saved->used = false;
            *code = saved->code;
            *body = saved->body;
            return true;
        }
    }
    return false;
}

static int accept_response(
    const char *command,
    unsigned long code,
    const char *body,
    char *response_body,
    size_t response_size)
{
    if (code != 0u) {
        ag_log(AG_LOG_ERROR, "Radio rejected '%s' with 0x%08lX", command, code);
        return -1;
    }
    if (response_body != NULL && response_size > 0u) {
        snprintf(response_body, response_size, "%s", body);
    }
    return 0;
}

static int send_command_once(
    ag_connection *connection,
    const char *command,
    char *response_body,
    size_t response_size,
    bool cleanup_command)
{
    char frame[AG_COMMAND_SIZE];
    unsigned sequence = ++connection->sequence;
    long long deadline = monotonic_milliseconds() + AG_COMMAND_TIMEOUT_MS;
    int frame_length;

    if (*connection->stop_requested && !cleanup_command) {
        return -1;
    }
    frame_length = snprintf(frame, sizeof(frame), "C%u|%s\n", sequence, command);
    if (frame_length < 0 || (size_t)frame_length >= sizeof(frame)) {
        ag_log(AG_LOG_ERROR, "Flex command exceeds the protocol limit");
        return -1;
    }
    ag_log(AG_LOG_DEBUG, "> %s", command);
    if (send_all(connection->socket_fd, frame, (size_t)frame_length) != 0) {
        ag_log(AG_LOG_ERROR, "Failed to send command: %s", strerror(errno));
        return -1;
    }

    while (cleanup_command || !*connection->stop_requested) {
        char line[AG_LINE_BUFFER];
        int remaining = (int)(deadline - monotonic_milliseconds());
        int read_result;
        unsigned response_sequence;
        unsigned long code;
        const char *body;
        if (take_response(connection, sequence, &code, &body)) {
            return accept_response(command, code, body, response_body, response_size);
        }
        if (remaining <= 0) {
            ag_log(AG_LOG_ERROR, "Radio did not answer command within %d ms", AG_COMMAND_TIMEOUT_MS);
            return -1;
        }
        read_result = read_line(connection, line, sizeof(line), remaining);
        if (read_result <= 0) {
            if (read_result < 0) {
                ag_log(AG_LOG_ERROR, "Radio connection closed while awaiting a response");
            }
            return -1;
        }
        ag_log(AG_LOG_DEBUG, "< %s", line);
        if (parse_response(line, &response_sequence, &code, &body)) {
            if (response_sequence != sequence) {
                save_response(connection, response_sequence, code, body);
                continue;
            }
            return accept_response(command, code, body, response_body, response_size);
        }
        if (handle_status(connection, line) != 0 && !cleanup_command) {
            return -1;
        }
    }
    return -1;
}

static int send_command(
    ag_connection *connection,
    const char *command,
    char *response_body,
    size_t response_size,
    bool cleanup_command)
{
    int result;

    if (connection->command_in_progress) {
        ag_log(AG_LOG_ERROR, "Attempted to send a nested Flex command");
        return -1;
    }

    connection->command_in_progress = true;
    result = send_command_once(connection, command, response_body, response_size, cleanup_command);
    connection->command_in_progress = false;
    if (result != 0 || cleanup_command) {
        return result;
    }

    while (connection->interlock_update_pending && !*connection->stop_requested) {
        char deferred_command[128];
        bool ready = connection->pending_interlock_ready;

        connection->interlock_update_pending = false;
        snprintf(deferred_command, sizeof(deferred_command), "interlock %s %s",
                 ready ? "ready" : "not_ready", connection->interlock_id);
        connection->command_in_progress = true;
        result = send_command_once(connection, deferred_command, NULL, 0u, false);
        connection->command_in_progress = false;
        if (result != 0) {
            return result;
        }
    }
    return 0;
}

static bool field_value(
    const char *payload,
    const char *field,
    char *value,
    size_t value_size)
{
    size_t field_length = strlen(field);
    const char *cursor = payload;
    while (*cursor != '\0') {
        const char *token_end;
        const char *equals;
        while (*cursor == ' ') {
            ++cursor;
        }
        token_end = strchr(cursor, ' ');
        if (token_end == NULL) {
            token_end = cursor + strlen(cursor);
        }
        equals = memchr(cursor, '=', (size_t)(token_end - cursor));
        if (equals != NULL && (size_t)(equals - cursor) == field_length
            && strncmp(cursor, field, field_length) == 0) {
            size_t length = (size_t)(token_end - equals - 1);
            if (length >= value_size) {
                length = value_size - 1u;
            }
            memcpy(value, equals + 1, length);
            value[length] = '\0';
            return true;
        }
        cursor = token_end;
    }
    return false;
}

static void log_decision(
    const ag_connection *connection,
    ag_policy_decision decision,
    bool observe_only)
{
    const char *allow_label = observe_only ? "WOULD ALLOW" : "ALLOWED";
    const char *block_label = observe_only ? "WOULD BLOCK" : "BLOCKED";

    if (decision.allowed) {
        ag_log(AG_LOG_INFO, "%s %s on %s at %.6f MHz", allow_label,
               connection->tx_antenna, decision.band, connection->frequency_mhz);
    } else if (!connection->has_frequency || connection->tx_antenna[0] == '\0') {
        ag_log(AG_LOG_WARNING, "%s transmit context is incomplete", block_label);
    } else if (decision.reason == AG_DECISION_OUTSIDE_KNOWN_BANDS) {
        ag_log(AG_LOG_WARNING, "%s %.6f MHz is outside known native bands", block_label,
               connection->frequency_mhz);
    } else {
        ag_log(AG_LOG_WARNING, "%s %s on %s at %.6f MHz", block_label,
               connection->tx_antenna, decision.band, connection->frequency_mhz);
    }
}

static int set_interlock(ag_connection *connection, bool ready)
{
    char command[128];
    if (connection->observe_only || connection->interlock_id[0] == '\0') {
        return 0;
    }
    if (connection->command_in_progress) {
        connection->interlock_update_pending = true;
        connection->pending_interlock_ready = ready;
        return 0;
    }
    snprintf(command, sizeof(command), "interlock %s %s", ready ? "ready" : "not_ready",
             connection->interlock_id);
    return send_command(connection, command, NULL, 0u, false);
}

static int handle_transmit(ag_connection *connection, const char *payload)
{
    char value[128];
    ag_policy_decision decision;
    if (field_value(payload, "freq", value, sizeof(value))) {
        char *end;
        errno = 0;
        connection->frequency_mhz = strtod(value, &end);
        connection->has_frequency = errno == 0 && end != value && *end == '\0';
    }
    if (field_value(payload, "tx_antenna", value, sizeof(value))) {
        if (strcasecmp(value, "INVALID") == 0) {
            connection->has_frequency = false;
            connection->tx_antenna[0] = '\0';
        } else if (strlen(value) > AG_MAX_ANTENNA_ID) {
            ag_log(AG_LOG_WARNING, "Radio reported an invalid transmit antenna token");
            connection->tx_antenna[0] = '\0';
        } else {
            strcpy(connection->tx_antenna, value);
        }
    }
    decision = ag_policy_evaluate(connection->config, connection->has_frequency,
                                  connection->frequency_mhz, connection->tx_antenna);
    if (!decision.allowed && connection->interlock_id[0] != '\0') {
        return set_interlock(connection, false);
    }
    return 0;
}

static int handle_interlock(ag_connection *connection, const char *payload)
{
    char state[64];
    ag_policy_decision decision;
    if (!field_value(payload, "state", state, sizeof(state))) {
        return 0;
    }
    if (strcmp(state, "PTT_REQUESTED") == 0) {
        decision = ag_policy_evaluate(connection->config, connection->has_frequency,
                                      connection->frequency_mhz, connection->tx_antenna);
        log_decision(connection, decision, connection->observe_only);
        return set_interlock(connection, decision.allowed);
    }
    if (strcmp(state, "UNKEY_REQUESTED") == 0) {
        if (connection->observe_only) {
            ag_log(AG_LOG_INFO, "Observed transmit ended");
            return 0;
        }
        ag_log(AG_LOG_INFO, "Transmit ended; interlock returned to not-ready");
        return set_interlock(connection, false);
    }
    if (strcmp(state, "TRANSMITTING") == 0) {
        decision = ag_policy_evaluate(connection->config, connection->has_frequency,
                                      connection->frequency_mhz, connection->tx_antenna);
        if (!decision.allowed) {
            if (connection->observe_only) {
                ag_log(AG_LOG_WARNING,
                       "OBSERVED TRANSMITTING outside policy: %s at %.6f MHz",
                       connection->tx_antenna, connection->frequency_mhz);
                return 0;
            }
            ag_log(AG_LOG_ERROR, "FAULT radio reports transmission outside policy");
            return set_interlock(connection, false);
        }
        if (connection->observe_only) {
            ag_log(AG_LOG_INFO, "OBSERVED TRANSMITTING %s on %s at %.6f MHz",
                   connection->tx_antenna, decision.band, connection->frequency_mhz);
            return 0;
        }
        ag_log(AG_LOG_INFO, "TRANSMITTING %s on %s at %.6f MHz", connection->tx_antenna,
               decision.band, connection->frequency_mhz);
    }
    return 0;
}

static int handle_status(ag_connection *connection, const char *line)
{
    const char *separator = strchr(line, '|');
    const char *payload;
    if (separator == NULL || separator[1] == '\0') {
        return 0;
    }
    payload = separator + 1;
    if (strncmp(payload, "transmit ", 9u) == 0) {
        return handle_transmit(connection, payload + 9);
    }
    if (strncmp(payload, "interlock ", 10u) == 0) {
        return handle_interlock(connection, payload + 10);
    }
    return 0;
}

static int initialize_session(ag_connection *connection)
{
    char antennas[AG_MAX_ANTENNAS * (AG_MAX_ANTENNA_ID + 1)];
    char command[AG_COMMAND_SIZE];
    char response[AG_RESPONSE_BODY];
    size_t offset = 0;
    size_t index;

    if (send_command(connection, "name AntennaGuardianPiLite", NULL, 0u, false) != 0
        || send_command(connection, "sub radio all", NULL, 0u, false) != 0
        || send_command(connection, "sub slice all", NULL, 0u, false) != 0
        || send_command(connection, "sub tx all", NULL, 0u, false) != 0) {
        return -1;
    }
    if (connection->observe_only) {
        ag_log(AG_LOG_INFO, "OBSERVE mode active; no interlock was created");
        return 0;
    }

    antennas[0] = '\0';
    for (index = 0; index < connection->config->antenna_count; ++index) {
        int written = snprintf(antennas + offset, sizeof(antennas) - offset, "%s%s",
                               index == 0u ? "" : ",", connection->config->antennas[index].id);
        if (written < 0 || (size_t)written >= sizeof(antennas) - offset) {
            return -1;
        }
        offset += (size_t)written;
    }
    if (snprintf(command, sizeof(command),
                 "interlock create type=ANT name=AntennaGuardianPiLite "
                 "serial=raspberrypi valid_antennas=%s",
                 antennas) >= (int)sizeof(command)) {
        ag_log(AG_LOG_ERROR, "Configured antenna list exceeds the Flex command limit");
        return -1;
    }
    if (send_command(connection, command, response, sizeof(response), false) != 0) {
        return -1;
    }
    response[strcspn(response, "|")] = '\0';
    if (response[0] == '\0' || strlen(response) >= sizeof(connection->interlock_id)) {
        ag_log(AG_LOG_ERROR, "Radio created an interlock without a valid ID");
        return -1;
    }
    strcpy(connection->interlock_id, response);
    if (set_interlock(connection, false) != 0) {
        return -1;
    }
    ag_log(AG_LOG_INFO, "PROTECTED interlock %s is armed and not-ready",
           connection->interlock_id);
    return 0;
}

static void remove_interlock(ag_connection *connection)
{
    char command[128];
    if (connection->observe_only || connection->interlock_id[0] == '\0'
        || connection->socket_fd < 0) {
        return;
    }
    snprintf(command, sizeof(command), "interlock remove %s", connection->interlock_id);
    if (send_command(connection, command, NULL, 0u, true) == 0) {
        ag_log(AG_LOG_INFO, "Removed interlock %s", connection->interlock_id);
    } else {
        ag_log(AG_LOG_WARNING, "Interlock cleanup was not confirmed");
    }
    connection->interlock_id[0] = '\0';
}

ag_session_result ag_flex_run_session(
    const ag_config *config,
    bool observe_only,
    volatile sig_atomic_t *stop_requested)
{
    ag_connection connection;
    ag_session_result result = AG_SESSION_DISCONNECTED;

    memset(&connection, 0, sizeof(connection));
    connection.config = config;
    connection.stop_requested = stop_requested;
    connection.socket_fd = -1;
    connection.observe_only = observe_only;

    ag_log(AG_LOG_INFO, "Connecting to %s:%u", config->host, config->port);
    connection.socket_fd = connect_socket(config, stop_requested);
    if (connection.socket_fd < 0) {
        if (!*stop_requested) {
            ag_log(AG_LOG_ERROR, "Could not connect to %s:%u", config->host, config->port);
        }
        return *stop_requested ? AG_SESSION_STOPPED : AG_SESSION_DISCONNECTED;
    }
    ag_log(AG_LOG_INFO, "Connected to %s:%u", config->host, config->port);

    if (initialize_session(&connection) != 0) {
        result = *stop_requested ? AG_SESSION_STOPPED : AG_SESSION_ERROR;
        goto cleanup;
    }

    while (!*stop_requested) {
        char line[AG_LINE_BUFFER];
        int read_result = read_line(&connection, line, sizeof(line), 1000);
        if (read_result < 0) {
            ag_log(AG_LOG_WARNING, "Radio connection lost");
            result = AG_SESSION_DISCONNECTED;
            goto cleanup;
        }
        if (read_result == 0) {
            continue;
        }
        ag_log(AG_LOG_DEBUG, "< %s", line);
        if (handle_status(&connection, line) != 0) {
            result = AG_SESSION_ERROR;
            goto cleanup;
        }
    }
    result = AG_SESSION_STOPPED;

cleanup:
    remove_interlock(&connection);
    if (connection.socket_fd >= 0) {
        close(connection.socket_fd);
    }
    return result;
}
