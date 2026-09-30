// SPDX-License-Identifier: MIT
/*
 * Turntable Streamer
 *
 * Captures line-level audio from a PCM1808 ADC, encodes it to AAC and
 * serves it as an HTTP radio stream (e.g. for Sonos). Status is reported
 * to Home Assistant via MQTT, which can also control the built-in rumble
 * filter and equalizer. All settings live in main/config.h.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "app_config.h"
#include "network.h"
#include "dsp.h"
#include "audio.h"
#include "stream_server.h"
#include "mqtt_ha.h"

#define GOT_IP_BIT BIT0

static const char *TAG = "main";
static EventGroupHandle_t s_events;

static void on_ip(const esp_ip4_addr_t *ip)
{
    char url[96];
    snprintf(url, sizeof(url), "http://" IPSTR ":%d" STREAM_PATH, IP2STR(ip), STREAM_PORT);
    ESP_LOGI(TAG, "Stream URL: %s", url);
    mqtt_ha_set_stream_url(url);
    xEventGroupSetBits(s_events, GOT_IP_BIT);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    dsp_init();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_events = xEventGroupCreate();
    network_start(on_ip);
    xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    audio_start();
    stream_server_start();
    mqtt_ha_start();
}
