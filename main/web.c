// Configuration web UI + JSON API (http://gemini-echo.local, or 192.168.4.1 in setup mode).
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "cJSON.h"
#include "app.h"
#include "config.h"
#include "settings.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

// ---------------------------------------------------------------- helpers
static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_sendstr(req, s);
    free(s);
    cJSON_Delete(root);
    return r;
}

static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 8192) return NULL;
    char *buf = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM);
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[got] = 0;
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    return root;
}

static bool take_str(cJSON *root, const char *key, char *dst, size_t size)
{
    cJSON *v = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsString(v)) return false;
    if (strcmp(dst, v->valuestring) == 0) return false;
    strlcpy(dst, v->valuestring, size);
    return true;
}

static bool take_int(cJSON *root, const char *key, int *dst, int lo, int hi)
{
    cJSON *v = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsNumber(v)) return false;
    int x = v->valueint < lo ? lo : v->valueint > hi ? hi : v->valueint;
    if (x == *dst) return false;
    *dst = x;
    return true;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static void schedule_restart(void)
{
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
}

// ---------------------------------------------------------------- handlers
static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

static esp_err_t h_get_config(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    settings_lock();
    cJSON_AddStringToObject(root, "wifi_ssid", g_cfg.wifi_ssid);
    cJSON_AddBoolToObject(root, "wifi_pass_set", g_cfg.wifi_pass[0] != 0);
    int klen = strlen(g_cfg.api_key);
    char masked[16] = "";
    if (klen) snprintf(masked, sizeof(masked), "…%.4s", g_cfg.api_key + (klen > 4 ? klen - 4 : 0));
    cJSON_AddStringToObject(root, "api_key_hint", masked);
    cJSON_AddStringToObject(root, "model", g_cfg.model);
    cJSON_AddStringToObject(root, "voice", g_cfg.voice);
    cJSON_AddStringToObject(root, "language", g_cfg.language);
    cJSON_AddStringToObject(root, "prompt", g_cfg.prompt);
    cJSON_AddStringToObject(root, "wake1", g_cfg.wake1);
    cJSON_AddStringToObject(root, "wake2", g_cfg.wake2);
    cJSON_AddNumberToObject(root, "volume", g_cfg.volume);
    cJSON_AddNumberToObject(root, "mic_gain", g_cfg.mic_gain);
    cJSON_AddNumberToObject(root, "idle_timeout", g_cfg.idle_timeout);
    cJSON_AddNumberToObject(root, "echo_tail_ms", g_cfg.echo_tail_ms);
    cJSON_AddBoolToObject(root, "search", g_cfg.search);
    cJSON_AddBoolToObject(root, "ha_enabled", g_cfg.ha_enabled);
    cJSON_AddStringToObject(root, "ha_url", g_cfg.ha_url);
    cJSON_AddBoolToObject(root, "ha_token_set", g_cfg.ha_token[0] != 0);
    cJSON_AddBoolToObject(root, "listen_beep", g_cfg.listen_beep);
    settings_unlock();

    const char *names[16], *words[16];
    int n = sr_wake_models(names, words, 16);
    cJSON *arr = cJSON_AddArrayToObject(root, "wake_models");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", names[i]);
        cJSON_AddStringToObject(o, "word", words[i] ? words[i] : names[i]);
        cJSON_AddItemToArray(arr, o);
    }
    return send_json(req, root);
}

