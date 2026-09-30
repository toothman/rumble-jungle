// SPDX-License-Identifier: MIT
#pragma once

/* Connects to the MQTT broker and announces the device to Home Assistant.
 * Does nothing if MQTT_ENABLED is 0.                                       */
void mqtt_ha_start(void);

/* Updates the published stream URL (e.g. after an IP change). */
void mqtt_ha_set_stream_url(const char *url);
