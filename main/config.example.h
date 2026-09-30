// SPDX-License-Identifier: MIT
/*
 * Turntable Streamer - user configuration
 *
 * Copy this file to "config.h" in the same folder and adapt it to your setup.
 * config.h is excluded from git, so your credentials stay private.
 *
 * All text values must be plain string literals ("..."), because they are
 * concatenated into topics and headers at compile time.
 */
#pragma once

/* ==========================================================================
 * Network
 * ========================================================================== */

/* How the device connects to your network:
 *   NETWORK_MODE_ETHERNET  on-board Ethernet (recommended, works with PoE)
 *   NETWORK_MODE_WIFI      Wi-Fi. The ESP32-P4 has no radio of its own, so
 *                          this needs a board with an ESP32-C6/C5 co-processor
 *                          and the Wi-Fi dependencies enabled in
 *                          main/idf_component.yml (see README).             */
#define NETWORK_MODE            NETWORK_MODE_ETHERNET

#define NETWORK_HOSTNAME        "turntable-streamer"

/* Wi-Fi credentials - only used with NETWORK_MODE_WIFI.
 * Leave WIFI_PASSWORD empty for an open network.                           */
#define WIFI_SSID               ""
#define WIFI_PASSWORD           ""

/* Ethernet PHY - defaults match the Waveshare ESP32-P4-ETH (IP101GRI).
 * The RMII data pins use the ESP-IDF defaults for the ESP32-P4.            */
#define ETH_PHY_ADDR            1
#define ETH_PHY_RST_GPIO        51      /* -1 if the PHY has no reset line  */
#define ETH_MDC_GPIO            31
#define ETH_MDIO_GPIO           52
#define ETH_RMII_CLK_GPIO       50      /* 50 MHz reference clock from PHY  */

/* ==========================================================================
 * Audio input: PCM1808 ADC via I2S (the ESP32 is clock master)
 * ========================================================================== */

/* Check these against your board's pinout before wiring! On the
 * Waveshare ESP32-P4-ETH avoid: Ethernet (28-35, 49-52), I2C (7, 8),
 * on-board codec (9-13), SD card (39-44) and strapping pins (34-38).       */
#define PIN_I2S_MCLK            20      /* -> PCM1808 SCK */
#define PIN_I2S_BCLK            21      /* -> PCM1808 BCK */
#define PIN_I2S_WS              22      /* -> PCM1808 LRC */
#define PIN_I2S_DIN             23      /* <- PCM1808 OUT */

#define AUDIO_SAMPLE_RATE       44100   /* 44100 or 48000                   */
#define AUDIO_CHANNELS          2

/* ==========================================================================
 * Encoder and stream
 * ========================================================================== */

#define AAC_BITRATE             192000  /* bit/s, 128000 ... 256000         */

#define STREAM_PORT             8000
#define STREAM_PATH             "/stream.aac"
#define STREAM_NAME             "Turntable"  /* shown by some players       */
#define STREAM_MAX_CLIENTS      4

/* ==========================================================================
 * MQTT / Home Assistant (optional)
 * ========================================================================== */

#define MQTT_ENABLED            1       /* 0 = streaming only, no MQTT      */

#define MQTT_BROKER_URI         "mqtt://homeassistant.local:1883"
#define MQTT_USER               ""      /* empty = no authentication        */
#define MQTT_PASSWORD           ""

#define DEVICE_ID               "turntable_streamer" /* unique, no spaces   */
#define DEVICE_NAME             "Turntable Streamer" /* name shown in HA    */
#define MQTT_BASE_TOPIC         "turntable_streamer"
#define HA_DISCOVERY_PREFIX     "homeassistant"

/* ==========================================================================
 * Signal detection (drives the "Signal" binary sensor)
 * ========================================================================== */

#define SIGNAL_THRESHOLD_DBFS   (-50.0f) /* above = music is playing        */
#define SIGNAL_ON_HOLD_MS       1500     /* above threshold this long -> ON */
#define SIGNAL_OFF_HOLD_MS      20000    /* below threshold this long -> OFF */

/* ==========================================================================
 * Audio processing (all adjustable at runtime via MQTT / Home Assistant;
 * the values here are the defaults and the allowed ranges)
 * ========================================================================== */

/* Equalizer: the first band is a low shelf, the last a high shelf and all
 * bands in between are peaking filters. EQ_BAND_FREQS and EQ_BAND_QS must
 * have exactly EQ_BANDS entries (2 ... 10).                                */
#define EQ_BANDS                5
#define EQ_BAND_FREQS           { 60, 250, 1000, 4000, 12000 }
#define EQ_BAND_QS              { 0.7f, 1.0f, 1.0f, 1.0f, 0.7f }
#define EQ_ENABLED_DEFAULT      1

#define EQ_GAIN_MIN_DB          (-12.0f)
#define EQ_GAIN_MAX_DB          (12.0f)
#define EQ_GAIN_STEP_DB         (0.5f)
#define EQ_FREQ_MIN_HZ          (20.0f)
#define EQ_FREQ_MAX_HZ          (20000.0f)
#define EQ_Q_MIN                (0.3f)
#define EQ_Q_MAX                (5.0f)

/* Rumble filter: steep high-pass that removes subsonic noise from warped
 * records and tonearm movement.                                            */
#define RUMBLE_FILTER_DEFAULT   1
#define RUMBLE_FREQ_DEFAULT_HZ  (20.0f)
#define RUMBLE_FREQ_MIN_HZ      (10.0f)
#define RUMBLE_FREQ_MAX_HZ      (60.0f)

/* Output gain, applied after the EQ */
#define OUTPUT_GAIN_DEFAULT_DB  (0.0f)
#define OUTPUT_GAIN_MIN_DB      (-24.0f)
#define OUTPUT_GAIN_MAX_DB      (12.0f)

/* Auto headroom: lowers the output gain by the largest EQ boost, so
 * boosting a band does not cause clipping.                                 */
#define AUTO_HEADROOM_DEFAULT   1

/* Settings are saved to flash this long after the last change */
#define DSP_SAVE_DELAY_MS       3000

/* ==========================================================================
 * Advanced - only change if you know why
 * ========================================================================== */

#define I2S_DMA_DESC_NUM        8
#define I2S_DMA_FRAME_NUM       512
#define OUTPUT_BUFFER_SIZE      (32 * 1024) /* encoded frames, bytes        */

#define AUDIO_TASK_CORE         1
#define AUDIO_TASK_PRIORITY     10
#define NET_TASK_CORE           0
#define NET_TASK_PRIORITY       6
