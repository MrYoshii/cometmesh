#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Comet release: build every board in deploy/site/boards.json, stage the images under
# out/firmware/ (a mirror of firmware.cometmesh.org), publish.
#
#   scripts/release.sh beta_2                  build all boards, publish to the beta channel
#   scripts/release.sh beta_2 --no-push        build and stage only
#   scripts/release.sh beta_2 --promote        copy the tested beta_2 images to stable
#   COMET_BOARDS="tdeck-max" scripts/release.sh beta_2    only these boards (ids)
#
# Needs, per board kind: ESP-IDF 5.5 exported (IDF_PATH) for "idf" boards, PlatformIO
# (pio, or $PIO) for "pio" boards; esptool for the merged images; COMET_VPS=user@host
# (or an ~/.ssh/config alias) to publish, never committed. COMET_VPS_PATH overrides
# the firmware root (/srv/comet/firmware).
#
# On the server (deploy/nginx/firmware.cometmesh.org.conf):
#   releases/<BETA|TOUCH>/<tag>/comet-<id>.bin          app image, what a device self-updates to
#   releases/<BETA|TOUCH>/<tag>/comet-<id>-{bootloader,partitions,otadata}.bin
#   releases/<BETA|TOUCH>/<tag>/comet-<id>-merged.bin   everything at 0x0 (erases settings)
#   releases/<BETA|TOUCH>/index-<id>.json               the tags that have an image for <id>;
#                                                       the device's update check reads this
#   releases/<BETA|TOUCH>/index.json                    every tag
#   latest-beta/, latest/  manifest-<id>.json           the browser installer (esp-web-tools),
#                                                       pointing at that board's newest tag
# Boards are released independently: a tag that leaves a board out is simply not in
# that board's listing. Tags are beta_<N>; a dev build (dev-<sha>) is below all of them.
set -euo pipefail

TAG="${1:?usage: release.sh beta_<N> [--no-push | --promote]}"
MODE="${2:-}"
[[ "$TAG" =~ ^beta_[0-9]+$ ]] || { echo "tag must be beta_<N>, not $TAG"; exit 1; }

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BOARDS_JSON="$ROOT/deploy/site/boards.json"
OUT="$ROOT/out/firmware"
DEST="${COMET_VPS:-}"
DEST_PATH="${COMET_VPS_PATH:-/srv/comet/firmware}"
PIO="${PIO:-pio}"
PUSH=1; [ "$MODE" = "--no-push" ] && PUSH=0
[ -n "$DEST" ] || PUSH=0

# "id kind target chip" per board, from boards.json (optionally only $COMET_BOARDS).
boards() {
  python3 - "$BOARDS_JSON" "${COMET_BOARDS:-}" <<'PY'
import json, sys
only = set(sys.argv[2].split())
for b in json.load(open(sys.argv[1]))["boards"]:
    if only and b["id"] not in only:
        continue
    (kind, target), = b["build"].items()
    print(b["id"], kind, target, b.get("chip", "ESP32-S3"))
PY
}
board_name() { python3 -c 'import json,sys; print(next(b["name"] for b in json.load(open(sys.argv[1]))["boards"] if b["id"]==sys.argv[2]))' "$BOARDS_JSON" "$1"; }

esptool_cmd() {
  if command -v esptool >/dev/null; then echo esptool
  elif command -v esptool.py >/dev/null; then echo esptool.py
  else echo "python3 -m esptool"; fi
}
merge() {   # <out> <bootloader> <partitions> <otadata> <app>
  local e; e="$(esptool_cmd)"
  $e --chip esp32s3 merge-bin -o "$1" 0x0 "$2" 0x8000 "$3" 0xe000 "$4" 0x10000 "$5" >/dev/null 2>&1 ||
  $e --chip esp32s3 merge_bin -o "$1" 0x0 "$2" 0x8000 "$3" 0xe000 "$4" 0x10000 "$5" >/dev/null
}

# Build one board with the tag embedded; leaves the four images in $STAGED/<id>-*.
build_board() {   # <id> <kind> <target>
  local id="$1" kind="$2" target="$3" b
  case "$kind" in
    idf)
      [ -n "${IDF_PATH:-}" ] || { echo "$id: IDF_PATH is not set (source esp-idf/export.sh)"; exit 1; }
      b="$ROOT/build/idf-$target-release"
      [ -n "${COMET_KEEP_BUILD:-}" ] || rm -rf "$b"
      COMET_FW_TAG="$TAG" COMET_BUILD_SUFFIX="-release" "$ROOT/idf/build.sh" "$target" build
      cp "$b/comet.bin"                         "$STAGED/$id.app"
      cp "$b/bootloader/bootloader.bin"         "$STAGED/$id.bootloader"
      cp "$b/partition_table/partition-table.bin" "$STAGED/$id.partitions"
      cp "$b/ota_data_initial.bin"              "$STAGED/$id.otadata"
      ;;
    pio)
      command -v "$PIO" >/dev/null || { echo "$id: PlatformIO ($PIO) not found"; exit 1; }
      PLATFORMIO_BUILD_FLAGS="-DFIRMWARE_RELEASE_TAG='\"$TAG\"' -DFIRMWARE_VERSION='\"$TAG\"'" \
        "$PIO" run -d "$ROOT" -e "$target"
      b="$ROOT/.pio/build/$target"
      local app0
      app0="$(ls "$HOME"/.platformio/packages/framework-arduinoespressif32*/tools/partitions/boot_app0.bin 2>/dev/null | head -1)"
      [ -n "$app0" ] || { echo "$id: boot_app0.bin not found in the PlatformIO framework"; exit 1; }
      cp "$b/firmware.bin"   "$STAGED/$id.app"
      cp "$b/bootloader.bin" "$STAGED/$id.bootloader"
      cp "$b/partitions.bin" "$STAGED/$id.partitions"
      cp "$app0"             "$STAGED/$id.otadata"
      ;;
    *) echo "$id: unknown build kind $kind"; exit 1 ;;
  esac
  # The image must carry the tag, or its About page and update check disagree with the
  # release it is published under. (grep -c: -q stops early and trips pipefail.)
  local n; n="$(strings "$STAGED/$id.app" | grep -c "$TAG" || true)"
  [ "${n:-0}" -gt 0 ] || { echo "$id: image does not embed $TAG"; exit 1; }
}

