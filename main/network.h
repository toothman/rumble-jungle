// SPDX-License-Identifier: MIT
#pragma once
#include "esp_netif.h"

/* Called on every (re)assigned IPv4 address. */
typedef void (*network_ip_cb_t)(const esp_ip4_addr_t *ip);

/* Brings up Ethernet or Wi-Fi, depending on NETWORK_MODE in config.h. */
void network_start(network_ip_cb_t on_ip);
