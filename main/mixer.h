// SPDX-License-Identifier: MIT
/*
 * Source selection for the two inputs: input trim, level metering, signal
 * detection, automatic switching and click-free cross-fading.
 *
 * Auto mode: the input that is playing stays on air until it goes silent
 * (no interruptions). When it stops and the other input is playing, the
 * mixer switches over. If both start at the same time, the priority input
 * wins.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"
#include "settings.h"

typedef enum { ACTIVE_NONE, ACTIVE_A, ACTIVE_B, ACTIVE_MIX } active_source_t;

typedef struct {
    float           level_dbfs[NUM_INPUTS];   /* peak with ~20 dB/s fall-back */
    bool            signal[NUM_INPUTS];       /* after on/off hold times      */
    active_source_t active;
} mixer_status_t;

/* Applies the audio settings (trim, thresholds, mode, timing). Any task. */
void mixer_configure(const audio_settings_t *a);

/* Fades both inputs in from silence, e.g. after an I2S resync. */
void mixer_fade_in(void);

/* Mixes two interleaved stereo blocks into out (audio task only). */
void mixer_process(const float *in_a, const float *in_b, float *out, int frames);

void mixer_get_status(mixer_status_t *out);

const char *mixer_active_token(active_source_t a);   /* "a", "b", "mix", "none" */
