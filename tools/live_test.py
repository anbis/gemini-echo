#!/usr/bin/env python3
"""Check which Gemini Live models your API key can use, and that they answer with native audio.

    pip install websockets
    GEMINI_API_KEY=... python3 tools/live_test.py              # list Live models and test each
    GEMINI_API_KEY=... python3 tools/live_test.py MODEL [...]  # test specific models
"""
import asyncio, base64, json, os, sys, time, urllib.request
import websockets

KEY = os.environ.get("GEMINI_API_KEY") or sys.exit("set GEMINI_API_KEY")
WS = ("wss://generativelanguage.googleapis.com/ws/"
      f"google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key={KEY}")


def live_models():
    url = f"https://generativelanguage.googleapis.com/v1beta/models?pageSize=1000&key={KEY}"
    data = json.load(urllib.request.urlopen(url))
    return [m["name"].removeprefix("models/") for m in data["models"]
            if "bidiGenerateContent" in m.get("supportedGenerationMethods", [])]


async def test(model, voice="Puck"):
    t0 = time.time()
    async with websockets.connect(WS, max_size=None) as ws:
        await ws.send(json.dumps({"setup": {
            "model": f"models/{model}",
            "generationConfig": {"responseModalities": ["AUDIO"],
                                 "speechConfig": {"voiceConfig": {"prebuiltVoiceConfig": {"voiceName": voice}}}}}}))
        await ws.recv()
        ready = time.time() - t0
        await ws.send(json.dumps({"realtimeInput": {"text": "Say hello in five words."}}))
        audio = 0
        while True:
            msg = json.loads(await asyncio.wait_for(ws.recv(), 20))
            sc = msg.get("serverContent", {})
            for p in sc.get("modelTurn", {}).get("parts", []):
                if "inlineData" in p:
                    audio += len(base64.b64decode(p["inlineData"]["data"]))
            if sc.get("turnComplete"):
                break
    return f"ok   setup {ready:.1f}s, {audio / 48000:.1f}s of 24 kHz audio"


async def main():
    for m in sys.argv[1:] or live_models():
        try:
            print(f"{m:50s}", await test(m))
        except Exception as e:
            print(f"{m:50s} FAIL {str(e)[:120]}")

asyncio.run(main())
