#!/usr/bin/env python3

# sudo apt-get install python3-evdev

import socket
from evdev import UInput, AbsInfo, ecodes as e

UDP_PORT = 6589

capabilities = {
  e.EV_KEY: [
    e.BTN_JOYSTICK,
  ],
  e.EV_ABS: [
    (e.ABS_X, AbsInfo(
      value=0,
      min=0,
      max=1023,
      fuzz=0,
      flat=0,
      resolution=0,
    ))
  ],
}

ui1 = UInput(capabilities, name="ESP Azipod Left")
ui2 = UInput(capabilities, name="ESP Azipod Right")

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", UDP_PORT))

print(f"Listening on UDP port {UDP_PORT}...")

try:
  while True:
    data, addr = s.recvfrom(4)

    if len(data) != 4:
      continue

    value = int.from_bytes(data, byteorder="little")

    print(value, flush=True)

    ui1.write(e.EV_ABS, e.ABS_X, value)
    ui1.syn()

    ui2.write(e.EV_ABS, e.ABS_X, value)
    ui2.syn()

finally:
  s.close()
  ui1.close()
  ui2.close()
