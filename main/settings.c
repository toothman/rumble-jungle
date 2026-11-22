// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "nvs.h"
#include "settings.h"

#define NVS_NAMESPACE   "rj"
#define NVS_VERSION     1
#define MAX_FIELDS      64      /* MQTT tracks fields in a 64 bit mask */
#define MAX_LISTENERS   6

static const char *TAG = "settings";

static settings_t s_set;            /* current values                   */
static settings_t s_boot;           /* values the device booted with    */
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_save_timer;
static uint32_t s_dirty_groups;     /* groups waiting to be saved       */
static volatile uint32_t s_rev;

static setting_field_t s_fields[MAX_FIELDS];
static int s_num_fields;

static struct { settings_listener_t cb; void *ctx; } s_listeners[MAX_LISTENERS];
static int s_num_listeners;

/* ------------------------------------------------------------------------ */
/* Enums                                                                    */
/* ------------------------------------------------------------------------ */

static const char *const k_mode_tokens[]  = { "auto", "a", "b", "mix" };
static const char *const k_mode_labels[]  = { "Auto", "Input A", "Input B", "Mix" };
static const char *const k_input_tokens[] = { "a", "b" };
static const char *const k_input_labels[] = { "Input A", "Input B" };

static const uint32_t k_bitrates[] = { 128000, 160000, 192000, 256000, 320000 };
static const char *const k_bitrate_tokens[] = { "128000", "160000", "192000", "256000", "320000" };
static const char *const k_bitrate_labels[] = { "128 kbit/s", "160 kbit/s", "192 kbit/s",
                                                "256 kbit/s", "320 kbit/s" };
#define NUM_BITRATES ((int)(sizeof(k_bitrates) / sizeof(k_bitrates[0])))

int settings_bitrate_count(void) { return NUM_BITRATES; }

uint32_t settings_bitrate_bps(int index)
{
    if (index < 0 || index >= NUM_BITRATES) index = 2;
    return k_bitrates[index];
}

/* ------------------------------------------------------------------------ */
/* Validators                                                               */
/* ------------------------------------------------------------------------ */

static bool v_nonempty(const char *v) { return v[0] != '\0'; }

static bool v_hostname(const char *v)
{
    size_t n = strlen(v);
    if (n == 0 || n > 31 || v[0] == '-' || v[n - 1] == '-') return false;
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)v[i]) && v[i] != '-') return false;
    }
    return true;
}

static bool v_ip(const char *v)
{
    esp_ip4_addr_t ip;
    return esp_netif_str_to_ip4(v, &ip) == ESP_OK;
}

static bool v_ip_or_empty(const char *v) { return v[0] == '\0' || v_ip(v); }

static bool v_mqtt_uri(const char *v)
{
    return strncmp(v, "mqtt://", 7) == 0 || strncmp(v, "mqtts://", 8) == 0 ||
           strncmp(v, "ws://", 5) == 0 || strncmp(v, "wss://", 6) == 0;
}

