#include "ag_config.h"
#include "ag_policy.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_valid_config(void)
{
    ag_config config;
    char error[AG_ERROR_SIZE];
    const ag_antenna_policy *ant1;

    assert(ag_config_load("config/config.example.json", &config, error, sizeof(error)) == 0);
    assert(strcmp(config.host, "10.0.0.107") == 0);
    assert(config.port == 4992u);
    assert(config.antenna_count == 2u);
    ant1 = ag_config_find_antenna(&config, "ant1");
    assert(ant1 != NULL);
    assert((ant1->allowed_bands & (1u << ag_band_index("160m"))) != 0u);
    assert((ant1->allowed_bands & (1u << ag_band_index("6m"))) == 0u);
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
}

int main(void)
{
    test_valid_config();
    test_policy();
    test_invalid_configs();
    puts("core tests passed");
    return 0;
}
