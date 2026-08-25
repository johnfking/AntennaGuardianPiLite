#include "ag_config.h"

#include "ag_policy.h"
#include "cJSON.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define AG_MAX_CONFIG_BYTES (1024u * 1024u)

static int fail(char *error, size_t error_size, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
    return -1;
}

static char *read_file(const char *path, char *error, size_t error_size)
{
    FILE *file;
    long length;
    size_t bytes_read;
    char *contents;

    file = fopen(path, "rb");
    if (file == NULL) {
        fail(error, error_size, "cannot open %s: %s", path, strerror(errno));
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0
        || (unsigned long)length > AG_MAX_CONFIG_BYTES || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        fail(error, error_size, "configuration file is invalid or exceeds 1 MiB");
        return NULL;
    }

    contents = malloc((size_t)length + 1u);
    if (contents == NULL) {
        fclose(file);
        fail(error, error_size, "out of memory while reading configuration");
        return NULL;
    }
    bytes_read = fread(contents, 1u, (size_t)length, file);
    fclose(file);
    if (bytes_read != (size_t)length) {
        free(contents);
        fail(error, error_size, "could not read the complete configuration file");
        return NULL;
    }
    contents[length] = '\0';
    return contents;
}

static bool key_is_allowed(const char *key, const char *const *allowed, size_t count)
{
    size_t index;
    for (index = 0; index < count; ++index) {
        if (strcmp(key, allowed[index]) == 0) {
            return true;
        }
    }
    return false;
}

static int reject_unknown_keys(
    const cJSON *object,
    const char *section,
    const char *const *allowed,
    size_t count,
    char *error,
    size_t error_size)
{
    const cJSON *item;
    cJSON_ArrayForEach(item, object) {
        const cJSON *later;
        if (item->string == NULL || !key_is_allowed(item->string, allowed, count)) {
            return fail(error, error_size, "unknown key in %s: %s", section,
                        item->string == NULL ? "<unnamed>" : item->string);
        }
        for (later = item->next; later != NULL; later = later->next) {
            if (later->string != NULL && strcmp(item->string, later->string) == 0) {
                return fail(error, error_size, "duplicate key in %s: %s", section, item->string);
            }
        }
    }
    return 0;
}

static bool valid_antenna_id(const char *id)
{
    size_t index;
    size_t length = strlen(id);
    if (length == 0u || length > AG_MAX_ANTENNA_ID) {
        return false;
    }
    for (index = 0; index < length; ++index) {
        unsigned char character = (unsigned char)id[index];
        if (!isalnum(character) && character != '_' && character != '-') {
            return false;
        }
    }
    return true;
}

static bool valid_radio_serial(const char *serial)
{
    size_t index;
    size_t length = strlen(serial);
    if (length == 0u || length > AG_MAX_RADIO_SERIAL) {
        return false;
    }
    for (index = 0; index < length; ++index) {
        unsigned char character = (unsigned char)serial[index];
        if (!isalnum(character) && character != '-' && character != '_' && character != '.') {
            return false;
        }
    }
    return true;
}

const ag_antenna_policy *ag_config_find_antenna(const ag_config *config, const char *id)
{
    size_t index;
    for (index = 0; index < config->antenna_count; ++index) {
        if (strcasecmp(config->antennas[index].id, id) == 0) {
            return &config->antennas[index];
        }
    }
    return NULL;
}

