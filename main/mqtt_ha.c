// SPDX-License-Identifier: MIT
/*
 * MQTT connection and Home Assistant integration.
 *
 * Topics (below MQTT_BASE_TOPIC):
 *   status, level, signal, clipping, listeners, stream_url   read-only
 *   <setting>          current value of a setting (retained)
 *   <setting>/set      command topic to change it
 *
 * Settings: eq, eq<N>_gain, eq<N>_freq, eq<N>_q, eq_reset, rumble,
 *           rumble_freq, output_gain, auto_headroom
 */
#include "app_config.h"
#include "mqtt_ha.h"

#if MQTT_ENABLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "audio.h"
#include "dsp.h"
#include "stream_server.h"

#define POLL_INTERVAL_MS   250
#define LEVEL_EVERY_POLLS  8       /* publish level every 2 s        */
#define CLIP_HOLD_MS       2000    /* keep "clipping" on this long   */

#define T_AVAIL      MQTT_BASE_TOPIC "/status"
#define T_LEVEL      MQTT_BASE_TOPIC "/level"
#define T_SIGNAL     MQTT_BASE_TOPIC "/signal"
#define T_CLIP       MQTT_BASE_TOPIC "/clipping"
#define T_LISTENERS  MQTT_BASE_TOPIC "/listeners"
#define T_URL        MQTT_BASE_TOPIC "/stream_url"
#define T_PREFIX     MQTT_BASE_TOPIC "/"
#define T_SET_SUFFIX "/set"
#define T_SET_ALL    MQTT_BASE_TOPIC "/+/set"

/* Shared part of every discovery payload */
#define DEVICE_JSON \
    "\"availability_topic\":\"" T_AVAIL "\"," \
    "\"device\":{\"identifiers\":[\"" DEVICE_ID "\"],\"name\":\"" DEVICE_NAME "\"," \
    "\"manufacturer\":\"DIY\",\"model\":\"Turntable Streamer\"}"

static const char *TAG = "mqtt";
static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static volatile bool s_republish;        /* discovery + all states       */
static volatile bool s_republish_dsp;    /* all DSP settings only        */
static char s_url[96];

static void pub(const char *topic, const char *payload, int qos, bool retain)
{
    esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain);
}

/* ------------------------------------------------------------------------ */
/* Settings: mapping between MQTT keys and DSP parameters                   */
/* ------------------------------------------------------------------------ */

typedef enum { SETTING_NONE, SETTING_BOOL, SETTING_FLOAT } setting_type_t;

typedef struct {
    setting_type_t type;
    bool  *b;
    float *f;
    int    decimals;
} setting_ref_t;

static setting_ref_t lookup_setting(dsp_params_t *p, const char *key)
{
    setting_ref_t r = { SETTING_NONE };

    if (!strcmp(key, "eq"))            return (setting_ref_t){ SETTING_BOOL, .b = &p->eq_enabled };
    if (!strcmp(key, "rumble"))        return (setting_ref_t){ SETTING_BOOL, .b = &p->rumble_enabled };
    if (!strcmp(key, "auto_headroom")) return (setting_ref_t){ SETTING_BOOL, .b = &p->auto_headroom };
    if (!strcmp(key, "rumble_freq"))   return (setting_ref_t){ SETTING_FLOAT, .f = &p->rumble_freq, .decimals = 0 };
    if (!strcmp(key, "output_gain"))   return (setting_ref_t){ SETTING_FLOAT, .f = &p->output_gain, .decimals = 1 };

    int band;
    char field[8], extra;
    if (sscanf(key, "eq%d_%7[a-z]%c", &band, field, &extra) == 2 && band >= 1 && band <= EQ_BANDS) {
        band--;
        if (!strcmp(field, "gain")) return (setting_ref_t){ SETTING_FLOAT, .f = &p->band_gain[band], .decimals = 1 };
        if (!strcmp(field, "freq")) return (setting_ref_t){ SETTING_FLOAT, .f = &p->band_freq[band], .decimals = 0 };
        if (!strcmp(field, "q"))    return (setting_ref_t){ SETTING_FLOAT, .f = &p->band_q[band],    .decimals = 2 };
    }
    return r;
}

