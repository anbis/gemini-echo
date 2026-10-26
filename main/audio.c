// Codec (ES8311) + I2S full-duplex loop, playback ring, 24k->16k resampler, tones.
//
// I2S runs at 16 kHz stereo. On capture, ES8311 puts the mic on the left slot and
// its own DAC output on the right slot, which gives a sample-aligned echo
// reference for the AFE ("MR" format).
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "app.h"
#include "config.h"
#include "settings.h"

static const char *TAG = "audio";

// ---------------------------------------------------------------- ring buffer
struct rb {
    int16_t *buf;
    size_t cap, head, tail, count;
    SemaphoreHandle_t lock;
};

rb_t *rb_create(size_t cap)
{
    rb_t *rb = calloc(1, sizeof(rb_t));
    rb->buf = heap_caps_malloc(cap * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    rb->cap = cap;
    rb->lock = xSemaphoreCreateMutex();
    return rb;
}

size_t rb_write(rb_t *rb, const int16_t *src, size_t n)
{
    xSemaphoreTake(rb->lock, portMAX_DELAY);
    if (n > rb->cap - rb->count) n = rb->cap - rb->count;
    for (size_t done = 0; done < n;) {
        size_t chunk = rb->cap - rb->head;
        if (chunk > n - done) chunk = n - done;
        memcpy(rb->buf + rb->head, src + done, chunk * sizeof(int16_t));
        rb->head = (rb->head + chunk) % rb->cap;
        done += chunk;
    }
    rb->count += n;
    xSemaphoreGive(rb->lock);
    return n;
}

size_t rb_read(rb_t *rb, int16_t *dst, size_t n)
{
    xSemaphoreTake(rb->lock, portMAX_DELAY);
    if (n > rb->count) n = rb->count;
    for (size_t done = 0; done < n;) {
        size_t chunk = rb->cap - rb->tail;
        if (chunk > n - done) chunk = n - done;
        memcpy(dst + done, rb->buf + rb->tail, chunk * sizeof(int16_t));
        rb->tail = (rb->tail + chunk) % rb->cap;
        done += chunk;
    }
    rb->count -= n;
    xSemaphoreGive(rb->lock);
    return n;
}

size_t rb_avail(rb_t *rb)
{
    return rb->count;
}

void rb_clear(rb_t *rb)
{
    xSemaphoreTake(rb->lock, portMAX_DELAY);
    rb->head = rb->tail = rb->count = 0;
    xSemaphoreGive(rb->lock);
}

// ---------------------------------------------------------------- codec / i2s
static esp_codec_dev_handle_t s_dev;
static rb_t *s_play;   // 16 kHz mono samples waiting for the speaker
static int s_frames;
static audio_feed_cb_t s_feed_cb;

void audio_init(void)
{
    i2c_master_bus_handle_t i2c_bus;
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx, &rx));
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = PIN_I2S_DIN,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &std_cfg));

    audio_codec_i2c_cfg_t ctrl_cfg = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c_bus };
    audio_codec_i2s_cfg_t data_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&ctrl_cfg);
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&data_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = PIN_PA_EN,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .no_dac_ref = false,   // right capture slot = DAC output (AEC reference)
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (!codec_if) {
        ESP_LOGE(TAG, "ES8311 not found on I2C");
        abort();
    }
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = AUDIO_RATE,
        .mclk_multiple = 256,
    };
    ESP_ERROR_CHECK(esp_codec_dev_open(s_dev, &fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL);
    esp_codec_dev_set_out_vol(s_dev, g_cfg.volume);
    esp_codec_dev_set_in_gain(s_dev, g_cfg.mic_gain);

    s_play = rb_create(AUDIO_RATE * 90);   // up to 90 s of buffered speech
    ESP_LOGI(TAG, "codec ready");
}

void audio_set_volume(int volume)
{
    esp_codec_dev_set_out_vol(s_dev, volume);
}

