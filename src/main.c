#define _POSIX_C_SOURCE 200809L

#include "ag_config.h"
#include "ag_discovery.h"
#include "ag_flex.h"
#include "ag_log.h"
#include "ag_policy.h"
#include "ag_retry.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef AG_VERSION
#define AG_VERSION "dev"
#endif

#define DEFAULT_CONFIG "/etc/antennaguardian-pilite/config.json"

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "Usage: antennaguardian-pilite [options]\n"
            "\n"
            "Options:\n"
            "  -c, --config PATH   Configuration file (default: %s)\n"
            "      --check-config  Validate configuration without opening a socket\n"
            "      --observe       Subscribe and report; never create an interlock\n"
            "      --once          Exit instead of reconnecting after a disconnect\n"
            "  -v, --verbose       Log Flex protocol traffic\n"
            "      --version       Print version and exit\n"
            "  -h, --help          Show this help\n",
            DEFAULT_CONFIG);
}

static void print_config_summary(const ag_config *config)
{
    size_t antenna_index;
    fprintf(stdout, "Configuration valid\n");
    if (config->radio_mode == AG_RADIO_DISCOVERY) {
        fprintf(stdout, "Radio: Flex UDP discovery");
        if (config->radio_serial[0] != '\0') {
            fprintf(stdout, " serial=%s", config->radio_serial);
        }
        if (config->discovery_ip[0] != '\0') {
            fprintf(stdout, " ip=%s", config->discovery_ip);
        }
        fprintf(stdout, "\n");
    } else {
        fprintf(stdout, "Radio: %s:%u\n", config->host, config->port);
        fprintf(stdout, "Reconnect: %u seconds initial, %u seconds maximum\n",
                config->reconnect_seconds, config->reconnect_max_seconds);
        fprintf(stdout, "Unavailable log interval: %u seconds\n",
                config->reconnect_log_seconds);
    }
    for (antenna_index = 0; antenna_index < config->antenna_count; ++antenna_index) {
        const ag_antenna_policy *antenna = &config->antennas[antenna_index];
        size_t band_index;
        bool first = true;
        fprintf(stdout, "%s:", antenna->id);
        for (band_index = 0; band_index < ag_band_count(); ++band_index) {
            if ((antenna->allowed_bands & (1u << band_index)) != 0u) {
                fprintf(stdout, "%s%s", first ? " " : ", ", ag_band_name(band_index));
                first = false;
            }
        }
        fprintf(stdout, "%s\n", first ? " <no bands allowed>" : "");
    }
}

static void sleep_interruptibly(unsigned seconds)
{
    struct timespec interval = {0, 200000000L};
    unsigned iterations = seconds * 5u;
    unsigned index;
    for (index = 0; index < iterations && !stop_requested; ++index) {
        while (nanosleep(&interval, &interval) < 0 && errno == EINTR && !stop_requested) {
        }
        interval = (struct timespec){0, 200000000L};
    }
}

static long long monotonic_milliseconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

