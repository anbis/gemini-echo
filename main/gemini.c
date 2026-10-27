// Gemini Live API session over a raw WebSocket (BidiGenerateContent).
// Uplink: 16 kHz PCM from the AFE. Downlink: 24 kHz PCM native audio.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <ctype.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_websocket_client.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "app.h"
#include "config.h"
#include "settings.h"

static const char *TAG = "gemini";

#define GEMINI_URL "wss://generativelanguage.googleapis.com/ws/" \
    "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key="

#define UPLINK_CHUNK (AUDIO_RATE / 10)   // 100 ms per message

static esp_websocket_client_handle_t s_ws;
static rb_t *s_up;
static volatile bool s_ready;
static volatile bool s_closing;
static SemaphoreHandle_t s_ws_lock;
static volatile bool s_drop_turn;   // user cut the reply: ignore its remaining audio
static char *s_msg;        // reassembly buffer for one server message
static int s_msg_len, s_msg_cap;

static char s_last_error[160];
static gemini_err_t s_err_kind;
static QueueHandle_t s_tool_q;   // char* toolCall JSON, handled by tool_task

void gemini_set_error(const char *msg, int len)
{
    if (len >= (int)sizeof(s_last_error)) len = sizeof(s_last_error) - 1;
    memcpy(s_last_error, msg, len);
    s_last_error[len] = 0;
    char low[160];
    for (int i = 0; i <= len; i++) low[i] = tolower((unsigned char)s_last_error[i]);
    if (strstr(low, "quota") || strstr(low, "exceeded") || strstr(low, "resource_exhausted") || strstr(low, "http 429"))
        s_err_kind = ERR_QUOTA;
    else if (strstr(low, "api key") || strstr(low, "api_key") || strstr(low, "permission") ||
             strstr(low, "http 400") || strstr(low, "http 401") || strstr(low, "http 403"))
        s_err_kind = ERR_APIKEY;
    else
        s_err_kind = ERR_CONNECT;
}

gemini_err_t gemini_error_kind(void)
{
    return s_err_kind;
}

const char *gemini_last_error(void)
{
    return s_last_error;
}

void gemini_uplink_push(const int16_t *pcm, int n)
{
    rb_write(s_up, pcm, n);
}

void gemini_cancel_turn(void)
{
    s_drop_turn = true;
    play_clear();
}

