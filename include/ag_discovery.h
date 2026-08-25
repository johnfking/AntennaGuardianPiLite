#ifndef AG_DISCOVERY_H
#define AG_DISCOVERY_H

#include <signal.h>
#include <stdint.h>

#include "ag_config.h"

#define AG_MAX_RADIO_MODEL 63
#define AG_MAX_RADIO_NAME 63

typedef struct {
    char host[AG_MAX_IPV4_TEXT + 1];
    uint16_t port;
    char serial[AG_MAX_RADIO_SERIAL + 1];
    char model[AG_MAX_RADIO_MODEL + 1];
    char name[AG_MAX_RADIO_NAME + 1];
} ag_discovered_radio;

typedef enum {
    AG_DISCOVERY_STOPPED = 0,
    AG_DISCOVERY_MATCHED = 1,
    AG_DISCOVERY_ERROR = 2
} ag_discovery_result;

ag_discovery_result ag_discovery_wait(
    const ag_config *config,
    volatile sig_atomic_t *stop_requested,
    ag_discovered_radio *radio);

#endif
