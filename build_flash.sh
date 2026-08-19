#!/bin/bash
# Build and OTA-flash ESP32_AdBlocker.
#
# Enforces the workflow discipline that was missing when a WiFi fix got lost
# on an abandoned branch for weeks (see TROUBLESHOOTING.md / git log): this
# repo now has exactly one branch (main), and this script refuses to build
# from anywhere else, or with uncommitted changes, so a fix can never again
# be silently left off the branch that actually gets flashed.
set -euo pipefail
cd "$(dirname "$0")"

DEVICE_IP="${1:-192.168.0.27}"
FQBN="esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=default_8MB,FlashSize=8M"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

branch="$(git rev-parse --abbrev-ref HEAD)"
if [ "$branch" != "main" ]; then
  echo "ERROR: on branch '$branch', not 'main'. This project keeps a single branch -" >&2
  echo "       merge/switch to main before building, don't flash from a side branch." >&2
  exit 1
fi

if [ -n "$(git status --porcelain)" ]; then
  echo "ERROR: working tree has uncommitted changes. Commit first - flashing" >&2
  echo "       uncommitted-only changes is exactly how the setSleep fix got lost before." >&2
  git status --short >&2
  exit 1
fi

echo "== Building ($FQBN) =="
arduino-cli compile --fqbn "$FQBN" --output-dir "$BUILD_DIR" .

BIN="$BUILD_DIR/ESP32_AdBlocker.ino.bin"
echo "== Flashing $BIN to http://$DEVICE_IP via OTA =="
curl -sf -m 8 "http://$DEVICE_IP/control?startOTA=firmware.bin" > /dev/null
curl -sf -m 60 -X POST "http://$DEVICE_IP/upload" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @"$BIN"
echo ""
echo "== Waiting for device to come back online =="
for i in $(seq 1 20); do
  if curl -s -m 2 -o /dev/null "http://$DEVICE_IP/"; then
    echo "Back online after ${i}x3s."
    exit 0
  fi
  sleep 3
done
echo "WARNING: device did not respond within 60s - check it manually." >&2
exit 1
