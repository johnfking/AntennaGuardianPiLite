#define _POSIX_C_SOURCE 200809L

#include "ag_discovery.h"

#include "ag_log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define AG_DISCOVERY_PORT 4992
#define AG_DISCOVERY_PACKET_MAX 4096
#define AG_VITA_EXT_DATA_WITH_STREAM 3u
#define AG_VITA_CLASS_PRESENT 0x08000000u
#define AG_VITA_DISCOVERY_STREAM 0x00000800u
#define AG_VITA_DISCOVERY_CLASS 0x534cffffu

static uint32_t read_network_word(const unsigned char *data)
{
    uint32_t value;
    memcpy(&value, data, sizeof(value));
    return ntohl(value);
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
        size_t length;

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
            length = (size_t)(token_end - equals - 1);
            if (length == 0u || length >= value_size) {
                return false;
            }
            memcpy(value, equals + 1, length);
            value[length] = '\0';
            return true;
        }
        cursor = token_end;
    }
    return false;
}

static bool parse_packet(
    const unsigned char *packet,
    size_t received,
    ag_discovered_radio *radio)
{
    uint32_t header;
    size_t packet_size;
    size_t payload_offset = 8u;
    size_t payload_size;
    char payload[AG_DISCOVERY_PACKET_MAX + 1];
    char port_text[16];
    char *end;
    unsigned long port;
    struct in_addr address;

    if (received < 16u) {
        return false;
    }
    header = read_network_word(packet);
    packet_size = (size_t)(header & 0xffffu) * 4u;
    if ((header >> 28) != AG_VITA_EXT_DATA_WITH_STREAM
        || (header & AG_VITA_CLASS_PRESENT) == 0u
        || packet_size > received || packet_size < 16u
        || read_network_word(packet + 4u) != AG_VITA_DISCOVERY_STREAM) {
        return false;
    }

    payload_offset += 8u;
    if (read_network_word(packet + 12u) != AG_VITA_DISCOVERY_CLASS) {
        return false;
    }
    if (((header >> 22) & 0x3u) != 0u) {
        payload_offset += 4u;
    }
    if (((header >> 20) & 0x3u) != 0u) {
        payload_offset += 8u;
    }
    if (payload_offset >= packet_size) {
        return false;
    }

    payload_size = packet_size - payload_offset;
    while (payload_size > 0u && packet[payload_offset + payload_size - 1u] == '\0') {
        --payload_size;
    }
    if (payload_size == 0u || payload_size > AG_DISCOVERY_PACKET_MAX
        || memchr(packet + payload_offset, '\0', payload_size) != NULL) {
        return false;
    }
    memcpy(payload, packet + payload_offset, payload_size);
    payload[payload_size] = '\0';

    memset(radio, 0, sizeof(*radio));
    if (!field_value(payload, "serial", radio->serial, sizeof(radio->serial))
        || !field_value(payload, "ip", radio->host, sizeof(radio->host))
        || !field_value(payload, "port", port_text, sizeof(port_text))
        || inet_pton(AF_INET, radio->host, &address) != 1) {
        return false;
    }
    errno = 0;
    port = strtoul(port_text, &end, 10);
    if (errno != 0 || end == port_text || *end != '\0' || port < 1u || port > 65535u) {
        return false;
    }
    radio->port = (uint16_t)port;
    (void)field_value(payload, "model", radio->model, sizeof(radio->model));
    if (!field_value(payload, "nickname", radio->name, sizeof(radio->name))) {
        (void)field_value(payload, "name", radio->name, sizeof(radio->name));
    }
    return true;
}

static bool matches_selector(const ag_config *config, const ag_discovered_radio *radio)
{
    return (config->radio_serial[0] == '\0'
            || strcasecmp(config->radio_serial, radio->serial) == 0)
        && (config->discovery_ip[0] == '\0'
            || strcmp(config->discovery_ip, radio->host) == 0);
}

ag_discovery_result ag_discovery_wait(
    const ag_config *config,
    volatile sig_atomic_t *stop_requested,
    ag_discovered_radio *radio)
{
    int socket_fd;
    int enabled = 1;
    struct sockaddr_in address;

    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        ag_log(AG_LOG_ERROR, "Could not create discovery socket: %s", strerror(errno));
        return AG_DISCOVERY_ERROR;
    }
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0) {
        ag_log(AG_LOG_ERROR, "Could not configure discovery socket: %s", strerror(errno));
        close(socket_fd);
        return AG_DISCOVERY_ERROR;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(AG_DISCOVERY_PORT);
    if (bind(socket_fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        ag_log(AG_LOG_ERROR, "Could not listen for Flex discovery on UDP port %d: %s",
               AG_DISCOVERY_PORT, strerror(errno));
        close(socket_fd);
        return AG_DISCOVERY_ERROR;
    }

    while (!*stop_requested) {
        struct pollfd descriptor = {socket_fd, POLLIN, 0};
        int poll_result = poll(&descriptor, 1, 1000);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            ag_log(AG_LOG_ERROR, "Flex discovery listener failed: %s", strerror(errno));
            close(socket_fd);
            return AG_DISCOVERY_ERROR;
        }
        if (poll_result == 0) {
            continue;
        }
        if ((descriptor.revents & POLLIN) != 0) {
            unsigned char packet[AG_DISCOVERY_PACKET_MAX];
            ssize_t received = recv(socket_fd, packet, sizeof(packet), 0);
            if (received > 0 && parse_packet(packet, (size_t)received, radio)
                && matches_selector(config, radio)) {
                close(socket_fd);
                return AG_DISCOVERY_MATCHED;
            }
        }
    }

    close(socket_fd);
    return AG_DISCOVERY_STOPPED;
}