static esp_err_t h_post_config(httpd_req_t *req)
{
    cJSON *in = read_json(req);
    if (!in) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    bool restart = false;
    int volume, mic_gain;

    settings_lock();
    restart |= take_str(in, "wifi_ssid", g_cfg.wifi_ssid, sizeof(g_cfg.wifi_ssid));
    cJSON *pass = cJSON_GetObjectItem(in, "wifi_pass");
    if (cJSON_IsString(pass) && pass->valuestring[0])
        restart |= take_str(in, "wifi_pass", g_cfg.wifi_pass, sizeof(g_cfg.wifi_pass));
    cJSON *key = cJSON_GetObjectItem(in, "api_key");
    if (cJSON_IsString(key) && key->valuestring[0]) take_str(in, "api_key", g_cfg.api_key, sizeof(g_cfg.api_key));
    take_str(in, "model", g_cfg.model, sizeof(g_cfg.model));
    take_str(in, "voice", g_cfg.voice, sizeof(g_cfg.voice));
    take_str(in, "language", g_cfg.language, sizeof(g_cfg.language));
    take_str(in, "prompt", g_cfg.prompt, sizeof(g_cfg.prompt));
    restart |= take_str(in, "wake1", g_cfg.wake1, sizeof(g_cfg.wake1));
    restart |= take_str(in, "wake2", g_cfg.wake2, sizeof(g_cfg.wake2));
    take_int(in, "volume", &g_cfg.volume, 0, 100);
    take_int(in, "mic_gain", &g_cfg.mic_gain, 0, 36);
    take_int(in, "idle_timeout", &g_cfg.idle_timeout, 5, 600);
    take_int(in, "echo_tail_ms", &g_cfg.echo_tail_ms, 0, 3000);
    cJSON *search = cJSON_GetObjectItem(in, "search");
    if (cJSON_IsBool(search)) g_cfg.search = cJSON_IsTrue(search);
    cJSON *ha_en = cJSON_GetObjectItem(in, "ha_enabled");
    if (cJSON_IsBool(ha_en)) g_cfg.ha_enabled = cJSON_IsTrue(ha_en);
    take_str(in, "ha_url", g_cfg.ha_url, sizeof(g_cfg.ha_url));
    cJSON *ha_tok = cJSON_GetObjectItem(in, "ha_token");
    if (cJSON_IsString(ha_tok) && ha_tok->valuestring[0]) take_str(in, "ha_token", g_cfg.ha_token, sizeof(g_cfg.ha_token));
    cJSON *beep = cJSON_GetObjectItem(in, "listen_beep");
    if (cJSON_IsBool(beep)) g_cfg.listen_beep = cJSON_IsTrue(beep);
    volume = g_cfg.volume;
    mic_gain = g_cfg.mic_gain;
    settings_unlock();
    cJSON_Delete(in);

    audio_set_volume(volume);
    audio_set_mic_gain(mic_gain);
    settings_save();
    ha_invalidate();
    if (restart && !app_wifi_is_ap()) ESP_LOGI(TAG, "wifi/wake word changed, restarting");
    // In setup-hotspot mode any save restarts, so new Wi-Fi credentials get tried
    restart |= app_wifi_is_ap();
    if (restart) schedule_restart();

    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddBoolToObject(out, "restart", restart);
    return send_json(req, out);
}

static esp_err_t h_status(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", app_state_name());
    cJSON_AddBoolToObject(root, "ap_mode", app_wifi_is_ap());
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddStringToObject(root, "ssid", (char *)ap.ssid);
        cJSON_AddNumberToObject(root, "rssi", ap.rssi);
    }
    esp_netif_ip_info_t ip;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr) {
        char s[16];
        snprintf(s, sizeof(s), IPSTR, IP2STR(&ip.ip));
        cJSON_AddStringToObject(root, "ip", s);
    }
    cJSON_AddNumberToObject(root, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(root, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "free_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddStringToObject(root, "last_error", gemini_last_error());
    cJSON_AddStringToObject(root, "firmware", esp_app_get_description()->version);
    return send_json(req, root);
}

static esp_err_t h_action(httpd_req_t *req)
{
    cJSON *in = read_json(req);
    cJSON *a = in ? cJSON_GetObjectItem(in, "action") : NULL;
    const char *act = cJSON_IsString(a) ? a->valuestring : "";
    bool ok = true;
    if (!strcmp(act, "talk")) app_post(EV_BUTTON);
    else if (!strcmp(act, "beep")) play_tone(TONE_WAKE);
    else if (!strcmp(act, "restart")) schedule_restart();
    else ok = false;
    cJSON_Delete(in);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", ok);
    return send_json(req, out);
}

// ---------------------------------------------------------------- model list / model test
typedef struct {
    char *buf;
    int len, cap;
} http_buf_t;

static esp_err_t http_collect(esp_http_client_event_t *e)
{
    http_buf_t *b = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        if (b->len + e->data_len + 1 > b->cap) {
            b->cap = (b->len + e->data_len + 1) * 2;
            b->buf = heap_caps_realloc(b->buf, b->cap, MALLOC_CAP_SPIRAM);
        }
        memcpy(b->buf + b->len, e->data, e->data_len);
        b->len += e->data_len;
        b->buf[b->len] = 0;
    }
    return ESP_OK;
}

