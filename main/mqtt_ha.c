// SPDX-License-Identifier: MIT
/*
 * MQTT connection and Home Assistant integration.
 *
 * Topics (below the configured base topic):
 *   status                      online / offline (availability, retained)
 *   active_source, active_input, signal, in_a_level, in_b_level,
 *   in_a_signal, in_b_signal, clipping, listeners, stream_url, ip, uptime
 *                               read-only status
 *   <setting>                   current value of a setting (retained)
 *   <setting>/set               command topic to change it
 *   eq_reset/set, restart/set   buttons (payload PRESS)
 *
 * All settings without SF_NO_MQTT in the settings table are exposed; the
 * Home Assistant discovery is generated from the same table.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "app_config.h"
#include "settings.h"
#include "audio.h"
#include "network.h"
#include "stream_server.h"
#include "mqtt_ha.h"

#define POLL_INTERVAL_MS   250
#define LEVEL_INTERVAL_MS  5000
#define UPTIME_INTERVAL_MS 60000

static const char *TAG = "mqtt";

static char s_device_id[32];
static char s_ip[16];
static volatile bool s_ip_changed;

/* Owned by the poller task */
static esp_mqtt_client_handle_t s_client;
static mqtt_settings_t s_cfg;           /* settings the client runs with */
static settings_t s_published;          /* last published setting states */
static char s_set_prefix[40];           /* "<base>/" for command parsing */

/* Flags set by event handlers / listeners, consumed by the poller */
static volatile bool s_connected;
static volatile bool s_reconfigure = true;
static volatile bool s_republish_all;   /* discovery + every state */
static volatile bool s_states_dirty;    /* settings changed         */
static volatile bool s_restart_req;
static uint64_t s_echo_fields;          /* field bits to republish (atomic) */
static char s_error[64];

void mqtt_ha_get_status(bool *enabled, bool *connected, char *error, size_t errlen)
{
    *enabled = s_cfg.enabled;
    *connected = s_connected;
    strlcpy(error, s_cfg.enabled && !s_connected ? s_error : "", errlen);
}

const char *mqtt_ha_device_id(void) { return s_device_id; }

void mqtt_ha_set_ip(const char *ip)
{
    strlcpy(s_ip, ip, sizeof(s_ip));
    s_ip_changed = true;
}

/* ------------------------------------------------------------------------ */
/* Publishing helpers (poller task only)                                    */
/* ------------------------------------------------------------------------ */

static void pub(const char *subtopic, const char *payload, bool retain)
{
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/%s", s_cfg.base_topic, subtopic);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, retain);
}

static void pub_setting(const settings_t *s, const setting_field_t *f)
{
    char value[64];
    settings_format(s, f, value, sizeof(value));
    pub(f->key, value, true);
}

static bool field_on_mqtt(const setting_field_t *f) { return !(f->flags & SF_NO_MQTT); }

static void publish_all_settings(const settings_t *s)
{
    int n;
    const setting_field_t *fields = settings_fields(&n);
    for (int i = 0; i < n; i++) {
        if (field_on_mqtt(&fields[i])) pub_setting(s, &fields[i]);
    }
}

