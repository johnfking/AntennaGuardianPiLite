#include "ag_policy.h"

#include <strings.h>

typedef struct {
    const char *name;
    double lower_mhz;
    double upper_mhz;
} ag_band;

static const ag_band BANDS[] = {
    {"160m", 1.8, 2.0},
    {"80m", 3.5, 4.0},
    {"60m", 5.25, 5.45},
    {"40m", 7.0, 7.3},
    {"30m", 10.1, 10.15},
    {"20m", 14.0, 14.35},
    {"17m", 18.068, 18.168},
    {"15m", 21.0, 21.45},
    {"12m", 24.89, 24.99},
    {"10m", 28.0, 29.7},
    {"6m", 50.0, 54.0},
};

size_t ag_band_count(void)
{
    return sizeof(BANDS) / sizeof(BANDS[0]);
}

const char *ag_band_name(size_t index)
{
    return index < ag_band_count() ? BANDS[index].name : NULL;
}

int ag_band_index(const char *name)
{
    size_t index;
    for (index = 0; index < ag_band_count(); ++index) {
        if (strcasecmp(name, BANDS[index].name) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static int band_for_frequency(double frequency_mhz)
{
    size_t index;
    for (index = 0; index < ag_band_count(); ++index) {
        if (frequency_mhz >= BANDS[index].lower_mhz
            && frequency_mhz <= BANDS[index].upper_mhz) {
            return (int)index;
        }
    }
    return -1;
}

ag_policy_decision ag_policy_evaluate(
    const ag_config *config,
    bool has_frequency,
    double frequency_mhz,
    const char *antenna)
{
    const ag_antenna_policy *policy;
    int band_index;

    if (!has_frequency) {
        return (ag_policy_decision){false, AG_DECISION_UNKNOWN_FREQUENCY, "Unknown"};
    }
    if (antenna == NULL || antenna[0] == '\0') {
        return (ag_policy_decision){false, AG_DECISION_UNKNOWN_ANTENNA, "Unknown"};
    }

    band_index = band_for_frequency(frequency_mhz);
    if (band_index < 0) {
        return (ag_policy_decision){false, AG_DECISION_OUTSIDE_KNOWN_BANDS, "Unknown"};
    }

    policy = ag_config_find_antenna(config, antenna);
    if (policy != NULL && (policy->allowed_bands & (uint16_t)(1u << band_index)) != 0u) {
        return (ag_policy_decision){true, AG_DECISION_ALLOWED, BANDS[band_index].name};
    }

    return (ag_policy_decision){
        false,
        AG_DECISION_COMBINATION_BLOCKED,
        BANDS[band_index].name,
    };
}