static void send_setup(void)
{
    static char model[80], voice[32];
    bool search;
    cJSON *ha_tools = ha_function_declarations();
    char *ha_text = ha_tools ? ha_prompt() : NULL;
    size_t cap = sizeof(g_cfg.prompt) + 1024 + (ha_text ? strlen(ha_text) : 0);
    char *prompt = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    settings_lock();
    snprintf(model, sizeof(model), "models/%s", g_cfg.model);
    strlcpy(voice, g_cfg.voice, sizeof(voice));
    if (strcasecmp(g_cfg.language, "auto") == 0 || !g_cfg.language[0])
        snprintf(prompt, cap, "%s\nAlways reply in the language the user speaks.", g_cfg.prompt);
    else
        snprintf(prompt, cap, "%s\nAlways reply in %s, unless the user explicitly asks for another language.",
                 g_cfg.prompt, g_cfg.language);
    search = g_cfg.search;
    settings_unlock();
    if (ha_tools) {
        strlcat(prompt, "\nYou can control and query the user's smart home with the Home Assistant tools. "
                "Use GetLiveContext to check current states before answering questions about the home. "
                "In tool calls, use area and device names exactly as Home Assistant lists them below, "
                "even when the user says them in another language. "
                "After an action, confirm it in a few words.", cap);
        if (ha_text) {
            strlcat(prompt, "\n\n", cap);
            strlcat(prompt, ha_text, cap);
        }
    }
    free(ha_text);
    char today[40];
    if (app_today(today, sizeof(today))) {
        strlcat(prompt, "\nToday is ", cap);
        strlcat(prompt, today, cap);
        strlcat(prompt, " (UTC date).", cap);
    }
    char *mem = memory_context();
    if (mem) {
        // after the HA text, which is static, so the notes come last
        size_t need = strlen(prompt) + strlen(mem) + 3;
        if (need > cap) prompt = heap_caps_realloc(prompt, cap = need, MALLOC_CAP_SPIRAM);
        strlcat(prompt, "\n\n", cap);
        strlcat(prompt, mem, cap);
        free(mem);
    }
    ESP_LOGI(TAG, "model %s, voice %s", model, voice);

    cJSON *root = cJSON_CreateObject();
    cJSON *setup = cJSON_AddObjectToObject(root, "setup");
    cJSON_AddStringToObject(setup, "model", model);

    cJSON *gen = cJSON_AddObjectToObject(setup, "generationConfig");
    cJSON *mods = cJSON_AddArrayToObject(gen, "responseModalities");
    cJSON_AddItemToArray(mods, cJSON_CreateString("AUDIO"));
    cJSON *voice_cfg = cJSON_AddObjectToObject(cJSON_AddObjectToObject(cJSON_AddObjectToObject(gen, "speechConfig"),
                                                                        "voiceConfig"), "prebuiltVoiceConfig");
    cJSON_AddStringToObject(voice_cfg, "voiceName", voice);

    cJSON *sys = cJSON_AddObjectToObject(setup, "systemInstruction");
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", prompt);
    ESP_LOGI(TAG, "system instruction %d chars", (int)strlen(prompt));
    free(prompt);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(sys, "parts"), part);

    if (search || ha_tools) {
        cJSON *tools = cJSON_AddArrayToObject(setup, "tools");
        if (search) {
            cJSON *tool = cJSON_CreateObject();
            cJSON_AddObjectToObject(tool, "googleSearch");
            cJSON_AddItemToArray(tools, tool);
        }
        if (ha_tools) {
            cJSON *tool = cJSON_CreateObject();
            cJSON_AddItemToObject(tool, "functionDeclarations", ha_tools);
            cJSON_AddItemToArray(tools, tool);
            ESP_LOGI(TAG, "%d Home Assistant tools enabled", cJSON_GetArraySize(ha_tools));
        }
    }

    cJSON *aad = cJSON_AddObjectToObject(cJSON_AddObjectToObject(setup, "realtimeInputConfig"),
                                         "automaticActivityDetection");
    cJSON_AddNumberToObject(aad, "silenceDurationMs", 700);
    cJSON_AddObjectToObject(setup, "inputAudioTranscription");    // logs only
    cJSON_AddObjectToObject(setup, "outputAudioTranscription");   // logs only
    cJSON_AddObjectToObject(cJSON_AddObjectToObject(setup, "contextWindowCompression"), "slidingWindow");

    char *json = cJSON_PrintUnformatted(root);
    esp_websocket_client_send_text(s_ws, json, strlen(json), pdMS_TO_TICKS(5000));
    free(json);
    cJSON_Delete(root);
}

