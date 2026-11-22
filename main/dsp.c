// SPDX-License-Identifier: MIT
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "dsp_filters.h"
#include "dsp.h"

/* Filter chain layout (fixed, so filter states always line up):
 *   stage 0..1  rumble filter, 4th order Butterworth high-pass
 *   stage 2..   EQ bands: low shelf, peaking ..., high shelf              */
#define RUMBLE_STAGES   2
#define NUM_STAGES      (RUMBLE_STAGES + EQ_BANDS)

typedef struct {
    biquad_coef_t stage[NUM_STAGES];
    float gain;                      /* linear output gain */
} chain_t;

_Static_assert(sizeof((float[])EQ_BAND_FREQS) / sizeof(float) == EQ_BANDS,
               "config.h: EQ_BAND_FREQS must have EQ_BANDS entries");
_Static_assert(sizeof((float[])EQ_BAND_QS) / sizeof(float) == EQ_BANDS,
               "config.h: EQ_BAND_QS must have EQ_BANDS entries");

const float dsp_default_freq[EQ_BANDS] = EQ_BAND_FREQS;
const float dsp_default_q[EQ_BANDS]    = EQ_BAND_QS;

/* Hand-over of new coefficients to the audio task */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static chain_t s_pending;
static volatile bool s_has_pending;

/* Owned by the audio task */
static chain_t s_cur, s_next;
static biquad_state_t s_state[AUDIO_CHANNELS][NUM_STAGES];
static biquad_state_t s_next_state[AUDIO_CHANNELS][NUM_STAGES];

/* ------------------------------------------------------------------------ */

float dsp_max_freq(void)
{
    return fminf(EQ_FREQ_MAX_HZ, AUDIO_SAMPLE_RATE * 0.45f);
}

static void build_chain(const dsp_params_t *p, chain_t *c)
{
    const float fs = AUDIO_SAMPLE_RATE;

    if (p->rumble_enabled) {
        /* Two cascaded biquads with these Q values form a 4th order Butterworth */
        c->stage[0] = biquad_design(BQ_HIGH_PASS, p->rumble_freq, 0.5412f, 0, fs);
        c->stage[1] = biquad_design(BQ_HIGH_PASS, p->rumble_freq, 1.3066f, 0, fs);
    } else {
        c->stage[0] = biquad_identity();
        c->stage[1] = biquad_identity();
    }

    float max_boost = 0.0f;
    for (int b = 0; b < EQ_BANDS; b++) {
        biquad_coef_t *s = &c->stage[RUMBLE_STAGES + b];
        float g = p->band_gain[b];
        if (!p->eq_enabled || fabsf(g) < 0.05f) {
            *s = biquad_identity();
            continue;
        }
        biquad_type_t type = (b == 0)            ? BQ_LOW_SHELF
                           : (b == EQ_BANDS - 1) ? BQ_HIGH_SHELF
                                                 : BQ_PEAK;
        *s = biquad_design(type, p->band_freq[b], p->band_q[b], g, fs);
        if (g > max_boost) max_boost = g;
    }

    float gain_db = p->output_gain - (p->auto_headroom ? max_boost : 0.0f);
    c->gain = powf(10.0f, gain_db / 20.0f);
}

static void hand_over(const dsp_params_t *p)
{
    chain_t c;
    build_chain(p, &c);
    taskENTER_CRITICAL(&s_mux);
    s_pending = c;
    s_has_pending = true;
    taskEXIT_CRITICAL(&s_mux);
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

void dsp_init(const dsp_params_t *params)
{
    build_chain(params, &s_cur);
    memset(s_state, 0, sizeof(s_state));
}

void dsp_configure(const dsp_params_t *params)
{
    hand_over(params);
}

void dsp_process(float *buf, int frames)
{
    /* Pick up new coefficients and cross-fade to them over this block,
     * which avoids clicks when settings change.                          */
    bool fading = false;
    if (s_has_pending) {
        taskENTER_CRITICAL(&s_mux);
        s_next = s_pending;
        s_has_pending = false;
        taskEXIT_CRITICAL(&s_mux);
        memcpy(s_next_state, s_state, sizeof(s_state));
        fading = true;
    }

    const float step = fading ? 1.0f / frames : 0.0f;
    float t = 0.0f;

    for (int f = 0; f < frames; f++) {
        t += step;
        for (int ch = 0; ch < AUDIO_CHANNELS; ch++) {
            float *sample = &buf[f * AUDIO_CHANNELS + ch];
            const float x = *sample;

            float y = x;
            for (int s = 0; s < NUM_STAGES; s++) y = biquad_run(&s_cur.stage[s], &s_state[ch][s], y);
            y *= s_cur.gain;

            if (fading) {
                float y2 = x;
                for (int s = 0; s < NUM_STAGES; s++) y2 = biquad_run(&s_next.stage[s], &s_next_state[ch][s], y2);
                y2 *= s_next.gain;
                y += (y2 - y) * t;
            }
            *sample = y;
        }
    }

    if (fading) {
        s_cur = s_next;
        memcpy(s_state, s_next_state, sizeof(s_state));
    }
}
