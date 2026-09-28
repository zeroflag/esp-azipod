#!/bin/bash

JOY_AZPIPOD_LEFT="ESP Azipod Left"
JOY_AZPIPOD_RIGHT="ESP Azipod Right"

JOY_THROTTLE_LEFT="ESP Throttle Left"
JOY_THROTTLE_RIGHT="ESP Throttle Right"

VS_DIR="$HOME/.wine/drive_c/apps/Virtual Sailor"
UINPUT="/dev/uinput"
THROTTLE_COMMAND=(python ship-throttle.py)

cd "$VS_DIR" || {
  echo "Error: cannot change to directory: $VS_DIR" >&2
  exit 1
}

if [ ! -c "$UINPUT" ]; then
  echo "Error: $UINPUT is not a character device" >&2
  exit 1
fi

sudo chgrp input "$UINPUT" || {
  echo "Error: failed to change group of $UINPUT" >&2
  exit 1
}

sudo chmod 660 "$UINPUT" || {
  echo "Error: failed to change permissions on $UINPUT" >&2
  exit 1
}

"${THROTTLE_COMMAND[@]}" &
THROTTLE_PID=$!

cleanup() {
  trap - EXIT INT TERM
  kill "$THROTTLE_PID" 2>/dev/null
  wait "$THROTTLE_PID" 2>/dev/null
}

trap cleanup EXIT INT TERM


echo "Waiting for Virtual Joysticks.."

for joystick in \
  "$JOY_AZPIPOD_LEFT" \
  "$JOY_AZPIPOD_RIGHT" \
  "$JOY_THROTTLE_LEFT" \
  "$JOY_THROTTLE_RIGHT"
do
  while ! xinput list --name-only | grep -qx "$joystick"; do
    sleep 0.05
  done
  echo "$joystick is ready."

  xinput set-prop "$joystick" "Generate Mouse Events" 0
  xinput set-prop "$joystick" "Generate Key Events" 0
done

#WINEDEBUG=+joystick SDL_JOYSTICK_DEVICE=/dev/input/js0

echo "Starting Virtual Sailor.."
wine vsf_ng.exe 