static void handle_message(const char *data, int len)
{
    g_last_server_us = esp_timer_get_time();
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "bad json (%d bytes)", len);
        return;
    }
    if (cJSON_GetObjectItem(root, "setupComplete")) {
        s_last_error[0] = 0;
        ESP_LOGI(TAG, "session ready, flushing %u ms of buffered mic audio", rb_avail(s_up) * 1000 / AUDIO_RATE);
        s_ready = true;
        app_post(EV_SESSION_READY);
    }
    cJSON *sc = cJSON_GetObjectItem(root, "serverContent");
    if (sc) {
        if (cJSON_IsTrue(cJSON_GetObjectItem(sc, "interrupted"))) {
            ESP_LOGI(TAG, "interrupted (barge-in)");
            play_clear();
            s_drop_turn = false;
        }
        cJSON *parts = cJSON_GetObjectItem(cJSON_GetObjectItem(sc, "modelTurn"), "parts");
        cJSON *p;
        cJSON_ArrayForEach(p, parts) {
            cJSON *b64 = cJSON_GetObjectItem(cJSON_GetObjectItem(p, "inlineData"), "data");
            if (!cJSON_IsString(b64) || s_drop_turn) continue;
            size_t in_len = strlen(b64->valuestring), out_len = 0;
            uint8_t *pcm = heap_caps_malloc(in_len * 3 / 4 + 4, MALLOC_CAP_SPIRAM);
            if (pcm && mbedtls_base64_decode(pcm, in_len * 3 / 4 + 4, &out_len,
                                             (const uint8_t *)b64->valuestring, in_len) == 0) {
                play_push_24k((const int16_t *)pcm, out_len / 2);
            }
            free(pcm);
        }
        cJSON *t;
        if ((t = cJSON_GetObjectItem(cJSON_GetObjectItem(sc, "inputTranscription"), "text")) && cJSON_IsString(t)) {
            ESP_LOGI(TAG, "you: %s", t->valuestring);
            memory_note(MEM_USER, t->valuestring);
        }
        if ((t = cJSON_GetObjectItem(cJSON_GetObjectItem(sc, "outputTranscription"), "text")) && cJSON_IsString(t)) {
            ESP_LOGI(TAG, "gemini: %s", t->valuestring);
            memory_note(MEM_ASSISTANT, t->valuestring);
        }
        if (cJSON_IsTrue(cJSON_GetObjectItem(sc, "turnComplete"))) {
            ESP_LOGI(TAG, "turn complete");
            s_drop_turn = false;
        }
    }
    cJSON *tc = cJSON_GetObjectItem(root, "toolCall");
    if (tc) {
        char *s = cJSON_PrintUnformatted(tc);
        if (xQueueSend(s_tool_q, &s, 0) != pdTRUE) free(s);
    }
    if (cJSON_GetObjectItem(root, "goAway")) ESP_LOGW(TAG, "server goAway");
    cJSON_Delete(root);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_websocket_event_data_t *d = event_data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected, sending setup");
        send_setup();
        break;
    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x08) {
            ESP_LOGW(TAG, "server close %d: %.*s", d->close_status_code,
                     d->data_len > 2 ? d->data_len - 2 : 0, d->data_ptr + 2);
            if (d->data_len > 2) gemini_set_error(d->data_ptr + 2, d->data_len - 2);
            break;
        }
        if (d->op_code != 0x01 && d->op_code != 0x02 && d->op_code != 0x00) break;
        if (d->payload_offset == 0 && d->op_code != 0x00) s_msg_len = 0;
        if (s_msg_len + d->data_len + 1 > s_msg_cap) {
            s_msg_cap = (s_msg_len + d->data_len + 1) * 2;
            s_msg = heap_caps_realloc(s_msg, s_msg_cap, MALLOC_CAP_SPIRAM);
        }
        memcpy(s_msg + s_msg_len, d->data_ptr, d->data_len);
        s_msg_len += d->data_len;
        if (d->fin && d->payload_offset + d->data_len >= d->payload_len) {
            handle_message(s_msg, s_msg_len);
            s_msg_len = 0;
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "websocket error (handshake status %d)", d->error_handle.esp_ws_handshake_status_code);
        if (s_err_kind == ERR_NONE) {
            char e[48];
            int code = d->error_handle.esp_ws_handshake_status_code;
            gemini_set_error(e, code ? snprintf(e, sizeof(e), "WebSocket handshake failed (HTTP %d)", code)
                                     : snprintf(e, sizeof(e), "can't reach Gemini (network error)"));
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGI(TAG, "disconnected");
        s_ready = false;
        if (!s_closing) app_post(EV_SESSION_CLOSED);
        break;
    }
}

