// Atom EchoS3R standalone Gemini Live voice assistant.
// Idle: on-device wake word (ESP-SR WakeNet). Active: live native-audio conversation.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "app.h"
#include "config.h"
#include "settings.h"
#include "mdns.h"
#include "esp_netif_sntp.h"
#include <time.h>
#include "cJSON.h"

static const char *TAG = "main";

volatile app_state_t g_state = APP_IDLE;
volatile int64_t g_last_voice_us;
volatile int64_t g_last_play_us;
volatile int64_t g_last_server_us;

static QueueHandle_t s_events;
static EventGroupHandle_t s_wifi_ev;
#define WIFI_UP BIT0
static volatile bool s_ap_mode;   // setup hotspot active

bool app_wifi_is_ap(void)
{
    return s_ap_mode;
}

void app_post(app_event_t ev)
{
    xQueueSend(s_events, &ev, 0);
}

// ---------------------------------------------------------------- wifi
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_ev, WIFI_UP);
        if (s_ap_mode) return;   // stop channel-hopping so the setup hotspot stays usable
        ESP_LOGW(TAG, "wifi disconnected, retrying");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "wifi up, ip " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_ev, WIFI_UP);
    }
}

static void wifi_init(void)
{
    s_wifi_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(sta, HOSTNAME);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, g_cfg.wifi_ssid, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, g_cfg.wifi_pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = g_cfg.wifi_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
}

static void start_setup_ap(void)
{
    ESP_LOGW(TAG, "can't join \"%s\" - starting hotspot \"%s\" (http://192.168.4.1)", g_cfg.wifi_ssid, SETUP_AP_SSID);
    s_ap_mode = true;
    esp_wifi_disconnect();
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, SETUP_AP_SSID, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, SETUP_AP_PASS, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(SETUP_AP_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
}

// Today's date (UTC) once SNTP has set the clock; false before that.
bool app_today(char *buf, size_t size)
{
    time_t now = time(NULL);
    struct tm tm;
    if (now < 1735689600) return false;   // 2025-01-01: clock not set yet
    gmtime_r(&now, &tm);
    strftime(buf, size, "%Y-%m-%d, %A", &tm);
    return true;
}

static void mdns_start(void)
{
    if (mdns_init() != ESP_OK) return;
    mdns_hostname_set(HOSTNAME);
    mdns_instance_name_set("Gemini Echo");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

const char *app_state_name(void)
{
    static const char *names[] = { "idle", "connecting", "active" };
    return names[g_state];
}

// ---------------------------------------------------------------- button
static void button_task(void *arg)
{
    gpio_config_t io = { .pin_bit_mask = 1ULL << PIN_BUTTON, .mode = GPIO_MODE_INPUT, .pull_up_en = 1 };
    gpio_config(&io);
    int last = 1;
    for (;;) {
        int v = gpio_get_level(PIN_BUTTON);
        if (v == 0 && last == 1) {
            ESP_LOGI(TAG, "button pressed");
            app_post(EV_BUTTON);
        }
        last = v;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ---------------------------------------------------------------- session control
static int64_t s_state_since;

static void set_state(app_state_t st)
{
    static const char *names[] = { "IDLE", "CONNECTING", "ACTIVE" };
    ESP_LOGI(TAG, "state %s -> %s (free psram %u, internal %u)", names[g_state], names[st],
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    g_state = st;
    s_state_since = esp_timer_get_time();
}

static void begin_session(void)
{
    if (!(xEventGroupGetBits(s_wifi_ev) & WIFI_UP)) {
        ESP_LOGW(TAG, "no wifi");
        play_tone(TONE_ERROR);
        play_clip(CLIP_WIFI);
        return;
    }
    set_state(APP_CONNECTING);   // the fetch task starts buffering mic audio now
    play_tone(TONE_WAKE);
    if (!gemini_start()) {
        play_tone(TONE_ERROR);
        play_clip(CLIP_CONNECT);
        set_state(APP_IDLE);
        sr_rearm();
    }
}

static void end_session(bool error)
{
    gemini_stop();
    memory_session_end();
    play_clear();
    gemini_err_t kind = gemini_error_kind();
    if (!error && kind == ERR_NONE) {
        play_tone(TONE_END);
    } else {
        // Say what went wrong, not just beep
        static const clip_t clip_for[] = { [ERR_NONE] = CLIP_CONNECT, [ERR_QUOTA] = CLIP_QUOTA,
                                           [ERR_APIKEY] = CLIP_APIKEY, [ERR_CONNECT] = CLIP_CONNECT };
        ESP_LOGW(TAG, "session error: %s", gemini_last_error());
        play_tone(TONE_ERROR);
        play_clip(clip_for[kind]);
    }
    set_state(APP_IDLE);
    sr_rearm();
}

// After an OTA update the new image runs in "pending verify" until it confirms itself;
// a crash or reset before that makes the bootloader go back to the previous image.
static void confirm_firmware(void)
{
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed");
    }
}

static void audio_feed(const int16_t *stereo, int frames)
{
    sr_feed(stereo, frames);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    s_events = xQueueCreate(8, sizeof(app_event_t));
    settings_load();

    audio_init();
    int chunk = sr_init();
    gemini_init();
    ha_init();
    memory_init();
    audio_start(chunk, audio_feed);
    sr_start();
    wifi_init();
    xTaskCreate(button_task, "button", 2048, NULL, 5, NULL);

    mdns_start();
    web_start();

    if (xEventGroupWaitBits(s_wifi_ev, WIFI_UP, false, true, pdMS_TO_TICKS(20000)) & WIFI_UP) {
        play_tone(TONE_BOOT);
        esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&sntp);   // dates for the conversation memory
        cJSON_Delete(ha_function_declarations());   // warm the HA tool cache
        ESP_LOGI(TAG, "ready - say the wake word or press the button; settings at http://" HOSTNAME ".local");
    } else {
        start_setup_ap();
        play_tone(TONE_ERROR);
    }
    confirm_firmware();   // the web UI is up, so a further update is always possible

    for (;;) {
        app_event_t ev;
        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(200))) {
            if (web_ota_active() && g_state == APP_IDLE) continue;   // no conversations while flashing
            switch (ev) {
            case EV_WAKE:
                if (g_state == APP_IDLE) begin_session();
                break;
            case EV_BUTTON:
                if (g_state == APP_IDLE) {
                    begin_session();
                } else if (g_state == APP_ACTIVE && play_pending() > 0) {
                    ESP_LOGI(TAG, "button: cut reply, listening");
                    gemini_cancel_turn();
                } else {
                    end_session(false);
                }
                break;
            case EV_SESSION_READY:
                if (g_state == APP_CONNECTING) set_state(APP_ACTIVE);
                break;
            case EV_SESSION_CLOSED:
                if (g_state != APP_IDLE) {
                    ESP_LOGW(TAG, "session closed by server/network");
                    end_session(g_state == APP_CONNECTING || gemini_error_kind() != ERR_NONE);
                }
                break;
            }
        }

        int64_t now = esp_timer_get_time();
        if (g_state == APP_CONNECTING && now - s_state_since > SESSION_CONNECT_TIMEOUT_S * 1000000LL) {
            ESP_LOGW(TAG, "connect timeout");
            end_session(true);
        } else if (g_state == APP_ACTIVE) {
            int64_t last = s_state_since;
            if (g_last_voice_us > last) last = g_last_voice_us;
            if (g_last_play_us > last) last = g_last_play_us;
            if (now - last > g_cfg.idle_timeout * 1000000LL) {
                ESP_LOGI(TAG, "conversation idle, closing session");
                end_session(false);
            }
        }
    }
}
