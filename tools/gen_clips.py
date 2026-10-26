#!/usr/bin/env python3
"""Re-record the spoken error warnings (main/clips/*.pcm) in any language/voice with Gemini Live.

    pip install websockets
    GEMINI_API_KEY=... python3 tools/gen_clips.py --voice Puck --lang en
Output: 16 kHz mono 16-bit little-endian PCM, as embedded by main/CMakeLists.txt.
"""
import argparse, asyncio, base64, json, os, sys
from array import array
import websockets

TEXTS = {
    "uk": {
        "quota": "Вибач, ліміт запитів до Google вичерпано. Спробуй пізніше або перевір тарифний план в Ей-Ай Студіо.",
        "apikey": "Не вдалося підключитися: ключ API Google недійсний. Перевір ключ у налаштуваннях.",
        "wifi": "Немає підключення до мережі. Перевір вай-фай.",
        "connect": "Не вдалося з'єднатися з Gemini. Спробуй ще раз трохи пізніше.",
    },
    "en": {
        "quota": "Sorry, the Google API quota is used up. Try again later or check your plan in AI Studio.",
        "apikey": "I can't connect: the Google API key is not valid. Please check it in the settings.",
        "wifi": "There's no network connection. Please check the Wi-Fi.",
        "connect": "I couldn't reach Gemini. Please try again a bit later.",
    },
}


def resample_24k_to_16k(pcm: bytes) -> bytes:
    """Linear 3:2 resample after a 3-tap smoothing filter (fine for speech prompts)."""
    x = array("h", pcm)
    sm = [(x[max(i - 1, 0)] + 2 * x[i] + x[min(i + 1, len(x) - 1)]) / 4 for i in range(len(x))]
    out = array("h")
    pos = 0.0
    while pos < len(sm) - 1:
        i = int(pos)
        f = pos - i
        out.append(int(sm[i] * (1 - f) + sm[i + 1] * f))
        pos += 1.5
    return out.tobytes()


async def record(ws_url, model, voice, text):
    async with websockets.connect(ws_url, max_size=None) as ws:
        await ws.send(json.dumps({"setup": {
            "model": f"models/{model}",
            "generationConfig": {"responseModalities": ["AUDIO"],
                                 "speechConfig": {"voiceConfig": {"prebuiltVoiceConfig": {"voiceName": voice}}}},
            "systemInstruction": {"parts": [{"text": "You are a voice prompt recorder. Read the user's text aloud "
                                                     "exactly as written, calmly and clearly, nothing else."}]}}}))
        await ws.recv()
        await ws.send(json.dumps({"realtimeInput": {"text": text}}))
        pcm = b""
        while True:
            sc = json.loads(await asyncio.wait_for(ws.recv(), 30)).get("serverContent", {})
            for p in sc.get("modelTurn", {}).get("parts", []):
                if "inlineData" in p:
                    pcm += base64.b64decode(p["inlineData"]["data"])
            if sc.get("turnComplete"):
                return pcm


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--voice", default="Puck")
    ap.add_argument("--lang", default="en", choices=TEXTS)
    ap.add_argument("--model", default="gemini-3.1-flash-live-preview")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "main", "clips"))
    a = ap.parse_args()
    key = os.environ.get("GEMINI_API_KEY") or sys.exit("set GEMINI_API_KEY")
    url = ("wss://generativelanguage.googleapis.com/ws/"
           f"google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key={key}")
    for name, text in TEXTS[a.lang].items():
        pcm = resample_24k_to_16k(await record(url, a.model, a.voice, text))
        with open(os.path.join(a.out, f"{name}.pcm"), "wb") as f:
            f.write(pcm)
        print(f"{name}: {len(pcm) / 32000:.1f}s")

asyncio.run(main())
