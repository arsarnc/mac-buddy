#!/bin/bash
# Save a PNG of every Mac Monitor page into ESP32-Screen/screenshots/.
# Needs the sender (run.sh) running: it owns the USB port, so it takes the pictures.
cd "$(dirname "$0")"
DONE=../screenshots/.done
mkdir -p ../screenshots && rm -f "$DONE"
if ! pkill -USR1 -f mac_monitor.py; then
  echo "The sender isn't running. Start it first: $(pwd)/run.sh"
  exit 1
fi
echo "Capturing all pages..."
for _ in $(seq 1 300); do
  if [ -f "$DONE" ]; then
    cat "$DONE"; echo; rm -f "$DONE"
    exit 0
  fi
  sleep 1
done
echo "Timed out waiting for the screenshots."
exit 1
