// SPDX-License-Identifier: MIT
/*
 * Audio processing: rumble filter, parametric equalizer and output gain.
 * All parameters can be changed at runtime and are stored in NVS.
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

/* Loads the saved parameters (or the defaults). Call after nvs_flash_init()
 * and before audio_start().                                                */
void dsp_init(void);

void dsp_get_params(dsp_params_t *out);

/* Applies new parameters (values are clamped to the configured ranges) and
 * schedules saving them to flash.                                          */
void dsp_update(const dsp_params_t *params);

/* Resets all EQ bands to flat with default frequencies and Q. */
void dsp_reset_eq(void);

/* Processes interleaved float samples in place (audio task only). */
void dsp_process(float *buf, int frames);
