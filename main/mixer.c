// SPDX-License-Identifier: MIT
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "mixer.h"

#define LEVEL_FALL_DB_PER_S  20.0f
#define LEVEL_FLOOR_DBFS     (-120.0f)

typedef struct {
    uint8_t  mode;
    uint8_t  priority;
    float    trim[NUM_INPUTS];          /* linear */
    float    threshold[NUM_INPUTS];     /* linear peak */
    uint32_t on_ms, off_ms, xfade_ms;
} mixer_cfg_t;

/* Hand-over from the settings listener */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static mixer_cfg_t s_pending;
static volatile bool s_has_pending;
static volatile bool s_fade_in;

/* Owned by the audio task */
static mixer_cfg_t s_cfg;
static bool s_configured;
static float s_peak[NUM_INPUTS];        /* linear, with fall-back */
static uint32_t s_above_ms[NUM_INPUTS], s_below_ms[NUM_INPUTS];
static bool s_signal[NUM_INPUTS];
static int s_selected = INPUT_A;        /* auto mode: input on air */
static float s_pos[NUM_INPUTS];         /* fade position 0..1 */

/* Published status, read by other tasks */
static volatile float s_level_dbfs[NUM_INPUTS] = { LEVEL_FLOOR_DBFS, LEVEL_FLOOR_DBFS };
static volatile bool s_status_signal[NUM_INPUTS];
static volatile active_source_t s_active = ACTIVE_NONE;

static inline float db_to_lin(float db) { return powf(10.0f, db / 20.0f); }

void mixer_configure(const audio_settings_t *a)
{
    mixer_cfg_t c = {
        .mode = a->source_mode,
        .priority = a->priority,
        .on_ms = a->signal_on_ms,
        .off_ms = a->signal_off_ms,
        .xfade_ms = a->xfade_ms ? a->xfade_ms : 1,
    };
    for (int i = 0; i < NUM_INPUTS; i++) {
        c.trim[i] = db_to_lin(a->input[i].trim_db);
        c.threshold[i] = db_to_lin(a->input[i].threshold_dbfs);
    }
    taskENTER_CRITICAL(&s_mux);
    s_pending = c;
    s_has_pending = true;
    taskEXIT_CRITICAL(&s_mux);
}

void mixer_fade_in(void) { s_fade_in = true; }

const char *mixer_active_token(active_source_t a)
{
    switch (a) {
    case ACTIVE_A:   return "a";
    case ACTIVE_B:   return "b";
    case ACTIVE_MIX: return "mix";
    default:         return "none";
    }
}

void mixer_get_status(mixer_status_t *out)
{
    for (int i = 0; i < NUM_INPUTS; i++) {
        out->level_dbfs[i] = s_level_dbfs[i];
        out->signal[i] = s_status_signal[i];
    }
    out->active = s_active;
}

/* Signal detection with on/off hold times, once per block */
static void detect(int i, float block_peak, uint32_t block_ms, float fall)
{
    s_peak[i] = fmaxf(block_peak, s_peak[i] * fall);

    if (s_peak[i] > s_cfg.threshold[i]) {
        s_below_ms[i] = 0;
        if (s_above_ms[i] < s_cfg.on_ms) s_above_ms[i] += block_ms;
        if (s_above_ms[i] >= s_cfg.on_ms) s_signal[i] = true;
    } else {
        s_above_ms[i] = 0;
        if (s_below_ms[i] < s_cfg.off_ms) s_below_ms[i] += block_ms;
        if (s_below_ms[i] >= s_cfg.off_ms) s_signal[i] = false;
    }

    s_level_dbfs[i] = s_peak[i] > 1e-6f ? 20.0f * log10f(s_peak[i]) : LEVEL_FLOOR_DBFS;
    s_status_signal[i] = s_signal[i];
}

/* Decides which inputs should be audible; returns the reported source */
static active_source_t select_source(float target[NUM_INPUTS])
{
    switch (s_cfg.mode) {
    case SRC_MODE_A:
        target[0] = 1; target[1] = 0;
        return ACTIVE_A;
    case SRC_MODE_B:
        target[0] = 0; target[1] = 1;
        return ACTIVE_B;
    case SRC_MODE_MIX:
        target[0] = 1; target[1] = 1;
        return ACTIVE_MIX;
    default:
        break;
    }

    /* Auto: keep the current input while it plays, no interruptions */
    if (!s_signal[s_selected]) {
        const int other = 1 - s_selected;
        if (s_signal[0] && s_signal[1]) s_selected = s_cfg.priority;
        else if (s_signal[other])       s_selected = other;
    }
    target[0] = s_selected == INPUT_A ? 1 : 0;
    target[1] = s_selected == INPUT_B ? 1 : 0;

    if (!s_signal[0] && !s_signal[1]) return ACTIVE_NONE;
    return s_selected == INPUT_A ? ACTIVE_A : ACTIVE_B;
}

void mixer_process(const float *in_a, const float *in_b, float *out, int frames)
{
    if (s_has_pending) {
        taskENTER_CRITICAL(&s_mux);
        mixer_cfg_t c = s_pending;
        s_has_pending = false;
        taskEXIT_CRITICAL(&s_mux);
        const bool mode_changed = !s_configured || c.mode != s_cfg.mode;
        s_cfg = c;
        if (!s_configured) s_selected = c.priority;
        /* Leaving a forced mode: start auto from the priority input */
        if (mode_changed && c.mode == SRC_MODE_AUTO && !s_signal[s_selected]) s_selected = c.priority;
        s_configured = true;
    }
    if (!s_configured) {
        memset(out, 0, frames * AUDIO_CHANNELS * sizeof(float));
        return;
    }
    if (s_fade_in) {
        s_fade_in = false;
        s_pos[0] = s_pos[1] = 0;
    }

    /* Metering on the trimmed inputs */
    const float *in[NUM_INPUTS] = { in_a, in_b };
    const uint32_t block_ms = (uint32_t)((frames * 1000 + AUDIO_SAMPLE_RATE / 2) / AUDIO_SAMPLE_RATE);
    const float fall = db_to_lin(-LEVEL_FALL_DB_PER_S * frames / (float)AUDIO_SAMPLE_RATE);
    for (int i = 0; i < NUM_INPUTS; i++) {
        float peak = 0;
        for (int n = 0; n < frames * AUDIO_CHANNELS; n++) peak = fmaxf(peak, fabsf(in[i][n]));
        detect(i, peak * s_cfg.trim[i], block_ms, fall);
    }

    float target[NUM_INPUTS];
    s_active = select_source(target);

    /* Raised-cosine cross-fade, one step per sample frame */
    const float step = 1000.0f / ((float)s_cfg.xfade_ms * AUDIO_SAMPLE_RATE);
    for (int f = 0; f < frames; f++) {
        float g[NUM_INPUTS];
        for (int i = 0; i < NUM_INPUTS; i++) {
            if (s_pos[i] < target[i])      s_pos[i] = fminf(target[i], s_pos[i] + step);
            else if (s_pos[i] > target[i]) s_pos[i] = fmaxf(target[i], s_pos[i] - step);
            const float shape = (s_pos[i] <= 0) ? 0 : (s_pos[i] >= 1) ? 1
                              : 0.5f - 0.5f * cosf((float)M_PI * s_pos[i]);
            g[i] = shape * s_cfg.trim[i];
        }
        for (int ch = 0; ch < AUDIO_CHANNELS; ch++) {
            const int n = f * AUDIO_CHANNELS + ch;
            out[n] = in_a[n] * g[0] + in_b[n] * g[1];
        }
    }
}
