// SPDX-License-Identifier: MIT
/*
 * Rumble Jungle - build-time configuration
 *
 * Only hardware wiring and structural constants live here. Everything a user
 * normally changes (equalizer, inputs, MQTT, network, stream) is a runtime
 * setting, edited in the web UI or via MQTT and stored in flash. The values
 * below marked "default" are what a freshly flashed or factory-reset device
 * starts with.
 */
#pragma once

/* ==========================================================================
 * Ethernet - defaults match the Waveshare ESP32-P4-ETH(-M) with IP101GRI PHY.
 * The RMII data pins use the ESP-IDF defaults for the ESP32-P4.
 * ========================================================================== */

#define ETH_PHY_ADDR            1
#define ETH_PHY_RST_GPIO        51      /* PHY reset/power, -1 if none      */
#define ETH_MDC_GPIO            31
#define ETH_MDIO_GPIO           52
#define ETH_RMII_CLK_GPIO       50      /* 50 MHz reference clock from PHY  */

/* ==========================================================================
 * Audio inputs: two PCM1808 ADCs
 *
 * I2S0 is clock master and drives MCLK/BCK/LRC for both ADCs (shared wires).
 * I2S1 runs as slave and reads the second ADC; it samples BCK/LRC from the
 * same pins, so both inputs are sample-synchronous. See README "Wiring".
 *
 * All five pins are on the right-hand row of the Waveshare header (pins 35,
 * 34, 32, 31, 29). Other free header GPIOs: 2-6, 14-19, 27, 32, 33, 46-48,
 * 54. Avoid Ethernet (28-31, 34, 35, 49-52), I2C (7, 8), the on-board codec
 * (9-13, 53), USB (24, 25), SD card (39-44) and strapping pins (34-38).
 * ========================================================================== */

#define PIN_I2S_MCLK            20      /* -> SCK of both ADCs              */
#define PIN_I2S_BCLK            21      /* -> BCK of both ADCs              */
#define PIN_I2S_WS              22      /* -> LRC of both ADCs              */
#define PIN_I2S_DIN_A           23      /* <- OUT of ADC A                  */
#define PIN_I2S_DIN_B           26      /* <- OUT of ADC B                  */

/* Pins the slave controller (input B) reads its clocks from. Normally the
 * same pins as above (internal loopback, no extra wires). If input B stays
 * silent on your board, set these to two free GPIOs and bridge them to
 * PIN_I2S_BCLK / PIN_I2S_WS with jumper wires.                             */
#define PIN_I2S_B_BCLK          PIN_I2S_BCLK
#define PIN_I2S_B_WS            PIN_I2S_WS

#define AUDIO_SAMPLE_RATE       44100   /* 44100 or 48000                   */

/* ==========================================================================
 * Equalizer structure. The first band is a low shelf, the last a high shelf
 * and all bands in between are peaking filters. Changing EQ_BANDS resets
 * the stored audio settings.
 * ========================================================================== */

#define EQ_BANDS                5
#define EQ_BAND_FREQS           { 60, 250, 1000, 4000, 12000 }
#define EQ_BAND_QS              { 0.7f, 1.0f, 1.0f, 1.0f, 0.7f }

#define EQ_GAIN_MIN_DB          (-12.0f)
#define EQ_GAIN_MAX_DB          (12.0f)
#define EQ_GAIN_STEP_DB         (0.5f)
#define EQ_FREQ_MIN_HZ          (20.0f)
#define EQ_FREQ_MAX_HZ          (20000.0f)
#define EQ_Q_MIN                (0.3f)
#define EQ_Q_MAX                (5.0f)

#define RUMBLE_FREQ_MIN_HZ      (10.0f)
#define RUMBLE_FREQ_MAX_HZ      (60.0f)
#define OUTPUT_GAIN_MIN_DB      (-24.0f)
#define OUTPUT_GAIN_MAX_DB      (12.0f)
#define INPUT_TRIM_MIN_DB       (-12.0f)
#define INPUT_TRIM_MAX_DB       (12.0f)
#define THRESHOLD_MIN_DBFS      (-90.0f)
#define THRESHOLD_MAX_DBFS      (-10.0f)

/* ==========================================================================
 * Defaults for runtime settings
 * ========================================================================== */

#define DEFAULT_HOSTNAME        "rumble-jungle"
#define DEFAULT_DEVICE_NAME     "Rumble Jungle"
#define DEFAULT_INPUT_A_NAME    "Aux 1"
#define DEFAULT_INPUT_B_NAME    "Aux 2"
#define DEFAULT_THRESHOLD_DBFS  (-50.0f)
#define DEFAULT_SIGNAL_ON_MS    1500
#define DEFAULT_SIGNAL_OFF_MS   20000
#define DEFAULT_XFADE_MS        300
#define DEFAULT_EQ_ENABLED      1
#define DEFAULT_RUMBLE_ENABLED  1
#define DEFAULT_RUMBLE_FREQ_HZ  (20.0f)
#define DEFAULT_OUTPUT_GAIN_DB  (0.0f)
#define DEFAULT_AUTO_HEADROOM   1

#define DEFAULT_MQTT_ENABLED    0
#define DEFAULT_MQTT_URI        "mqtt://homeassistant.local:1883"
#define DEFAULT_MQTT_BASE_TOPIC "rumble_jungle"
#define DEFAULT_HA_PREFIX       "homeassistant"

#define DEFAULT_AAC_BITRATE     192000
#define DEFAULT_STREAM_NAME     "Rumble Jungle"

/* ==========================================================================
 * Ports, stream and timing
 * ========================================================================== */

#define WEB_PORT                80
#define STREAM_PORT             8000
#define STREAM_PATH             "/stream.aac"
#define STREAM_MAX_CLIENTS      4
#define STREAM_CLIENT_FIFO      (64 * 1024) /* per listener, bytes          */
#define STREAM_PREBUFFER_BYTES  (24 * 1024) /* sent to new listeners first  */

#define SETTINGS_SAVE_DELAY_MS  3000    /* flash write after last change    */

/* ==========================================================================
 * Advanced - only change if you know why
 * ========================================================================== */

#define I2S_DMA_DESC_NUM        8
#define I2S_DMA_FRAME_NUM       480     /* max 504 at 32 bit stereo */

#define AUDIO_TASK_CORE         1
#define AUDIO_TASK_PRIORITY     10
#define NET_TASK_CORE           0
#define NET_TASK_PRIORITY       6
