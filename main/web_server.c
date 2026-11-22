// SPDX-License-Identifier: MIT
/*
 * Web UI and JSON API.
 *
 *   GET  /                    single page app (embedded, gzip)
 *   GET  /api/status          live status
 *   GET  /api/settings        all settings + field metadata
 *   PATCH/POST /api/settings  partial update {"key": value, ...}
 *   POST /api/eq/reset        flat EQ
 *   POST /api/restart         restart
 *   POST /api/factory_reset   {"confirm":"RESET"}
 *   POST /api/ota             raw firmware image (.bin)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_ota_ops.h"
#include "cJSON.h"
#include "app_config.h"
#include "settings.h"
#include "audio.h"
#include "network.h"
#include "stream_server.h"
#include "mqtt_ha.h"
#include "web_server.h"

#define MAX_BODY       8192
#define OTA_CHUNK      4096

static const char *TAG = "web";

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");
extern const uint8_t app_js_gz_start[]     asm("_binary_app_js_gz_start");
extern const uint8_t app_js_gz_end[]       asm("_binary_app_js_gz_end");
extern const uint8_t app_css_gz_start[]    asm("_binary_app_css_gz_start");
extern const uint8_t app_css_gz_end[]      asm("_binary_app_css_gz_end");

typedef struct {
    const uint8_t *start, *end;
    const char *type;
} asset_t;

static const asset_t k_index = { index_html_gz_start, index_html_gz_end, "text/html; charset=utf-8" };
static const asset_t k_js    = { app_js_gz_start, app_js_gz_end, "application/javascript" };
static const asset_t k_css   = { app_css_gz_start, app_css_gz_end, "text/css" };

static char s_etag[64];

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static esp_err_t send_json(httpd_req_t *req, cJSON *o)
{
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!json) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    httpd_resp_set_status(req, status);
    return send_json(req, o);
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, o);
}

/* Reads a small request body; returns NULL on error (response sent) */
static char *read_body(httpd_req_t *req)
{
    if (req->content_len > MAX_BODY) {
        send_error(req, "413 Payload Too Large", "body too large");
        return NULL;
    }
    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        httpd_resp_send_500(req);
        return NULL;
    }
    size_t off = 0;
    while (off < req->content_len) {
        int n = httpd_req_recv(req, buf + off, req->content_len - off);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            free(buf);
            send_error(req, "400 Bad Request", "incomplete body");
            return NULL;
        }
        off += n;
    }
    buf[off] = '\0';
    return buf;
}

static void restart_cb(void *arg)
{
    settings_flush();
    esp_restart();
}

/* Restarts shortly after the response has been sent */
static void schedule_restart(void)
{
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t args = { .callback = restart_cb, .name = "restart" };
        esp_timer_create(&args, &t);
    }
    esp_timer_start_once(t, 800 * 1000);
}

/* ------------------------------------------------------------------------ */
/* Static assets                                                            */
/* ------------------------------------------------------------------------ */

static esp_err_t asset_handler(httpd_req_t *req)
{
    const asset_t *a = req->user_ctx;

    char inm[64];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK &&
        strcmp(inm, s_etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, a->type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "ETag", s_etag);
    return httpd_resp_send(req, (const char *)a->start, a->end - a->start);
}

/* ------------------------------------------------------------------------ */
/* API                                                                      */
/* ------------------------------------------------------------------------ */