static int parse_radio(
    const cJSON *root,
    ag_config *config,
    char *error,
    size_t error_size)
{
    static const char *const allowed[] = {
        "host", "serial", "discovery_ip", "port", "reconnect_seconds", "reconnect_max_seconds",
        "reconnect_log_seconds"};
    const cJSON *radio = cJSON_GetObjectItemCaseSensitive(root, "radio");
    const cJSON *host;
    const cJSON *serial;
    const cJSON *discovery_ip;
    const cJSON *port;
    const cJSON *reconnect;
    const cJSON *reconnect_max;
    const cJSON *reconnect_log;

    if (!cJSON_IsObject(radio)) {
        return fail(error, error_size, "radio must be an object");
    }
    if (reject_unknown_keys(radio, "radio", allowed, 7u, error, error_size) != 0) {
        return -1;
    }

    host = cJSON_GetObjectItemCaseSensitive(radio, "host");
    serial = cJSON_GetObjectItemCaseSensitive(radio, "serial");
    discovery_ip = cJSON_GetObjectItemCaseSensitive(radio, "discovery_ip");
    if (host == NULL && serial == NULL && discovery_ip == NULL) {
        return fail(error, error_size,
                    "radio must specify host, serial, discovery_ip, or both discovery selectors");
    }
    if (host != NULL && (serial != NULL || discovery_ip != NULL)) {
        return fail(error, error_size,
                    "radio.host cannot be combined with serial or discovery_ip");
    }
    if (host != NULL && (!cJSON_IsString(host) || host->valuestring[0] == '\0'
        || strlen(host->valuestring) > AG_MAX_HOST)) {
        return fail(error, error_size, "radio.host must be a non-empty string of at most %d characters",
                    AG_MAX_HOST);
    }
    if (serial != NULL && (!cJSON_IsString(serial) || !valid_radio_serial(serial->valuestring))) {
        return fail(error, error_size,
                    "radio.serial must use 1-%d letters, digits, '-', '_' or '.'",
                    AG_MAX_RADIO_SERIAL);
    }
    if (discovery_ip != NULL) {
        struct in_addr address;
        if (!cJSON_IsString(discovery_ip)
            || strlen(discovery_ip->valuestring) > AG_MAX_IPV4_TEXT
            || inet_pton(AF_INET, discovery_ip->valuestring, &address) != 1) {
            return fail(error, error_size, "radio.discovery_ip must be a valid IPv4 address");
        }
    }

    config->radio_mode = host == NULL ? AG_RADIO_DISCOVERY : AG_RADIO_DIRECT;
    if (host != NULL) {
        strcpy(config->host, host->valuestring);
    }
    if (serial != NULL) {
        strcpy(config->radio_serial, serial->valuestring);
    }
    if (discovery_ip != NULL) {
        strcpy(config->discovery_ip, discovery_ip->valuestring);
    }

    port = cJSON_GetObjectItemCaseSensitive(radio, "port");
    if (port != NULL && (!cJSON_IsNumber(port) || port->valuedouble < 1
                         || port->valuedouble > 65535 || port->valuedouble != port->valueint)) {
        return fail(error, error_size, "radio.port must be an integer from 1 through 65535");
    }
    config->port = port == NULL ? 4992u : (uint16_t)port->valueint;

    reconnect = cJSON_GetObjectItemCaseSensitive(radio, "reconnect_seconds");
    if (reconnect != NULL && (!cJSON_IsNumber(reconnect) || reconnect->valuedouble < 1
                              || reconnect->valuedouble > 300
                              || reconnect->valuedouble != reconnect->valueint)) {
        return fail(error, error_size, "radio.reconnect_seconds must be an integer from 1 through 300");
    }
    config->reconnect_seconds = reconnect == NULL ? 3u : (unsigned)reconnect->valueint;

    reconnect_max = cJSON_GetObjectItemCaseSensitive(radio, "reconnect_max_seconds");
    if (reconnect_max != NULL
        && (!cJSON_IsNumber(reconnect_max) || reconnect_max->valuedouble < 1
            || reconnect_max->valuedouble > 3600
            || reconnect_max->valuedouble != reconnect_max->valueint)) {
        return fail(error, error_size,
                    "radio.reconnect_max_seconds must be an integer from 1 through 3600");
    }
    config->reconnect_max_seconds = reconnect_max == NULL
        ? (config->reconnect_seconds > 30u ? config->reconnect_seconds : 30u)
        : (unsigned)reconnect_max->valueint;
    if (config->reconnect_max_seconds < config->reconnect_seconds) {
        return fail(error, error_size,
                    "radio.reconnect_max_seconds must be at least reconnect_seconds");
    }

    reconnect_log = cJSON_GetObjectItemCaseSensitive(radio, "reconnect_log_seconds");
    if (config->radio_mode == AG_RADIO_DISCOVERY
        && (port != NULL || reconnect != NULL || reconnect_max != NULL || reconnect_log != NULL)) {
        return fail(error, error_size,
                    "radio.port and reconnect settings are only valid with radio.host");
    }
    if (reconnect_log != NULL
        && (!cJSON_IsNumber(reconnect_log) || reconnect_log->valuedouble < 1
            || reconnect_log->valuedouble > 86400
            || reconnect_log->valuedouble != reconnect_log->valueint)) {
        return fail(error, error_size,
                    "radio.reconnect_log_seconds must be an integer from 1 through 86400");
    }
    config->reconnect_log_seconds = reconnect_log == NULL
        ? 300u
        : (unsigned)reconnect_log->valueint;
    return 0;
}