static void publish_setting(dsp_params_t *p, const char *key)
{
    setting_ref_t r = lookup_setting(p, key);
    char topic[96], value[16];

    if (r.type == SETTING_BOOL)       snprintf(value, sizeof(value), "%s", *r.b ? "ON" : "OFF");
    else if (r.type == SETTING_FLOAT) snprintf(value, sizeof(value), "%.*f", r.decimals, *r.f);
    else return;

    snprintf(topic, sizeof(topic), T_PREFIX "%s", key);
    pub(topic, value, 1, true);
}

static void publish_all_settings(void)
{
    static const char *const keys[] = { "eq", "rumble", "rumble_freq", "output_gain", "auto_headroom" };
    static const char *const band_fields[] = { "gain", "freq", "q" };
    dsp_params_t p;
    char key[24];

    dsp_get_params(&p);
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) publish_setting(&p, keys[i]);
    for (int b = 1; b <= EQ_BANDS; b++) {
        for (size_t i = 0; i < 3; i++) {
            snprintf(key, sizeof(key), "eq%d_%s", b, band_fields[i]);
            publish_setting(&p, key);
        }
    }
}

static bool parse_bool(const char *s, bool *out)
{
    if (!strcasecmp(s, "ON")  || !strcmp(s, "1") || !strcasecmp(s, "true"))  { *out = true;  return true; }
    if (!strcasecmp(s, "OFF") || !strcmp(s, "0") || !strcasecmp(s, "false")) { *out = false; return true; }
    return false;
}

static bool parse_float(const char *s, float *out)
{
    char *end;
    float v = strtof(s, &end);
    if (end == s || !isfinite(v)) return false;
    *out = v;
    return true;
}

static void handle_command(const char *topic, const char *value)
{
    const size_t plen = strlen(T_PREFIX), slen = strlen(T_SET_SUFFIX), tlen = strlen(topic);
    if (tlen <= plen + slen || strncmp(topic, T_PREFIX, plen) != 0 ||
        strcmp(topic + tlen - slen, T_SET_SUFFIX) != 0) {
        return;
    }

    char key[32];
    const size_t klen = tlen - plen - slen;
    if (klen >= sizeof(key)) return;
    memcpy(key, topic + plen, klen);
    key[klen] = '\0';

    if (!strcmp(key, "eq_reset")) {
        ESP_LOGI(TAG, "Resetting equalizer");
        dsp_reset_eq();
        s_republish_dsp = true;
        return;
    }

    dsp_params_t p;
    dsp_get_params(&p);
    setting_ref_t r = lookup_setting(&p, key);
    bool ok = (r.type == SETTING_BOOL)  ? parse_bool(value, r.b)
            : (r.type == SETTING_FLOAT) ? parse_float(value, r.f)
            : false;
    if (!ok) {
        ESP_LOGW(TAG, "Ignoring invalid command %s = '%s'", key, value);
        return;
    }

    dsp_update(&p);
    dsp_get_params(&p);          /* report the value after clamping */
    publish_setting(&p, key);
    ESP_LOGI(TAG, "Set %s = %s", key, value);
}

/* ------------------------------------------------------------------------ */
/* Home Assistant discovery                                                 */
/* ------------------------------------------------------------------------ */

static void discovery(const char *component, const char *key, const char *body)
{
    char topic[160], payload[1024];
    snprintf(topic, sizeof(topic), HA_DISCOVERY_PREFIX "/%s/" DEVICE_ID "/%s/config", component, key);
    snprintf(payload, sizeof(payload),
             "{\"unique_id\":\"" DEVICE_ID "_%s\",%s," DEVICE_JSON "}", key, body);
    pub(topic, payload, 1, true);
}

