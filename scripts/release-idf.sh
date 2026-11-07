#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Release the ESP-IDF boards (today: the T-Deck Max) to firmware.cometmesh.org.
#
#   scripts/release-idf.sh beta_1                 build, stage under out/firmware/, publish
#   scripts/release-idf.sh beta_1 --no-push       build and stage only (rsync it yourself)
#   scripts/release-idf.sh beta_1 --promote       copy the tested beta_1 bins to stable
#
# Needs an exported ESP-IDF 5.5 (see idf/README.md) for the build steps, and
# COMET_VPS=user@host to publish (never commit it). COMET_VPS_PATH overrides the
# firmware root (/srv/comet/firmware).
#
# What the device reads (src/ui-touch/UITask.cpp, the update check):
#   http://firmware.cometmesh.org/releases/TOUCH   stable listing (releases/TOUCH/index.json)
#   http://firmware.cometmesh.org/releases/BETA    beta listing   (About > Get test builds)
#   .../releases/<TOUCH|BETA>/beta_<N>/comet-<board id>.bin   the app image it installs
# It offers the highest beta_<N> above its own tag; a dev build (dev-<sha>) is below
# every release.
#
# The PlatformIO boards still go through scripts/release.sh. Boards built here are
# left out of it, so a PlatformIO image is never offered to a device running the
# IDF build.
set -euo pipefail

TAG="${1:?usage: release-idf.sh beta_<N> [--no-push | --promote]}"
MODE="${2:-}"
[[ "$TAG" =~ ^beta_[0-9]+$ ]] || { echo "tag must be beta_<N>, not $TAG"; exit 1; }

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/out/firmware"                       # local mirror of the firmware root
DEST="${COMET_VPS:-}"
DEST_PATH="${COMET_VPS_PATH:-/srv/comet/firmware}"

# idf/boards/<dir> : the published name (comet-<WADA_BOARD_ID>)
BOARDS="tdeck_max:comet-tdeck-max"
title() { case "$1" in comet-tdeck-max) echo "Comet for the LilyGo T-Deck Max";; *) echo "Comet ($1)";; esac; }

listing() {   # regenerate <archive>/index.json, the listing the device scans for beta_<N>
  python3 - "$1" > "$1/index.json" <<'PY'
import os, sys, json
rel = sys.argv[1]
tags = sorted((d for d in os.listdir(rel) if d.startswith("beta_") and os.path.isdir(os.path.join(rel, d))),
              key=lambda d: int(d[5:]))
print(json.dumps([{"name": t, "type": "dir"} for t in tags], indent=2))
PY
  echo "listing $(basename "$1"): $(python3 -c 'import json,sys;print(", ".join(x["name"] for x in json.load(open(sys.argv[1]))))' "$1/index.json")"
}

# The browser installer's manifest (esp-web-tools) for one board: the four images at
# their offsets rather than the merged one, so NVS (settings) and the data partitions
# are left alone; esp-web-tools itself offers to erase everything on a new install.
# Written into the channel feed, pointing at the immutable per-tag files over HTTPS.
manifest() {   # <feed dir> <archive name TOUCH|BETA> <board name> <title>
  python3 - "$1" "$2" "$TAG" "$3" "$4" <<'PY'
import json, os, sys
feed, arch, tag, name, title = sys.argv[1:]
base = "https://firmware.cometmesh.org/releases/%s/%s/%s" % (arch, tag, name)
m = {
  "name": title, "version": tag, "new_install_prompt_erase": True,
  "builds": [{"chipFamily": "ESP32-S3", "parts": [
    {"path": base + "-bootloader.bin", "offset": 0x0},
    {"path": base + "-partitions.bin", "offset": 0x8000},
    {"path": base + "-otadata.bin",    "offset": 0xe000},
    {"path": base + ".bin",            "offset": 0x10000}]}]}
out = os.path.join(feed, "manifest-%s.json" % name[len("comet-"):])
json.dump(m, open(out, "w"), indent=2)
print("manifest:", out)
PY
}