static int parse_antennas(
    const cJSON *root,
    ag_config *config,
    char *error,
    size_t error_size)
{
    static const char *const allowed[] = {"antennas"};
    const cJSON *interlock = cJSON_GetObjectItemCaseSensitive(root, "interlock");
    const cJSON *antennas;
    const cJSON *item;
    int array_size;
    int index;

    if (!cJSON_IsObject(interlock)) {
        return fail(error, error_size, "interlock must be an object");
    }
    if (reject_unknown_keys(interlock, "interlock", allowed, 1u, error, error_size) != 0) {
        return -1;
    }
    antennas = cJSON_GetObjectItemCaseSensitive(interlock, "antennas");
    array_size = cJSON_GetArraySize(antennas);
    if (!cJSON_IsArray(antennas) || array_size < 1 || array_size > AG_MAX_ANTENNAS) {
        return fail(error, error_size, "interlock.antennas must contain 1 through %d antenna IDs",
                    AG_MAX_ANTENNAS);
    }

    cJSON_ArrayForEach(item, antennas) {
        if (!cJSON_IsString(item) || !valid_antenna_id(item->valuestring)) {
            return fail(error, error_size, "each antenna ID must use 1-%d letters, digits, '-' or '_'",
                        AG_MAX_ANTENNA_ID);
        }
        for (index = 0; index < (int)config->antenna_count; ++index) {
            if (strcasecmp(config->antennas[index].id, item->valuestring) == 0) {
                return fail(error, error_size, "duplicate antenna ID: %s", item->valuestring);
            }
        }
        strcpy(config->antennas[config->antenna_count++].id, item->valuestring);
    }
    return 0;
}

static int parse_policy(
    const cJSON *root,
    ag_config *config,
    char *error,
    size_t error_size)
{
    const cJSON *policy = cJSON_GetObjectItemCaseSensitive(root, "policy");
    const cJSON *entry;
    size_t antenna_index;

    if (!cJSON_IsObject(policy)) {
        return fail(error, error_size, "policy must be an object");
    }

    cJSON_ArrayForEach(entry, policy) {
        const cJSON *later;
        if (entry->string == NULL || ag_config_find_antenna(config, entry->string) == NULL) {
            return fail(error, error_size, "policy contains an antenna not listed in interlock.antennas: %s",
                        entry->string == NULL ? "<unnamed>" : entry->string);
        }
        for (later = entry->next; later != NULL; later = later->next) {
            if (later->string != NULL && strcasecmp(entry->string, later->string) == 0) {
                return fail(error, error_size, "duplicate antenna in policy: %s", entry->string);
            }
        }
    }

    for (antenna_index = 0; antenna_index < config->antenna_count; ++antenna_index) {
        ag_antenna_policy *antenna = &config->antennas[antenna_index];
        const cJSON *bands = cJSON_GetObjectItem(policy, antenna->id);
        const cJSON *band;
        if (!cJSON_IsArray(bands)) {
            return fail(error, error_size, "policy.%s must be an array", antenna->id);
        }
        cJSON_ArrayForEach(band, bands) {
            int band_index;
            uint16_t flag;
            if (!cJSON_IsString(band) || (band_index = ag_band_index(band->valuestring)) < 0) {
                return fail(error, error_size, "policy.%s contains an unknown band", antenna->id);
            }
            flag = (uint16_t)(1u << band_index);
            if ((antenna->allowed_bands & flag) != 0u) {
                return fail(error, error_size, "policy.%s contains duplicate band %s",
                            antenna->id, band->valuestring);
            }
            antenna->allowed_bands |= flag;
        }
    }
    return 0;
}

int ag_config_load(const char *path, ag_config *config, char *error, size_t error_size)
{
    static const char *const allowed[] = {"radio", "interlock", "policy"};
    char *contents;
    cJSON *root;
    int result = -1;

    memset(config, 0, sizeof(*config));
    contents = read_file(path, error, error_size);
    if (contents == NULL) {
        return -1;
    }
    root = cJSON_Parse(contents);
    free(contents);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return fail(error, error_size, "configuration must contain one JSON object");
    }
    if (reject_unknown_keys(root, "root", allowed, 3u, error, error_size) == 0
        && parse_radio(root, config, error, error_size) == 0
        && parse_antennas(root, config, error, error_size) == 0
        && parse_policy(root, config, error, error_size) == 0) {
        result = 0;
    }
    cJSON_Delete(root);
    return result;
}
