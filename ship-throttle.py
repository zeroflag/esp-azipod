#!/usr/bin/env python3

# sudo apt-get install python3-evdev

import socket
from evdev import UInput, AbsInfo, ecodes as e

UDP_PORT = 6589

min_val = 300
max_val = 860

throttle_capabilities = {
  e.EV_KEY: [
    e.BTN_JOYSTICK,
  ],
  e.EV_ABS: [
    (e.ABS_X, AbsInfo(
      value=0,
      # min=0,
      # max=1023,
      min=min_val,
      max=max_val,
      fuzz=0,
      flat=0,
      resolution=0,
    ))
  ],
}

azipod_capabilities = {
  e.EV_KEY: [
    e.BTN_JOYSTICK,
  ],
  e.EV_ABS: [
    (e.ABS_X, AbsInfo(
      value=0,
      min=0,
      max=359,
      fuzz=0,
      flat=0,
      resolution=0,
    ))
  ],
}

ui1 = UInput(azipod_capabilities, name="ESP Azipod Left")
ui2 = UInput(azipod_capabilities, name="ESP Azipod Right")

ui3 = UInput(throttle_capabilities, name="ESP Throttle Left")
ui4 = UInput(throttle_capabilities, name="ESP Throttle Right")

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", UDP_PORT))

print(f"Listening on UDP port {UDP_PORT}...")

try:
  while True:
    data, addr = s.recvfrom(4)

    if len(data) != 4:
      continue

    value = int.from_bytes(data, byteorder="little")
    throttle = value & 0xFFFF
    azipod = 359 - (value >> 16)

    if throttle < min_val:
      throttle = min_val
    elif throttle > max_val:
      throttle = max_val
      
    if throttle >= 510 and throttle <= 620:
      throttle = (max_val - min_val) // 2 + min_val
    else:
      throttle = max_val - throttle + min_val
    

    print("Throttle: %d; Azipod: %d" % (throttle, azipod), flush=True)

    ui1.write(e.EV_ABS, e.ABS_X, azipod)
    ui1.syn()
    ui2.write(e.EV_ABS, e.ABS_X, azipod)
    ui2.syn()

    ui3.write(e.EV_ABS, e.ABS_X, throttle)
    ui3.syn()
    ui4.write(e.EV_ABS, e.ABS_X, throttle)
    ui4.syn()

finally:
  s.close()
  ui1.close()
  ui2.close()
  ui3.close()
  ui4.close()
