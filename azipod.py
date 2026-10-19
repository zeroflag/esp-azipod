#!/usr/bin/env python3

# sudo apt-get install python3-evdev

import socket
from evdev import UInput, AbsInfo, ecodes
import subprocess
import os

UDP_PORT = 6589

PORT_THROTTLE_MIN_VAL = 110
PORT_THROTTLE_MAX_VAL = 860

STARBOARD_THROTTLE_MIN_VAL = 1
STARBOARD_THROTTLE_MAX_VAL = 700

PORT_AZIPOD_NEUTRAL = 103
STARBOARD_AZIPOD_NEUTRAL = 103

VS_DIR="/home/zeroflag/.wine/drive_c/apps/Virtual Sailor"
CLICK_WAV=os.path.join(VS_DIR, "sounds/Click.wav")

class Events:
  def __init__(self, a_dict, sender):
    self.callbacks = a_dict or {}
    self.sender = sender

  def signal(self, event):
    if event in self.callbacks:
      self.callbacks[event](self.sender)

class Throttle:
  def __init__(self, min_val, max_val, invert=False, deadband=30, events=None):
    self.min_val = min_val
    self.max_val = max_val
    self.mid_val = (max_val - min_val) // 2 + min_val
    self.invert = invert
    self._value = self.mid_val
    self.deadband = deadband
    self.neutral = True
    self.events = Events(events, self)

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
        self.events.signal('NEUTRAL')
    else:
      self.neutral = False
    self.events.signal('CHANGED')

  def _clamp(self, val):
    val = max(self.min_val, min(val, self.max_val))
    if abs(val - self.mid_val) <= self.deadband:
      val = self.mid_val
    return val

  def _at_center(self):
    return self._value == self.mid_val

class Rotary:
  def __init__(self, events=None, offset = 0):
    self._angle = 0
    self._offset = offset
    self.events = Events(events, self)

  @property
  def value(self):
    return self._angle

  def move_to(self, val):
    new_value = self._add_offset(self._clamp(val))
    if self._angle != new_value:
      self._angle = new_value
      self.events.signal('CHANGED')

  def _add_offset(self, val):
    return (val + self._offset) % 360 

  def _clamp(self, val):
    return max(0, min(val, 359))

class Azipod:
  def __init__(self, min_val, max_val, suffix, invert_throttle=False, azipod_neutral = 180):
    self.joy_rotary = UInput(self._build_capab(0, 359), name="ESP Azipod %s" % suffix)
    self.joy_throttle = UInput(self._build_capab(min_val, max_val), name="ESP Throttle %s" % suffix)
    self.throttle = Throttle(
      min_val=min_val,
      max_val=max_val,
      invert=invert_throttle,
      events={'NEUTRAL' : lambda t: subprocess.Popen(["aplay", CLICK_WAV]),
              'CHANGED' : self._throttle_changed})
    self.rotary = Rotary(
      events={'CHANGED' : self._rotary_changed},
      offset=180 - azipod_neutral)

  def _build_capab(self, min_val, max_val):
    return {
      ecodes.EV_KEY: [ ecodes.BTN_JOYSTICK ],
      ecodes.EV_ABS: [
        (ecodes.ABS_X, AbsInfo(
          value=0,
          min=min_val,
          max=max_val,
          fuzz=0,
          flat=0,
          resolution=0,
        ))
      ]
    }

  def move_to(self, packet):
    self.throttle.move_to(packet.throttle)
    self.rotary.move_to(packet.angle)

  def close(self):
    self.joy_rotary.close()
    self.joy_throttle.close()

  def _throttle_changed(self, sender):
    self._set_joy_x(self.joy_throttle, sender.value)

  def _rotary_changed(self, sender):
    self._set_joy_x(self.joy_rotary, sender.value)

  def _set_joy_x(self, joy, val):
    print("%s: %d" % (joy.name, val))
    joy.write(ecodes.EV_ABS, ecodes.ABS_X, val)
    joy.syn()

class Packet:
  def __init__(self, val):
    self.throttle = val & 0x3FF
    self.angle = (val >> 10) & 0x1FF
    self.packet_id = (val >> 19) & 0xFFF
    self.starboard = bool(val & 0x80000000)

  def __str__(self):
    return "Seq: %d; Throttle: %d; Angle: %d; SB: %s" % (self.packet_id, self.throttle, self.angle, self.starboard)

class Server:
  def __init__(self, port, events=None, logging=False):
    self.events = Events(events, self)
    self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    self.socket.bind(("0.0.0.0", port))
    self.packet = Packet(0)
    self.last_packet_id_starboard = None
    self.last_packet_id_port = None
    self.logging = logging
    print(f"Listening on UDP port {port}...")

  def start(self):
    while True:
      val, addr = self.socket.recvfrom(4)
      if len(val) != 4:
        continue
      self._process(Packet(int.from_bytes(val, byteorder="little")))

  def _process(self, packet):
    self.packet = packet
    self._log_packet(packet)
    if packet.starboard:
      delta = self._delta(packet.packet_id, self.last_packet_id_starboard)
      if self._check_delta(delta):
        self.last_packet_id_starboard = packet.packet_id
        self.events.signal('ON_DATA_STARBOARD')
    else:
      delta = self._delta(packet.packet_id, self.last_packet_id_port)
      if self._check_delta(delta):
        self.last_packet_id_port = packet.packet_id
        self.events.signal('ON_DATA_PORT')

  def _log_packet(self, packet):
    if self.logging:
      print(packet)
    
  def _check_delta(self, delta):
    if delta == 1:
      return True
    elif 1 < delta < 0x800:
      print(f"Missing {delta - 1} packet(s)")
      return True
    else:
      return False
        
  def _delta(self, seq_num, previous_seq_num):
    if previous_seq_num is None:
      return 1
    return (seq_num - previous_seq_num) & 0xFFF

  def close(self):
    self.socket.close()

def main_loop():
  port_azipod      = Azipod(PORT_THROTTLE_MIN_VAL,
                            PORT_THROTTLE_MAX_VAL,
                            "Left",
                            invert_throttle=True,
                            azipod_neutral=PORT_AZIPOD_NEUTRAL)

  starboard_azipod = Azipod(STARBOARD_THROTTLE_MIN_VAL,
                            STARBOARD_THROTTLE_MAX_VAL,
                            "Right",
                            invert_throttle=False,
                            azipod_neutral=STARBOARD_AZIPOD_NEUTRAL)

  server = Server(UDP_PORT, events={
    'ON_DATA_STARBOARD': lambda server: starboard_azipod.move_to(server.packet),
    'ON_DATA_PORT': lambda server: port_azipod.move_to(server.packet)
  }, logging=True)

  try:
    server.start()
  finally:
    server.close()
    port_azipod.close()
    starboard_azipod.close()
  
if __name__ == '__main__':
  main_loop()
