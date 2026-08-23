#define _POSIX_C_SOURCE 200809L

#include "ag_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static bool verbose_enabled;

void ag_log_set_verbose(bool verbose)
{
    verbose_enabled = verbose;
}

void ag_log(ag_log_level level, const char *format, ...)
{
    static const char *const labels[] = {"INFO", "WARN", "ERROR", "DEBUG"};
    char timestamp[32];
    struct tm local_time;
    time_t now;
    va_list arguments;

    if (level == AG_LOG_DEBUG && !verbose_enabled) {
        return;
    }

    now = time(NULL);
    localtime_r(&now, &local_time);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time);
    fprintf(stderr, "%s %-5s ", timestamp, labels[level]);
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
    fflush(stderr);
}
