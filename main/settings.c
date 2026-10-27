// Runtime settings: NVS blob, factory defaults from config.h.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "esp_log.h"
#include "settings.h"
#include "config.h"

static const char *TAG = "settings";
#define SETTINGS_VERSION 1

settings_t g_cfg;
static SemaphoreHandle_t s_lock;

typedef struct {
    int version;
    settings_t cfg;
} stored_t;

static void set_defaults(settings_t *c)
{
    memset(c, 0, sizeof(*c));
    strlcpy(c->wifi_ssid, WIFI_SSID, sizeof(c->wifi_ssid));
    strlcpy(c->wifi_pass, WIFI_PASS, sizeof(c->wifi_pass));
    strlcpy(c->api_key, GEMINI_API_KEY, sizeof(c->api_key));
    strlcpy(c->model, GEMINI_MODEL, sizeof(c->model));
    strlcpy(c->voice, GEMINI_VOICE, sizeof(c->voice));
    strlcpy(c->language, GEMINI_LANGUAGE, sizeof(c->language));
    strlcpy(c->prompt, GEMINI_SYSTEM_PROMPT, sizeof(c->prompt));
    strlcpy(c->wake1, WAKE_MODEL_1, sizeof(c->wake1));
    strlcpy(c->wake2, WAKE_MODEL_2, sizeof(c->wake2));
    c->volume = SPEAKER_VOLUME;
    c->mic_gain = (int)MIC_GAIN_DB;
    c->idle_timeout = SESSION_IDLE_TIMEOUT_S;
    c->echo_tail_ms = ECHO_TAIL_MS;
    c->search = GEMINI_ENABLE_SEARCH;
    strlcpy(c->ha_url, HA_URL, sizeof(c->ha_url));
    c->listen_beep = LISTEN_BEEP;
    c->memory = MEMORY_ENABLED;
    strlcpy(c->memory_model, MEMORY_MODEL, sizeof(c->memory_model));
}

void settings_load(void)
{
    s_lock = xSemaphoreCreateMutex();
    set_defaults(&g_cfg);
    nvs_handle_t h;
    if (nvs_open("gemini", NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved settings, using defaults");
        return;
    }
    static stored_t st;
    size_t len = sizeof(st);
    // Fields are only ever appended, so an older (shorter) blob is a valid prefix over the defaults
    static stored_t def;
    def.version = SETTINGS_VERSION;
    def.cfg = g_cfg;
    st = def;
    if (nvs_get_blob(h, "cfg", &st, &len) == ESP_OK && len <= sizeof(st) && st.version == SETTINGS_VERSION) {
        if (len < sizeof(st)) memcpy((char *)&st + len, (char *)&def + len, sizeof(st) - len);
        g_cfg = st.cfg;
        ESP_LOGI(TAG, "loaded saved settings");
    } else {
        ESP_LOGW(TAG, "saved settings missing or outdated, using defaults");
    }
    nvs_close(h);
}

void settings_save(void)
{
    static stored_t st;
    settings_lock();
    st.version = SETTINGS_VERSION;
    st.cfg = g_cfg;
    settings_unlock();
    nvs_handle_t h;
    if (nvs_open("gemini", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "cfg", &st, sizeof(st));
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "settings saved");
    }
}

void settings_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void settings_unlock(void)
{
    xSemaphoreGive(s_lock);
}
