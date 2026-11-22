// SPDX-License-Identifier: MIT
/*
 * Ethernet (internal EMAC + IP101 RMII PHY on the Waveshare ESP32-P4-ETH).
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_netif_glue.h"
#include "mdns.h"
#include "app_config.h"
#include "network.h"

static const char *TAG = "network";
static network_ip_cb_t s_on_ip;
static esp_netif_t *s_netif;
static esp_eth_handle_t s_eth;
static uint8_t s_mac[6];
static volatile bool s_link_up, s_has_ip;
static volatile int s_speed;

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED: {
        eth_speed_t speed = ETH_SPEED_100M;
        esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, &speed);
        s_speed = speed == ETH_SPEED_100M ? 100 : 10;
        s_link_up = true;
        ESP_LOGI(TAG, "Link up, %d Mbit/s", s_speed);
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
        s_link_up = false;
        ESP_LOGW(TAG, "Link down");
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        s_has_ip = true;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        if (s_on_ip) s_on_ip(&ev->ip_info.ip);
    } else if (id == IP_EVENT_ETH_LOST_IP) {
        s_has_ip = false;
        ESP_LOGW(TAG, "Lost IP");
    }
}

static void apply_static_ip(const net_settings_t *cfg)
{
    esp_netif_ip_info_t info = { 0 };
    esp_netif_str_to_ip4(cfg->ip, &info.ip);
    esp_netif_str_to_ip4(cfg->netmask, &info.netmask);
    if (cfg->gateway[0]) esp_netif_str_to_ip4(cfg->gateway, &info.gw);

    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(s_netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_netif, &info));

    const char *dns = cfg->dns[0] ? cfg->dns : cfg->gateway;
    if (dns[0]) {
        esp_netif_dns_info_t d = { .ip.type = ESP_IPADDR_TYPE_V4 };
        esp_netif_str_to_ip4(dns, &d.ip.u_addr.ip4);
        esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &d);
    }
    ESP_LOGI(TAG, "Static IP %s", cfg->ip);
}

void network_start(const net_settings_t *cfg, network_ip_cb_t on_ip)
{
    s_on_ip = on_ip;

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_cfg);
    esp_netif_set_hostname(s_netif, cfg->hostname);

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
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &s_eth));
    esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, s_mac);
    ESP_ERROR_CHECK(esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth)));

    if (!cfg->dhcp) apply_static_ip(cfg);

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_eth_start(s_eth));
    ESP_LOGI(TAG, "Ethernet started, hostname %s", cfg->hostname);

    /* <hostname>.local and service discovery for the web UI and stream */
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(cfg->hostname);
        mdns_instance_name_set("Rumble Jungle");
        mdns_service_add(NULL, "_http", "_tcp", WEB_PORT, NULL, 0);
        mdns_txt_item_t txt[] = { { "path", STREAM_PATH } };
        mdns_service_add("Rumble Jungle Stream", "_rumble-stream", "_tcp", STREAM_PORT, txt, 1);
    }
}

void network_get_mac(uint8_t mac[6])
{
    memcpy(mac, s_mac, 6);
}

void network_get_info(network_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->link_up = s_link_up;
    out->has_ip = s_has_ip;
    out->speed_mbps = s_link_up ? s_speed : 0;
    snprintf(out->mac, sizeof(out->mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);

    const char *host = NULL;
    if (s_netif && esp_netif_get_hostname(s_netif, &host) == ESP_OK && host) {
        strlcpy(out->hostname, host, sizeof(out->hostname));
    }
    esp_netif_ip_info_t info;
    if (s_has_ip && s_netif && esp_netif_get_ip_info(s_netif, &info) == ESP_OK) {
        snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&info.ip));
        snprintf(out->netmask, sizeof(out->netmask), IPSTR, IP2STR(&info.netmask));
        snprintf(out->gateway, sizeof(out->gateway), IPSTR, IP2STR(&info.gw));
    }
}
