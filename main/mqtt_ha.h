// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Starts the MQTT / Home Assistant service. The broker connection follows
 * the MQTT settings and is re-established whenever they change.           */
void mqtt_ha_start(void);

/* Tells MQTT the current stream URL (http://<ip>:<port>/stream.aac). */
void mqtt_ha_set_ip(const char *ip);

/* Connection state for the web UI. error is "" when there is none. */
void mqtt_ha_get_status(bool *enabled, bool *connected, char *error, size_t errlen);

/* Stable device id, e.g. "rumble_jungle_a1b2c3" (from the MAC address). */
const char *mqtt_ha_device_id(void);