static void uplink_task(void *arg)
{
    int16_t pcm[UPLINK_CHUNK];
    static const char pre[] = "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"";
    static const char post[] = "\"}}}";
    size_t b64_max = (UPLINK_CHUNK * 2 + 2) / 3 * 4 + 1;
    char *msg = heap_caps_malloc(sizeof(pre) + b64_max + sizeof(post), MALLOC_CAP_SPIRAM);

    for (;;) {
        if (!s_ready || rb_avail(s_up) < UPLINK_CHUNK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        rb_read(s_up, pcm, UPLINK_CHUNK);
        size_t olen = 0;
        memcpy(msg, pre, sizeof(pre) - 1);
        mbedtls_base64_encode((uint8_t *)msg + sizeof(pre) - 1, b64_max, &olen, (uint8_t *)pcm, sizeof(pcm));
        memcpy(msg + sizeof(pre) - 1 + olen, post, sizeof(post));
        int len = sizeof(pre) - 1 + olen + sizeof(post) - 1;
        xSemaphoreTake(s_ws_lock, portMAX_DELAY);
        if (s_ws && s_ready && esp_websocket_client_send_text(s_ws, msg, len, pdMS_TO_TICKS(2000)) < 0)
            ESP_LOGW(TAG, "uplink send failed");
        xSemaphoreGive(s_ws_lock);
    }
}

// Executes Gemini's function calls against Home Assistant and returns the results.
static void tool_task(void *arg)
{
    for (;;) {
        char *s;
        xQueueReceive(s_tool_q, &s, portMAX_DELAY);
        cJSON *tc = cJSON_Parse(s);
        free(s);
        cJSON *resp = cJSON_CreateObject();
        cJSON *list = cJSON_AddArrayToObject(cJSON_AddObjectToObject(resp, "toolResponse"), "functionResponses");
        cJSON *fc;
        cJSON_ArrayForEach(fc, cJSON_GetObjectItem(tc, "functionCalls")) {
            cJSON *name = cJSON_GetObjectItem(fc, "name");
            cJSON *id = cJSON_GetObjectItem(fc, "id");
            if (!cJSON_IsString(name)) continue;
            char *args = cJSON_PrintUnformatted(cJSON_GetObjectItem(fc, "args"));
            ESP_LOGI(TAG, "tool call %s %s", name->valuestring, args ? args : "{}");
            char note[200];
            snprintf(note, sizeof(note), "%s %.150s", name->valuestring, args ? args : "{}");
            memory_note(MEM_TOOL, note);
            free(args);
            cJSON *r = ha_call_tool(name->valuestring, cJSON_GetObjectItem(fc, "args"));
            cJSON *e = cJSON_GetObjectItem(r, "error");
            if (e) ESP_LOGW(TAG, "tool %s failed: %.120s", name->valuestring, e->valuestring);
            cJSON *item = cJSON_CreateObject();
            if (cJSON_IsString(id)) cJSON_AddStringToObject(item, "id", id->valuestring);
            cJSON_AddStringToObject(item, "name", name->valuestring);
            cJSON_AddItemToObject(item, "response", r);
            cJSON_AddItemToArray(list, item);
        }
        cJSON_Delete(tc);
        char *json = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        xSemaphoreTake(s_ws_lock, portMAX_DELAY);
        if (s_ws && s_ready) esp_websocket_client_send_text(s_ws, json, strlen(json), pdMS_TO_TICKS(5000));
        xSemaphoreGive(s_ws_lock);
        free(json);
    }
}

void gemini_init(void)
{
    s_tool_q = xQueueCreate(4, sizeof(char *));
    xTaskCreatePinnedToCore(tool_task, "tools", 8192, NULL, 9, NULL, 0);
    s_ws_lock = xSemaphoreCreateMutex();
    s_up = rb_create(AUDIO_RATE * 15);   // 15 s: covers connection setup time
    xTaskCreatePinnedToCore(uplink_task, "uplink", 6144, NULL, 10, NULL, 0);
}

bool gemini_start(void)
{
    rb_clear(s_up);
    s_ready = false;
    s_closing = false;
    s_drop_turn = false;
    s_err_kind = ERR_NONE;
    s_msg_len = 0;
    static char url[256];
    settings_lock();
    snprintf(url, sizeof(url), GEMINI_URL "%s", g_cfg.api_key);
    settings_unlock();
    esp_websocket_client_config_t cfg = {
        .uri = url,
        .buffer_size = 16384,
        .task_stack = 10240,
        .task_prio = 12,
        .task_core_id_set = true,
        .task_core_id = 0,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = true,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 20,
        .pingpong_timeout_sec = 60,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) return false;
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        return false;
    }
    return true;
}

void gemini_stop(void)
{
    if (!s_ws) return;
    s_closing = true;
    s_ready = false;
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    if (esp_websocket_client_is_connected(s_ws)) esp_websocket_client_close(s_ws, pdMS_TO_TICKS(2000));
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
    xSemaphoreGive(s_ws_lock);
    rb_clear(s_up);
}