static esp_err_t status_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    settings_t s;
    settings_get(&s);
    audio_status_t a;
    audio_get_status(&a);
    network_info_t net;
    network_get_info(&net);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "fw", app->version);
    cJSON_AddStringToObject(o, "idf", esp_get_idf_version());
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(o, "hostname", net.hostname);
    cJSON_AddStringToObject(o, "ip", net.ip);
    cJSON_AddStringToObject(o, "netmask", net.netmask);
    cJSON_AddStringToObject(o, "gateway", net.gateway);
    cJSON_AddStringToObject(o, "mac", net.mac);
    cJSON_AddBoolToObject(o, "link", net.link_up);
    cJSON_AddNumberToObject(o, "speed", net.speed_mbps);

    char url[64] = "";
    if (net.ip[0]) snprintf(url, sizeof(url), "http://%s:%d" STREAM_PATH, net.ip, STREAM_PORT);
    cJSON_AddStringToObject(o, "stream_url", url);
    cJSON_AddNumberToObject(o, "listeners", stream_server_client_count());
    cJSON_AddBoolToObject(o, "clipping", a.clipping);
    cJSON_AddNumberToObject(o, "bitrate", a.bitrate);
    cJSON_AddBoolToObject(o, "audio_running", a.running);
    cJSON_AddStringToObject(o, "active_source", mixer_active_token(a.mix.active));

    cJSON *inputs = cJSON_AddArrayToObject(o, "inputs");
    for (int i = 0; i < NUM_INPUTS; i++) {
        cJSON *in = cJSON_CreateObject();
        cJSON_AddStringToObject(in, "name", s.audio.input[i].name);
        cJSON_AddNumberToObject(in, "level", (int)(a.mix.level_dbfs[i] * 10) / 10.0);
        cJSON_AddBoolToObject(in, "signal", a.mix.signal[i]);
        cJSON_AddItemToArray(inputs, in);
    }

    bool m_enabled, m_connected;
    char m_err[64];
    mqtt_ha_get_status(&m_enabled, &m_connected, m_err, sizeof(m_err));
    cJSON *m = cJSON_AddObjectToObject(o, "mqtt");
    cJSON_AddBoolToObject(m, "enabled", m_enabled);
    cJSON_AddBoolToObject(m, "connected", m_connected);
    cJSON_AddStringToObject(m, "error", m_err);
    cJSON_AddStringToObject(m, "device_id", mqtt_ha_device_id());

    cJSON_AddNumberToObject(o, "heap_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(o, "settings_rev", settings_rev());
    cJSON_AddBoolToObject(o, "restart_required", settings_restart_required());
    return send_json(req, o);
}

/* Field metadata so the UI can render limits and options generically */
static cJSON *fields_meta(void)
{
    int n;
    const setting_field_t *fields = settings_fields(&n);
    cJSON *meta = cJSON_CreateObject();
    for (int i = 0; i < n; i++) {
        const setting_field_t *f = &fields[i];
        cJSON *m = cJSON_AddObjectToObject(meta, f->key);
        cJSON_AddStringToObject(m, "name", f->name);
        if (f->type == F_FLOAT || f->type == F_INT) {
            cJSON_AddNumberToObject(m, "min", f->min);
            cJSON_AddNumberToObject(m, "max", f->max);
            cJSON_AddNumberToObject(m, "step", f->step);
            if (f->unit) cJSON_AddStringToObject(m, "unit", f->unit);
        } else if (f->type == F_ENUM) {
            cJSON *opts = cJSON_AddArrayToObject(m, "options");
            for (int k = 0; k < f->enum_count; k++) {
                cJSON *opt = cJSON_CreateObject();
                cJSON_AddStringToObject(opt, "value", f->enum_tokens[k]);
                cJSON_AddStringToObject(opt, "label", f->enum_labels[k]);
                cJSON_AddItemToArray(opts, opt);
            }
        } else if (f->type == F_STR) {
            cJSON_AddNumberToObject(m, "maxlen", f->size - 1);
        }
    }
    return meta;
}

static esp_err_t send_settings(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "settings", settings_to_json());
    cJSON_AddItemToObject(o, "fields", fields_meta());
    cJSON_AddNumberToObject(o, "eq_bands", EQ_BANDS);
    cJSON_AddNumberToObject(o, "sample_rate", AUDIO_SAMPLE_RATE);
    cJSON_AddNumberToObject(o, "settings_rev", settings_rev());
    cJSON_AddBoolToObject(o, "restart_required", settings_restart_required());
    return send_json(req, o);
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    return send_settings(req);
}

static esp_err_t settings_patch_handler(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return ESP_FAIL;
    cJSON *obj = cJSON_Parse(body);
    free(body);
    if (!obj) return send_error(req, "400 Bad Request", "invalid JSON");

    char err[96];
    esp_err_t r = settings_apply_json(obj, SET_FROM_WEB, err, sizeof(err));
    cJSON_Delete(obj);
    if (r != ESP_OK) return send_error(req, "400 Bad Request", err);
    return send_settings(req);
}

