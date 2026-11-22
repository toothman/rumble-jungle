// SPDX-License-Identifier: MIT
/*
 * Audio pipeline (audio task, core 1):
 *
 *   ADC A (I2S0, master) --+
 *                          +-> mixer -> rumble/EQ/gain -> limiter -> AAC -> stream server
 *   ADC B (I2S1, slave) ---+
 *
 * I2S0 generates MCLK, BCK and LRC for both ADCs. I2S1 runs as slave on the
 * same BCK/LRC pins, so both inputs are sampled by the same clock edges.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "soc/soc_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_audio_enc.h"
#include "esp_audio_enc_default.h"
#include "esp_aac_enc.h"
#include "app_config.h"
#include "settings.h"
#include "dsp.h"
#include "mixer.h"
#include "stream_server.h"
#include "audio.h"

#define SOFT_CLIP_KNEE   0.891f  /* -1 dBFS, soft limiting above this */
#define CLIP_HOLD_US     (2000 * 1000)

static const char *TAG = "audio";
static i2s_chan_handle_t s_rx[NUM_INPUTS];
static volatile bool s_resync;
static volatile int64_t s_clip_until;
static volatile uint32_t s_bitrate_req;   /* requested by settings, 0 = none */
static volatile uint32_t s_bitrate;
static volatile bool s_running;

void audio_get_status(audio_status_t *out)
{
    mixer_get_status(&out->mix);
    out->clipping = esp_timer_get_time() < s_clip_until;
    out->bitrate = s_bitrate;
    out->running = s_running;
}

/* Float -> 16 bit with a soft knee instead of hard clipping */
static inline int16_t to_s16(float x, bool *clipped)
{
    float a = fabsf(x);
    if (a > SOFT_CLIP_KNEE) {
        *clipped = true;
        float over = (a - SOFT_CLIP_KNEE) / (1.0f - SOFT_CLIP_KNEE);
        a = SOFT_CLIP_KNEE + (1.0f - SOFT_CLIP_KNEE) * tanhf(over);
        x = copysignf(a, x);
    }
    int32_t v = lrintf(x * 32767.0f);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

/* ------------------------------------------------------------------------ */
/* I2S                                                                      */
/* ------------------------------------------------------------------------ */

static bool IRAM_ATTR on_overflow(i2s_chan_handle_t h, i2s_event_data_t *ev, void *ctx)
{
    s_resync = true;   /* the two DMA streams may no longer line up */
    return false;
}

static void i2s_init(void)
{
    /* PCM1808: 24 bit in a 32 bit slot, I2S format, MCLK = 256 fs, BCK = 64 fs */
    i2s_chan_config_t ca = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ca.dma_desc_num  = I2S_DMA_DESC_NUM;
    ca.dma_frame_num = I2S_DMA_FRAME_NUM;
    ESP_ERROR_CHECK(i2s_new_channel(&ca, NULL, &s_rx[0]));

    i2s_std_config_t sa = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = PIN_I2S_DIN_A,
        },
    };
    sa.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
#if SOC_I2S_SUPPORTS_APLL
    sa.clk_cfg.clk_src = I2S_CLK_SRC_APLL;   /* cleaner audio clocks */
#endif
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx[0], &sa));

    /* Input B: slave, clocks sampled from the master's pins (initialised
     * after A, which keeps the pins driven as outputs).                   */
    i2s_chan_config_t cb = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_SLAVE);
    cb.dma_desc_num  = I2S_DMA_DESC_NUM;
    cb.dma_frame_num = I2S_DMA_FRAME_NUM;
    ESP_ERROR_CHECK(i2s_new_channel(&cb, NULL, &s_rx[1]));

    i2s_std_config_t sb = sa;
    sb.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    sb.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    sb.gpio_cfg.bclk = PIN_I2S_B_BCLK;
    sb.gpio_cfg.ws   = PIN_I2S_B_WS;
    sb.gpio_cfg.din  = PIN_I2S_DIN_B;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx[1], &sb));

    const i2s_event_callbacks_t cbs = { .on_recv_q_ovf = on_overflow };
    for (int i = 0; i < NUM_INPUTS; i++) {
        ESP_ERROR_CHECK(i2s_channel_register_event_callback(s_rx[i], &cbs, NULL));
    }

    /* Slave first, so it is ready when the master starts the clocks */
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx[1]));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx[0]));
}

static void i2s_resync(void)
{
    ESP_LOGW(TAG, "I2S overflow, resynchronising inputs");
    i2s_channel_disable(s_rx[0]);
    i2s_channel_disable(s_rx[1]);
    i2s_channel_enable(s_rx[1]);
    i2s_channel_enable(s_rx[0]);
    mixer_fade_in();
}

