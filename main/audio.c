// SPDX-License-Identifier: MIT
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "driver/i2s_std.h"
#include "soc/soc_caps.h"
#include "esp_log.h"
#include "esp_audio_enc.h"
#include "esp_audio_enc_default.h"
#include "esp_aac_enc.h"
#include "app_config.h"
#include "dsp.h"
#include "audio.h"

#define LEVEL_WINDOW_FRAMES 10      /* ~10 x 23 ms per level update      */
#define SOFT_CLIP_KNEE      0.891f  /* -1 dBFS, soft limiting above this */

static const char *TAG = "audio";
static i2s_chan_handle_t s_rx;
static RingbufHandle_t s_out_rb;
static volatile float s_level_dbfs = -120.0f;
static volatile bool s_clipped;

RingbufHandle_t audio_get_output(void) { return s_out_rb; }
float audio_get_level_dbfs(void) { return s_level_dbfs; }

bool audio_take_clip_flag(void)
{
    bool v = s_clipped;
    s_clipped = false;
    return v;
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

static void i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = I2S_DMA_FRAME_NUM;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx));

    /* PCM1808: 24 bit in a 32 bit slot, I2S format, MCLK = 256 fs, BCK = 64 fs */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT,
                        AUDIO_CHANNELS == 2 ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = PIN_I2S_DIN,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
#if SOC_I2S_SUPPORTS_APLL
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;   /* cleaner audio clocks */
#endif
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));
}

static esp_audio_enc_handle_t encoder_open(void)
{
    esp_audio_enc_register_default();

    esp_aac_enc_config_t aac_cfg = ESP_AAC_ENC_CONFIG_DEFAULT();
    aac_cfg.sample_rate     = AUDIO_SAMPLE_RATE;
    aac_cfg.channel         = AUDIO_CHANNELS;
    aac_cfg.bits_per_sample = 16;
    aac_cfg.bitrate         = AAC_BITRATE;
    aac_cfg.adts_used       = true;   /* ADTS headers make the output streamable */

    esp_audio_enc_config_t enc_cfg = {
        .type   = ESP_AUDIO_TYPE_AAC,
        .cfg    = &aac_cfg,
        .cfg_sz = sizeof(aac_cfg),
    };
    esp_audio_enc_handle_t enc = NULL;
    if (esp_audio_enc_open(&enc_cfg, &enc) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open AAC encoder");
        return NULL;
    }
    return enc;
}

static void audio_task(void *arg)
{
    esp_audio_enc_handle_t enc = encoder_open();
    if (!enc) vTaskDelete(NULL);

    int in_size = 0, out_size = 0;
    esp_audio_enc_get_frame_size(enc, &in_size, &out_size);
    const int samples = in_size / 2;   /* interleaved 16 bit samples per frame */
    ESP_LOGI(TAG, "AAC frame: %d bytes PCM in, up to %d bytes out", in_size, out_size);

    int32_t *raw = malloc(samples * sizeof(int32_t));
    float   *flt = malloc(samples * sizeof(float));
    int16_t *pcm = malloc(in_size);
    uint8_t *out = malloc(out_size);
    assert(raw && flt && pcm && out);

    int32_t window_peak = 0;
    int window_frames = 0;

    while (true) {
        /* Read exactly one encoder frame from I2S */
        size_t need = samples * sizeof(int32_t), off = 0, got = 0;
        while (off < need) {
            i2s_channel_read(s_rx, (uint8_t *)raw + off, need - off, &got, portMAX_DELAY);
            off += got;
        }

        /* 24 bit (left-aligned in 32) -> float, tracking the input peak */
        for (int i = 0; i < samples; i++) {
            int32_t s24 = raw[i] >> 8;
            int32_t a = s24 < 0 ? -s24 : s24;
            if (a > window_peak) window_peak = a;
            flt[i] = s24 / 8388608.0f;
        }

        /* Rumble filter, EQ, gain */
        dsp_process(flt, samples / AUDIO_CHANNELS);

        bool clipped = false;
        for (int i = 0; i < samples; i++) pcm[i] = to_s16(flt[i], &clipped);
        if (clipped) s_clipped = true;
        if (++window_frames >= LEVEL_WINDOW_FRAMES) {
            s_level_dbfs = 20.0f * log10f((window_peak > 0 ? window_peak : 1) / 8388608.0f);
            window_peak = 0;
            window_frames = 0;
        }

        esp_audio_enc_in_frame_t  in_f  = { .buffer = (uint8_t *)pcm, .len = in_size };
        esp_audio_enc_out_frame_t out_f = { .buffer = out, .len = out_size };
        if (esp_audio_enc_process(enc, &in_f, &out_f) == ESP_AUDIO_ERR_OK &&
            out_f.encoded_bytes > 0) {
            /* If the network stalls, drop the frame instead of blocking I2S */
            if (xRingbufferSend(s_out_rb, out, out_f.encoded_bytes, 0) != pdTRUE) {
                ESP_LOGW(TAG, "Output buffer full, frame dropped");
            }
        }
    }
}

void audio_start(void)
{
    s_out_rb = xRingbufferCreate(OUTPUT_BUFFER_SIZE, RINGBUF_TYPE_NOSPLIT);
    assert(s_out_rb);
    i2s_init();
    xTaskCreatePinnedToCore(audio_task, "audio", 16384, NULL,
                            AUDIO_TASK_PRIORITY, NULL, AUDIO_TASK_CORE);
}
