#!/usr/bin/env python3
"""Install firmware over Wi-Fi (OTA) and wait until the device is back.

    python3 tools/ota.py [host] [image]

host   defaults to gemini-echo.local (an IP works too)
image  defaults to build/gemini-echo.bin

Only the app is updated; settings are kept. Wake word models and the partition table
still need a full USB flash (idf.py flash).
"""
import http.client
import json
import os
import socket
import sys
import time


def resolve(host):
    for _ in range(5):   # the first .local lookup often fails while mDNS warms up
        try:
            return socket.gethostbyname(host)
        except socket.gaierror:
            time.sleep(1)
    sys.exit(f"can't resolve {host}")


def status(ip):
    c = http.client.HTTPConnection(ip, 80, timeout=5)
    c.request("GET", "/api/status")
    return json.loads(c.getresponse().read())


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "gemini-echo.local"
    image = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "build", "gemini-echo.bin")
    with open(image, "rb") as f:
        data = f.read()

    ip = resolve(host)
    before = status(ip)
    print(f"{host} ({ip}): firmware {before['firmware']}, {before['state']}")
    if before["state"] != "idle":
        sys.exit("device is in a conversation, try again when it's idle")

    c = http.client.HTTPConnection(ip, 80, timeout=60)
    c.putrequest("POST", "/api/ota")
    c.putheader("Content-Type", "application/octet-stream")
    c.putheader("Content-Length", str(len(data)))
    c.endheaders()
    t0 = time.time()
    step = 16384
    for off in range(0, len(data), step):
        c.send(data[off:off + step])
        done = min(off + step, len(data))
        print(f"\ruploading {done * 100 // len(data):3d}%  {done // 1024} KB", end="", flush=True)
    print(f"  ({time.time() - t0:.0f} s)")
    r = json.loads(c.getresponse().read())
    if not r.get("ok"):
        sys.exit(f"update failed: {r.get('error')}")
    print(f"installed {r['version']}, restarting…")

    time.sleep(6)
    for _ in range(30):
        try:
            s = status(ip)
        except OSError:
            time.sleep(1)
            continue
        if s["firmware"] != r["version"]:
            sys.exit(f"device came back with {s['firmware']}: the new firmware failed to start and was rolled back")
        print(f"back online: firmware {s['firmware']}")
        return
    sys.exit("device didn't come back - check the serial log")


if __name__ == "__main__":
    main()