static void read_exact(i2s_chan_handle_t ch, void *buf, size_t len)
{
    size_t off = 0, got = 0;
    while (off < len) {
        if (i2s_channel_read(ch, (uint8_t *)buf + off, len - off, &got, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        off += got;
    }
}

/* ------------------------------------------------------------------------ */
/* Encoder                                                                  */
/* ------------------------------------------------------------------------ */

static esp_audio_enc_handle_t encoder_open(uint32_t bitrate)
{
    esp_aac_enc_config_t aac_cfg = ESP_AAC_ENC_CONFIG_DEFAULT();
    aac_cfg.sample_rate     = AUDIO_SAMPLE_RATE;
    aac_cfg.channel         = AUDIO_CHANNELS;
    aac_cfg.bits_per_sample = 16;
    aac_cfg.bitrate         = bitrate;
    aac_cfg.adts_used       = true;   /* ADTS headers make the output streamable */

    esp_audio_enc_config_t enc_cfg = {
        .type   = ESP_AUDIO_TYPE_AAC,
        .cfg    = &aac_cfg,
        .cfg_sz = sizeof(aac_cfg),
    };
    esp_audio_enc_handle_t enc = NULL;
    if (esp_audio_enc_open(&enc_cfg, &enc) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open AAC encoder at %u bit/s", (unsigned)bitrate);
        return NULL;
    }
    ESP_LOGI(TAG, "AAC encoder at %u bit/s", (unsigned)bitrate);
    return enc;
}

/* Switches the bitrate between frames. ADTS frames describe themselves,
 * so connected players simply keep playing.                              */
static void encoder_switch(esp_audio_enc_handle_t *enc, uint32_t bitrate)
{
    esp_audio_enc_handle_t next = encoder_open(bitrate);
    if (!next) return;   /* keep the old encoder */
    esp_audio_enc_close(*enc);
    *enc = next;
    s_bitrate = bitrate;
}

/* ------------------------------------------------------------------------ */
/* Audio task                                                               */
/* ------------------------------------------------------------------------ */

static void *alloc_internal(size_t len)
{
    void *p = heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(p);
    return p;
}

static void audio_task(void *arg)
{
    esp_audio_enc_register_default();
    settings_t s;
    settings_get(&s);
    s_bitrate = settings_bitrate_bps(s.stream.bitrate);
    esp_audio_enc_handle_t enc = encoder_open(s_bitrate);
    if (!enc) {
        s_bitrate = DEFAULT_AAC_BITRATE;
        enc = encoder_open(s_bitrate);
    }
    assert(enc);

    int in_size = 0, out_size = 0;
    esp_audio_enc_get_frame_size(enc, &in_size, &out_size);
    const int samples = in_size / 2;            /* interleaved 16 bit samples per frame */
    const int frames  = samples / AUDIO_CHANNELS;
    const size_t raw_len = samples * sizeof(int32_t);
    ESP_LOGI(TAG, "AAC frame: %d samples/channel", frames);

    int32_t *raw[NUM_INPUTS];
    float   *in[NUM_INPUTS];
    for (int i = 0; i < NUM_INPUTS; i++) {
        raw[i] = alloc_internal(raw_len);
        in[i]  = alloc_internal(samples * sizeof(float));
    }
    float   *mix = alloc_internal(samples * sizeof(float));
    int16_t *pcm = alloc_internal(in_size);
    uint8_t *out = alloc_internal(out_size * 2);  /* headroom for a larger bitrate */
    int out_cap = out_size * 2;

    i2s_init();

    while (true) {
        if (s_resync) {
            s_resync = false;
            i2s_resync();
        }
        const uint32_t req = s_bitrate_req;
        if (req) {
            s_bitrate_req = 0;
            if (req != s_bitrate) encoder_switch(&enc, req);
        }

        /* Same number of frames from both inputs keeps them aligned */
        for (int i = 0; i < NUM_INPUTS; i++) {
            read_exact(s_rx[i], raw[i], raw_len);
            /* 24 bit (left-aligned in 32) -> float */
            for (int n = 0; n < samples; n++) in[i][n] = (raw[i][n] >> 8) * (1.0f / 8388608.0f);
        }

        mixer_process(in[0], in[1], mix, frames);
        dsp_process(mix, frames);

        bool clipped = false;
        for (int n = 0; n < samples; n++) pcm[n] = to_s16(mix[n], &clipped);
        if (clipped) s_clip_until = esp_timer_get_time() + CLIP_HOLD_US;

        esp_audio_enc_in_frame_t  in_f  = { .buffer = (uint8_t *)pcm, .len = in_size };
        esp_audio_enc_out_frame_t out_f = { .buffer = out, .len = out_cap };
        if (esp_audio_enc_process(enc, &in_f, &out_f) == ESP_AUDIO_ERR_OK &&
            out_f.encoded_bytes > 0) {
            stream_server_broadcast(out, out_f.encoded_bytes);
            s_running = true;
        }
    }
}

static void on_settings(uint32_t groups, settings_source_t src, void *ctx)
{
    settings_t s;
    settings_get(&s);
    if (groups & SG_AUDIO) {
        dsp_configure(&s.audio.dsp);
        mixer_configure(&s.audio);
    }
    if (groups & SG_STREAM) s_bitrate_req = settings_bitrate_bps(s.stream.bitrate);
}

void audio_start(void)
{
    settings_t s;
    settings_get(&s);
    dsp_init(&s.audio.dsp);
    mixer_configure(&s.audio);
    settings_add_listener(on_settings, NULL);

    xTaskCreatePinnedToCore(audio_task, "audio", 16384, NULL,
                            AUDIO_TASK_PRIORITY, NULL, AUDIO_TASK_CORE);
}