# 1. Start from what is published, so listings stay complete. apps/ is not ours to
#    push (scripts/deploy-apps.sh owns it).
if [ -n "$DEST" ] && [ "$MODE" != "--no-push" ]; then
  mkdir -p "$OUT"
  rsync -a --exclude 'apps/' "$DEST:$DEST_PATH/" "$OUT/" 2>/dev/null || echo "note: nothing published yet"
fi

if [ "$MODE" = "--promote" ]; then
  # 2s. Stable: no rebuild, the bins the testers ran.
  SRC="$OUT/releases/BETA/$TAG"; ARCH="$OUT/releases/TOUCH"; FEED="$OUT/latest"
  [ -d "$SRC" ] || { echo "no $SRC: release $TAG as a beta first"; exit 1; }
  mkdir -p "$ARCH/$TAG" "$FEED"
  for pair in $BOARDS; do
    name="${pair##*:}"
    cp "$SRC/$name".bin "$SRC/$name"-*.bin "$ARCH/$TAG/"
    cp "$SRC/$name.bin" "$SRC/$name-merged.bin" "$FEED/"
    manifest "$FEED" TOUCH "$name" "$(title "$name")"
  done
  listing "$ARCH"
else
  # 2b. Beta: build every IDF board with the tag embedded.
  [ -n "${IDF_PATH:-}" ] || { echo "IDF_PATH is not set (source esp-idf/export.sh)"; exit 1; }
  ARCH="$OUT/releases/BETA"; FEED="$OUT/latest-beta"
  mkdir -p "$ARCH/$TAG" "$FEED"
  for pair in $BOARDS; do
    board="${pair%%:*}"; name="${pair##*:}"
    BUILD="$ROOT/build/idf-$board-release"
    [ -n "${COMET_KEEP_BUILD:-}" ] || rm -rf "$BUILD"
    COMET_FW_TAG="$TAG" COMET_BUILD_SUFFIX="-release" "$ROOT/idf/build.sh" "$board" build
    # The app image must carry the tag, or its About page and update check disagree
    # with the release it is published under.
    # (grep -c, not -q: -q stops reading early and pipefail then fails the pipeline.)
    n="$(strings "$BUILD/comet.bin" | grep -c "$TAG" || true)"
    [ "${n:-0}" -gt 0 ] || { echo "$name: image does not embed $TAG"; exit 1; }
    cp "$BUILD/comet.bin" "$ARCH/$TAG/$name.bin"
    # Merged image for a fresh install at 0x0 (wipes settings; the browser installer).
    (cd "$BUILD" && python -m esptool --chip esp32s3 merge-bin -o "$ARCH/$TAG/$name-merged.bin" @flash_args)
    # The pieces the browser installer writes (see manifest()).
    cp "$BUILD/bootloader/bootloader.bin"            "$ARCH/$TAG/$name-bootloader.bin"
    cp "$BUILD/partition_table/partition-table.bin"  "$ARCH/$TAG/$name-partitions.bin"
    cp "$BUILD/ota_data_initial.bin"                 "$ARCH/$TAG/$name-otadata.bin"
    cp "$ARCH/$TAG/$name.bin" "$ARCH/$TAG/$name-merged.bin" "$FEED/"
    manifest "$FEED" BETA "$name" "$(title "$name") (test build)"
    echo "$name: $(stat -c %s "$ARCH/$TAG/$name.bin") bytes"
  done
  listing "$ARCH"
fi

# 3. Publish.
if [ -n "$DEST" ] && [ "$MODE" != "--no-push" ]; then
  rsync -av --exclude 'apps/' "$OUT/" "$DEST:$DEST_PATH/"
  echo "published $TAG ($([ "$MODE" = "--promote" ] && echo stable || echo beta)) -> $DEST:$DEST_PATH"
else
  echo "staged in $OUT (not published: set COMET_VPS, or rsync -av --exclude apps/ out/firmware/ host:/srv/comet/firmware/)"
fi
