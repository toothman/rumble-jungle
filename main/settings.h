// SPDX-License-Identifier: MIT
/*
 * Runtime settings: the single source of truth for everything that can be
 * changed from the web UI or MQTT. Settings are stored in NVS (one blob per
 * group) and every change notifies registered listeners, so web, MQTT and
 * the audio pipeline always agree.
 *
 * Every setting is described by a field in a flat table. The same key is
 * used in the JSON API ("eq2_gain"), the MQTT topics (<base>/eq2_gain) and
 * the Home Assistant discovery.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"
#include "app_config.h"
#include "dsp.h"

typedef enum { SRC_MODE_AUTO, SRC_MODE_A, SRC_MODE_B, SRC_MODE_MIX } src_mode_t;
typedef enum { INPUT_A = 0, INPUT_B = 1 } input_id_t;

typedef enum {
    SG_AUDIO  = 1 << 0,
    SG_MQTT   = 1 << 1,
    SG_NET    = 1 << 2,
    SG_STREAM = 1 << 3,
} settings_group_t;

typedef enum { SET_FROM_BOOT, SET_FROM_WEB, SET_FROM_MQTT } settings_source_t;

typedef struct {
    char  name[24];
    float trim_db;
    float threshold_dbfs;
} input_settings_t;

typedef struct {
    dsp_params_t     dsp;
    uint8_t          source_mode;       /* src_mode_t */
    uint8_t          priority;          /* input_id_t */
    input_settings_t input[NUM_INPUTS];
    uint32_t         signal_on_ms;
    uint32_t         signal_off_ms;
    uint32_t         xfade_ms;
} audio_settings_t;

typedef struct {
    bool enabled;
    char uri[96];
    char user[32];
    char password[64];
    char base_topic[32];
    char discovery_prefix[32];
    char device_name[32];
} mqtt_settings_t;

typedef struct {
    char hostname[32];
    bool dhcp;
    char ip[16];
    char netmask[16];
    char gateway[16];
    char dns[16];
} net_settings_t;

typedef struct {
    uint8_t bitrate;                    /* index into the bitrate list */
    char    name[32];
} stream_settings_t;

typedef struct {
    audio_settings_t  audio;
    mqtt_settings_t   mqtt;
    net_settings_t    net;
    stream_settings_t stream;
} settings_t;

typedef enum { F_BOOL, F_FLOAT, F_INT, F_ENUM, F_STR } field_type_t;

enum {
    SF_SECRET  = 1 << 0,    /* never sent back (passwords)                  */
    SF_RESTART = 1 << 1,    /* takes effect after a restart                 */
    SF_NO_MQTT = 1 << 2,    /* not exposed via MQTT                         */
    SF_CONFIG  = 1 << 3,    /* Home Assistant "configuration" entity        */
};

typedef bool (*settings_validator_t)(const char *value);

typedef struct {
    char                 key[24];
    char                 name[32];      /* human readable (HA entity name)  */
    field_type_t         type;
    settings_group_t     group;
    uint16_t             flags;
    uint16_t             offset;        /* offsetof(settings_t, ...)        */
    uint16_t             size;          /* bytes, for strings incl. NUL     */
    float                min, max, step;
    uint8_t              decimals;
    const char          *unit;
    const char          *icon;
    const char *const   *enum_tokens;   /* JSON / MQTT command values       */
    const char *const   *enum_labels;   /* MQTT state / HA select options   */
    uint8_t              enum_count;
    settings_validator_t validate;      /* strings only, NULL = any         */
} setting_field_t;

/* Loads settings from NVS (or defaults). Call once after nvs_flash_init(). */
void settings_init(void);

void settings_get(settings_t *out);

/* Monotonic counter, incremented on every change. */
uint32_t settings_rev(void);

const setting_field_t *settings_fields(int *count);
const setting_field_t *settings_find(const char *key);

/* Parses, validates and stores one value. err receives a short message. */
esp_err_t settings_set_str(const char *key, const char *value, settings_source_t src,
                           char *err, size_t errlen);

/* Applies a flat JSON object of key/value pairs. All values are validated
 * first; nothing is stored if one of them is invalid.                      */
esp_err_t settings_apply_json(const cJSON *obj, settings_source_t src,
                              char *err, size_t errlen);

/* Flat JSON object of all settings. Secrets are returned as "" together
 * with "<key>_set": true/false.                                            */
cJSON *settings_to_json(void);

/* Formats one value as an MQTT state payload (ON/OFF, number, enum label). */
void settings_format(const settings_t *s, const setting_field_t *f, char *buf, size_t len);

void settings_reset_eq(settings_source_t src);

/* True if a setting that only applies after a restart was changed. */
bool settings_restart_required(void);

/* Erases all settings and restarts. Does not return. */
void settings_factory_reset(void);

/* Writes pending changes to flash immediately (e.g. before a restart). */
void settings_flush(void);

/* Bitrate list (bit/s) used by the stream settings. */
int      settings_bitrate_count(void);
uint32_t settings_bitrate_bps(int index);

/* Called after every change, outside the settings lock, in the task that
 * made the change. Must not block. groups is a mask of settings_group_t.  */
typedef void (*settings_listener_t)(uint32_t groups, settings_source_t src, void *ctx);
void settings_add_listener(settings_listener_t cb, void *ctx);
