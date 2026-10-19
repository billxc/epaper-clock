#!/usr/bin/env python3
"""Set RTC from host UTC epoch while the USB-connected firmware is running."""
import argparse
import time

import serial

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("port", help="USB Serial/JTAG port, e.g. /dev/ttyACM0 or COM5")
args = parser.parse_args()

with serial.Serial(args.port, 115200, timeout=0.2) as port:
    deadline = time.monotonic() + 20
    received = bytearray()
    while time.monotonic() < deadline:
        port.write(f"TIME {int(time.time())}\n".encode())
        received.extend(port.read(256))
        if b"TIME_OK" in received:
            print("RTC set from host time. Firmware applies its configured time zone.")
            break
    else:
        raise SystemExit("No TIME_OK: restart/power on with USB data connected, close other serial tools, and retry.")
