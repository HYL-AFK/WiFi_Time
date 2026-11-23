"""Serial reader that avoids toggling DTR/RTS (so the ESP32-S3 USB-Serial/JTAG
does not get parked in ROM download mode) and logs every line with a timestamp.

Usage: python serial_read.py [port] [seconds] [out_file]
"""

import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
seconds = int(sys.argv[2]) if len(sys.argv) > 2 else 60
out_path = sys.argv[3] if len(sys.argv) > 3 else "d:/Project/WiTime/tmp/serial_read.log"

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 1
ser.dsrdtr = False
ser.rtscts = False
ser.dtr = False
ser.rts = False
ser.open()
ser.dtr = False
ser.rts = False

start = time.time()
lines = 0
with open(out_path, "w", encoding="utf-8", errors="replace") as out:
    out.write("# reading %s for %ds, DTR/RTS held low\n" % (port, seconds))
    out.flush()
    while time.time() - start < seconds:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
        out.write("[%6.1f] %s\n" % (time.time() - start, line))
        out.flush()
        lines += 1
ser.close()
print("lines=%d out=%s" % (lines, out_path))
