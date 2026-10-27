// Conversation memory: a short rolling summary of past conversations, kept in NVS and
// added to Gemini's system instruction so a new session can pick up where the last one stopped.
// During a session the transcriptions are collected; when it ends, a text model folds them into
// the previous summary. Only the latest summary is kept, never the full history.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "cJSON.h"
#include "app.h"
#include "settings.h"

static const char *TAG = "memory";

#define SUMMARY_MAX    3000    // bytes (NVS string limit is ~4000)
#define TRANSCRIPT_MAX 12000   // current + not yet summarized conversations; the oldest part is dropped

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static char *s_summary;       // saved notes
static char *s_live;          // transcript of the session in progress
static char *s_pending;       // finished sessions waiting to be summarized
static int s_live_role;       // 0 none, 1 user, 2 assistant
static uint32_t s_gen;        // bumped by memory_clear(): drops a summary that was in flight
static volatile bool s_busy;
static char s_error[120];

static const char *SUMMARY_PROMPT =
    "You keep the memory of a voice assistant that lives in a small smart speaker at home. "
    "You get the previous memory notes and the transcript of the latest conversation (speech recognition, "
    "may contain mistakes). Write the updated notes: keep what still matters from the previous notes "
    "(facts about the user and the household, preferences, plans, unfinished requests, promises), "
    "add what is new, and end with what the latest conversation was about and where it stopped, "
    "so the assistant can continue from there next time. Drop greetings, small talk and finished one-off "
    "actions that won't matter later. Write plain text without markdown, in the language the user speaks, "
    "at most 1200 characters. Turn relative dates (today, tomorrow, next week) into absolute dates, "
    "and drop plans whose date has passed unless they still matter. "
    "If nothing new is worth keeping, return the previous notes unchanged.";

// Appends src, keeping the buffer under max by dropping its oldest part (at a line start).
static void append_tail(char *buf, int max, const char *src)
{
    int len = strlen(buf), add = strlen(src);
    if (add >= max) {
        src += add - max / 2;
        add = strlen(src);
    }
    if (len + add >= max) {
        int drop = len + add - max + 1 + max / 4;
        if (drop > len) drop = len;
        char *nl = strchr(buf + drop, '\n');
        drop = nl ? nl - buf + 1 : len;
        memmove(buf, buf + drop, len - drop + 1);
        len -= drop;
    }
    memcpy(buf + len, src, add + 1);
}

// Cuts s to at most max bytes without splitting a UTF-8 character.
static void utf8_cut(char *s, int max)
{
    int len = strlen(s);
    if (len <= max) return;
    while (max > 0 && ((unsigned char)s[max] & 0xC0) == 0x80) max--;
    s[max] = 0;
}

void memory_note(memory_role_t role, const char *text)
{
    if (!s_live || !text || !*text) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (role == MEM_TOOL) {
        append_tail(s_live, TRANSCRIPT_MAX, "\n(");
        append_tail(s_live, TRANSCRIPT_MAX, text);
        append_tail(s_live, TRANSCRIPT_MAX, ")");
        s_live_role = 0;
    } else {
        if (role != s_live_role) append_tail(s_live, TRANSCRIPT_MAX, role == MEM_USER ? "\nUser:" : "\nAssistant:");
        s_live_role = role;
        append_tail(s_live, TRANSCRIPT_MAX, text);
    }
    xSemaphoreGive(s_lock);
}

void memory_session_end(void)
{
    if (!s_live) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool any = s_live[0] != 0;
    if (any && g_cfg.memory) {
        append_tail(s_pending, TRANSCRIPT_MAX, "\n--- conversation ---");
        append_tail(s_pending, TRANSCRIPT_MAX, s_live);
    }
    s_live[0] = 0;
    s_live_role = 0;
    xSemaphoreGive(s_lock);
    if (any && g_cfg.memory) xTaskNotifyGive(s_task);
}

char *memory_context(void)
{
    if (!s_summary || !g_cfg.memory) return NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    char *out = NULL;
    if (s_summary[0] || s_pending[0]) {
        size_t cap = strlen(s_summary) + strlen(s_pending) + 400;
        out = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
        snprintf(out, cap, "Your memory of earlier conversations with the user. Use it when it helps and "
                 "continue from where you stopped if the user asks; don't recite it unprompted.\n%s%s%s",
                 s_summary, s_pending[0] ? "\nLatest conversation transcript (not yet in the notes):" : "",
                 s_pending);
    }
    xSemaphoreGive(s_lock);
    return out;
}

void memory_clear(void)
{
    if (!s_summary) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_summary[0] = s_pending[0] = s_live[0] = 0;
    s_live_role = 0;
    s_gen++;
    s_error[0] = 0;
    xSemaphoreGive(s_lock);
    nvs_handle_t h;
    if (nvs_open("gemini", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "memory");
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "memory cleared");
}

cJSON *memory_status(void)
{
    cJSON *o = cJSON_CreateObject();
    if (!s_summary) return o;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddStringToObject(o, "notes", s_summary);
    cJSON_AddNumberToObject(o, "pending_chars", strlen(s_pending));
    cJSON_AddBoolToObject(o, "updating", s_busy);
    cJSON_AddStringToObject(o, "error", s_error);
    xSemaphoreGive(s_lock);
    return o;
}

typedef struct {
    char *buf;
    int len, cap;
} resp_t;

