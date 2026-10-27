#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum { APP_IDLE, APP_CONNECTING, APP_ACTIVE } app_state_t;

typedef enum {
    EV_WAKE,            // wake phrase heard
    EV_BUTTON,          // button pressed
    EV_SESSION_READY,   // setupComplete received
    EV_SESSION_CLOSED,  // websocket closed / failed
} app_event_t;

extern volatile app_state_t g_state;
extern volatile int64_t g_last_voice_us;   // last time VAD saw user speech
extern volatile int64_t g_last_play_us;    // last time speaker played non-silence
extern volatile int64_t g_last_server_us;  // last message from Gemini

void app_post(app_event_t ev);

// ---- ring buffer of int16 samples (thread-safe) ----
typedef struct rb rb_t;
rb_t  *rb_create(size_t cap);
size_t rb_write(rb_t *rb, const int16_t *src, size_t n);
size_t rb_read(rb_t *rb, int16_t *dst, size_t n);
size_t rb_avail(rb_t *rb);
void   rb_clear(rb_t *rb);

// ---- audio.c ----
typedef void (*audio_feed_cb_t)(const int16_t *stereo, int frames);
void   audio_init(void);
void   audio_start(int frames_per_chunk, audio_feed_cb_t cb);
void   play_push_24k(const int16_t *pcm, int n);
void   play_push_16k(const int16_t *pcm, int n);
void   play_clear(void);
size_t play_pending(void);
uint32_t play_reply_seq(void);   // bumps whenever Gemini audio is queued
typedef enum { TONE_WAKE, TONE_END, TONE_ERROR, TONE_BOOT, TONE_LISTEN } tone_t;
void   play_tone(tone_t t);
typedef enum { CLIP_QUOTA, CLIP_APIKEY, CLIP_WIFI, CLIP_CONNECT } clip_t;
void   play_clip(clip_t c);   // pre-recorded spoken warnings
void   audio_set_volume(int volume);
void   audio_set_mic_gain(int db);

// ---- wake.c ----
int    sr_init(void);                 // returns AFE feed chunk (frames)
void   sr_feed(const int16_t *stereo, int frames);
void   sr_start(void);
void   sr_rearm(void);
int    sr_wake_models(const char **names, const char **words, int max);   // bundled WakeNet models

// ---- ha.c ----
typedef struct cJSON cJSON;
void   ha_init(void);
cJSON *ha_function_declarations(void);   // NULL if HA not configured/reachable
char  *ha_prompt(void);                  // HA's tool instructions + areas/devices overview (caller frees), or NULL
cJSON *ha_call_tool(const char *name, cJSON *args);
cJSON *ha_test(void);
void   ha_invalidate(void);

// ---- web.c ----
void   web_start(void);
const char *app_state_name(void);
bool   app_wifi_is_ap(void);
bool   web_ota_active(void);       // firmware upload in progress
bool   app_today(char *buf, size_t size);   // "2026-10-27, Tuesday" (UTC), false until the clock is set

// ---- gemini.c ----
void   gemini_init(void);
bool   gemini_start(void);
void   gemini_stop(void);
void   gemini_uplink_push(const int16_t *pcm, int n);
void   gemini_cancel_turn(void);   // drop the rest of the current reply
void   gemini_set_error(const char *msg, int len);
typedef enum { ERR_NONE, ERR_QUOTA, ERR_APIKEY, ERR_CONNECT } gemini_err_t;
gemini_err_t gemini_error_kind(void);
const char *gemini_last_error(void);

// ---- memory.c ----
typedef enum { MEM_USER = 1, MEM_ASSISTANT = 2, MEM_TOOL = 3 } memory_role_t;
void   memory_init(void);
void   memory_note(memory_role_t role, const char *text);   // transcript of the session in progress
void   memory_session_end(void);   // queue the transcript for summarizing
char  *memory_context(void);       // text for the system instruction (caller frees), or NULL
void   memory_clear(void);
cJSON *memory_status(void);
