#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Fetch everything the Comet ESP-IDF build needs, pinned, from GitHub, into idf/deps/
# (gitignored). Works without the ESP component registry or PlatformIO, so the local
# build and CI build the exact same sources.
#
#   idf/deps/idf/<component>/   ESP-IDF components (arduino-esp32 as a component, NimBLE C++, LittleFS)
#   idf/deps/lib/<library>/     Arduino libraries and the MeshCore core, consumed by the
#                               committed wrappers in idf/components/
#
# Re-run after changing a pin. Already-present checkouts at the right commit are kept.
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(cd .. && pwd)"
DEPS="$PWD/deps"
mkdir -p "$DEPS/idf" "$DEPS/lib"

# name | repo | ref (tag or commit) | dest (idf|lib) | submodules (y|n)
PINS=$(cat <<'EOF'
arduino-esp32            | espressif/arduino-esp32           | 3.3.10        | idf | n
h2zero__esp-nimble-cpp   | h2zero/esp-nimble-cpp             | 2.3.4         | idf | n
joltwallet__littlefs     | joltwallet/esp_littlefs           | v1.23.1       | idf | y
meshcore                 | ALLFATHER-BV/meshcomod            | core-v1.18.0  | lib | n
lvgl                     | lvgl/lvgl                         | v8.4.0        | lib | n
RadioLib                 | jgromes/RadioLib                  | 7.8.1         | lib | n
Crypto                   | rweather/arduinolibs              | 37a76b8f7516568e1c575b6dc9268da1ccaac6b6 | lib | n
RTClib                   | adafruit/RTClib                   | 2.1.4         | lib | n
Adafruit_BusIO           | adafruit/Adafruit_BusIO           | 1.17.4        | lib | n
Adafruit_Sensor          | adafruit/Adafruit_Sensor          | 1.1.15        | lib | n
Adafruit_GFX             | adafruit/Adafruit-GFX-Library     | 1.12.6        | lib | n
Adafruit_TCA8418         | adafruit/Adafruit_TCA8418         | 1.0.2         | lib | n
Melopero_RV3028          | melopero/Melopero_RV-3028_Arduino_Library | 1.2.0 | lib | n
CayenneLPP               | ElectronicCats/CayenneLPP         | 1.6.1         | lib | n
ArduinoJson              | bblanchon/ArduinoJson             | v7.4.3        | lib | n
base64                   | densaugeo/base64_arduino          | 1.3.0         | lib | n
MicroNMEA                | stevemarple/MicroNMEA             | v2.0.6        | lib | n
LovyanGFX                | lovyan03/LovyanGFX                | 1.2.21        | lib | n
SensorLib                | lewisxhe/SensorLib                | v0.3.3        | lib | n
XPowersLib               | lewisxhe/XPowersLib               | v0.3.3        | lib | n
GxEPD2                   | ZinggJM/GxEPD2                    | 1.6.9         | lib | n
CSE_Touch                | CIRCUITSTATE/CSE_Touch            | b44f23b6f870b848f1fbe453c190879bc6cfaafa | lib | n
CSE_CST328               | CIRCUITSTATE/CSE_CST328           | v0.0.4        | lib | n
AsyncTCP                 | ESP32Async/AsyncTCP               | v3.4.10       | lib | n
ESPAsyncWebServer        | ESP32Async/ESPAsyncWebServer      | v3.10.3       | lib | n
PubSubClient             | knolleary/pubsubclient            | v2.8          | lib | n
EOF
)

fetch() {  # name repo ref dest subm
  local name="$1" repo="$2" ref="$3" dest="$4" subm="$5"
  local dir="$DEPS/$dest/$name"
  if [ -f "$dir/.comet-ref" ] && [ "$(cat "$dir/.comet-ref")" = "$repo@$ref" ]; then
    echo "  = $name ($ref)"; return
  fi
  rm -rf "$dir"
  if [[ "$ref" =~ ^[0-9a-f]{40}$ ]]; then
    git init -q "$dir"
    git -C "$dir" fetch -q --depth 1 "https://github.com/$repo" "$ref"
    git -C "$dir" checkout -q FETCH_HEAD
  else
    git -c advice.detachedHead=false clone -q --depth 1 -b "$ref" "https://github.com/$repo" "$dir"
  fi
  [ "$subm" = y ] && git -C "$dir" submodule update -q --init --recursive --depth 1
  rm -rf "$dir/.git"
  echo "$repo@$ref" > "$dir/.comet-ref"
  echo "  + $name ($ref)"
}

echo "fetching into $DEPS"
while IFS='|' read -r name repo ref dest subm; do
  name=$(echo $name); repo=$(echo $repo); ref=$(echo $ref); dest=$(echo $dest); subm=$(echo $subm)
  [ -n "$name" ] && fetch "$name" "$repo" "$ref" "$dest" "$subm"
done <<< "$PINS"

# AsyncElegantOTA lives in the repo (arch/esp32/AsyncElegantOTA).
rm -rf "$DEPS/lib/AsyncElegantOTA"
cp -R "$ROOT/arch/esp32/AsyncElegantOTA" "$DEPS/lib/AsyncElegantOTA"

# ---- Patches. Each one fails the script when the upstream shape changed. ----
python3 "$ROOT/scripts/build/patch_lvgl_anim_uaf.py"  --patch-file "$DEPS/lib/lvgl/src/misc/lv_anim.c"
python3 "$ROOT/scripts/build/patch_lvgl_arabic.py"    --patch-file "$DEPS/lib/lvgl/src/misc/lv_txt_ap.c"
python3 "$ROOT/scripts/build/patch_meshcore_txt_timestamp.py" \
  --patch-file "$DEPS/lib/meshcore/src/helpers/BaseChatMesh.cpp"
python3 "$PWD/patches/apply.py" "$DEPS"

# Generated headers the PlatformIO build makes in its pre: scripts.
python3 "$ROOT/scripts/build/pre_gen_baked.py"

echo "done."
