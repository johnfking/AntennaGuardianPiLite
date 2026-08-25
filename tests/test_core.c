#include "ag_config.h"
#include "ag_policy.h"
#include "ag_retry.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_valid_config(void)
{
    ag_config config;
    char error[AG_ERROR_SIZE];
    const ag_antenna_policy *ant1;

    assert(ag_config_load("config/config.example.json", &config, error, sizeof(error)) == 0);
    assert(config.radio_mode == AG_RADIO_DISCOVERY);
    assert(strcmp(config.radio_serial, "YOUR-FLEX-SERIAL") == 0);
    assert(config.host[0] == '\0');
    assert(config.port == 4992u);
    assert(config.reconnect_seconds == 3u);
    assert(config.reconnect_max_seconds == 30u);
    assert(config.reconnect_log_seconds == 300u);
    assert(config.antenna_count == 2u);
    ant1 = ag_config_find_antenna(&config, "ant1");
    assert(ant1 != NULL);
    assert((ant1->allowed_bands & (1u << ag_band_index("160m"))) != 0u);
    assert((ant1->allowed_bands & (1u << ag_band_index("6m"))) == 0u);
}

static void test_retry_backoff(void)
{
    assert(ag_retry_next_delay(3u, 30u) == 6u);
    assert(ag_retry_next_delay(6u, 30u) == 12u);
    assert(ag_retry_next_delay(12u, 30u) == 24u);
    assert(ag_retry_next_delay(24u, 30u) == 30u);
    assert(ag_retry_next_delay(30u, 30u) == 30u);
}

static void test_legacy_reconnect_config(void)
{
    ag_config config;
    char error[AG_ERROR_SIZE];

    assert(ag_config_load("tests/fixtures/valid-legacy-reconnect.json", &config, error,
                          sizeof(error)) == 0);
    assert(config.reconnect_seconds == 300u);
    assert(config.reconnect_max_seconds == 300u);
    assert(config.reconnect_log_seconds == 300u);
}

static void test_discovery_configs(void)
{
    ag_config config;
    char error[AG_ERROR_SIZE];

    assert(ag_config_load("tests/fixtures/valid-discovery-serial.json", &config, error,
                          sizeof(error)) == 0);
    assert(config.radio_mode == AG_RADIO_DISCOVERY);
    assert(strcmp(config.radio_serial, "1234-5678-6600-ABCD") == 0);
    assert(config.discovery_ip[0] == '\0');
    assert(ag_config_load("tests/fixtures/valid-discovery-ip.json", &config, error,
                          sizeof(error)) == 0);
    assert(config.radio_mode == AG_RADIO_DISCOVERY);
    assert(strcmp(config.discovery_ip, "192.0.2.20") == 0);
    assert(ag_config_load("tests/fixtures/valid-discovery-dual.json", &config, error,
                          sizeof(error)) == 0);
    assert(strcmp(config.radio_serial, "1234-5678-6600-ABCD") == 0);
    assert(strcmp(config.discovery_ip, "192.0.2.20") == 0);
    assert(ag_config_load("tests/fixtures/invalid-mixed-radio-selector.json", &config, error,
                          sizeof(error)) != 0);
    assert(ag_config_load("tests/fixtures/invalid-discovery-ip.json", &config, error,
                          sizeof(error)) != 0);
    assert(ag_config_load("tests/fixtures/invalid-discovery-reconnect.json", &config, error,
                          sizeof(error)) != 0);
    assert(ag_config_load("tests/fixtures/invalid-radio-selector-missing.json", &config, error,
                          sizeof(error)) != 0);
}

static void test_policy(void)
{
    ag_config config;
    char error[AG_ERROR_SIZE];
    ag_policy_decision decision;
    assert(ag_config_load("config/config.example.json", &config, error, sizeof(error)) == 0);

    decision = ag_policy_evaluate(&config, true, 14.074, "ANT1");
    assert(decision.allowed && strcmp(decision.band, "20m") == 0);

    decision = ag_policy_evaluate(&config, true, 7.074, "ANT2");
    assert(!decision.allowed && decision.reason == AG_DECISION_COMBINATION_BLOCKED);

    decision = ag_policy_evaluate(&config, false, 0.0, "ANT1");
    assert(!decision.allowed && decision.reason == AG_DECISION_UNKNOWN_FREQUENCY);

    decision = ag_policy_evaluate(&config, true, 144.2, "ANT1");
    assert(!decision.allowed && decision.reason == AG_DECISION_OUTSIDE_KNOWN_BANDS);

    decision = ag_policy_evaluate(&config, true, 14.35, "ANT1");
    assert(decision.allowed);
    decision = ag_policy_evaluate(&config, true, 14.350001, "ANT1");
    assert(!decision.allowed && decision.reason == AG_DECISION_OUTSIDE_KNOWN_BANDS);
}

static void test_invalid_configs(void)
{
    ag_config config;
    char error[AG_ERROR_SIZE];

    assert(ag_config_load("tests/fixtures/invalid-2m.json", &config, error, sizeof(error)) != 0);
    assert(strstr(error, "unknown band") != NULL);

    assert(ag_config_load("tests/fixtures/invalid-missing-policy.json", &config, error,
                          sizeof(error)) != 0);
    assert(strstr(error, "policy.ANT2") != NULL);

    assert(ag_config_load("tests/fixtures/invalid-unknown-key.json", &config, error,
                          sizeof(error)) != 0);
    assert(strstr(error, "unknown key") != NULL);

    assert(ag_config_load("tests/fixtures/invalid-duplicate-policy.json", &config, error,
                          sizeof(error)) != 0);
    assert(strstr(error, "duplicate antenna") != NULL);

    assert(ag_config_load("tests/fixtures/invalid-reconnect-range.json", &config, error,
                          sizeof(error)) != 0);
    assert(strstr(error, "must be at least reconnect_seconds") != NULL);
}

int main(void)
{
    test_valid_config();
    test_policy();
    test_retry_backoff();
    test_legacy_reconnect_config();
    test_discovery_configs();
    test_invalid_configs();
    puts("core tests passed");
    return 0;
}