static void disc_number(const char *key, const char *name, float min, float max, float step,
                        const char *unit, bool is_config)
{
    char body[512];
    snprintf(body, sizeof(body),
             "\"name\":\"%s\",\"state_topic\":\"" T_PREFIX "%s\","
             "\"command_topic\":\"" T_PREFIX "%s" T_SET_SUFFIX "\","
             "\"min\":%g,\"max\":%g,\"step\":%g,\"mode\":\"%s\"%s%s%s%s",
             name, key, key, min, max, step, is_config ? "box" : "slider",
             unit[0] ? ",\"unit_of_measurement\":\"" : "", unit, unit[0] ? "\"" : "",
             is_config ? ",\"entity_category\":\"config\"" : "");
    discovery("number", key, body);
}

static void disc_switch(const char *key, const char *name, const char *icon, bool is_config)
{
    char body[384];
    snprintf(body, sizeof(body),
             "\"name\":\"%s\",\"state_topic\":\"" T_PREFIX "%s\","
             "\"command_topic\":\"" T_PREFIX "%s" T_SET_SUFFIX "\",\"icon\":\"%s\"%s",
             name, key, key, icon, is_config ? ",\"entity_category\":\"config\"" : "");
    discovery("switch", key, body);
}

static void disc_button(const char *key, const char *name, const char *icon)
{
    char body[384];
    snprintf(body, sizeof(body),
             "\"name\":\"%s\",\"command_topic\":\"" T_PREFIX "%s" T_SET_SUFFIX "\","
             "\"payload_press\":\"PRESS\",\"icon\":\"%s\",\"entity_category\":\"config\"",
             name, key, icon);
    discovery("button", key, body);
}

static void publish_discovery(void)
{
    /* Status */
    discovery("sensor", "level",
              "\"name\":\"Level\",\"state_topic\":\"" T_LEVEL "\","
              "\"unit_of_measurement\":\"dBFS\",\"state_class\":\"measurement\","
              "\"icon\":\"mdi:waveform\"");
    discovery("binary_sensor", "signal",
              "\"name\":\"Signal\",\"state_topic\":\"" T_SIGNAL "\",\"device_class\":\"sound\"");
    discovery("binary_sensor", "clipping",
              "\"name\":\"Clipping\",\"state_topic\":\"" T_CLIP "\",\"device_class\":\"problem\"");
    discovery("sensor", "listeners",
              "\"name\":\"Listeners\",\"state_topic\":\"" T_LISTENERS "\","
              "\"icon\":\"mdi:speaker-wireless\"");
    discovery("sensor", "stream_url",
              "\"name\":\"Stream URL\",\"state_topic\":\"" T_URL "\","
              "\"icon\":\"mdi:link\",\"entity_category\":\"diagnostic\"");

    /* Equalizer */
    disc_switch("eq", "Equalizer", "mdi:equalizer", false);
    char key[24], name[40];
    for (int b = 1; b <= EQ_BANDS; b++) {
        snprintf(key, sizeof(key), "eq%d_gain", b);
        snprintf(name, sizeof(name), "EQ %d Gain", b);
        disc_number(key, name, EQ_GAIN_MIN_DB, EQ_GAIN_MAX_DB, EQ_GAIN_STEP_DB, "dB", false);

        snprintf(key, sizeof(key), "eq%d_freq", b);
        snprintf(name, sizeof(name), "EQ %d Frequency", b);
        disc_number(key, name, EQ_FREQ_MIN_HZ, EQ_FREQ_MAX_HZ, 1, "Hz", true);

        snprintf(key, sizeof(key), "eq%d_q", b);
        snprintf(name, sizeof(name), "EQ %d Q", b);
        disc_number(key, name, EQ_Q_MIN, EQ_Q_MAX, 0.05f, "", true);
    }
    disc_button("eq_reset", "Reset Equalizer", "mdi:restore");

    /* Rumble filter and gain */
    disc_switch("rumble", "Rumble Filter", "mdi:sine-wave", false);
    disc_number("rumble_freq", "Rumble Filter Frequency",
                RUMBLE_FREQ_MIN_HZ, RUMBLE_FREQ_MAX_HZ, 1, "Hz", true);
    disc_number("output_gain", "Output Gain",
                OUTPUT_GAIN_MIN_DB, OUTPUT_GAIN_MAX_DB, 0.5f, "dB", false);
    disc_switch("auto_headroom", "Auto Headroom", "mdi:arrow-collapse-vertical", true);
}

