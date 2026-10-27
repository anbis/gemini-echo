// Home Assistant control through HA's built-in MCP server (POST {url}/api/mcp, JSON-RPC).
// HA's tools are handed to Gemini as function declarations; Gemini's tool calls run here.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "app.h"
#include "settings.h"

static const char *TAG = "ha";

#define TOOLS_TTL_US (10 * 60 * 1000000LL)

static SemaphoreHandle_t s_lock;
static cJSON *s_tools;           // Gemini functionDeclarations array (owned)
static char *s_prompt;           // HA's instructions for its tools, incl. the areas/devices overview (owned)
static int64_t s_tools_at;
static char s_session[96];       // Mcp-Session-Id, if the server issues one
static char s_error[160];
static int s_rpc_id;

typedef struct {
    char *buf;
    int len, cap;
} body_t;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    body_t *b = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(e->header_key, "Mcp-Session-Id")) {
        strlcpy(s_session, e->header_value, sizeof(s_session));
    } else if (e->event_id == HTTP_EVENT_ON_DATA) {
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

// One JSON-RPC call. Returns the "result" object (caller frees the returned root via *root_out), or NULL.
static cJSON *rpc(const char *method, cJSON *params, cJSON **root_out)
{
    char url[160], auth[400];
    settings_lock();
    snprintf(url, sizeof(url), "%s/api/mcp", g_cfg.ha_url);
    snprintf(auth, sizeof(auth), "Bearer %s", g_cfg.ha_token);
    settings_unlock();
    // strip a trailing slash in the configured base URL
    char *dbl = strstr(url, "//api/mcp");
    if (dbl) memmove(dbl, dbl + 1, strlen(dbl));

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    bool notify = !strncmp(method, "notifications/", 14);
    if (!notify) cJSON_AddNumberToObject(req, "id", ++s_rpc_id);
    cJSON_AddStringToObject(req, "method", method);
    if (params) cJSON_AddItemToObject(req, "params", params);
    char *payload = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    body_t b = { 0 };
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = on_http,
        .user_data = &b,
        .timeout_ms = 6000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Accept", "application/json, text/event-stream");
    if (s_session[0]) esp_http_client_set_header(c, "Mcp-Session-Id", s_session);
    esp_http_client_set_post_field(c, payload, strlen(payload));
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    free(payload);

    cJSON *result = NULL;
    *root_out = NULL;
    if (err != ESP_OK) {
        snprintf(s_error, sizeof(s_error), "can't reach Home Assistant (%s)", esp_err_to_name(err));
    } else if (status == 401 || status == 403) {
        snprintf(s_error, sizeof(s_error), "HA rejected the token (HTTP %d, admin token needed)", status);
    } else if (status == 404) {
        snprintf(s_error, sizeof(s_error), "MCP Server integration not enabled in HA (HTTP 404)");
    } else if (notify) {
        s_error[0] = 0;
    } else if (status != 200 || !b.buf) {
        snprintf(s_error, sizeof(s_error), "HA MCP error (HTTP %d)", status);
    } else {
        // Streamable HTTP may answer as SSE: take the JSON from the last "data:" line
        char *json = b.buf, *d;
        while ((d = strstr(json, "data:")) != NULL) json = d + 5;
        cJSON *root = cJSON_Parse(json);
        cJSON *e = root ? cJSON_GetObjectItem(root, "error") : NULL;
        if (!root) {
            snprintf(s_error, sizeof(s_error), "bad response from HA MCP");
        } else if (e) {
            cJSON *msg = cJSON_GetObjectItem(e, "message");
            snprintf(s_error, sizeof(s_error), "HA MCP: %s", cJSON_IsString(msg) ? msg->valuestring : "error");
            cJSON_Delete(root);
        } else {
            result = cJSON_GetObjectItem(root, "result");
            *root_out = root;
            s_error[0] = 0;
        }
    }
    free(b.buf);
    return result;
}

static bool ha_configured(void)
{
    settings_lock();
    bool ok = g_cfg.ha_enabled && g_cfg.ha_url[0] && g_cfg.ha_token[0];
    settings_unlock();
    return ok;
}

static bool mcp_initialize(void)
{
    s_session[0] = 0;
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "protocolVersion", "2025-03-26");
    cJSON_AddObjectToObject(p, "capabilities");
    cJSON *ci = cJSON_AddObjectToObject(p, "clientInfo");
    cJSON_AddStringToObject(ci, "name", "gemini-echo");
    cJSON_AddStringToObject(ci, "version", "1.4");
    cJSON *root;
    if (!rpc("initialize", p, &root)) return false;
    cJSON_Delete(root);
    rpc("notifications/initialized", NULL, &root);
    cJSON_Delete(root);
    return true;
}