void audio_set_mic_gain(int db)
{
    esp_codec_dev_set_in_gain(s_dev, db);
}

static void audio_task(void *arg)
{
    int16_t *in = heap_caps_malloc(s_frames * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    int16_t *out = heap_caps_malloc(s_frames * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    int16_t *mono = heap_caps_malloc(s_frames * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    int64_t stat_t = esp_timer_get_time();
    int64_t mic_e = 0, ref_e = 0, stat_n = 0;

    for (;;) {
        size_t got = rb_read(s_play, mono, s_frames);
        if (got) g_last_play_us = esp_timer_get_time();
        memset(mono + got, 0, (s_frames - got) * sizeof(int16_t));
        for (int i = 0; i < s_frames; i++) {
            out[2 * i] = out[2 * i + 1] = mono[i];
        }
        esp_codec_dev_write(s_dev, out, s_frames * 2 * sizeof(int16_t));
        esp_codec_dev_read(s_dev, in, s_frames * 2 * sizeof(int16_t));
        s_feed_cb(in, s_frames);

        for (int i = 0; i < s_frames; i++) {
            mic_e += abs(in[2 * i]);
            ref_e += abs(in[2 * i + 1]);
        }
        stat_n += s_frames;
        if (esp_timer_get_time() - stat_t > 10 * 1000000LL) {
            ESP_LOGI(TAG, "level mic=%lld ref=%lld", mic_e / stat_n, ref_e / stat_n);
            mic_e = ref_e = stat_n = 0;
            stat_t = esp_timer_get_time();
        }
    }
}

void audio_start(int frames_per_chunk, audio_feed_cb_t cb)
{
    s_frames = frames_per_chunk;
    s_feed_cb = cb;
    xTaskCreatePinnedToCore(audio_task, "audio", 6144, NULL, 20, NULL, 1);
}

// ---------------------------------------------------------------- resampler 24k -> 16k
// Output step is 1.5 input samples, so only two fractional phases (0 and 0.5) occur.
#define RS_H     12               // half-length of the kernel
#define RS_TAPS  (2 * RS_H)
#define RS_BLOCK 480
static float s_kern[2][RS_TAPS];
static float s_x[RS_TAPS + RS_BLOCK + 4];
static int s_xlen;
static float s_pos;
static bool s_rs_init;

static void rs_init(void)
{
    const float fc = 7200.0f / 24000.0f;   // cutoff, cycles/sample at 24 kHz
    for (int p = 0; p < 2; p++) {
        float f = p * 0.5f, sum = 0;
        for (int j = 0; j < RS_TAPS; j++) {
            float t = (j - RS_H + 1) - f;
            float s = (t == 0) ? 2 * fc : sinf(2 * M_PI * fc * t) / (M_PI * t);
            float w = 0.42f + 0.5f * cosf(M_PI * t / RS_H) + 0.08f * cosf(2 * M_PI * t / RS_H);
            s_kern[p][j] = s * w;
            sum += s_kern[p][j];
        }
        for (int j = 0; j < RS_TAPS; j++) s_kern[p][j] /= sum;
    }
    s_xlen = 0;
    s_pos = RS_H - 1;
    s_rs_init = true;
}

static volatile uint32_t s_reply_seq;

uint32_t play_reply_seq(void)
{
    return s_reply_seq;
}

void play_push_24k(const int16_t *pcm, int n)
{
    static int16_t outbuf[RS_BLOCK];   // only called from the websocket task
    s_reply_seq++;   // only Gemini's voice comes in at 24 kHz
    if (!s_rs_init) rs_init();
    while (n > 0) {
        int take = n > RS_BLOCK ? RS_BLOCK : n;
        for (int i = 0; i < take; i++) s_x[s_xlen++] = pcm[i];
        pcm += take;
        n -= take;
        int nout = 0;
        while ((int)s_pos + RS_H < s_xlen) {
            int i0 = (int)s_pos;
            const float *k = s_kern[(s_pos - i0) > 0.25f ? 1 : 0];
            const float *x = s_x + i0 - RS_H + 1;
            float acc = 0;
            for (int j = 0; j < RS_TAPS; j++) acc += x[j] * k[j];
            if (acc > 32767) acc = 32767;
            if (acc < -32768) acc = -32768;
            outbuf[nout++] = (int16_t)acc;
            s_pos += 1.5f;
        }
        int shift = (int)s_pos - RS_H + 1;
        if (shift > 0) {
            memmove(s_x, s_x + shift, (s_xlen - shift) * sizeof(float));
            s_xlen -= shift;
            s_pos -= shift;
        }
        play_push_16k(outbuf, nout);
    }
}

void play_push_16k(const int16_t *pcm, int n)
{
    if (rb_write(s_play, pcm, n) < (size_t)n) ESP_LOGW(TAG, "playback buffer full");
}

void play_clear(void)
{
    rb_clear(s_play);
    s_rs_init = false;
}

size_t play_pending(void)
{
    return rb_avail(s_play);
}

// ---------------------------------------------------------------- spoken warnings
// 16 kHz mono PCM recorded once in Gemini's voice (main/clips/*.pcm)
extern const uint8_t quota_pcm_start[] asm("_binary_quota_pcm_start");
extern const uint8_t quota_pcm_end[] asm("_binary_quota_pcm_end");
extern const uint8_t apikey_pcm_start[] asm("_binary_apikey_pcm_start");
extern const uint8_t apikey_pcm_end[] asm("_binary_apikey_pcm_end");
extern const uint8_t wifi_pcm_start[] asm("_binary_wifi_pcm_start");
extern const uint8_t wifi_pcm_end[] asm("_binary_wifi_pcm_end");
extern const uint8_t connect_pcm_start[] asm("_binary_connect_pcm_start");
extern const uint8_t connect_pcm_end[] asm("_binary_connect_pcm_end");

void play_clip(clip_t c)
{
    static const struct { const uint8_t *start, *end; } clips[] = {
        [CLIP_QUOTA] = { quota_pcm_start, quota_pcm_end },
        [CLIP_APIKEY] = { apikey_pcm_start, apikey_pcm_end },
        [CLIP_WIFI] = { wifi_pcm_start, wifi_pcm_end },
        [CLIP_CONNECT] = { connect_pcm_start, connect_pcm_end },
    };
    play_push_16k((const int16_t *)clips[c].start, (clips[c].end - clips[c].start) / 2);
}

// ---------------------------------------------------------------- tones
static void tone(float f, int ms, float amp)
{
    int n = AUDIO_RATE * ms / 1000;
    int16_t buf[160];
    for (int i = 0; i < n; i += 160) {
        int m = n - i < 160 ? n - i : 160;
        for (int j = 0; j < m; j++) {
            int k = i + j;
            float env = 1.0f;
            int fade = AUDIO_RATE / 200;   // 5 ms fade in/out
            if (k < fade) env = (float)k / fade;
            if (n - k < fade) env = (float)(n - k) / fade;
            buf[j] = f > 0 ? (int16_t)(amp * env * 32767 * sinf(2 * M_PI * f * k / AUDIO_RATE)) : 0;
        }
        play_push_16k(buf, m);
    }
}

void play_tone(tone_t t)
{
    switch (t) {
    case TONE_WAKE:  tone(880, 70, 0.35f); tone(0, 30, 0); tone(1320, 90, 0.35f); break;
    case TONE_END:   tone(1320, 70, 0.3f); tone(0, 30, 0); tone(660, 110, 0.3f); break;
    case TONE_ERROR: tone(300, 150, 0.4f); tone(0, 60, 0); tone(300, 150, 0.4f); break;
    case TONE_BOOT:  tone(660, 60, 0.3f); tone(880, 60, 0.3f); tone(1100, 80, 0.3f); break;
    case TONE_LISTEN: tone(990, 60, 0.25f); break;
    }
}
