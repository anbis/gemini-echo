# CLAUDE.md — Gemini Echo

Firmware for **M5Stack Atom EchoS3R** (ESP32-S3-PICO-1-N8R8): a standalone voice assistant that streams
mic audio to the **Gemini Live API** (native audio in/out — deliberately no STT/TTS anywhere) and plays
Gemini's own voice back. Optional Home Assistant control via HA's built-in MCP server.
Personal setup (IPs, accounts, device state, open items) is in `CLAUDE.local.md` (git-ignored).

## Build / flash

ESP-IDF **v5.5.1**, installed at `~/esp/esp-idf` (tools in `~/.espressif`). Target `esp32s3`.

```sh
. ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbmodem1101 app-flash   # code/web/clip changes (fast, keeps settings)
idf.py -p /dev/cu.usbmodem1101 flash       # FULL: needed after wake-word model, partition or bootloader changes
python3 tools/ota.py [host] [image]        # app update over Wi-Fi (POST /api/ota), waits for reboot
python3 tools/serial_log.py /dev/cu.usbmodem1101 30 --reset   # read logs non-interactively
```

**New Mac setup**
1. `git clone -b v5.5.1 --recursive --depth 1 --shallow-submodules https://github.com/espressif/esp-idf.git ~/esp/esp-idf`
2. `cd ~/esp/esp-idf && ./install.sh esp32s3`
3. macOS: install.sh skips cmake/ninja → `. ./export.sh && python tools/idf_tools.py install cmake ninja`
4. In the project (iCloud doesn't sync `*.nosync`, so the symlink targets are missing on a fresh device):
   `mkdir -p build.nosync managed_components.nosync` — `build` and `managed_components` are symlinks to them.
5. `main/secrets.h` (git-ignored, but synced by iCloud) gives build-time defaults; template `main/secrets.example.h`.

Changing `sdkconfig.defaults` → `rm sdkconfig` then rebuild. Firmware version: `CONFIG_APP_PROJECT_VER`.
Device serial port appears as `/dev/cu.usbmodem*` (USB-Serial/JTAG); USB product "USB JTAG_serial debug unit".

## Hardware (verified against M5Unified source, board_M5AtomVoiceS3R)

ES8311 I2C SDA 45 / SCL 0 (addr 0x18 = `ES8311_CODEC_DEFAULT_ADDR` 0x30 8-bit) · I2S MCLK 11, BCLK 17,
WS 3, DOUT 48, DIN 4 · NS4150B amp enable 18 · button 41 (active low) · no LED · 8 MB flash, 8 MB octal PSRAM.

## Architecture

| File | Role |
|---|---|
| `main/main.c` | Wi-Fi (STA, fallback hotspot), mDNS, button, session state machine IDLE→CONNECTING→ACTIVE |
| `main/audio.c` | ES8311 via esp_codec_dev, full-duplex I2S loop, playback ring, 24→16 kHz resampler, tones, clips |
| `main/wake.c` | ESP-SR AFE ("MR": AEC→VAD→AGC→WakeNet9), mic lock, feeds uplink |
| `main/gemini.c` | Live WebSocket session: setup, uplink task, downlink audio, toolCall → HA, error classification |
| `main/ha.c` | MCP client for HA `/api/mcp` (JSON-RPC: initialize, tools/list, tools/call; JSON or SSE replies) |
| `main/web.c` + `main/web/index.html` | web UI + JSON API (`/api/config`, `/status`, `/action`, `/models`, `/test_model`, `/ha_test`, `/ota`) |
| `main/settings.c` | `settings_t` blob in NVS; defaults from `config.h` ← `secrets.h` |
| `main/clips/*.pcm` | spoken warnings, 16 kHz mono s16le (Ukrainian, Puck voice); `tools/gen_clips.py` regenerates |

Partitions (`partitions.csv`): `ota_0`/`ota_1` 2.875 MB each (image ~2.3 MB), `model` 2.125 MB (srmodels ~2.0 MB:
6 WakeNet9 + vadnet1, ~290 KB per model). NVS stays at 0x9000 so settings survive repartitioning.

## Key decisions (don't undo without reason)

- **I2S 16 kHz stereo; ES8311 `no_dac_ref=false`** puts the DAC output on the right capture slot →
  sample-aligned AEC reference. Gemini's 24 kHz output is resampled to 16 kHz (2-phase windowed sinc).
- **Mic lock instead of voice barge-in**: speaker and mic are millimetres apart; at full volume residual echo
  after AEC reached 6–10k mean level and Gemini kept interrupting itself. While playback is pending
  (+`echo_tail_ms`), silence is sent. Only in ACTIVE state — locking during the wake beep lost the first words.
  The user explicitly asked for this lock. Button while Gemini talks = `gemini_cancel_turn()` (drops the rest
  of the turn until `turnComplete`/`interrupted`).
- **Wake words = pre-trained WakeNet9** (user's choice). There is no "Gemini" model. MultiNet7 custom phrase
  "gemini" was tried first: probabilities only 0.15–0.25 and decoys stole real wakes.
- **Prebuffer**: mic audio is buffered from wake until `setupComplete` (~1.5 s) plus the VAD cache, so
  "Jarvis, what's…" works in one breath.
- **Google Search tool off by default**: with it, 3.x Live models fail at setup with a quota error on free tier.
- **Settings struct is append-only** (no version bump): older shorter NVS blobs load as a prefix over defaults.
  Never reorder/remove fields in `settings_t`. Append only int-aligned fields (`int`, not `bool`): a small field
  lands in the old blob's tail padding and loads as 0 instead of its default.
- **HA tools** are sent as `functionDeclarations` with `parametersJsonSchema` (MCP `inputSchema` verbatim —
  verified accepted by Live). Tool results go back as `toolResponse.functionResponses[].response`.
  Tool list cached 10 min, warmed at boot, invalidated on settings save.
- **HA prompt** (MCP `prompts/list` → `prompts/get`, cached with the tools) is appended to Gemini's
  systemInstruction. It carries HA's tool-usage rules and the "Static Context" YAML of every exposed entity with
  its area (~11.5 KB here). Without it Gemini guessed area/device names and HassTurnOn failed with "not found".
  Gemini can only control entities **exposed to Assist**; names like "Підсвітка" repeat across areas.
- **Error → spoken clip** via `gemini_set_error()` text classification: quota/exceeded/429 → quota clip,
  api key/permission/400/401/403 → key clip, else connect clip; no Wi-Fi → wifi clip.
- **OTA** (v1.3): raw app image POSTed to `/api/ota`; the first 4 KB block is checked (image magic, chip, project
  name), then written with `OTA_WITH_SEQUENTIAL_WRITES`. Bootloader rollback is enabled; `confirm_firmware()`
  in main.c marks the image valid once Wi-Fi/setup-AP and the web UI are up. Wake events are ignored during upload.
  To fit two app slots, wake words were cut from 14 to 6 (user's choice: Alexa default + a few).
- **Listen bip** (`TONE_LISTEN`, setting `listen_beep`): played when the mic lock releases in ACTIVE.
  `play_reply_seq()` counts 24 kHz (Gemini) pushes; only Gemini audio newer than the last unlock can lock the mic,
  so the bip itself doesn't re-lock it.
- Web UI has **no login** — the user considers the LAN trusted and declined adding protections.

## Gemini Live API notes

- `wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=KEY`
- Frames from Google arrive as **binary** WebSocket frames containing JSON; messages up to ~60 KB.
- Up: `{"realtimeInput":{"audio":{"mimeType":"audio/pcm;rate=16000","data":b64}}}` every 100 ms.
- Down: `serverContent.modelTurn.parts[].inlineData` = `audio/pcm;rate=24000`; also `interrupted`,
  `turnComplete`, `inputTranscription`/`outputTranscription` (enabled for logs only), `toolCall`, `goAway`.
- Errors come as a WebSocket close frame with the reason text (e.g. "You exceeded your current quota…",
  "API key not valid…") — `close_status_code` is often 0.
- List Live models: `GET /v1beta/models?key=…`, filter `supportedGenerationMethods` contains `bidiGenerateContent`.
  `tools/live_test.py` does this from a Mac.

## Testing tips

- Speak to the device from the Mac: `say -a <id> "Jarvis"` — `say -a '?'` lists outputs; the default output
  may be Bluetooth headphones, so pick the built-in speakers explicitly.
- Logs show `you:` / `gemini:` transcriptions, `mic locked/open`, `tool call …`, `session error: …`.
- API from the Mac: `curl http://gemini-echo.local/api/status`; first `.local` lookup can fail (cold mDNS) — retry or `curl -4`.
- Test a spoken error safely: POST a bad `api_key` to `/api/config`, POST `{"action":"talk"}`, then restore the key.

## Gotchas

- An interrupted USB flash leaves the app slot unbootable → just `app-flash` again. A failed OTA leaves the running
  slot untouched.
- `app-flash` writes the slot `idf.py` thinks is the boot slot (ota_0); after an OTA the device may be running ota_1
  and `otadata` keeps pointing there. If an `app-flash` "doesn't take", use a full `flash` (rewrites otadata).
- After `idf.py set-target`/fresh clone the component manager downloads esp-sr (~160 MB) on first configure.
- `afe_config_init` with wakenet models: override `wakenet_model_name(_2)` with names that pass `esp_srmodel_exists() >= 0`.
- ESP-SR WakeNet labels are raw (`Hi,Joy`, `Hi,Wall,E or Hi,瓦力`) — the web UI prettifies them.
- Free internal RAM is ~48 KB idle with web server + mDNS; keep big buffers in PSRAM (`MALLOC_CAP_SPIRAM`).