static esp_err_t eq_reset_handler(httpd_req_t *req)
{
    settings_reset_eq(SET_FROM_WEB);
    return send_settings(req);
}

static esp_err_t restart_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Restart requested");
    send_ok(req);
    schedule_restart();
    return ESP_OK;
}

static esp_err_t factory_reset_handler(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return ESP_FAIL;
    cJSON *obj = cJSON_Parse(body);
    free(body);
    const cJSON *c = cJSON_GetObjectItem(obj, "confirm");
    const bool ok = cJSON_IsString(c) && !strcmp(c->valuestring, "RESET");
    cJSON_Delete(obj);
    if (!ok) return send_error(req, "400 Bad Request", "confirmation missing");

    send_ok(req);
    vTaskDelay(pdMS_TO_TICKS(500));
    settings_factory_reset();
    return ESP_OK;
}

static esp_err_t ota_handler(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return send_error(req, "500 Internal Server Error", "no OTA partition");
    if (req->content_len == 0 || req->content_len > part->size) {
        return send_error(req, "400 Bad Request", "invalid image size");
    }

    char *buf = malloc(OTA_CHUNK);
    if (!buf) return httpd_resp_send_500(req);

    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        free(buf);
        return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "OTA: writing %u bytes to %s", (unsigned)req->content_len, part->label);

    size_t remaining = req->content_len;
    bool first = true;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < OTA_CHUNK ? remaining : OTA_CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { err = ESP_FAIL; break; }
        if (first) {
            first = false;
            if ((uint8_t)buf[0] != 0xE9) { err = ESP_ERR_OTA_VALIDATE_FAILED; break; }
        }
        if ((err = esp_ota_write(ota, buf, n)) != ESP_OK) break;
        remaining -= n;
    }
    free(buf);

    if (err != ESP_OK) {
        esp_ota_abort(ota);
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        return send_error(req, "400 Bad Request",
                          err == ESP_ERR_OTA_VALIDATE_FAILED ? "not a firmware image"
                                                             : esp_err_to_name(err));
    }
    if ((err = esp_ota_end(ota)) != ESP_OK ||
        (err = esp_ota_set_boot_partition(part)) != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        return send_error(req, "400 Bad Request",
                          err == ESP_ERR_OTA_VALIDATE_FAILED ? "image is corrupt"
                                                             : esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "OTA complete, restarting");
    send_ok(req);
    schedule_restart();
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */

void web_server_start(void)
{
    snprintf(s_etag, sizeof(s_etag), "\"%s-%s\"", esp_app_get_description()->version,
             esp_app_get_description()->time);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = WEB_PORT;
    cfg.stack_size = 8192;
    cfg.core_id = NET_TASK_CORE;
    cfg.task_priority = 5;
    cfg.max_open_sockets = 6;
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot start web server");
        return;
    }

    const httpd_uri_t routes[] = {
        { .uri = "/",                  .method = HTTP_GET,   .handler = asset_handler, .user_ctx = (void *)&k_index },
        { .uri = "/index.html",        .method = HTTP_GET,   .handler = asset_handler, .user_ctx = (void *)&k_index },
        { .uri = "/app.js",            .method = HTTP_GET,   .handler = asset_handler, .user_ctx = (void *)&k_js },
        { .uri = "/app.css",           .method = HTTP_GET,   .handler = asset_handler, .user_ctx = (void *)&k_css },
        { .uri = "/api/status",        .method = HTTP_GET,   .handler = status_handler },
        { .uri = "/api/settings",      .method = HTTP_GET,   .handler = settings_get_handler },
        { .uri = "/api/settings",      .method = HTTP_PATCH, .handler = settings_patch_handler },
        { .uri = "/api/settings",      .method = HTTP_POST,  .handler = settings_patch_handler },
        { .uri = "/api/eq/reset",      .method = HTTP_POST,  .handler = eq_reset_handler },
        { .uri = "/api/restart",       .method = HTTP_POST,  .handler = restart_handler },
        { .uri = "/api/factory_reset", .method = HTTP_POST,  .handler = factory_reset_handler },
        { .uri = "/api/ota",           .method = HTTP_POST,  .handler = ota_handler },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
    ESP_LOGI(TAG, "Web UI on port %d", WEB_PORT);
}