static bool v_topic(const char *v)
{
    if (!v[0] || v[0] == '/' || v[strlen(v) - 1] == '/') return false;
    for (const char *p = v; *p; p++) {
        if (*p == '+' || *p == '#' || *p == ' ') return false;
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* Field table                                                              */
/* ------------------------------------------------------------------------ */

#define OFS(member)  ((uint16_t)offsetof(settings_t, member))
#define SZ(member)   ((uint16_t)sizeof(((settings_t *)0)->member))

static setting_field_t *add(const char *key, const char *name, field_type_t type,
                            settings_group_t group, uint16_t flags,
                            uint16_t offset, uint16_t size)
{
    assert(s_num_fields < MAX_FIELDS);
    setting_field_t *f = &s_fields[s_num_fields++];
    memset(f, 0, sizeof(*f));
    strlcpy(f->key, key, sizeof(f->key));
    strlcpy(f->name, name, sizeof(f->name));
    f->type = type;
    f->group = group;
    f->flags = flags;
    f->offset = offset;
    f->size = size;
    return f;
}

static void add_float(const char *key, const char *name, settings_group_t g, uint16_t flags,
                      uint16_t ofs, float min, float max, float step, int dec,
                      const char *unit, const char *icon)
{
    setting_field_t *f = add(key, name, F_FLOAT, g, flags, ofs, sizeof(float));
    f->min = min; f->max = max; f->step = step; f->decimals = dec;
    f->unit = unit; f->icon = icon;
}

static void add_int(const char *key, const char *name, uint16_t flags, uint16_t ofs,
                    float min, float max, float step, const char *unit, const char *icon)
{
    setting_field_t *f = add(key, name, F_INT, SG_AUDIO, flags, ofs, sizeof(uint32_t));
    f->min = min; f->max = max; f->step = step; f->unit = unit; f->icon = icon;
}

static void add_bool(const char *key, const char *name, settings_group_t g, uint16_t flags,
                     uint16_t ofs, const char *icon)
{
    setting_field_t *f = add(key, name, F_BOOL, g, flags, ofs, sizeof(bool));
    f->icon = icon;
}

static void add_str(const char *key, const char *name, settings_group_t g, uint16_t flags,
                    uint16_t ofs, uint16_t size, settings_validator_t validate, const char *icon)
{
    setting_field_t *f = add(key, name, F_STR, g, flags, ofs, size);
    f->validate = validate;
    f->icon = icon;
}

static void add_enum(const char *key, const char *name, settings_group_t g, uint16_t flags,
                     uint16_t ofs, const char *const *tokens, const char *const *labels,
                     int count, const char *icon)
{
    setting_field_t *f = add(key, name, F_ENUM, g, flags, ofs, sizeof(uint8_t));
    f->enum_tokens = tokens; f->enum_labels = labels; f->enum_count = count;
    f->icon = icon;
}

static void build_fields(void)
{
    const float fmax = dsp_max_freq();
    char key[24], name[32];

    /* Source selection */
    add_enum("source_mode", "Source Mode", SG_AUDIO, 0, OFS(audio.source_mode),
             k_mode_tokens, k_mode_labels, 4, "mdi:source-branch");
    add_enum("priority", "Priority Input", SG_AUDIO, SF_CONFIG, OFS(audio.priority),
             k_input_tokens, k_input_labels, 2, "mdi:priority-high");

    /* Inputs */
    for (int i = 0; i < NUM_INPUTS; i++) {
        const char id = 'a' + i, ID = 'A' + i;
        snprintf(key, sizeof(key), "in_%c_name", id);
        snprintf(name, sizeof(name), "Input %c Name", ID);
        add_str(key, name, SG_AUDIO, SF_CONFIG, OFS(audio.input[i].name),
                SZ(audio.input[i].name), v_nonempty, "mdi:rename");
        snprintf(key, sizeof(key), "in_%c_trim", id);
        snprintf(name, sizeof(name), "Input %c Trim", ID);
        add_float(key, name, SG_AUDIO, 0, OFS(audio.input[i].trim_db),
                  INPUT_TRIM_MIN_DB, INPUT_TRIM_MAX_DB, 0.5f, 1, "dB", "mdi:knob");
        snprintf(key, sizeof(key), "in_%c_threshold", id);
        snprintf(name, sizeof(name), "Input %c Threshold", ID);
        add_float(key, name, SG_AUDIO, SF_CONFIG, OFS(audio.input[i].threshold_dbfs),
                  THRESHOLD_MIN_DBFS, THRESHOLD_MAX_DBFS, 1, 0, "dBFS", "mdi:volume-low");
    }
    add_int("signal_on_ms", "Signal On Delay", SF_CONFIG, OFS(audio.signal_on_ms),
            100, 10000, 100, "ms", "mdi:timer-play-outline");
    add_int("signal_off_ms", "Signal Off Delay", SF_CONFIG, OFS(audio.signal_off_ms),
            1000, 300000, 1000, "ms", "mdi:timer-stop-outline");
    add_int("xfade_ms", "Crossfade Time", SF_CONFIG, OFS(audio.xfade_ms),
            10, 3000, 10, "ms", "mdi:transition");

    /* Processing */
    add_bool("rumble", "Rumble Filter", SG_AUDIO, 0, OFS(audio.dsp.rumble_enabled), "mdi:sine-wave");
    add_float("rumble_freq", "Rumble Filter Frequency", SG_AUDIO, SF_CONFIG,
              OFS(audio.dsp.rumble_freq), RUMBLE_FREQ_MIN_HZ, RUMBLE_FREQ_MAX_HZ, 1, 0,
              "Hz", "mdi:sine-wave");
    add_bool("eq", "Equalizer", SG_AUDIO, 0, OFS(audio.dsp.eq_enabled), "mdi:equalizer");
    for (int b = 0; b < EQ_BANDS; b++) {
        snprintf(key, sizeof(key), "eq%d_gain", b + 1);
        snprintf(name, sizeof(name), "EQ %d Gain", b + 1);
        add_float(key, name, SG_AUDIO, 0, OFS(audio.dsp.band_gain[b]),
                  EQ_GAIN_MIN_DB, EQ_GAIN_MAX_DB, EQ_GAIN_STEP_DB, 1, "dB", "mdi:tune-vertical");
        snprintf(key, sizeof(key), "eq%d_freq", b + 1);
        snprintf(name, sizeof(name), "EQ %d Frequency", b + 1);
        add_float(key, name, SG_AUDIO, SF_CONFIG, OFS(audio.dsp.band_freq[b]),
                  EQ_FREQ_MIN_HZ, fmax, 1, 0, "Hz", "mdi:sine-wave");
        snprintf(key, sizeof(key), "eq%d_q", b + 1);
        snprintf(name, sizeof(name), "EQ %d Q", b + 1);
        add_float(key, name, SG_AUDIO, SF_CONFIG, OFS(audio.dsp.band_q[b]),
                  EQ_Q_MIN, EQ_Q_MAX, 0.05f, 2, NULL, "mdi:arrow-expand-horizontal");
    }
    add_float("output_gain", "Output Gain", SG_AUDIO, 0, OFS(audio.dsp.output_gain),
              OUTPUT_GAIN_MIN_DB, OUTPUT_GAIN_MAX_DB, 0.5f, 1, "dB", "mdi:volume-high");
    add_bool("auto_headroom", "Auto Headroom", SG_AUDIO, SF_CONFIG,
             OFS(audio.dsp.auto_headroom), "mdi:arrow-collapse-vertical");

    /* Stream */
    add_enum("bitrate", "Stream Bitrate", SG_STREAM, SF_CONFIG, OFS(stream.bitrate),
             k_bitrate_tokens, k_bitrate_labels, NUM_BITRATES, "mdi:speedometer");
    add_str("stream_name", "Stream Name", SG_STREAM, SF_CONFIG, OFS(stream.name),
            SZ(stream.name), v_nonempty, "mdi:radio");

    /* MQTT - web UI only */
    const uint16_t m = SF_NO_MQTT;
    add_bool("mqtt_enabled", "MQTT", SG_MQTT, m, OFS(mqtt.enabled), NULL);
    add_str("mqtt_uri", "Broker", SG_MQTT, m, OFS(mqtt.uri), SZ(mqtt.uri), v_mqtt_uri, NULL);
    add_str("mqtt_user", "User", SG_MQTT, m, OFS(mqtt.user), SZ(mqtt.user), NULL, NULL);
    add_str("mqtt_password", "Password", SG_MQTT, m | SF_SECRET, OFS(mqtt.password),
            SZ(mqtt.password), NULL, NULL);
    add_str("mqtt_base_topic", "Base Topic", SG_MQTT, m, OFS(mqtt.base_topic),
            SZ(mqtt.base_topic), v_topic, NULL);
    add_str("mqtt_discovery_prefix", "Discovery Prefix", SG_MQTT, m,
            OFS(mqtt.discovery_prefix), SZ(mqtt.discovery_prefix), v_topic, NULL);
    add_str("device_name", "Device Name", SG_MQTT, m, OFS(mqtt.device_name),
            SZ(mqtt.device_name), v_nonempty, NULL);

    /* Network - web UI only, applied on restart */
    const uint16_t n = SF_NO_MQTT | SF_RESTART;
    add_str("hostname", "Hostname", SG_NET, n, OFS(net.hostname), SZ(net.hostname), v_hostname, NULL);
    add_bool("dhcp", "DHCP", SG_NET, n, OFS(net.dhcp), NULL);
    add_str("ip", "IP Address", SG_NET, n, OFS(net.ip), SZ(net.ip), v_ip_or_empty, NULL);
    add_str("netmask", "Netmask", SG_NET, n, OFS(net.netmask), SZ(net.netmask), v_ip_or_empty, NULL);
    add_str("gateway", "Gateway", SG_NET, n, OFS(net.gateway), SZ(net.gateway), v_ip_or_empty, NULL);
    add_str("dns", "DNS Server", SG_NET, n, OFS(net.dns), SZ(net.dns), v_ip_or_empty, NULL);
}

/* ------------------------------------------------------------------------ */
/* Defaults and value access                                                */
/* ------------------------------------------------------------------------ */

static void reset_bands(dsp_params_t *p)
{
    for (int b = 0; b < EQ_BANDS; b++) {
        p->band_gain[b] = 0.0f;
        p->band_freq[b] = dsp_default_freq[b];
        p->band_q[b]    = dsp_default_q[b];
    }
}

static void defaults_audio(audio_settings_t *a)
{
    memset(a, 0, sizeof(*a));
    a->dsp.eq_enabled     = DEFAULT_EQ_ENABLED;
    reset_bands(&a->dsp);
    a->dsp.rumble_enabled = DEFAULT_RUMBLE_ENABLED;
    a->dsp.rumble_freq    = DEFAULT_RUMBLE_FREQ_HZ;
    a->dsp.output_gain    = DEFAULT_OUTPUT_GAIN_DB;
    a->dsp.auto_headroom  = DEFAULT_AUTO_HEADROOM;
    a->source_mode        = SRC_MODE_AUTO;
    a->priority           = INPUT_A;
    strlcpy(a->input[0].name, DEFAULT_INPUT_A_NAME, sizeof(a->input[0].name));
    strlcpy(a->input[1].name, DEFAULT_INPUT_B_NAME, sizeof(a->input[1].name));
    for (int i = 0; i < NUM_INPUTS; i++) a->input[i].threshold_dbfs = DEFAULT_THRESHOLD_DBFS;
    a->signal_on_ms  = DEFAULT_SIGNAL_ON_MS;
    a->signal_off_ms = DEFAULT_SIGNAL_OFF_MS;
    a->xfade_ms      = DEFAULT_XFADE_MS;
}

static void defaults_mqtt(mqtt_settings_t *m)
{
    memset(m, 0, sizeof(*m));
    m->enabled = DEFAULT_MQTT_ENABLED;
    strlcpy(m->uri, DEFAULT_MQTT_URI, sizeof(m->uri));
    strlcpy(m->base_topic, DEFAULT_MQTT_BASE_TOPIC, sizeof(m->base_topic));
    strlcpy(m->discovery_prefix, DEFAULT_HA_PREFIX, sizeof(m->discovery_prefix));
    strlcpy(m->device_name, DEFAULT_DEVICE_NAME, sizeof(m->device_name));
}

static void defaults_net(net_settings_t *n)
{
    memset(n, 0, sizeof(*n));
    strlcpy(n->hostname, DEFAULT_HOSTNAME, sizeof(n->hostname));
    n->dhcp = true;
}

static void defaults_stream(stream_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->bitrate = 2;
    for (int i = 0; i < NUM_BITRATES; i++) {
        if (k_bitrates[i] == DEFAULT_AAC_BITRATE) s->bitrate = i;
    }
    strlcpy(s->name, DEFAULT_STREAM_NAME, sizeof(s->name));
}

static inline void *field_ptr(settings_t *s, const setting_field_t *f)
{
    return (uint8_t *)s + f->offset;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Brings every numeric field into range (stored values may predate a
 * change of the ranges in config.h).                                     */
static void clamp_all(settings_t *s)
{
    for (int i = 0; i < s_num_fields; i++) {
        const setting_field_t *f = &s_fields[i];
        void *p = field_ptr(s, f);
        switch (f->type) {
        case F_FLOAT: {
            float v = *(float *)p;
            *(float *)p = isfinite(v) ? clampf(v, f->min, f->max) : f->min;
            break;
        }
        case F_INT:
            *(uint32_t *)p = (uint32_t)clampf(*(uint32_t *)p, f->min, f->max);
            break;
        case F_ENUM:
            if (*(uint8_t *)p >= f->enum_count) *(uint8_t *)p = 0;
            break;
        case F_STR:
            ((char *)p)[f->size - 1] = '\0';
            break;
        default:
            break;
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
    while (*end == ' ') end++;
    if (end == s || *end != '\0' || !isfinite(v)) return false;
    *out = v;
    return true;
}

/* Parses value into the field of s. Returns NULL or an error message. */
static const char *parse_into(settings_t *s, const setting_field_t *f, const char *value)
{
    void *p = field_ptr(s, f);
    float v;

    switch (f->type) {
    case F_BOOL:
        return parse_bool(value, (bool *)p) ? NULL : "expected ON or OFF";
    case F_FLOAT:
        if (!parse_float(value, &v)) return "not a number";
        *(float *)p = clampf(v, f->min, f->max);
        return NULL;
    case F_INT:
        if (!parse_float(value, &v)) return "not a number";
        *(uint32_t *)p = (uint32_t)lrintf(clampf(v, f->min, f->max));
        return NULL;
    case F_ENUM:
        for (int i = 0; i < f->enum_count; i++) {
            if (!strcasecmp(value, f->enum_tokens[i]) || !strcasecmp(value, f->enum_labels[i])) {
                *(uint8_t *)p = i;
                return NULL;
            }
        }
        return "unknown option";
    case F_STR:
        if (strlen(value) >= f->size) return "too long";
        if (f->validate && !f->validate(value)) return "invalid value";
        /* Zero the whole buffer so byte-wise change detection works */
        memset(p, 0, f->size);
        memcpy(p, value, strlen(value));
        return NULL;
    }
    return "unsupported";
}

/* Checks constraints that span several fields. */
static const char *validate_all(const settings_t *s, const char **key)
{
    if (!strcmp(s->audio.input[0].name, s->audio.input[1].name)) {
        *key = "in_b_name";
        return "inputs need different names";
    }
    if (!s->net.dhcp) {
        if (!v_ip(s->net.ip))      { *key = "ip";      return "static IP required"; }
        if (!v_ip(s->net.netmask)) { *key = "netmask"; return "netmask required"; }
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Persistence                                                              */
/* ------------------------------------------------------------------------ */

typedef struct {
    const char *key;
    uint32_t    group;
    size_t      offset, size;
    void      (*defaults)(void *);
} group_desc_t;

static const group_desc_t k_groups[] = {
    { "audio",  SG_AUDIO,  offsetof(settings_t, audio),  sizeof(audio_settings_t),  (void (*)(void *))defaults_audio },
    { "mqtt",   SG_MQTT,   offsetof(settings_t, mqtt),   sizeof(mqtt_settings_t),   (void (*)(void *))defaults_mqtt },
    { "net",    SG_NET,    offsetof(settings_t, net),    sizeof(net_settings_t),    (void (*)(void *))defaults_net },
    { "stream", SG_STREAM, offsetof(settings_t, stream), sizeof(stream_settings_t), (void (*)(void *))defaults_stream },
};
#define NUM_GROUPS (sizeof(k_groups) / sizeof(k_groups[0]))

static void load_group(nvs_handle_t h, const group_desc_t *g)
{
    uint8_t *dst = (uint8_t *)&s_set + g->offset;
    g->defaults(dst);
    if (!h) return;

    /* Blob = version + struct. The size check also catches a changed
     * EQ_BANDS or a firmware with a different layout.                    */
    size_t len = 0;
    if (nvs_get_blob(h, g->key, NULL, &len) != ESP_OK || len != sizeof(uint32_t) + g->size) {
        ESP_LOGI(TAG, "%s: using defaults", g->key);
        return;
    }
    uint8_t *buf = malloc(len);
    if (!buf) return;
    if (nvs_get_blob(h, g->key, buf, &len) == ESP_OK &&
        *(uint32_t *)buf == NVS_VERSION) {
        memcpy(dst, buf + sizeof(uint32_t), g->size);
        ESP_LOGI(TAG, "%s: loaded", g->key);
    }
    free(buf);
}

static void save_groups(uint32_t groups, const settings_t *s)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot open NVS");
        return;
    }
    for (size_t i = 0; i < NUM_GROUPS; i++) {
        const group_desc_t *g = &k_groups[i];
        if (!(groups & g->group)) continue;
        size_t len = sizeof(uint32_t) + g->size;
        uint8_t *buf = malloc(len);
        if (!buf) continue;
        *(uint32_t *)buf = NVS_VERSION;
        memcpy(buf + sizeof(uint32_t), (const uint8_t *)s + g->offset, g->size);
        if (nvs_set_blob(h, g->key, buf, len) != ESP_OK) ESP_LOGE(TAG, "Saving %s failed", g->key);
        free(buf);
    }
    nvs_commit(h);
    nvs_close(h);
}

void settings_flush(void)
{
    settings_t copy;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t groups = s_dirty_groups;
    s_dirty_groups = 0;
    copy = s_set;
    xSemaphoreGive(s_lock);
    if (groups) {
        esp_timer_stop(s_save_timer);
        save_groups(groups, &copy);
        ESP_LOGI(TAG, "Settings saved");
    }
}

static void save_timer_cb(void *arg)
{
    settings_flush();
}

/* ------------------------------------------------------------------------ */
/* Change handling                                                          */
/* ------------------------------------------------------------------------ */

static void notify(uint32_t groups, settings_source_t src)
{
    if (!groups) return;
    s_rev++;
    /* Debounced: dragging a slider must not wear out the flash */
    esp_timer_stop(s_save_timer);
    esp_timer_start_once(s_save_timer, (uint64_t)SETTINGS_SAVE_DELAY_MS * 1000);
    for (int i = 0; i < s_num_listeners; i++) s_listeners[i].cb(groups, src, s_listeners[i].ctx);
}

/* Stores candidate as the new settings. Returns the changed groups. */
static uint32_t commit(const settings_t *candidate)
{
    uint32_t changed = 0;
    for (size_t i = 0; i < NUM_GROUPS; i++) {
        const group_desc_t *g = &k_groups[i];
        if (memcmp((const uint8_t *)candidate + g->offset,
                   (const uint8_t *)&s_set + g->offset, g->size) != 0) {
            changed |= g->group;
        }
    }
    s_set = *candidate;
    s_dirty_groups |= changed;
    return changed;
}

static void set_err(char *err, size_t errlen, const char *key, const char *msg)
{
    if (err && errlen) snprintf(err, errlen, "%s: %s", key, msg);
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

void settings_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    build_fields();

    nvs_handle_t h = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) h = 0;
    for (size_t i = 0; i < NUM_GROUPS; i++) load_group(h, &k_groups[i]);
    if (h) nvs_close(h);

    clamp_all(&s_set);
    s_boot = s_set;

    const esp_timer_create_args_t args = { .callback = save_timer_cb, .name = "settings_save" };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_save_timer));
}

void settings_get(settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_set;
    xSemaphoreGive(s_lock);
}

uint32_t settings_rev(void) { return s_rev; }

const setting_field_t *settings_fields(int *count)
{
    *count = s_num_fields;
    return s_fields;
}

const setting_field_t *settings_find(const char *key)
{
    for (int i = 0; i < s_num_fields; i++) {
        if (!strcmp(s_fields[i].key, key)) return &s_fields[i];
    }
    return NULL;
}

esp_err_t settings_set_str(const char *key, const char *value, settings_source_t src,
                           char *err, size_t errlen)
{
    const setting_field_t *f = settings_find(key);
    if (!f) {
        set_err(err, errlen, key, "unknown setting");
        return ESP_ERR_NOT_FOUND;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    settings_t cand = s_set;
    const char *msg = parse_into(&cand, f, value);
    const char *bad_key = key;
    if (!msg) msg = validate_all(&cand, &bad_key);
    uint32_t changed = msg ? 0 : commit(&cand);
    xSemaphoreGive(s_lock);

    if (msg) {
        set_err(err, errlen, bad_key, msg);
        return ESP_ERR_INVALID_ARG;
    }
    notify(changed, src);
    return ESP_OK;
}

esp_err_t settings_apply_json(const cJSON *obj, settings_source_t src, char *err, size_t errlen)
{
    if (!cJSON_IsObject(obj)) {
        set_err(err, errlen, "body", "expected a JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    settings_t cand = s_set;
    const char *msg = NULL, *bad_key = NULL;
    const cJSON *item;
    cJSON_ArrayForEach(item, obj) {
        const setting_field_t *f = settings_find(item->string);
        bad_key = item->string;
        if (!f) { msg = "unknown setting"; break; }

        char buf[128];
        if (cJSON_IsBool(item))        strlcpy(buf, cJSON_IsTrue(item) ? "1" : "0", sizeof(buf));
        else if (cJSON_IsNumber(item)) snprintf(buf, sizeof(buf), "%.6g", item->valuedouble);
        else if (cJSON_IsString(item)) {
            if (strlen(item->valuestring) >= sizeof(buf)) { msg = "too long"; break; }
            strlcpy(buf, item->valuestring, sizeof(buf));
        } else { msg = "unsupported value type"; break; }

        /* Secrets: the UI omits an untouched password, so "" clears it */
        if ((msg = parse_into(&cand, f, buf)) != NULL) break;
    }
    if (!msg) msg = validate_all(&cand, &bad_key);
    uint32_t changed = msg ? 0 : commit(&cand);
    xSemaphoreGive(s_lock);

    if (msg) {
        set_err(err, errlen, bad_key ? bad_key : "body", msg);
        return ESP_ERR_INVALID_ARG;
    }
    notify(changed, src);
    return ESP_OK;
}

cJSON *settings_to_json(void)
{
    settings_t s;
    settings_get(&s);

    cJSON *o = cJSON_CreateObject();
    for (int i = 0; i < s_num_fields; i++) {
        const setting_field_t *f = &s_fields[i];
        const void *p = field_ptr(&s, f);
        switch (f->type) {
        case F_BOOL:  cJSON_AddBoolToObject(o, f->key, *(const bool *)p); break;
        case F_FLOAT: {
            /* Round to the field's precision so the UI shows clean numbers */
            double scale = pow(10, f->decimals);
            cJSON_AddNumberToObject(o, f->key, round(*(const float *)p * scale) / scale);
            break;
        }
        case F_INT:   cJSON_AddNumberToObject(o, f->key, *(const uint32_t *)p); break;
        case F_ENUM:  cJSON_AddStringToObject(o, f->key, f->enum_tokens[*(const uint8_t *)p]); break;
        case F_STR:
            if (f->flags & SF_SECRET) {
                char set_key[32];
                strlcpy(set_key, f->key, sizeof(set_key));
                strlcat(set_key, "_set", sizeof(set_key));
                cJSON_AddStringToObject(o, f->key, "");
                cJSON_AddBoolToObject(o, set_key, ((const char *)p)[0] != '\0');
            } else {
                cJSON_AddStringToObject(o, f->key, (const char *)p);
            }
            break;
        }
    }
    return o;
}

void settings_format(const settings_t *s, const setting_field_t *f, char *buf, size_t len)
{
    const void *p = (const uint8_t *)s + f->offset;
    switch (f->type) {
    case F_BOOL:  snprintf(buf, len, "%s", *(const bool *)p ? "ON" : "OFF"); break;
    case F_FLOAT: snprintf(buf, len, "%.*f", f->decimals, *(const float *)p); break;
    case F_INT:   snprintf(buf, len, "%" PRIu32, *(const uint32_t *)p); break;
    case F_ENUM:  snprintf(buf, len, "%s", f->enum_labels[*(const uint8_t *)p]); break;
    case F_STR:   snprintf(buf, len, "%s", (f->flags & SF_SECRET) ? "" : (const char *)p); break;
    }
}

void settings_reset_eq(settings_source_t src)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    settings_t cand = s_set;
    reset_bands(&cand.audio.dsp);
    uint32_t changed = commit(&cand);
    xSemaphoreGive(s_lock);
    notify(changed, src);
}

bool settings_restart_required(void)
{
    bool req = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_num_fields && !req; i++) {
        const setting_field_t *f = &s_fields[i];
        if ((f->flags & SF_RESTART) &&
            memcmp(field_ptr(&s_set, f), field_ptr(&s_boot, f), f->size) != 0) {
            req = true;
        }
    }
    xSemaphoreGive(s_lock);
    return req;
}

void settings_factory_reset(void)
{
    ESP_LOGW(TAG, "Factory reset");
    esp_timer_stop(s_save_timer);
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    esp_restart();
}

void settings_add_listener(settings_listener_t cb, void *ctx)
{
    assert(s_num_listeners < MAX_LISTENERS);
    s_listeners[s_num_listeners].cb = cb;
    s_listeners[s_num_listeners].ctx = ctx;
    s_num_listeners++;
}
