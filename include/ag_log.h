#ifndef AG_LOG_H
#define AG_LOG_H

#include <stdbool.h>

typedef enum {
    AG_LOG_INFO,
    AG_LOG_WARNING,
    AG_LOG_ERROR,
    AG_LOG_DEBUG
} ag_log_level;

void ag_log_set_verbose(bool verbose);
void ag_log(ag_log_level level, const char *format, ...);

#endif
