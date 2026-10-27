#pragma once

// Values below are factory defaults. Everything except the pins can be changed
// at runtime in the web UI (http://gemini-echo.local); those settings live in NVS.

#if __has_include("secrets.h")
#include "secrets.h"   // personal defaults, git-ignored (see secrets.example.h)
#endif

// ---- Wi-Fi ----
#ifndef WIFI_SSID
#define WIFI_SSID       ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS       ""
#endif

// ---- Gemini Live (native audio in / native audio out, no STT/TTS) ----
#ifndef GEMINI_API_KEY
#define GEMINI_API_KEY  ""
#endif
#define GEMINI_MODEL    "gemini-3.1-flash-live-preview"
#define GEMINI_VOICE    "Puck"
// Google Search grounding; hits a separate quota on the free tier
#define GEMINI_ENABLE_SEARCH 0
#ifndef GEMINI_LANGUAGE
#define GEMINI_LANGUAGE "auto"   // "auto" = reply in the language the user speaks
#endif
#define GEMINI_SYSTEM_PROMPT \
    "You are Gemini, a voice assistant living in a small smart speaker. " \
    "Speak naturally, warmly and briefly, like in a real conversation: usually one to three sentences. " \
    "Never use markdown, lists or emoji - your reply is only ever heard out loud."

// ---- Behaviour ----
#define SESSION_IDLE_TIMEOUT_S   20    // close the Live session after this much silence
#define SESSION_CONNECT_TIMEOUT_S 12
#define SPEAKER_VOLUME           100    // 0..100
#define MIC_GAIN_DB              20.0f
// Mic is locked (silence sent) while Gemini talks, plus this tail for room echo.
// Press the button while it talks to cut the reply and speak.
#define ECHO_TAIL_MS             600
// Short "bip" when the mic opens again after Gemini spoke: your turn to talk
#define LISTEN_BEEP              1
// Conversation memory: after each conversation a text model folds its transcript into a short
// summary (NVS), which the next session gets in its system instruction. Clear it in the web UI.
#define MEMORY_ENABLED           1
#define MEMORY_MODEL             "gemini-flash-lite-latest"

// Wake words: ESP-SR pre-trained WakeNet9 models bundled via sdkconfig.defaults
// (CONFIG_SR_WN_WN9_*); up to two of them are active, picked in the web UI.
#define WAKE_MODEL_1  "wn9_alexa"
#define WAKE_MODEL_2  ""

// Fallback hotspot when the configured Wi-Fi can't be joined
#define SETUP_AP_SSID "Gemini-Echo-Setup"
#define SETUP_AP_PASS "geminiecho"
#define HOSTNAME      "gemini-echo"

// Home Assistant (built-in MCP Server integration); token is set in the web UI
#ifndef HA_URL
#define HA_URL        ""
#endif

// ---- Atom EchoS3R pins ----
#define PIN_I2C_SDA   45
#define PIN_I2C_SCL   0
#define PIN_I2S_MCLK  11
#define PIN_I2S_BCLK  17
#define PIN_I2S_WS    3
#define PIN_I2S_DOUT  48
#define PIN_I2S_DIN   4
#define PIN_PA_EN     18
#define PIN_BUTTON    41

#define AUDIO_RATE    16000
