// SPDX-License-Identifier: MIT
/*
 * Loads the build-time configuration (config.h), validates it and defines
 * derived constants. Source files include this header instead of config.h.
 */
#pragma once

#include "config.h"

#define NUM_INPUTS      2
#define AUDIO_CHANNELS  2       /* PCM1808 is always stereo */

#if AUDIO_SAMPLE_RATE != 44100 && AUDIO_SAMPLE_RATE != 48000
#error "config.h: AUDIO_SAMPLE_RATE must be 44100 or 48000"
#endif

#if STREAM_MAX_CLIENTS < 1 || STREAM_MAX_CLIENTS > 8
#error "config.h: STREAM_MAX_CLIENTS must be between 1 and 8"
#endif

#if EQ_BANDS < 2 || EQ_BANDS > 10
#error "config.h: EQ_BANDS must be between 2 and 10"
#endif

#if STREAM_PREBUFFER_BYTES >= STREAM_CLIENT_FIFO
#error "config.h: STREAM_PREBUFFER_BYTES must be smaller than STREAM_CLIENT_FIFO"
#endif
