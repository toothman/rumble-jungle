// SPDX-License-Identifier: MIT
/*
 * Audio processing: rumble filter, parametric equalizer and output gain.
 * Parameters come from the settings module and can change at runtime.
 */
#pragma once
#include <stdbool.h>
#include "app_config.h"

typedef struct {
    bool  eq_enabled;
    float band_gain[EQ_BANDS];   /* dB */
    float band_freq[EQ_BANDS];   /* Hz */
    float band_q[EQ_BANDS];
    bool  rumble_enabled;
    float rumble_freq;           /* Hz */
    float output_gain;           /* dB */
    bool  auto_headroom;         /* lower the gain by the largest EQ boost */
} dsp_params_t;

/* Default EQ band frequencies and Q values from config.h. */
extern const float dsp_default_freq[EQ_BANDS];
extern const float dsp_default_q[EQ_BANDS];

/* Highest usable filter frequency for the configured sample rate. */
float dsp_max_freq(void);

/* Builds the initial filter chain. Call before audio_start(). */
void dsp_init(const dsp_params_t *params);

/* Switches to new parameters; the audio task cross-fades to them. */
void dsp_configure(const dsp_params_t *params);

/* Processes interleaved stereo float samples in place (audio task only). */
void dsp_process(float *buf, int frames);
