// SPDX-License-Identifier: MIT
/*
 * Loads the user configuration (config.h) and validates it.
 * Source files include this header instead of config.h directly.
 */
#pragma once

#define NETWORK_MODE_ETHERNET   1
#define NETWORK_MODE_WIFI       2

#include "config.h"

#if !defined(NETWORK_MODE) || \
    (NETWORK_MODE != NETWORK_MODE_ETHERNET && NETWORK_MODE != NETWORK_MODE_WIFI)
#error "config.h: NETWORK_MODE must be NETWORK_MODE_ETHERNET or NETWORK_MODE_WIFI"
#endif

#if AUDIO_CHANNELS != 1 && AUDIO_CHANNELS != 2
#error "config.h: AUDIO_CHANNELS must be 1 or 2"
#endif

#if STREAM_MAX_CLIENTS < 1
#error "config.h: STREAM_MAX_CLIENTS must be at least 1"
#endif

#if EQ_BANDS < 2 || EQ_BANDS > 10
#error "config.h: EQ_BANDS must be between 2 and 10"
#endif
