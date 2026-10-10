#!/usr/bin/env python3

# sudo apt-get install python3-evdev

import socket
from evdev import UInput, AbsInfo, ecodes as e
import subprocess
import os

UDP_PORT = 6589

PORT_THROTTLE_MIN_VAL = 250
PORT_THROTTLE_MAX_VAL = 1024

VS_DIR="/home/zeroflag/.wine/drive_c/apps/Virtual Sailor"
CLICK_WAV=os.path.join(VS_DIR, "sounds/Click.wav")

throttle_capabilities = {
  e.EV_KEY: [
    e.BTN_JOYSTICK,
  ],
  e.EV_ABS: [
    (e.ABS_X, AbsInfo(
      value=0,
      # min=0,
      # max=1023,
      min=PORT_THROTTLE_MIN_VAL,
      max=PORT_THROTTLE_MAX_VAL,
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

class Throttle:
  def __init__(self, min_val, max_val, invert=False, deadband=30, events=None):
    self.min_val = min_val
    self.max_val = max_val
    self.mid_val = (max_val - min_val) // 2 + min_val
    self.invert = invert
    self._value = self.mid_val
    self.deadband = deadband
    self.neutral = True
    self.events = events

  @property
  def value(self):
    if self.invert:
      return self.min_val + self.max_val - self._value
    return self._value

  def move_to(self, val):
    new_value = self._clamp(val)
    if self._value != new_value:
      self._update(new_value)

  def _update(self, new_value):
    self._value = new_value
    if self._at_center():
      if not self.neutral:
        self.neutral = True
        self._signal('NEUTRAL')
    else:
      self.neutral = False
    self._signal('CHANGED')

  def _clamp(self, val):
    val = max(self.min_val, min(val, self.max_val))
    if abs(val - self.mid_val) <= self.deadband:
      val = self.mid_val
    return val

  def _at_center(self):
    return self._value == self.mid_val

  def _signal(self, event):
    if self.events and event in self.events:
      self.events[event](self)

def set_joy_x(joy, val):
  print("JOY %s: %d" % (joy.name, val))
  joy.write(e.EV_ABS, e.ABS_X, val)
  joy.syn()

port_throttle = Throttle(
  min_val=PORT_THROTTLE_MIN_VAL,
  max_val=PORT_THROTTLE_MAX_VAL,
  invert=True,
  events={'NEUTRAL' : lambda t: subprocess.Popen(["aplay", CLICK_WAV]),
          'CHANGED' : lambda t: set_joy_x(ui3, t.value)})

try:
  while True:
    data, addr = s.recvfrom(4)

    if len(data) != 4:
      continue

    value = int.from_bytes(data, byteorder="little")
    port_throttle.move_to(value & 0xFFFF) 
    azipod = value >> 16

    if azipod < 0:
      azipod = 0
    else:
      azipod = 359 - azipod

#    ui1.write(e.EV_ABS, e.ABS_X, azipod)
#    ui1.syn()
#    ui2.write(e.EV_ABS, e.ABS_X, azipod)
#    ui2.syn()

finally:
  s.close()
  ui1.close()
  ui2.close()
  ui3.close()
  ui4.close()