static esp_err_t collect(esp_http_client_event_t *e)
{
    resp_t *b = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && b->len + e->data_len < 32768) {
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

// One generateContent call: previous notes + transcript in, new notes out (caller frees). NULL + s_error on failure.
static char *summarize(const char *notes, const char *transcript)
{
    static char url[320];
    settings_lock();
    snprintf(url, sizeof(url), "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent?key=%s",
             g_cfg.memory_model, g_cfg.api_key);
    settings_unlock();

    char today[40];
    if (!app_today(today, sizeof(today))) strlcpy(today, "unknown", sizeof(today));
    size_t cap = strlen(notes) + strlen(transcript) + 160;
    char *text = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    snprintf(text, cap, "TODAY: %s\n\nPREVIOUS NOTES:\n%s\n\nLATEST CONVERSATION:%s", today,
             notes[0] ? notes : "(none)", transcript);
    cJSON *root = cJSON_CreateObject();
    cJSON *sys = cJSON_AddArrayToObject(cJSON_AddObjectToObject(root, "systemInstruction"), "parts");
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "text", SUMMARY_PROMPT);
    cJSON_AddItemToArray(sys, p);
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "text", text);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(msg, "parts"), p);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(root, "contents"), msg);
    free(text);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    resp_t b = { 0 };
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = collect,
        .user_data = &b,
        .timeout_ms = 30000,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, body, strlen(body));
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    free(body);

    char *out = NULL;
    cJSON *r = (err == ESP_OK && b.buf) ? cJSON_Parse(b.buf) : NULL;
    if (status == 200 && r) {
        cJSON *parts = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(r, "candidates"), 0),
                                                               "content"), "parts");
        cJSON *part;
        size_t n = 0;
        out = heap_caps_calloc(1, SUMMARY_MAX + 4, MALLOC_CAP_SPIRAM);
        cJSON_ArrayForEach(part, parts) {
            cJSON *t = cJSON_GetObjectItem(part, "text");
            if (!cJSON_IsString(t) || cJSON_IsTrue(cJSON_GetObjectItem(part, "thought"))) continue;
            n = strlcat(out, t->valuestring, SUMMARY_MAX + 4);
        }
        if (n == 0) {
            free(out);
            out = NULL;
            strlcpy(s_error, "empty reply from the summary model", sizeof(s_error));
        }
    } else {
        cJSON *m = cJSON_GetObjectItem(cJSON_GetObjectItem(r, "error"), "message");
        if (cJSON_IsString(m)) snprintf(s_error, sizeof(s_error), "%.110s", m->valuestring);
        else snprintf(s_error, sizeof(s_error), "request failed (%s, HTTP %d)", esp_err_to_name(err), status);
    }
    cJSON_Delete(r);
    free(b.buf);
    if (out) {
        utf8_cut(out, SUMMARY_MAX);
        // trim whitespace the model may add around the notes
        int len = strlen(out);
        while (len && (out[len - 1] == '\n' || out[len - 1] == ' ')) out[--len] = 0;
    }
    return out;
}

static void memory_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t gen = s_gen;
        char *notes = strdup(s_summary), *transcript = strdup(s_pending);
        int used = strlen(s_pending);
        s_busy = used > 0;
        xSemaphoreGive(s_lock);
        if (used) {
            ESP_LOGI(TAG, "summarizing %d chars of conversation", used);
            char *out = summarize(notes, transcript);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool keep = out && gen == s_gen;
            if (keep) {
                strlcpy(s_summary, out, SUMMARY_MAX + 1);
                // sessions that ended meanwhile stay pending for the next round
                int left = strlen(s_pending) - used;
                if (left > 0) memmove(s_pending, s_pending + used, left + 1);
                else s_pending[0] = 0;
                s_error[0] = 0;
            }
            bool again = s_pending[0] != 0 && keep;
            s_busy = false;
            xSemaphoreGive(s_lock);
            if (keep) {
                nvs_handle_t h;
                if (nvs_open("gemini", NVS_READWRITE, &h) == ESP_OK) {
                    nvs_set_str(h, "memory", out);
                    nvs_commit(h);
                    nvs_close(h);
                }
                ESP_LOGI(TAG, "notes updated (%d chars): %s", (int)strlen(out), out);
            } else if (!out) {
                ESP_LOGW(TAG, "summary failed: %s", s_error);   // transcript stays pending, retried after the next session
            }
            free(out);
            if (again) xTaskNotifyGive(s_task);
        }
        free(notes);
        free(transcript);
    }
}

void memory_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_summary = heap_caps_calloc(1, SUMMARY_MAX + 1, MALLOC_CAP_SPIRAM);
    s_live = heap_caps_calloc(1, TRANSCRIPT_MAX, MALLOC_CAP_SPIRAM);
    s_pending = heap_caps_calloc(1, TRANSCRIPT_MAX, MALLOC_CAP_SPIRAM);
    nvs_handle_t h;
    if (nvs_open("gemini", NVS_READONLY, &h) == ESP_OK) {
        size_t len = SUMMARY_MAX + 1;
        if (nvs_get_str(h, "memory", s_summary, &len) != ESP_OK) s_summary[0] = 0;
        nvs_close(h);
    }
    ESP_LOGI(TAG, "%d chars of notes", (int)strlen(s_summary));
    xTaskCreatePinnedToCore(memory_task, "memory", 8192, NULL, 4, &s_task, 0);
}