# Per-board listings, the all-tags listing and the installer manifests of one channel.
index_channel() {   # <TOUCH|BETA> <feed dir>
  python3 - "$OUT/releases/$1" "$1" "$2" "$BOARDS_JSON" <<'PY'
import json, os, sys
arch, chan, feed, boards = sys.argv[1:]
os.makedirs(arch, exist_ok=True); os.makedirs(feed, exist_ok=True)
tags = sorted((d for d in os.listdir(arch) if d.startswith("beta_") and os.path.isdir(os.path.join(arch, d))),
              key=lambda d: int(d[5:]))
dump = lambda p, v: json.dump(v, open(p, "w"), indent=2)
dump(os.path.join(arch, "index.json"), [{"name": t, "type": "dir"} for t in tags])
for b in json.load(open(boards))["boards"]:
    bid = b["id"]
    have = [t for t in tags if os.path.exists(os.path.join(arch, t, "comet-%s.bin" % bid))]
    dump(os.path.join(arch, "index-%s.json" % bid), [{"name": t, "type": "dir"} for t in have])
    man = os.path.join(feed, "manifest-%s.json" % bid)
    if not have:
        if os.path.exists(man): os.remove(man)
        continue
    t = have[-1]
    base = "https://firmware.cometmesh.org/releases/%s/%s/comet-%s" % (chan, t, bid)
    dump(man, {
        "name": "Comet for the %s%s" % (b["name"], " (test build)" if chan == "BETA" else ""),
        "version": t, "new_install_prompt_erase": True,
        "builds": [{"chipFamily": b.get("chip", "ESP32-S3"), "parts": [
            {"path": base + "-bootloader.bin", "offset": 0x0},
            {"path": base + "-partitions.bin", "offset": 0x8000},
            {"path": base + "-otadata.bin",    "offset": 0xe000},
            {"path": base + ".bin",            "offset": 0x10000}]}]})
    print("%s %-20s %s" % (chan, bid, ", ".join(have)))
PY
}

# 1. Start from what is published, so the listings stay complete. apps/ belongs to
#    scripts/deploy-apps.sh.
if [ "$PUSH" = 1 ]; then
  mkdir -p "$OUT"
  rsync -a --exclude 'apps/' "$DEST:$DEST_PATH/" "$OUT/" 2>/dev/null || echo "note: nothing published yet"
fi

if [ "$MODE" = "--promote" ]; then
  # 2s. Stable gets the very images the testers ran, no rebuild.
  SRC="$OUT/releases/BETA/$TAG"
  [ -d "$SRC" ] || { echo "no $SRC: release $TAG as a beta first"; exit 1; }
  mkdir -p "$OUT/releases/TOUCH/$TAG"
  while read -r id _ _ _; do
    if [ -f "$SRC/comet-$id.bin" ]; then cp "$SRC/comet-$id"*.bin "$OUT/releases/TOUCH/$TAG/"
    else echo "note: $TAG has no image for $id"; fi
  done < <(boards)
  index_channel TOUCH "$OUT/latest"
  CHAN=stable
else
  # 2b. Beta: build each board.
  ARCH="$OUT/releases/BETA/$TAG"
  mkdir -p "$ARCH"
  STAGED="$(mktemp -d)"; trap 'rm -rf "$STAGED"' EXIT
  while read -r id kind target _; do
    echo "== $id ($kind $target)"
    build_board "$id" "$kind" "$target"
    cp "$STAGED/$id.app"        "$ARCH/comet-$id.bin"
    cp "$STAGED/$id.bootloader" "$ARCH/comet-$id-bootloader.bin"
    cp "$STAGED/$id.partitions" "$ARCH/comet-$id-partitions.bin"
    cp "$STAGED/$id.otadata"    "$ARCH/comet-$id-otadata.bin"
    merge "$ARCH/comet-$id-merged.bin" "$STAGED/$id.bootloader" "$STAGED/$id.partitions" \
          "$STAGED/$id.otadata" "$STAGED/$id.app"
    echo "$id: $(stat -c %s "$ARCH/comet-$id.bin") bytes"
  done < <(boards)
  index_channel BETA "$OUT/latest-beta"
  index_channel TOUCH "$OUT/latest"   # so every board has a stable listing, empty or not
  CHAN=beta
fi

# 3. Publish.
if [ "$PUSH" = 1 ]; then
  rsync -av --exclude 'apps/' "$OUT/" "$DEST:$DEST_PATH/"
  echo "published $TAG ($CHAN) -> $DEST:$DEST_PATH"
else
  echo "staged in $OUT; publish with: rsync -av --exclude apps/ out/firmware/ <host>:$DEST_PATH/"
fi
