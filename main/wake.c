// Audio front-end (AEC + VAD + WakeNet) and wake word handling.
// Uses ESP-SR's pre-trained WakeNet9 models bundled via sdkconfig; up to two are active (settings).
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_afe_sr_models.h"
#include "model_path.h"
#include "app.h"
#include "config.h"
#include "settings.h"

static const char *TAG = "wake";

static esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static volatile bool s_rearm;
static volatile int64_t s_quiet_until;
static srmodel_list_t *s_models;
static uint32_t s_heard_seq;   // Gemini audio up to this play_reply_seq() no longer locks the mic

int sr_wake_models(const char **names, const char **words, int max)
{
    int n = 0;
    for (int i = 0; s_models && i < s_models->num && n < max; i++) {
        if (strncmp(s_models->model_name[i], ESP_WN_PREFIX, strlen(ESP_WN_PREFIX)) != 0) continue;
        names[n] = s_models->model_name[i];
        words[n] = esp_srmodel_get_wake_words(s_models, s_models->model_name[i]);
        n++;
    }
    return n;
}

int sr_init(void)
{
    srmodel_list_t *models = esp_srmodel_init("model");
    s_models = models;
    afe_config_t *cfg = afe_config_init("MR", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    // Wake words from settings; fall back to the AFE's pick if a saved model isn't bundled
    static char wake1[32], wake2[32];
    strlcpy(wake1, g_cfg.wake1, sizeof(wake1));
    strlcpy(wake2, g_cfg.wake2, sizeof(wake2));
    if (wake1[0] && esp_srmodel_exists(models, wake1) >= 0) cfg->wakenet_model_name = wake1;
    if (wake2[0] && strcmp(wake1, wake2) && esp_srmodel_exists(models, wake2) >= 0)
        cfg->wakenet_model_name_2 = wake2;
    else
        cfg->wakenet_model_name_2 = NULL;
    ESP_LOGI(TAG, "wake words: %s + %s", cfg->wakenet_model_name ? cfg->wakenet_model_name : "-",
             cfg->wakenet_model_name_2 ? cfg->wakenet_model_name_2 : "-");
    cfg->aec_init = true;
    cfg->vad_init = true;
    cfg->vad_min_noise_ms = 600;
    cfg->wakenet_mode = DET_MODE_95;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->afe_perferred_core = 1;
    cfg->afe_perferred_priority = 18;
    cfg->afe_ringbuf_size = 50;
    cfg->agc_init = true;
    cfg = afe_config_check(cfg);
    afe_config_print(cfg);
    s_afe = esp_afe_handle_from_config(cfg);
    s_afe_data = s_afe->create_from_config(cfg);

    int feed = s_afe->get_feed_chunksize(s_afe_data);
    ESP_LOGI(TAG, "afe feed=%d fetch=%d", feed, s_afe->get_fetch_chunksize(s_afe_data));
    return feed;
}

void sr_feed(const int16_t *stereo, int frames)
{
    s_afe->feed(s_afe_data, stereo);
}

void sr_rearm(void)
{
    s_rearm = true;
}

static void fetch_task(void *arg)
{
    app_state_t prev = APP_IDLE;

    for (;;) {
        afe_fetch_result_t *res = s_afe->fetch(s_afe_data);
        if (!res || res->ret_value == ESP_FAIL) continue;
        int64_t now = esp_timer_get_time();
        app_state_t st = g_state;

        if (res->vad_state == VAD_SPEECH) g_last_voice_us = now;

        if (st != prev) {
            if (prev == APP_IDLE) {
                s_afe->disable_wakenet(s_afe_data);
                // Push the VAD cache so words spoken right after the wake word aren't lost
                if (res->vad_cache_size > 0)
                    gemini_uplink_push(res->vad_cache, res->vad_cache_size / sizeof(int16_t));
                s_heard_seq = play_reply_seq();
            }
            prev = st;
        }

        if (st != APP_IDLE) {
            int n = res->data_size / sizeof(int16_t);
            static bool locked;
            // Only Gemini's voice locks the mic; the wake beep (while connecting) and the listen
            // bip must not, or the first words after them get lost.
            uint32_t seq = play_reply_seq();
            bool lock = st == APP_ACTIVE && seq != s_heard_seq &&
                        (play_pending() > 0 || now - g_last_play_us < g_cfg.echo_tail_ms * 1000);
            if (lock != locked) {
                ESP_LOGI(TAG, "mic %s", lock ? "locked (speaker active)" : "open");
                locked = lock;
                if (!lock && st == APP_ACTIVE) {
                    s_heard_seq = seq;
                    if (g_cfg.listen_beep) play_tone(TONE_LISTEN);   // your turn
                }
            }
            if (lock) {
                // Gemini is talking: lock the mic so it can't hear itself
                static int16_t zeros[1024];
                gemini_uplink_push(zeros, n);
                continue;
            }
            gemini_uplink_push(res->data, n);
            continue;
        }

        if (s_rearm) {
            s_afe->enable_wakenet(s_afe_data);
            s_rearm = false;
            s_quiet_until = now + 800 * 1000;   // ignore the tail of our own end tone
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "wake word #%d (model %d), volume %.1f dB", res->wake_word_index,
                     res->wakenet_model_index, res->data_volume);
            if (now > s_quiet_until) app_post(EV_WAKE);
        }
    }
}

void sr_start(void)
{
    xTaskCreatePinnedToCore(fetch_task, "sr_fetch", 8192, NULL, 15, NULL, 0);
}
