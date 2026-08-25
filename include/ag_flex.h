#ifndef AG_FLEX_H
#define AG_FLEX_H

#include <signal.h>
#include <stdbool.h>

#include "ag_config.h"

typedef enum {
    AG_SESSION_STOPPED = 0,
    AG_SESSION_UNAVAILABLE = 1,
    AG_SESSION_DISCONNECTED = 2,
    AG_SESSION_ERROR = 3
} ag_session_result;

ag_session_result ag_flex_run_session(
    const ag_config *config,
    bool observe_only,
    volatile sig_atomic_t *stop_requested,
    bool log_connection_attempt);

#endif
