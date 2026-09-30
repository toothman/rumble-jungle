// SPDX-License-Identifier: MIT
/*
 * Biquad filter design and processing (pure C, no ESP-IDF dependencies).
 * Formulas from Robert Bristow-Johnson's "Audio EQ Cookbook".
 */
#pragma once
#include <math.h>

typedef struct { float b0, b1, b2, a1, a2; } biquad_coef_t;
typedef struct { float z1, z2; } biquad_state_t;

typedef enum {
    BQ_PEAK,
    BQ_LOW_SHELF,
    BQ_HIGH_SHELF,
    BQ_HIGH_PASS,
} biquad_type_t;

static inline biquad_coef_t biquad_identity(void)
{
    return (biquad_coef_t){ .b0 = 1.0f };
}

static inline biquad_coef_t biquad_design(biquad_type_t type, float freq, float q,
                                          float gain_db, float fs)
{
    const float A     = powf(10.0f, gain_db / 40.0f);
    const float w0    = 2.0f * (float)M_PI * freq / fs;
    const float cw    = cosf(w0);
    const float alpha = sinf(w0) / (2.0f * q);
    const float sa    = 2.0f * sqrtf(A) * alpha;
    float b0, b1, b2, a0, a1, a2;

    switch (type) {
    case BQ_LOW_SHELF:
        b0 =        A * ((A + 1) - (A - 1) * cw + sa);
        b1 =  2.0f * A * ((A - 1) - (A + 1) * cw);
        b2 =        A * ((A + 1) - (A - 1) * cw - sa);
        a0 =             (A + 1) + (A - 1) * cw + sa;
        a1 = -2.0f *    ((A - 1) + (A + 1) * cw);
        a2 =             (A + 1) + (A - 1) * cw - sa;
        break;
    case BQ_HIGH_SHELF:
        b0 =        A * ((A + 1) + (A - 1) * cw + sa);
        b1 = -2.0f * A * ((A - 1) + (A + 1) * cw);
        b2 =        A * ((A + 1) + (A - 1) * cw - sa);
        a0 =             (A + 1) - (A - 1) * cw + sa;
        a1 =  2.0f *    ((A - 1) - (A + 1) * cw);
        a2 =             (A + 1) - (A - 1) * cw - sa;
        break;
    case BQ_HIGH_PASS:
        b0 =  (1 + cw) / 2;
        b1 = -(1 + cw);
        b2 =  (1 + cw) / 2;
        a0 =  1 + alpha;
        a1 = -2 * cw;
        a2 =  1 - alpha;
        break;
    case BQ_PEAK:
    default:
        b0 =  1 + alpha * A;
        b1 = -2 * cw;
        b2 =  1 - alpha * A;
        a0 =  1 + alpha / A;
        a1 = -2 * cw;
        a2 =  1 - alpha / A;
        break;
    }

    return (biquad_coef_t){
        .b0 = b0 / a0, .b1 = b1 / a0, .b2 = b2 / a0,
        .a1 = a1 / a0, .a2 = a2 / a0,
    };
}

/* Transposed direct form II */
static inline float biquad_run(const biquad_coef_t *c, biquad_state_t *z, float x)
{
    const float y = c->b0 * x + z->z1;
    z->z1 = c->b1 * x - c->a1 * y + z->z2;
    z->z2 = c->b2 * x - c->a2 * y;
    return y;
}