int main(int argc, char **argv)
{
    const char *config_path = DEFAULT_CONFIG;
    bool check_config = false;
    bool observe_only = false;
    bool once = false;
    bool verbose = false;
    ag_config config;
    char error[AG_ERROR_SIZE];
    struct sigaction signal_action;
    unsigned retry_delay;
    long long next_unavailable_log = 0;
    bool log_connection_attempt = true;
    int index;

    for (index = 1; index < argc; ++index) {
        if ((strcmp(argv[index], "-c") == 0 || strcmp(argv[index], "--config") == 0)
            && index + 1 < argc) {
            config_path = argv[++index];
        } else if (strcmp(argv[index], "--check-config") == 0) {
            check_config = true;
        } else if (strcmp(argv[index], "--observe") == 0) {
            observe_only = true;
        } else if (strcmp(argv[index], "--once") == 0) {
            once = true;
        } else if (strcmp(argv[index], "-v") == 0 || strcmp(argv[index], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[index], "--version") == 0) {
            printf("AntennaGuardianPiLite %s\n", AG_VERSION);
            return 0;
        } else if (strcmp(argv[index], "-h") == 0 || strcmp(argv[index], "--help") == 0) {
            print_usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[index]);
            print_usage(stderr);
            return 2;
        }
    }

    if (ag_config_load(config_path, &config, error, sizeof(error)) != 0) {
        fprintf(stderr, "Configuration error: %s\n", error);
        return 2;
    }
    if (check_config) {
        print_config_summary(&config);
        return 0;
    }

    ag_log_set_verbose(verbose);
    retry_delay = config.reconnect_seconds;
    memset(&signal_action, 0, sizeof(signal_action));
    signal_action.sa_handler = handle_signal;
    sigemptyset(&signal_action.sa_mask);
    sigaction(SIGINT, &signal_action, NULL);
    sigaction(SIGTERM, &signal_action, NULL);

    ag_log(AG_LOG_INFO, "AntennaGuardianPiLite %s starting in %s mode", AG_VERSION,
           observe_only ? "OBSERVE" : "PROTECT");
    if (config.radio_mode == AG_RADIO_DISCOVERY) {
        bool log_connection = true;
        ag_log(AG_LOG_INFO, "Waiting for a matching Flex radio discovery announcement");
        while (!stop_requested) {
            ag_discovered_radio discovered;
            ag_config session_config;
            ag_discovery_result discovery_result = ag_discovery_wait(
                &config, &stop_requested, &discovered);
            ag_session_result session_result;

            if (discovery_result == AG_DISCOVERY_STOPPED || stop_requested) {
                break;
            }
            if (discovery_result == AG_DISCOVERY_ERROR) {
                return 1;
            }

            session_config = config;
            snprintf(session_config.host, sizeof(session_config.host), "%s", discovered.host);
            session_config.port = discovered.port;
            if (log_connection) {
                ag_log(AG_LOG_INFO, "Discovered %s%s%s serial %s at %s:%u",
                       discovered.model[0] == '\0' ? "Flex radio" : discovered.model,
                       discovered.name[0] == '\0' ? "" : " ",
                       discovered.name, discovered.serial, discovered.host, discovered.port);
            }
            session_result = ag_flex_run_session(
                &session_config, observe_only, &stop_requested, log_connection);
            if (stop_requested || session_result == AG_SESSION_STOPPED) {
                break;
            }
            if (once) {
                return 1;
            }
            if (session_result == AG_SESSION_UNAVAILABLE) {
                log_connection = false;
                continue;
            }
            log_connection = true;
            ag_log(AG_LOG_WARNING,
                   "Radio session ended; waiting for its next discovery announcement");
        }
        ag_log(AG_LOG_INFO, "AntennaGuardianPiLite stopped");
        return 0;
    }

    while (!stop_requested) {
        ag_session_result result = ag_flex_run_session(
            &config, observe_only, &stop_requested, log_connection_attempt);
        if (stop_requested || result == AG_SESSION_STOPPED) {
            break;
        }
        if (once) {
            return 1;
        }
        if (result == AG_SESSION_UNAVAILABLE) {
            if (log_connection_attempt) {
                ag_log(AG_LOG_WARNING,
                       "Radio unavailable; retrying in %u seconds "
                       "(repeated failures logged every %u seconds)",
                       retry_delay, config.reconnect_log_seconds);
                next_unavailable_log = monotonic_milliseconds()
                    + (long long)config.reconnect_log_seconds * 1000LL;
            }
            sleep_interruptibly(retry_delay);
            retry_delay = ag_retry_next_delay(
                retry_delay, config.reconnect_max_seconds);
            log_connection_attempt = monotonic_milliseconds() >= next_unavailable_log;
            continue;
        }

        retry_delay = config.reconnect_seconds;
        log_connection_attempt = true;
        ag_log(AG_LOG_WARNING, "Reconnecting in %u seconds", config.reconnect_seconds);
        sleep_interruptibly(config.reconnect_seconds);
    }
    ag_log(AG_LOG_INFO, "AntennaGuardianPiLite stopped");
    return 0;
}
