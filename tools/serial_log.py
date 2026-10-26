#!/usr/bin/env python3
"""Read the device's USB serial log for N seconds (non-interactive alternative to `idf.py monitor`).

    python3 tools/serial_log.py /dev/cu.usbmodem1101 30          # just listen
    python3 tools/serial_log.py /dev/cu.usbmodem1101 30 --reset  # reset the chip first
Needs pyserial (bundled in the ESP-IDF python env).
"""
import sys, time
import serial

port, seconds = sys.argv[1], float(sys.argv[2])
s = serial.Serial(port, 115200, timeout=0.2)
if "--reset" in sys.argv:
    s.dtr = False
    s.rts = True
    time.sleep(0.1)
    s.rts = False
end = time.time() + seconds
while time.time() < end:
    data = s.read(4096)
    if data:
        sys.stdout.write(data.decode(errors="replace"))
        sys.stdout.flush()