static esp_err_t h_models(httpd_req_t *req)
{
    char url[256];
    settings_lock();
    snprintf(url, sizeof(url), "https://generativelanguage.googleapis.com/v1beta/models?pageSize=1000&key=%s",
             g_cfg.api_key);
    settings_unlock();
    http_buf_t b = { 0 };
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_collect,
        .user_data = &b,
        .timeout_ms = 10000,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    cJSON *out = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(out, "models");
    cJSON *root = (err == ESP_OK && b.buf) ? cJSON_Parse(b.buf) : NULL;
    if (!root || status != 200) {
        cJSON *msg = root ? cJSON_GetObjectItem(cJSON_GetObjectItem(root, "error"), "message") : NULL;
        cJSON_AddStringToObject(out, "error", cJSON_IsString(msg) ? msg->valuestring : "request failed");
    } else {
        cJSON *m;
        cJSON_ArrayForEach(m, cJSON_GetObjectItem(root, "models")) {
            cJSON *methods = cJSON_GetObjectItem(m, "supportedGenerationMethods"), *x;
            bool live = false;
            cJSON_ArrayForEach(x, methods) live |= cJSON_IsString(x) && !strcmp(x->valuestring, "bidiGenerateContent");
            if (!live) continue;
            const char *name = cJSON_GetObjectItem(m, "name")->valuestring;
            if (!strncmp(name, "models/", 7)) name += 7;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", name);
            cJSON *dn = cJSON_GetObjectItem(m, "displayName");
            cJSON_AddStringToObject(o, "name", cJSON_IsString(dn) ? dn->valuestring : name);
            cJSON_AddItemToArray(list, o);
        }
    }
    cJSON_Delete(root);
    free(b.buf);
    return send_json(req, out);
}

// One Live session handshake with the given model and current voice: setupComplete or the server's error.
#define T_DONE BIT0
static EventGroupHandle_t s_t_ev;
static bool s_t_ok;
static char s_t_err[160];
static char *s_t_msg;
static int s_t_len;

static void test_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_websocket_event_data_t *d = event_data;
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        char setup[256];
        int n = snprintf(setup, sizeof(setup),
                         "{\"setup\":{\"model\":\"models/%s\",\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],"
                         "\"speechConfig\":{\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":\"%s\"}}}}}}",
                         (char *)arg, g_cfg.voice);
        esp_websocket_client_send_text(d->client, setup, n, pdMS_TO_TICKS(3000));
    } else if (id == WEBSOCKET_EVENT_DATA) {
        if (d->op_code == 0x08) {
            int n = d->data_len > 2 ? d->data_len - 2 : 0;
            if (n >= (int)sizeof(s_t_err)) n = sizeof(s_t_err) - 1;
            memcpy(s_t_err, d->data_ptr + 2, n);
            s_t_err[n] = 0;
            if (!n) snprintf(s_t_err, sizeof(s_t_err), "closed by server (code %d)", d->close_status_code);
            xEventGroupSetBits(s_t_ev, T_DONE);
        } else if (d->op_code <= 0x02 && d->payload_len < 4096) {
            if (d->payload_offset == 0) s_t_len = 0;
            memcpy(s_t_msg + s_t_len, d->data_ptr, d->data_len);
            s_t_len += d->data_len;
            s_t_msg[s_t_len] = 0;
            if (d->payload_offset + d->data_len >= d->payload_len && strstr(s_t_msg, "setupComplete")) {
                s_t_ok = true;
                xEventGroupSetBits(s_t_ev, T_DONE);
            }
        }
    } else if (id == WEBSOCKET_EVENT_ERROR) {
        if (!s_t_err[0]) {
            snprintf(s_t_err, sizeof(s_t_err), "connection failed (HTTP %d)",
                     d->error_handle.esp_ws_handshake_status_code);
        }
        xEventGroupSetBits(s_t_ev, T_DONE);
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED) {
        if (!s_t_err[0] && !s_t_ok) strlcpy(s_t_err, "disconnected", sizeof(s_t_err));
        xEventGroupSetBits(s_t_ev, T_DONE);
    }
}

