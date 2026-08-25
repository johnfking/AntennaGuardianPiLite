#ifndef AG_CONFIG_H
#define AG_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AG_MAX_ANTENNAS 8
#define AG_MAX_ANTENNA_ID 31
#define AG_MAX_HOST 255
#define AG_ERROR_SIZE 512

typedef struct {
    char id[AG_MAX_ANTENNA_ID + 1];
    uint16_t allowed_bands;
} ag_antenna_policy;

typedef struct {
    char host[AG_MAX_HOST + 1];
    uint16_t port;
    unsigned reconnect_seconds;
    unsigned reconnect_max_seconds;
    unsigned reconnect_log_seconds;
    size_t antenna_count;
    ag_antenna_policy antennas[AG_MAX_ANTENNAS];
} ag_config;

int ag_config_load(const char *path, ag_config *config, char *error, size_t error_size);
const ag_antenna_policy *ag_config_find_antenna(const ag_config *config, const char *id);

#endif