// HA's API prompt (MCP prompts/get): how to use its tools plus a static overview of every exposed
// entity with its area. Without it Gemini has to guess area and device names.
static char *fetch_prompt(void)
{
    cJSON *root, *root2;
    cJSON *res = rpc("prompts/list", NULL, &root);
    cJSON *first = res ? cJSON_GetArrayItem(cJSON_GetObjectItem(res, "prompts"), 0) : NULL;
    cJSON *name = first ? cJSON_GetObjectItem(first, "name") : NULL;
    char *text = NULL;
    if (cJSON_IsString(name)) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", name->valuestring);
        cJSON *got = rpc("prompts/get", p, &root2);
        cJSON *msgs = got ? cJSON_GetObjectItem(got, "messages") : NULL, *m;
        size_t tl = 0;
        cJSON_ArrayForEach(m, msgs) {
            cJSON *t = cJSON_GetObjectItem(cJSON_GetObjectItem(m, "content"), "text");
            if (!cJSON_IsString(t)) continue;
            size_t n = strlen(t->valuestring);
            text = heap_caps_realloc(text, tl + n + 2, MALLOC_CAP_SPIRAM);
            memcpy(text + tl, t->valuestring, n);
            tl += n;
            text[tl++] = '\n';
            text[tl] = 0;
        }
        cJSON_Delete(root2);
    }
    cJSON_Delete(root);
    if (!text) ESP_LOGW(TAG, "no HA prompt: %s", s_error[0] ? s_error : "empty");
    return text;
}

// Fetch HA's tools and convert them to Gemini function declarations.
static bool refresh_tools(void)
{
    if (!mcp_initialize()) return false;
    char *prompt = fetch_prompt();
    cJSON *root;
    cJSON *res = rpc("tools/list", NULL, &root);
    if (!res) {
        free(prompt);
        return false;
    }
    cJSON *decls = cJSON_CreateArray(), *t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItem(res, "tools")) {
        cJSON *name = cJSON_GetObjectItem(t, "name");
        if (!cJSON_IsString(name)) continue;
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "name", name->valuestring);
        cJSON *desc = cJSON_GetObjectItem(t, "description");
        if (cJSON_IsString(desc)) cJSON_AddStringToObject(d, "description", desc->valuestring);
        cJSON *schema = cJSON_GetObjectItem(t, "inputSchema");
        cJSON_AddItemToObject(d, "parametersJsonSchema",
                              schema ? cJSON_Duplicate(schema, true) : cJSON_CreateObject());
        cJSON_AddItemToArray(decls, d);
    }
    cJSON_Delete(root);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_Delete(s_tools);
    s_tools = decls;
    free(s_prompt);
    s_prompt = prompt;
    s_tools_at = esp_timer_get_time();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%d Home Assistant tools available, prompt %d chars", cJSON_GetArraySize(decls),
             prompt ? (int)strlen(prompt) : 0);
    return true;
}

cJSON *ha_function_declarations(void)
{
    if (!ha_configured()) return NULL;
    if (!s_tools || esp_timer_get_time() - s_tools_at > TOOLS_TTL_US) {
        if (!refresh_tools()) ESP_LOGW(TAG, "%s", s_error);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *copy = s_tools && cJSON_GetArraySize(s_tools) ? cJSON_Duplicate(s_tools, true) : NULL;
    xSemaphoreGive(s_lock);
    return copy;
}

char *ha_prompt(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    char *copy = s_prompt ? strdup(s_prompt) : NULL;
    xSemaphoreGive(s_lock);
    return copy;
}

// Runs one tool; returns the function response object for Gemini (caller owns).
cJSON *ha_call_tool(const char *name, cJSON *args)
{
    cJSON *out = cJSON_CreateObject();
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "name", name);
    cJSON_AddItemToObject(p, "arguments", args ? cJSON_Duplicate(args, true) : cJSON_CreateObject());
    cJSON *root;
    cJSON *res = rpc("tools/call", p, &root);
    if (!res && s_session[0] && mcp_initialize()) {   // session expired: re-initialize once
        p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", name);
        cJSON_AddItemToObject(p, "arguments", args ? cJSON_Duplicate(args, true) : cJSON_CreateObject());
        res = rpc("tools/call", p, &root);
    }
    if (!res) {
        cJSON_AddStringToObject(out, "error", s_error);
        return out;
    }
    // Concatenate the text content blocks
    char *text = NULL;
    size_t tl = 0;
    cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(res, "content")) {
        cJSON *t = cJSON_GetObjectItem(c, "text");
        if (!cJSON_IsString(t)) continue;
        size_t n = strlen(t->valuestring);
        text = heap_caps_realloc(text, tl + n + 2, MALLOC_CAP_SPIRAM);
        memcpy(text + tl, t->valuestring, n);
        tl += n;
        text[tl++] = '\n';
        text[tl] = 0;
    }
    bool is_err = cJSON_IsTrue(cJSON_GetObjectItem(res, "isError"));
    cJSON_AddStringToObject(out, is_err ? "error" : "result", text ? text : (is_err ? "failed" : "ok"));
    free(text);
    cJSON_Delete(root);
    return out;
}

// For the web UI: (re)connect and list tool names.
cJSON *ha_test(void)
{
    cJSON *out = cJSON_CreateObject();
    bool ok = ha_configured() && refresh_tools();
    cJSON_AddBoolToObject(out, "ok", ok);
    if (!ok) {
        cJSON_AddStringToObject(out, "error", ha_configured() ? s_error : "set URL and token, and enable it");
        return out;
    }
    cJSON *names = cJSON_AddArrayToObject(out, "tools");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddNumberToObject(out, "prompt_chars", s_prompt ? strlen(s_prompt) : 0);
    cJSON *t;
    cJSON_ArrayForEach(t, s_tools) cJSON_AddItemToArray(names, cJSON_CreateString(cJSON_GetObjectItem(t, "name")->valuestring));
    xSemaphoreGive(s_lock);
    return out;
}

void ha_invalidate(void)
{
    s_tools_at = 0;
}

void ha_init(void)
{
    s_lock = xSemaphoreCreateMutex();
}
