#!/usr/bin/env bash
# Build and (optionally) flash a Pocket Voice sketch.
#   ./build.sh p1_hello          compile only
#   ./build.sh p1_hello upload   compile and flash
set -euo pipefail

export PATH="$HOME/.local/bin:$PATH"

SKETCH="${1:?usage: build.sh <sketch-dir> [upload]}"
ACTION="${2:-compile}"
PORT="${PORT:-/dev/cu.usbmodem1101}"
HERE="$(cd "$(dirname "$0")" && pwd)"

# ESP32-S3R8: 8 MB octal PSRAM, 16 MB flash, native USB serial/JTAG.
FQBN="esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,CPUFreq=240,FlashMode=qio,DebugLevel=none"

arduino-cli compile --fqbn "$FQBN" --warnings none "$HERE/$SKETCH"

if [ "$ACTION" = "upload" ]; then
  arduino-cli upload --fqbn "$FQBN" -p "$PORT" "$HERE/$SKETCH"
fi
