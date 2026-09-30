# Turntable Streamer

![Build](../../actions/workflows/build.yml/badge.svg)
![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)

Stream a turntable (or any line-level source) to Sonos and other network players.
An ESP32-P4 captures the audio from a PCM1808 ADC, runs it through a rumble filter and a
parametric equalizer, encodes it to AAC and serves it as an HTTP radio stream. Home Assistant gets a "music is playing" sensor via MQTT,
so playback on your speakers can start and stop automatically.

```
Turntable -> phono preamp -> PCM1808 -> ESP32-P4 (EQ + AAC) -> Ethernet/PoE -> Sonos
                                             |
                                             +-> MQTT -> Home Assistant
```

> **Status:** early prototype, not yet verified on hardware. Feedback and fixes welcome.

## Hardware

| Part | Notes |
|---|---|
| Waveshare ESP32-P4-ETH | Optionally with PoE module. Other ESP32-P4 boards work with adjusted pins. |
| PCM1808 ADC module | Common low-cost I2S breakout, 24 bit / 44.1-48 kHz |
| Phono preamp | Required for turntables without a built-in preamp (MM cartridge) |
| Cables | RCA cables, jumper wires |

The on-board ES8311 codec of the Waveshare board is a mono voice codec and is not used.

## Wiring

**Check the GPIOs against your board's pinout before wiring.** All pins are set in `main/config.h`.

| PCM1808 | ESP32-P4 (default) | Function |
|---|---|---|
| SCK | GPIO20 | Master clock (256 x fs) |
| BCK | GPIO21 | Bit clock |
| LRC | GPIO22 | Word select |
| OUT | GPIO23 | Audio data to the ESP32 |
| 5V | 5V | Analog supply |
| 3.3V | 3V3 | Digital supply |
| GND | GND | Ground |
| FMT, MD0, MD1 | GND | I2S format, slave mode |

Many modules already pull FMT/MD0/MD1 low; in that case leave them unconnected.
Keep the clock wires short (below 10-15 cm).

**Noise:** The PoE converter is a switching regulator. Feed the PCM1808's 5 V analog supply
through a small LC filter (e.g. 10 µH + 100 µF) or a separate LDO. Connect the turntable's
ground wire to the phono preamp.

## Configuration

All settings (pins, network, Wi-Fi credentials, MQTT, stream and detection parameters) live in
a single file:

```
cp main/config.example.h main/config.h
```

Then edit `main/config.h`. It is listed in `.gitignore`, so credentials never end up in the
repository. The build stops with a hint if the file is missing.

### Ethernet or Wi-Fi

Ethernet is the default and recommended. The ESP32-P4 has no radio of its own, so Wi-Fi only
works on boards with an ESP32-C6/C5 co-processor (for example the Waveshare ESP32-P4-WIFI6-POE-ETH).
To use Wi-Fi:

1. Set `NETWORK_MODE` to `NETWORK_MODE_WIFI` and fill in `WIFI_SSID` / `WIFI_PASSWORD`.
2. Uncomment `esp_wifi_remote` and `esp_hosted` in `main/idf_component.yml`.
3. Check the co-processor settings (interface and pins) in `idf.py menuconfig` for your board.

### MQTT

Set `MQTT_ENABLED` to `0` if you only want the stream without Home Assistant.

## Build and flash

Requires ESP-IDF **5.4 or 5.5**.

```
idf.py set-target esp32p4
idf.py build flash monitor
```

The `esp_audio_codec` component is downloaded automatically on the first build.

Some early ESP32-P4 boards have chip revision < 3. If the firmware does not boot, enable support
for older revisions in `idf.py menuconfig`. The revision is shown in the boot log.

## Testing

1. The monitor prints the stream URL, e.g. `http://192.168.1.50:8000/stream.aac`.
2. Open it in **VLC** first (Media -> Open Network Stream). If it plays there, the stream itself is fine.
3. Give the board a fixed IP in your router (DHCP reservation).

## Home Assistant

With the MQTT integration enabled, a device appears automatically with these entities:

