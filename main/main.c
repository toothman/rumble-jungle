// SPDX-License-Identifier: MIT
/*
 * Rumble Jungle
 *
 * Captures two line-level sources from two PCM1808 ADCs, picks the one that
 * is playing (or mixes them), runs it through a rumble filter and an
 * equalizer, encodes it to AAC and serves it as an HTTP radio stream (e.g.
 * for Sonos). Everything is configurable in the web UI and via MQTT, with
 * Home Assistant discovery.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "app_config.h"
#include "settings.h"
#include "network.h"
#include "audio.h"
#include "stream_server.h"
#include "web_server.h"
#include "mqtt_ha.h"

#define ROLLBACK_TIMEOUT_MS 60000

static const char *TAG = "main";
static volatile bool s_got_ip;

static void on_ip(const esp_ip4_addr_t *ip)
{
    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(ip));
    ESP_LOGI(TAG, "Web UI:     http://%s/", ip_str);
    ESP_LOGI(TAG, "Stream URL: http://%s:%d" STREAM_PATH, ip_str, STREAM_PORT);
    mqtt_ha_set_ip(ip_str);
    s_got_ip = true;
}

/* After an OTA update the new firmware has to prove itself: once audio
 * runs and the network is up, it is marked valid. If it crashes before
 * that, the bootloader rolls back to the previous version.              */
static void confirm_firmware(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    for (int ms = 0; ms < ROLLBACK_TIMEOUT_MS; ms += 500) {
        audio_status_t a;
        audio_get_status(&a);
        if (a.running && s_got_ip) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "New firmware confirmed");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGE(TAG, "New firmware did not start properly, rolling back");
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Rumble Jungle %s", esp_app_get_description()->version);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    settings_init();

    settings_t s;
    settings_get(&s);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    network_start(&s.net, on_ip);

    /* All services bind to any address and work as soon as an IP arrives */
    stream_server_start();
    audio_start();
    web_server_start();
    mqtt_ha_start();

    confirm_firmware();
}
