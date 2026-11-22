// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "mixer.h"

typedef struct {
    mixer_status_t mix;
    bool           clipping;       /* limiter engaged in the last 2 s */
    uint32_t       bitrate;        /* bit/s currently encoded         */
    bool           running;        /* audio frames are being produced */
} audio_status_t;

/* Starts I2S capture of both inputs, processing and AAC encoding. Encoded
 * frames are handed to the stream server. Call after settings_init().     */
void audio_start(void);

void audio_get_status(audio_status_t *out);