| Entity | Type | Purpose |
|---|---|---|
| Signal | binary sensor | On while a record is playing |
| Level | sensor | Input peak level in dBFS |
| Clipping | binary sensor | On when the output limiter engages (lower the gain) |
| Listeners | sensor | Number of connected players |
| Stream URL | sensor | Current URL of the stream |
| Equalizer | switch | EQ on/off (bypass) |
| EQ 1 ... EQ N Gain | number | Gain per band |
| EQ 1 ... EQ N Frequency, Q | number | Band frequency and width (configuration section) |
| Reset Equalizer | button | All bands flat, default frequencies |
| Rumble Filter | switch | Subsonic high-pass on/off |
| Rumble Filter Frequency | number | Cutoff frequency (configuration section) |
| Output Gain | number | Overall gain after the EQ |
| Auto Headroom | switch | Lowers the gain by the largest EQ boost |

All audio settings are stored in flash and survive a reboot.

Example automations (adjust entity IDs to your setup):

```yaml
alias: Turntable starts living room
triggers:
  - trigger: state
    entity_id: binary_sensor.turntable_streamer_signal
    to: "on"
actions:
  - action: media_player.play_media
    target:
      entity_id: media_player.living_room
    data:
      media_content_id: "http://192.168.1.50:8000/stream.aac"
      media_content_type: music
```

```yaml
alias: Turntable stops living room
triggers:
  - trigger: state
    entity_id: binary_sensor.turntable_streamer_signal
    to: "off"
actions:
  - action: media_player.media_stop
    target:
      entity_id: media_player.living_room
```

If Sonos treats the stream like a file and stops, pass it as a radio stream instead:
`x-rincon-mp3radio://192.168.1.50:8000/stream.aac`

Expect a few seconds of latency on Sonos; this is caused by its internal buffering.

## Audio processing

The signal chain on the ESP32 is:

```
input -> rumble filter -> equalizer -> output gain -> soft limiter -> AAC
```

- **Rumble filter:** 4th order Butterworth high-pass (24 dB/octave), default 20 Hz. Removes
  subsonic noise from warped records and tonearm movement.
- **Equalizer:** `EQ_BANDS` bands (default 5). The first band is a low shelf, the last a high
  shelf, the others are peaking filters. Default frequencies: 60, 250, 1000, 4000, 12000 Hz.
- **Output gain and auto headroom:** With auto headroom on, the gain is reduced by the largest
  EQ boost, so boosting the bass does not overload the signal.
- **Soft limiter:** Above -1 dBFS the signal is compressed smoothly instead of clipping hard.
  The Clipping sensor shows when this happens.

Setting changes are cross-faded over one audio block (~23 ms), so adjusting a slider does not
click. Processing runs in 32-bit float on the full 24-bit input.

Band count, default frequencies and Q values, and all allowed ranges are set in `config.h`.
When you change `EQ_BANDS`, stored settings are reset to the defaults.

### MQTT topics

Besides Home Assistant, the settings can be changed with any MQTT client. Each setting has a
state topic `<base>/<setting>` (retained) and a command topic `<base>/<setting>/set`:

```
turntable_streamer/eq2_gain/set      -> 3.5
turntable_streamer/rumble/set        -> ON
turntable_streamer/eq_reset/set      -> PRESS
```

Settings: `eq`, `eq<N>_gain`, `eq<N>_freq`, `eq<N>_q`, `eq_reset`, `rumble`, `rumble_freq`,
`output_gain`, `auto_headroom`. Values outside the configured ranges are clamped; the state
topic always reports the value actually in use.

## Tuning

- `SIGNAL_THRESHOLD_DBFS`: Read the level in Home Assistant while a record plays and with the
  tonearm lifted, then set the threshold in between.
- `SIGNAL_OFF_HOLD_MS`: How long silence must last before the signal turns off (default 20 s,
  which bridges the gaps between tracks).
- `AAC_BITRATE`: 192 kbit/s is a good default; 256 kbit/s also works.

## Project layout

```
main/
  config.example.h   template for all user settings
  app_config.h       loads and validates config.h
  main.c             startup sequence
  network.c          Ethernet or Wi-Fi
  audio.c            I2S capture, level metering, limiter, AAC encoding
  dsp.c              rumble filter, equalizer, gain, settings storage
  dsp_filters.h      biquad filter design (RBJ Audio EQ Cookbook)
.github/workflows/
  build.yml          CI build with ESP-IDF 5.4 and 5.5
LICENSE              MIT License
  stream_server.c    HTTP streaming to multiple listeners
  mqtt_ha.c          MQTT and Home Assistant discovery
```

## License

This project is licensed under the [MIT License](LICENSE). It is completely free to use,
modify, adapt and share, for private and commercial purposes alike. Build your own version,
change whatever you like, and use parts of it in your own projects. The only condition is
that the license notice stays included.

Contributions, forks and improvements are very welcome.
