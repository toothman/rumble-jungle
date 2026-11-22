// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include "esp_netif.h"
#include "settings.h"

typedef struct {
    bool link_up;
    bool has_ip;
    int  speed_mbps;
    char ip[16];
    char netmask[16];
    char gateway[16];
    char mac[18];        /* aa:bb:cc:dd:ee:ff */
    char hostname[32];
} network_info_t;

/* Called on every (re)assigned IPv4 address. */
typedef void (*network_ip_cb_t)(const esp_ip4_addr_t *ip);

/* Brings up Ethernet with hostname and DHCP / static IP from the settings. */
void network_start(const net_settings_t *cfg, network_ip_cb_t on_ip);

void network_get_info(network_info_t *out);

/* Raw 6-byte MAC address (valid after network_start). */
void network_get_mac(uint8_t mac[6]);
