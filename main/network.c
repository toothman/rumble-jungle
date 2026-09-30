// SPDX-License-Identifier: MIT
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "app_config.h"
#include "network.h"

static const char *TAG = "network";
static network_ip_cb_t s_on_ip;

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *ev = data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
    if (s_on_ip) s_on_ip(&ev->ip_info.ip);
}

#if NETWORK_MODE == NETWORK_MODE_ETHERNET
/* ------------------------------------------------------------------------ */
/* Ethernet (internal EMAC + RMII PHY)                                      */
/* ------------------------------------------------------------------------ */
#include "esp_eth.h"
#include "esp_eth_netif_glue.h"

static void network_backend_start(void)
{
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    esp_netif_set_hostname(netif, NETWORK_HOSTNAME);

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num  = ETH_MDC_GPIO;
    emac_cfg.smi_gpio.mdio_num = ETH_MDIO_GPIO;
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_cfg.clock_config.rmii.clock_gpio = ETH_RMII_CLK_GPIO;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = ETH_PHY_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth));
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth));
    ESP_LOGI(TAG, "Ethernet started");
}

#else
/* ------------------------------------------------------------------------ */
/* Wi-Fi (native radio, or co-processor via esp_wifi_remote on ESP32-P4)    */
/* ------------------------------------------------------------------------ */
#include "soc/soc_caps.h"
#if !SOC_WIFI_SUPPORTED && !__has_include("esp_wifi_remote.h")
#error "NETWORK_MODE_WIFI on this chip needs esp_wifi_remote and esp_hosted - enable them in main/idf_component.yml"
#endif
#include "esp_wifi.h"

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting");
        esp_wifi_connect();
    }
}

static void network_backend_start(void)
{
    if (strlen(WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "WIFI_SSID is empty - set it in config.h");
        return;
    }

    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, NETWORK_HOSTNAME);

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL));

    wifi_config_t wifi_cfg = { 0 };
    strlcpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode =
        strlen(WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);  /* power saving causes stream dropouts */
    ESP_LOGI(TAG, "Wi-Fi started, connecting to \"%s\"", WIFI_SSID);
}
#endif

void network_start(network_ip_cb_t on_ip)
{
    s_on_ip = on_ip;
    network_backend_start();
}
