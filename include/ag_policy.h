#ifndef AG_POLICY_H
#define AG_POLICY_H

#include <stdbool.h>
#include <stddef.h>

#include "ag_config.h"

typedef enum {
    AG_DECISION_ALLOWED,
    AG_DECISION_UNKNOWN_FREQUENCY,
    AG_DECISION_UNKNOWN_ANTENNA,
    AG_DECISION_OUTSIDE_KNOWN_BANDS,
    AG_DECISION_COMBINATION_BLOCKED
} ag_decision_reason;

typedef struct {
    bool allowed;
    ag_decision_reason reason;
    const char *band;
} ag_policy_decision;

size_t ag_band_count(void);
const char *ag_band_name(size_t index);
int ag_band_index(const char *name);
ag_policy_decision ag_policy_evaluate(
    const ag_config *config,
    bool has_frequency,
    double frequency_mhz,
    const char *antenna);

#endif
