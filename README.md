# Gemini Echo

Standalone **Gemini Live** voice assistant firmware for the **M5Stack Atom EchoS3R** (ESP32-S3).

Say a wake word and talk. Your voice streams straight to Google's native-audio Live models and Gemini
answers in its own voice — no speech-to-text, no text-to-speech, no Home Assistant or server in between.
Optionally it controls your Home Assistant through HA's built-in MCP server.

## Features

- **Native voice-to-voice** over the Gemini Live API (WebSocket, 16 kHz PCM up / 24 kHz PCM down)
- **On-device wake word** with ESP-SR WakeNet9 — 6 bundled (Alexa, Jarvis, Computer, Hi ESP, Sophia,
  Mycroft), any two active — plus the **button**
- **Natural conversation**: follow-ups need no wake word; words spoken right after the wake word aren't lost
- **No self-hearing**: the mic is muted while Gemini talks; press the button to cut a reply.
  A short bip says when the mic is open again (can be turned off)
- **OTA firmware updates** from the web UI or `tools/ota.py`, with automatic rollback if the new firmware fails to start
- **Web UI** at `http://gemini-echo.local`: model picker with live model list and per-model test, 30 voices,
  reply language, system prompt, Google Search, wake words, volume, mic gain, timeouts, Wi-Fi, API key
- **Home Assistant control** via the built-in MCP Server integration (Assist tools as Gemini functions)
- **Spoken error messages** (quota exceeded, invalid key, no Wi-Fi, can't connect)
- **Setup hotspot** when Wi-Fi isn't configured or can't be joined

## Hardware

M5Stack **Atom EchoS3R** (ESP32-S3-PICO-1-N8R8: 8 MB flash, 8 MB octal PSRAM, ES8311 codec, NS4150B amp).

| Function | GPIO |
|---|---|
| ES8311 I2C (addr 0x18) | SDA 45, SCL 0 |
| I2S | MCLK 11, BCLK 17, WS 3, DOUT 48, DIN 4 |
| Amplifier enable | 18 |
| Button | 41 (active low) |

## Build & flash

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) 5.4+ (tested with 5.5.1).

```sh
. ~/esp/esp-idf/export.sh
cp main/secrets.example.h main/secrets.h   # optional: build-time Wi-Fi / API key / HA URL
idf.py set-target esp32s3
idf.py -p /dev/ttyACM0 flash monitor       # first time: also flashes the wake word models
idf.py -p /dev/ttyACM0 app-flash           # later code-only changes over USB
python3 tools/ota.py                       # ...or over Wi-Fi (uploads build/gemini-echo.bin)
```

The first USB `flash` writes the partition table (two OTA app slots + wake word models). After that, app
updates can go over Wi-Fi: `tools/ota.py [host] [image]`, or **Firmware update** in the web UI. Settings
are kept. A new image that resets before it finishes booting is rolled back automatically. Wake word models
and the partition table can only be changed with a full USB `flash`.

Components (ESP-SR, esp_codec_dev, esp_websocket_client, mDNS) are fetched automatically by the
IDF component manager.

## First start

1. Without `secrets.h` the device opens Wi-Fi hotspot **Gemini-Echo-Setup** (password `geminiecho`).
   Join it and open **http://192.168.4.1**.
2. Enter your 2.4 GHz Wi-Fi and a **Gemini API key** from [Google AI Studio](https://aistudio.google.com/apikey), save.
3. After the restart open **http://gemini-echo.local**, pick model, voice and language.

## Using it

| Action | Result |
|---|---|
| Say the wake word / press button | rising beep → talk |
| Talk while Gemini is silent | it answers; keep talking, no wake word needed |
| Short bip after Gemini's reply | mic is open again, your turn |
| Press button while Gemini talks | cuts the reply, mic opens |
| Press button while Gemini is silent | ends the conversation |
| 20 s of silence | conversation ends (falling beep) |

## Home Assistant

1. HA → Settings → Devices & services → Add integration → **Model Context Protocol Server** (LLM API: Assist).
2. HA → admin profile → Security → **Long-lived access tokens** → create.
3. Web UI → Home Assistant: enable, URL `http://<ha-ip>:8123`, paste token, save, **Test connection**.

Gemini can only see and control entities exposed to Assist. Requests go device → HA on your LAN.

## How it works

```
mic ─► ES8311 ─► I2S 16 kHz stereo (L: mic, R: DAC loopback)
          └─► ESP-SR AFE: AEC ─► VAD ─► AGC ─► WakeNet9
                                   │ wake
                                   ▼
          Gemini Live WebSocket ◄── 16 kHz PCM (muted while Gemini speaks)
                 │  24 kHz PCM            ▲ toolResponse
                 ▼                        │
          resample 24→16 kHz ─► speaker   └── Home Assistant MCP (/api/mcp) ◄── toolCall
```

| File | Role |
|---|---|
| `main/audio.c` | codec, full-duplex I2S, playback buffer, resampler, tones, spoken clips |
| `main/wake.c` | ESP-SR audio front-end, wake word, mic lock |
| `main/gemini.c` | Live API session, uplink, playback, tool calls, error classification |
| `main/ha.c` | Home Assistant MCP client |
| `main/web.c`, `main/web/index.html` | web UI, JSON API, OTA upload |
| `main/settings.c` | settings in NVS (defaults from `config.h` / `secrets.h`) |
| `main/clips/*.pcm` | spoken warnings (Ukrainian; regenerate with `tools/gen_clips.py`) |

## Tools

```sh
pip install websockets
GEMINI_API_KEY=... python3 tools/live_test.py              # which Live models work with your key
GEMINI_API_KEY=... python3 tools/gen_clips.py --lang en    # re-record warnings in another language
python3 tools/serial_log.py /dev/ttyACM0 30 --reset         # read device logs
python3 tools/ota.py gemini-echo.local                      # firmware update over Wi-Fi
```

## Notes

- The web UI has no login — keep the device on a trusted network.
- Google Search grounding uses a separate quota on the free tier and fails on some Live models.
- Speaker and mic sit millimetres apart, so echo cancellation alone can't stop Gemini hearing itself
  at full volume — hence the mic lock instead of voice barge-in.

## License

MIT
