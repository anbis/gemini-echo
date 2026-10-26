#pragma once
// Optional build-time defaults. Copy to secrets.h (git-ignored) and fill in.
// Everything here can also be set later in the web UI; without it the device
// starts the "Gemini-Echo-Setup" hotspot on first boot.
#define WIFI_SSID       "your-2.4GHz-wifi"
#define WIFI_PASS       "your-wifi-password"
#define GEMINI_API_KEY  "your-google-ai-studio-key"
#define GEMINI_LANGUAGE "English"            // or "auto"
#define HA_URL          "http://192.168.1.10:8123"