static void publish_changed_settings(const settings_t *s)
{
    int n;
    const setting_field_t *fields = settings_fields(&n);
    for (int i = 0; i < n; i++) {
        const setting_field_t *f = &fields[i];
        if (field_on_mqtt(f) &&
            memcmp((const uint8_t *)s + f->offset, (const uint8_t *)&s_published + f->offset,
                   f->size) != 0) {
            pub_setting(s, f);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Home Assistant discovery                                                 */
/* ------------------------------------------------------------------------ */

static void discovery_topic(char *buf, size_t len, const char *prefix, const char *component,
                            const char *key)
{
    snprintf(buf, len, "%s/%s/%s/%s/config", prefix, component, s_device_id, key);
}

/* Common part of every discovery payload */
static cJSON *disc_base(const settings_t *s, const char *key, const char *name, const char *component)
{
    char buf[128], topic[96];
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *o = cJSON_CreateObject();

    snprintf(buf, sizeof(buf), "%s_%s", s_device_id, key);
    cJSON_AddStringToObject(o, "unique_id", buf);

    /* Entity id suggestion: switch.rumble_jungle_eq etc. */
    snprintf(buf, sizeof(buf), "%s.%s_%s", component, s->mqtt.base_topic, key);
    for (char *p = buf; *p; p++) if (*p == '/' || *p == '-') *p = '_';
    cJSON_AddStringToObject(o, "default_entity_id", buf);

    cJSON_AddBoolToObject(o, "has_entity_name", true);
    cJSON_AddStringToObject(o, "name", name);
    snprintf(topic, sizeof(topic), "%s/status", s->mqtt.base_topic);
    cJSON_AddStringToObject(o, "availability_topic", topic);

    cJSON *dev = cJSON_AddObjectToObject(o, "device");
    cJSON *ids = cJSON_AddArrayToObject(dev, "identifiers");
    cJSON_AddItemToArray(ids, cJSON_CreateString(s_device_id));
    network_info_t net;
    network_get_info(&net);
    cJSON *conns = cJSON_AddArrayToObject(dev, "connections");
    cJSON *mac = cJSON_CreateArray();
    cJSON_AddItemToArray(mac, cJSON_CreateString("mac"));
    cJSON_AddItemToArray(mac, cJSON_CreateString(net.mac));
    cJSON_AddItemToArray(conns, mac);
    cJSON_AddStringToObject(dev, "name", s->mqtt.device_name);
    cJSON_AddStringToObject(dev, "manufacturer", "DIY");
    cJSON_AddStringToObject(dev, "model", "Rumble Jungle");
    cJSON_AddStringToObject(dev, "hw_version", "ESP32-P4-ETH + 2x PCM1808");
    cJSON_AddStringToObject(dev, "sw_version", app->version);
    if (s_ip[0]) {
        snprintf(buf, sizeof(buf), "http://%s/", s_ip);
        cJSON_AddStringToObject(dev, "configuration_url", buf);
    }

    cJSON *origin = cJSON_AddObjectToObject(o, "origin");
    cJSON_AddStringToObject(origin, "name", "Rumble Jungle");
    cJSON_AddStringToObject(origin, "sw", app->version);
    cJSON_AddStringToObject(origin, "support_url", "https://github.com/toothman/rumble-jungle");
    return o;
}

static void disc_state(cJSON *o, const settings_t *s, const char *key, bool command)
{
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/%s", s->mqtt.base_topic, key);
    cJSON_AddStringToObject(o, "state_topic", topic);
    if (command) {
        snprintf(topic, sizeof(topic), "%s/%s/set", s->mqtt.base_topic, key);
        cJSON_AddStringToObject(o, "command_topic", topic);
    }
}

static void disc_send(const char *component, const char *key, cJSON *o)
{
    char topic[128];
    discovery_topic(topic, sizeof(topic), s_cfg.discovery_prefix, component, key);
    char *json = cJSON_PrintUnformatted(o);
    if (json) {
        esp_mqtt_client_publish(s_client, topic, json, 0, 0, true);
        free(json);
    }
    cJSON_Delete(o);
}

/* Entity name for a setting: "Input A Trim" becomes "Aux 1 Trim" */
static void field_entity_name(const settings_t *s, const setting_field_t *f, char *buf, size_t len)
{
    if (!strncmp(f->key, "in_", 3) && strcmp(f->key + 4, "_name") != 0) {
        const int i = f->key[3] - 'a';
        snprintf(buf, len, "%s%s", s->audio.input[i].name, f->name + strlen("Input A"));
    } else {
        strlcpy(buf, f->name, len);
    }
}

static const char *field_component(const setting_field_t *f)
{
    switch (f->type) {
    case F_BOOL: return "switch";
    case F_ENUM: return "select";
    case F_STR:  return "text";
    default:     return "number";
    }
}

static void publish_field_discovery(const settings_t *s, const setting_field_t *f)
{
    char name[48];
    const char *component = field_component(f);
    field_entity_name(s, f, name, sizeof(name));
    cJSON *o = disc_base(s, f->key, name, component);
    disc_state(o, s, f->key, true);
    if (f->icon) cJSON_AddStringToObject(o, "icon", f->icon);
    if (f->flags & SF_CONFIG) cJSON_AddStringToObject(o, "entity_category", "config");

    switch (f->type) {
    case F_FLOAT:
    case F_INT:
        cJSON_AddNumberToObject(o, "min", f->min);
        cJSON_AddNumberToObject(o, "max", f->max);
        cJSON_AddNumberToObject(o, "step", f->step);
        cJSON_AddStringToObject(o, "mode", (f->flags & SF_CONFIG) ? "box" : "slider");
        if (f->unit) cJSON_AddStringToObject(o, "unit_of_measurement", f->unit);
        break;
    case F_ENUM: {
        cJSON *opts = cJSON_AddArrayToObject(o, "options");
        for (int i = 0; i < f->enum_count; i++) {
            cJSON_AddItemToArray(opts, cJSON_CreateString(f->enum_labels[i]));
        }
        break;
    }
    case F_STR:
        cJSON_AddNumberToObject(o, "min", 1);
        cJSON_AddNumberToObject(o, "max", f->size - 1);
        break;
    default:
        break;
    }
    disc_send(component, f->key, o);
}

static void publish_sensor(const settings_t *s, const char *component, const char *key,
                           const char *name, const char *device_class, const char *unit,
                           const char *icon, const char *category)
{
    cJSON *o = disc_base(s, key, name, component);
    disc_state(o, s, key, false);
    if (device_class) cJSON_AddStringToObject(o, "device_class", device_class);
    if (unit) {
        cJSON_AddStringToObject(o, "unit_of_measurement", unit);
        cJSON_AddStringToObject(o, "state_class", "measurement");
    }
    if (icon) cJSON_AddStringToObject(o, "icon", icon);
    if (category) cJSON_AddStringToObject(o, "entity_category", category);
    disc_send(component, key, o);
}

static void publish_button(const settings_t *s, const char *key, const char *name,
                           const char *device_class, const char *icon)
{
    char topic[96];
    cJSON *o = disc_base(s, key, name, "button");
    snprintf(topic, sizeof(topic), "%s/%s/set", s->mqtt.base_topic, key);
    cJSON_AddStringToObject(o, "command_topic", topic);
    cJSON_AddStringToObject(o, "payload_press", "PRESS");
    if (device_class) cJSON_AddStringToObject(o, "device_class", device_class);
    if (icon) cJSON_AddStringToObject(o, "icon", icon);
    cJSON_AddStringToObject(o, "entity_category", "config");
    disc_send("button", key, o);
}

static void publish_discovery(const settings_t *s)
{
    char name[48], key[16];

    /* Which source is on air */
    publish_sensor(s, "sensor", "active_source", "Active Source", NULL, NULL, "mdi:import", NULL);
    cJSON *o = disc_base(s, "active_input", "Active Input", "sensor");
    disc_state(o, s, "active_input", false);
    cJSON_AddStringToObject(o, "device_class", "enum");
    cJSON *opts = cJSON_AddArrayToObject(o, "options");
    static const char *const k_active[] = { "a", "b", "mix", "none" };
    for (int i = 0; i < 4; i++) cJSON_AddItemToArray(opts, cJSON_CreateString(k_active[i]));
    cJSON_AddStringToObject(o, "icon", "mdi:import");
    cJSON_AddStringToObject(o, "entity_category", "diagnostic");
    disc_send("sensor", "active_input", o);

    publish_sensor(s, "binary_sensor", "signal", "Signal", "sound", NULL, NULL, NULL);
    for (int i = 0; i < NUM_INPUTS; i++) {
        snprintf(key, sizeof(key), "in_%c_level", 'a' + i);
        snprintf(name, sizeof(name), "%s Level", s->audio.input[i].name);
        publish_sensor(s, "sensor", key, name, NULL, "dBFS", "mdi:waveform", NULL);
        snprintf(key, sizeof(key), "in_%c_signal", 'a' + i);
        snprintf(name, sizeof(name), "%s Signal", s->audio.input[i].name);
        publish_sensor(s, "binary_sensor", key, name, "sound", NULL, NULL, NULL);
    }
    publish_sensor(s, "binary_sensor", "clipping", "Clipping", "problem", NULL, NULL, NULL);
    publish_sensor(s, "sensor", "listeners", "Listeners", NULL, NULL, "mdi:speaker-wireless", NULL);
    publish_sensor(s, "sensor", "stream_url", "Stream URL", NULL, NULL, "mdi:link", "diagnostic");
    publish_sensor(s, "sensor", "ip", "IP Address", NULL, NULL, "mdi:ip-network", "diagnostic");

    o = disc_base(s, "uptime", "Uptime", "sensor");
    disc_state(o, s, "uptime", false);
    cJSON_AddStringToObject(o, "device_class", "duration");
    cJSON_AddStringToObject(o, "unit_of_measurement", "s");
    cJSON_AddStringToObject(o, "entity_category", "diagnostic");
    disc_send("sensor", "uptime", o);

    /* Settings */
    int n;
    const setting_field_t *fields = settings_fields(&n);
    for (int i = 0; i < n; i++) {
        if (field_on_mqtt(&fields[i])) publish_field_discovery(s, &fields[i]);
    }

    publish_button(s, "eq_reset", "Reset Equalizer", NULL, "mdi:restore");
    publish_button(s, "restart", "Restart", "restart", NULL);
}

/* Removes all discovery entries (retained, empty payload) */
static void clear_discovery(const mqtt_settings_t *cfg)
{
    static const char *const status[][2] = {
        { "sensor", "active_source" }, { "sensor", "active_input" }, { "binary_sensor", "signal" },
        { "sensor", "in_a_level" }, { "sensor", "in_b_level" },
        { "binary_sensor", "in_a_signal" }, { "binary_sensor", "in_b_signal" },
        { "binary_sensor", "clipping" }, { "sensor", "listeners" }, { "sensor", "stream_url" },
        { "sensor", "ip" }, { "sensor", "uptime" }, { "button", "eq_reset" }, { "button", "restart" },
    };
    char topic[128];
    for (size_t i = 0; i < sizeof(status) / sizeof(status[0]); i++) {
        discovery_topic(topic, sizeof(topic), cfg->discovery_prefix, status[i][0], status[i][1]);
        esp_mqtt_client_publish(s_client, topic, "", 0, 1, true);
    }
    int n;
    const setting_field_t *fields = settings_fields(&n);
    for (int i = 0; i < n; i++) {
        if (!field_on_mqtt(&fields[i])) continue;
        discovery_topic(topic, sizeof(topic), cfg->discovery_prefix,
                        field_component(&fields[i]), fields[i].key);
        esp_mqtt_client_publish(s_client, topic, "", 0, 1, true);
    }
}

/* ------------------------------------------------------------------------ */
/* Commands and events                                                      */
/* ------------------------------------------------------------------------ */

static void handle_command(const char *topic, const char *value)
{
    const size_t plen = strlen(s_set_prefix), tlen = strlen(topic);
    if (tlen <= plen + 4 || strncmp(topic, s_set_prefix, plen) != 0 ||
        strcmp(topic + tlen - 4, "/set") != 0) {
        return;
    }
    char key[32];
    const size_t klen = tlen - plen - 4;
    if (klen >= sizeof(key)) return;
    memcpy(key, topic + plen, klen);
    key[klen] = '\0';

    if (!strcmp(key, "eq_reset")) {
        ESP_LOGI(TAG, "Resetting equalizer");
        settings_reset_eq(SET_FROM_MQTT);
        return;
    }
    if (!strcmp(key, "restart")) {
        s_restart_req = true;
        return;
    }

    const setting_field_t *f = settings_find(key);
    if (!f || !field_on_mqtt(f)) return;

    char err[64];
    if (settings_set_str(key, value, SET_FROM_MQTT, err, sizeof(err)) != ESP_OK) {
        ESP_LOGW(TAG, "Ignoring %s = '%s' (%s)", key, value, err);
    }
    /* Report the value actually in use (clamped, or unchanged if invalid) */
    int n;
    const setting_field_t *fields = settings_fields(&n);
    __atomic_fetch_or(&s_echo_fields, 1ULL << (f - fields), __ATOMIC_SEQ_CST);
}

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "Connected to broker");
        char topic[64];
        snprintf(topic, sizeof(topic), "%s/+/set", s_cfg.base_topic);
        esp_mqtt_client_subscribe(s_client, topic, 0);
        s_error[0] = '\0';
        s_connected = true;
        s_republish_all = true;
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        if (s_connected) ESP_LOGW(TAG, "Disconnected from broker");
        s_connected = false;
        if (!s_error[0]) strlcpy(s_error, "not connected", sizeof(s_error));
        break;

    case MQTT_EVENT_ERROR:
        if (ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            snprintf(s_error, sizeof(s_error), "connection refused (code %d)",
                     ev->error_handle->connect_return_code);
        } else if (ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            strlcpy(s_error, "broker unreachable", sizeof(s_error));
        }
        break;

    case MQTT_EVENT_DATA: {
        /* Commands are small; ignore anything fragmented or oversized */
        char topic[128], value[128];
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

static void on_settings(uint32_t groups, settings_source_t src, void *ctx)
{
    if (groups & SG_MQTT) s_reconfigure = true;
    if (groups & (SG_AUDIO | SG_STREAM)) s_states_dirty = true;
}

/* ------------------------------------------------------------------------ */
/* Client lifecycle (poller task)                                           */
/* ------------------------------------------------------------------------ */

static void client_stop(bool remove_discovery)
{
    if (!s_client) return;
    if (s_connected) {
        if (remove_discovery) clear_discovery(&s_cfg);
        char topic[64];
        snprintf(topic, sizeof(topic), "%s/status", s_cfg.base_topic);
        esp_mqtt_client_publish(s_client, topic, "offline", 0, 1, true);
        vTaskDelay(pdMS_TO_TICKS(300));   /* let the outbox drain */
    }
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_connected = false;
}

static void client_start(void)
{
    static char will_topic[64];
    snprintf(will_topic, sizeof(will_topic), "%s/status", s_cfg.base_topic);
    snprintf(s_set_prefix, sizeof(s_set_prefix), "%s/", s_cfg.base_topic);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_cfg.uri,
        .credentials.client_id = s_device_id,
        .credentials.username = s_cfg.user[0] ? s_cfg.user : NULL,
        .credentials.authentication.password = s_cfg.password[0] ? s_cfg.password : NULL,
        .session.last_will = {
            .topic = will_topic, .msg = "offline", .qos = 1, .retain = 1,
        },
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 10000,
        .buffer = { .size = 2048, .out_size = 4096 },
        .outbox.limit = 64 * 1024,
        .task.stack_size = 6144,
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        strlcpy(s_error, "invalid broker address", sizeof(s_error));
        ESP_LOGE(TAG, "Invalid MQTT configuration");
        return;
    }
    strlcpy(s_error, "connecting", sizeof(s_error));
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "Connecting to %s", s_cfg.uri);
}

static void reconfigure(void)
{
    settings_t s;
    settings_get(&s);
    const mqtt_settings_t *n = &s.mqtt;

    /* Topics move: remove the old discovery so HA has no stale entities */
    const bool moved = strcmp(n->base_topic, s_cfg.base_topic) != 0 ||
                       strcmp(n->discovery_prefix, s_cfg.discovery_prefix) != 0 || !n->enabled;
    if (s_client && memcmp(n, &s_cfg, sizeof(*n)) == 0) return;

    client_stop(moved);
    s_cfg = *n;
    if (s_cfg.enabled) client_start();
    else s_error[0] = '\0';
}

/* ------------------------------------------------------------------------ */
/* Poller task                                                              */
/* ------------------------------------------------------------------------ */

static void publish_status(bool full, int64_t now_ms)
{
    static active_source_t last_active = -1;
    static bool last_signal[NUM_INPUTS + 1], last_clip;
    static float last_level[NUM_INPUTS] = { 999, 999 };
    static int last_listeners = -1;
    static int64_t next_level, next_uptime;
    char buf[48], key[16];

    settings_t s;
    settings_get(&s);
    audio_status_t a;
    audio_get_status(&a);

    if (full || a.mix.active != last_active) {
        const char *name = a.mix.active == ACTIVE_A ? s.audio.input[0].name
                         : a.mix.active == ACTIVE_B ? s.audio.input[1].name
                         : a.mix.active == ACTIVE_MIX ? "Mix" : "None";
        pub("active_source", name, true);
        pub("active_input", mixer_active_token(a.mix.active), true);
        last_active = a.mix.active;
    }
    const bool any = a.mix.signal[0] || a.mix.signal[1];
    if (full || any != last_signal[NUM_INPUTS]) {
        pub("signal", any ? "ON" : "OFF", true);
        last_signal[NUM_INPUTS] = any;
    }
    for (int i = 0; i < NUM_INPUTS; i++) {
        if (full || a.mix.signal[i] != last_signal[i]) {
            snprintf(key, sizeof(key), "in_%c_signal", 'a' + i);
            pub(key, a.mix.signal[i] ? "ON" : "OFF", true);
            last_signal[i] = a.mix.signal[i];
        }
    }
    if (full || now_ms >= next_level) {
        next_level = now_ms + LEVEL_INTERVAL_MS;
        for (int i = 0; i < NUM_INPUTS; i++) {
            if (!full && fabsf(a.mix.level_dbfs[i] - last_level[i]) < 1.0f) continue;
            snprintf(key, sizeof(key), "in_%c_level", 'a' + i);
            snprintf(buf, sizeof(buf), "%.1f", a.mix.level_dbfs[i]);
            pub(key, buf, false);
            last_level[i] = a.mix.level_dbfs[i];
        }
    }
    if (full || a.clipping != last_clip) {
        pub("clipping", a.clipping ? "ON" : "OFF", true);
        last_clip = a.clipping;
    }
    const int listeners = stream_server_client_count();
    if (full || listeners != last_listeners) {
        snprintf(buf, sizeof(buf), "%d", listeners);
        pub("listeners", buf, true);
        last_listeners = listeners;
    }
    if (full || now_ms >= next_uptime) {
        next_uptime = now_ms + UPTIME_INTERVAL_MS;
        snprintf(buf, sizeof(buf), "%lld", (long long)(now_ms / 1000));
        pub("uptime", buf, false);
    }
}

static void publish_ip(void)
{
    char url[64];
    if (!s_ip[0]) return;
    snprintf(url, sizeof(url), "http://%s:%d" STREAM_PATH, s_ip, STREAM_PORT);
    pub("stream_url", url, true);
    pub("ip", s_ip, true);
}

static void mqtt_task(void *arg)
{
    char last_names[NUM_INPUTS][24] = { "", "" };
    char last_device[32] = "";

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
        const int64_t now_ms = esp_timer_get_time() / 1000;

        if (s_restart_req) {
            s_restart_req = false;
            ESP_LOGW(TAG, "Restart requested via MQTT");
            client_stop(false);
            settings_flush();
            esp_restart();
        }
        if (s_reconfigure) {
            s_reconfigure = false;
            reconfigure();
        }
        if (!s_client || !s_connected) continue;

        settings_t s;
        bool full = s_republish_all;
        s_republish_all = false;
        bool ip_changed = s_ip_changed;
        s_ip_changed = false;

        if (full || s_states_dirty || ip_changed) {
            const bool dirty = s_states_dirty;
            s_states_dirty = false;
            settings_get(&s);

            /* Entity names and configuration_url are part of the discovery */
            bool names_changed = strcmp(s.mqtt.device_name, last_device) != 0;
            for (int i = 0; i < NUM_INPUTS; i++) {
                names_changed |= strcmp(s.audio.input[i].name, last_names[i]) != 0;
            }
            if (full || names_changed || ip_changed) {
                publish_discovery(&s);
                for (int i = 0; i < NUM_INPUTS; i++) {
                    strlcpy(last_names[i], s.audio.input[i].name, sizeof(last_names[i]));
                }
                strlcpy(last_device, s.mqtt.device_name, sizeof(last_device));
            }
            if (full) {
                pub("status", "online", true);
                publish_all_settings(&s);
            } else if (dirty || names_changed) {
                publish_changed_settings(&s);
            }
            if (full || ip_changed) publish_ip();
            s_published = s;
        }
        /* Echo commanded values, even if they did not change anything */
        const uint64_t echo = __atomic_exchange_n(&s_echo_fields, 0, __ATOMIC_SEQ_CST);
        if (echo) {
            int n;
            const setting_field_t *fields = settings_fields(&n);
            settings_get(&s);
            for (int i = 0; i < n && i < 64; i++) {
                if (echo & (1ULL << i)) pub_setting(&s, &fields[i]);
            }
        }
        publish_status(full, now_ms);
    }
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

void mqtt_ha_start(void)
{
    uint8_t mac[6];
    network_get_mac(mac);
    snprintf(s_device_id, sizeof(s_device_id), "rumble_jungle_%02x%02x%02x", mac[3], mac[4], mac[5]);

    settings_add_listener(on_settings, NULL);
    xTaskCreatePinnedToCore(mqtt_task, "mqtt_ha", 6144, NULL, 4, NULL, NET_TASK_CORE);
}