/* ------------------------------------------------------------------------ */
/* MQTT events and status task                                              */
/* ------------------------------------------------------------------------ */

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to broker");
        pub(T_AVAIL, "online", 1, true);
        esp_mqtt_client_subscribe(s_client, T_SET_ALL, 1);
        s_connected = true;
        s_republish = true;
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from broker");
        s_connected = false;
        break;

    case MQTT_EVENT_DATA: {
        /* Commands are tiny; ignore anything fragmented or oversized */
        char topic[128], value[32];
        if (ev->current_data_offset != 0 || ev->data_len != ev->total_data_len) break;
        if (ev->topic_len <= 0 || ev->topic_len >= (int)sizeof(topic)) break;
        if (ev->data_len >= (int)sizeof(value)) break;
        memcpy(topic, ev->topic, ev->topic_len);
        topic[ev->topic_len] = '\0';
        memcpy(value, ev->data, ev->data_len);
        value[ev->data_len] = '\0';
        handle_command(topic, value);
        break;
    }

    default:
        break;
    }
}

static void mqtt_task(void *arg)
{
    bool signal = false, clipping = false;
    int64_t above_since = 0, below_since = 0, clip_until = 0;
    int last_listeners = -1, polls = 0;
    char buf[16];

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
        const float level = audio_get_level_dbfs();
        const int64_t now = esp_timer_get_time() / 1000;
        bool signal_changed = false;

        /* Signal detection with hysteresis */
        if (level > SIGNAL_THRESHOLD_DBFS) {
            below_since = 0;
            if (!above_since) above_since = now;
            if (!signal && now - above_since >= SIGNAL_ON_HOLD_MS) { signal = true; signal_changed = true; }
        } else {
            above_since = 0;
            if (!below_since) below_since = now;
            if (signal && now - below_since >= SIGNAL_OFF_HOLD_MS) { signal = false; signal_changed = true; }
        }

        if (audio_take_clip_flag()) clip_until = now + CLIP_HOLD_MS;
        const bool clipping_now = now < clip_until;

        if (!s_connected) continue;

        const bool full = s_republish;
        if (full) {
            s_republish = false;
            publish_discovery();
            if (s_url[0]) pub(T_URL, s_url, 1, true);
        }
        if (full || s_republish_dsp) {
            s_republish_dsp = false;
            publish_all_settings();
        }

        if (signal_changed || full) pub(T_SIGNAL, signal ? "ON" : "OFF", 1, true);

        if (clipping_now != clipping || full) {
            clipping = clipping_now;
            pub(T_CLIP, clipping ? "ON" : "OFF", 1, true);
        }

        const int listeners = stream_server_client_count();
        if (listeners != last_listeners || full) {
            snprintf(buf, sizeof(buf), "%d", listeners);
            pub(T_LISTENERS, buf, 1, true);
            last_listeners = listeners;
        }

        if (++polls >= LEVEL_EVERY_POLLS) {
            snprintf(buf, sizeof(buf), "%.1f", level);
            pub(T_LEVEL, buf, 0, false);
            polls = 0;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

void mqtt_ha_set_stream_url(const char *url)
{
    strlcpy(s_url, url, sizeof(s_url));
    if (s_connected) pub(T_URL, s_url, 1, true);
}

void mqtt_ha_start(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.client_id = DEVICE_ID,
        .credentials.username = strlen(MQTT_USER) ? MQTT_USER : NULL,
        .credentials.authentication.password = strlen(MQTT_PASSWORD) ? MQTT_PASSWORD : NULL,
        .session.last_will = {
            .topic = T_AVAIL, .msg = "offline", .qos = 1, .retain = 1,
        },
        .buffer = { .size = 2048, .out_size = 2048 },
    };
    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL);
    esp_mqtt_client_start(s_client);
    xTaskCreatePinnedToCore(mqtt_task, "mqtt_ha", 6144, NULL, 4, NULL, NET_TASK_CORE);
}

#else /* MQTT disabled */

void mqtt_ha_start(void) {}
void mqtt_ha_set_stream_url(const char *url) { (void)url; }

#endif
