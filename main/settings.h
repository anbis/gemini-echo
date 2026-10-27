#pragma once
#include <stdbool.h>

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char api_key[128];
    char model[64];
    char voice[32];
    char language[32];
    char prompt[2048];
    char wake1[32];
    char wake2[32];      // "" = only one wake word
    int  volume;         // 0..100
    int  mic_gain;       // dB
    int  idle_timeout;   // s
    int  echo_tail_ms;
    bool search;
    // v1.2: Home Assistant via MCP (appended so older saved settings still load)
    bool ha_enabled;
    char ha_url[128];
    char ha_token[384];
    // v1.3. Appended fields must be int-aligned: a bool here would land in the old blob's
    // tail padding and load as 0 instead of its default.
    int  listen_beep;    // bip when the mic opens after Gemini spoke
    // v1.4: conversation memory (memory.c)
    int  memory;            // keep a summary of past conversations
    char memory_model[64];  // text model that writes the summary
} settings_t;

extern settings_t g_cfg;

void settings_load(void);
void settings_save(void);
void settings_lock(void);
void settings_unlock(void);
