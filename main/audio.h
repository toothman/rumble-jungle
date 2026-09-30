// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

/* Starts I2S capture, audio processing and AAC encoding. */
void audio_start(void);

/* Ring buffer carrying encoded ADTS-AAC frames (one item per frame). */
RingbufHandle_t audio_get_output(void);

/* Input peak level of the last ~0.25 s in dBFS (before processing). */
float audio_get_level_dbfs(void);

/* True if the output limiter engaged since the last call. */
bool audio_take_clip_flag(void);