static esp_err_t h_test_model(httpd_req_t *req)
{
    cJSON *in = read_json(req);
    cJSON *m = in ? cJSON_GetObjectItem(in, "model") : NULL;
    static char model[64];
    strlcpy(model, cJSON_IsString(m) ? m->valuestring : "", sizeof(model));
    cJSON_Delete(in);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "model", model);
    if (g_state != APP_IDLE) {
        cJSON_AddBoolToObject(out, "ok", false);
        cJSON_AddStringToObject(out, "error", "device is busy in a conversation");
        return send_json(req, out);
    }
    if (!s_t_ev) {
        s_t_ev = xEventGroupCreate();
        s_t_msg = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    }
    xEventGroupClearBits(s_t_ev, T_DONE);
    s_t_ok = false;
    s_t_err[0] = 0;
    s_t_len = 0;

    static char url[256];
    settings_lock();
    snprintf(url, sizeof(url), "wss://generativelanguage.googleapis.com/ws/"
             "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=%s", g_cfg.api_key);
    settings_unlock();
    esp_websocket_client_config_t cfg = {
        .uri = url,
        .buffer_size = 4096,
        .task_stack = 8192,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = true,
        .network_timeout_ms = 8000,
    };
    int64_t t0 = esp_timer_get_time();
    esp_websocket_client_handle_t c = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(c, WEBSOCKET_EVENT_ANY, test_event, model);
    esp_websocket_client_start(c);
    if (!(xEventGroupWaitBits(s_t_ev, T_DONE, false, true, pdMS_TO_TICKS(12000)) & T_DONE))
        strlcpy(s_t_err, "timeout", sizeof(s_t_err));
    int ms = (esp_timer_get_time() - t0) / 1000;
    if (esp_websocket_client_is_connected(c)) esp_websocket_client_close(c, pdMS_TO_TICKS(1000));
    esp_websocket_client_destroy(c);

    ESP_LOGI(TAG, "model test %s: %s %s", model, s_t_ok ? "ok" : "FAIL", s_t_err);
    cJSON_AddBoolToObject(out, "ok", s_t_ok);
    cJSON_AddNumberToObject(out, "ms", ms);
    if (!s_t_ok) cJSON_AddStringToObject(out, "error", s_t_err);
    return send_json(req, out);
}

static esp_err_t h_ha_test(httpd_req_t *req)
{
    return send_json(req, ha_test());
}

// ---------------------------------------------------------------- firmware update (OTA)
// POST /api/ota with the raw app image (build/gemini-echo.bin) as the body.
// The new image boots once in "pending verify"; main.c confirms it, otherwise the bootloader rolls back.
static volatile bool s_ota_active;

bool web_ota_active(void)
{
    return s_ota_active;
}

