#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build Comet with ESP-IDF for one board:   idf/build.sh <board> [idf.py command ...]
#   idf/build.sh tdeck_max            -> build/idf-tdeck_max/comet.bin (+ bootloader, partitions)
#   idf/build.sh tdeck_max menuconfig
# Needs an exported ESP-IDF 5.5 (IDF_PATH set) and idf/fetch-deps.sh run once.
# The registry is not used: every component comes from idf/deps/.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BOARD="${1:-tdeck_max}"; shift || true
[ -n "${IDF_PATH:-}" ] || { echo "IDF_PATH is not set (source esp-idf/export.sh)"; exit 1; }
[ -d "$HERE/boards/$BOARD" ] || { echo "unknown board: $BOARD (see idf/boards/)"; exit 1; }
[ -d "$HERE/deps/idf/arduino-esp32" ] || "$HERE/fetch-deps.sh"

TAG="${COMET_FW_TAG:-dev-$(git -C "$ROOT" rev-parse --short=7 HEAD 2>/dev/null || echo local)}"
BUILD="$ROOT/build/idf-$BOARD${COMET_BUILD_SUFFIX:-}"
export IDF_COMPONENT_MANAGER=0
idf.py -C "$HERE" -B "$BUILD" \
  -DCOMET_BOARD="$BOARD" -DCOMET_FW_TAG="$TAG" -DSDKCONFIG="$BUILD/sdkconfig" \
  "${@:-build}"
