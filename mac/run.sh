#!/bin/bash
# Start the Mac Monitor sender. The first run creates a Python environment with psutil + pyserial.
cd "$(dirname "$0")"
if [ ! -x .venv/bin/python ]; then
  echo "First run: setting up Python environment..."
  python3 -m venv .venv && .venv/bin/pip install --quiet psutil pyserial || exit 1
fi
exec .venv/bin/python -u mac_monitor.py