static const char *ota_check_image(const uint8_t *buf, int len)
{
    const esp_image_header_t *img = (const esp_image_header_t *)buf;
    const esp_app_desc_t *d = (const esp_app_desc_t *)(buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (len < sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t) ||
        img->magic != ESP_IMAGE_HEADER_MAGIC || d->magic_word != ESP_APP_DESC_MAGIC_WORD)
        return "not an app image (use build/gemini-echo.bin)";
    if (img->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) return "image is for another chip";
    if (strncmp(d->project_name, esp_app_get_description()->project_name, sizeof(d->project_name)))
        return "image is for another project";
    ESP_LOGI(TAG, "OTA: %.32s %.32s", d->project_name, d->version);
    return NULL;
}

static esp_err_t h_ota(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    const char *err = NULL;
    if (!part) err = "no OTA slot - flash over USB once with the OTA partition table";
    else if (g_state != APP_IDLE) err = "device is busy in a conversation";
    else if (s_ota_active) err = "another update is running";
    else if (req->content_len <= 0 || req->content_len > part->size) err = "image doesn't fit the OTA slot";

    esp_ota_handle_t ota = 0;
    uint8_t *buf = NULL;
    bool started = !err;
    if (started) {
        s_ota_active = true;
        buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
        if (!buf) err = "out of memory";
        else ESP_LOGI(TAG, "OTA: receiving %d bytes into %s", req->content_len, part->label);
    }
    int got = 0, timeouts = 0;
    int64_t t0 = esp_timer_get_time();
    while (!err && got < req->content_len) {
        // Fill whole 4 KB blocks so the first one holds the image header for the check
        int want = req->content_len - got > 4096 ? 4096 : req->content_len - got, fill = 0;
        while (fill < want) {
            int r = httpd_req_recv(req, (char *)buf + fill, want - fill);
            if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 4) continue;
            if (r <= 0) break;
            fill += r;
        }
        if (fill < want) {
            err = "upload interrupted";
        } else if (got == 0 && (err = ota_check_image(buf, want)) == NULL &&
                   esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
            err = "can't start the update";
        } else if (!err && esp_ota_write(ota, buf, want) != ESP_OK) {
            err = "flash write failed";
        }
        got += want;
    }
    if (!err && ota) {
        esp_err_t e = esp_ota_end(ota);
        ota = 0;
        if (e == ESP_ERR_OTA_VALIDATE_FAILED) err = "image is corrupted";
        else if (e != ESP_OK) err = "can't finish the update";
        else if (esp_ota_set_boot_partition(part) != ESP_OK) err = "can't switch to the new image";
    }
    if (ota) esp_ota_abort(ota);
    free(buf);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", !err);
    if (err) {
        ESP_LOGW(TAG, "OTA failed: %s", err);
        cJSON_AddStringToObject(out, "error", err);
    } else {
        esp_app_desc_t d;
        esp_ota_get_partition_description(part, &d);
        ESP_LOGI(TAG, "OTA: %s installed in %lld ms, restarting", d.version, (esp_timer_get_time() - t0) / 1000);
        cJSON_AddStringToObject(out, "version", d.version);
        schedule_restart();
    }
    if (started) s_ota_active = false;
    return send_json(req, out);
}

// ---------------------------------------------------------------- server
void web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 10240;
    cfg.max_uri_handlers = 12;
    cfg.lru_purge_enable = true;
    cfg.core_id = 0;
    httpd_handle_t srv;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "http server failed to start");
        return;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = h_index },
        { .uri = "/api/config", .method = HTTP_GET, .handler = h_get_config },
        { .uri = "/api/config", .method = HTTP_POST, .handler = h_post_config },
        { .uri = "/api/status", .method = HTTP_GET, .handler = h_status },
        { .uri = "/api/action", .method = HTTP_POST, .handler = h_action },
        { .uri = "/api/models", .method = HTTP_GET, .handler = h_models },
        { .uri = "/api/test_model", .method = HTTP_POST, .handler = h_test_model },
        { .uri = "/api/ha_test", .method = HTTP_POST, .handler = h_ha_test },
        { .uri = "/api/ota", .method = HTTP_POST, .handler = h_ota },
    };
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(srv, &uris[i]);
    ESP_LOGI(TAG, "web UI on port 80");
}
