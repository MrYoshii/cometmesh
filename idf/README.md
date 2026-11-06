# Comet on ESP-IDF

Comet's new framework base: **ESP-IDF 5.5.1** with **arduino-esp32 3.3.10** as an
IDF component. The shared code in `src/` builds unchanged on both this and the
PlatformIO build (Arduino-ESP32 2.0.17 / IDF 4.4), which stays the reference until
this build replaces it.

Why: IDF 4.4 is end of life, and its prebuilt Arduino libraries fix the settings
that matter most on a 512 KB-SRAM chip: TCP windows of 5.7 KB (downloads top out
near 130 KB/s), TLS buffers in internal RAM, no PSRAM for large statics. Here they
are ours to set (`sdkconfig.defaults`), and the newer building blocks (esp_modem,
TinyUSB mass storage, NimBLE 2, LVGL 9) are within reach.

## Layout

| Path | What |
|---|---|
| `CMakeLists.txt` | the project; picks the board, sets the shared defines |
| `sdkconfig.defaults` | settings every board shares, each one explained |
| `boards/<board>/board.cmake` | the board's defines, include dirs and sources |
| `boards/<board>/sdkconfig.defaults` | the board's chip, flash and partition table |
| `components/comet_app` | `src/`, the board's variant sources, the MeshCore core |
| `components/comet_libs` | every third-party Arduino library, and LVGL |
| `main` | anchors the project; `app_main` is arduino-esp32's |
| `fetch-deps.sh` | fetches every dependency, pinned, from GitHub into `deps/` |
| `patches/apply.py` | the few source patches to those dependencies |

No component registry and no PlatformIO are involved: the build uses only what
`fetch-deps.sh` put in `deps/`, so a local build and CI build the same sources.

## Build

```bash
# once: ESP-IDF v5.5.1 (https://docs.espressif.com/projects/esp-idf/en/v5.5.1/)
. $IDF_PATH/export.sh
idf/fetch-deps.sh
idf/build.sh tdeck_max          # -> build/idf-tdeck_max/comet.bin
idf/build.sh tdeck_max size     # memory map
```

## Flash

Over an existing Comet install (keeps settings, contacts and chats):

```bash
esptool --chip esp32s3 --port <PORT> write-flash 0xe000 ota_data_initial.bin 0x10000 comet.bin
```

`ota_data_initial.bin` makes the device boot the slot just written. The bootloader
already on the device runs this app. A first install also writes
`0x0 bootloader.bin` and `0x8000 partition-table.bin`.

## Boards

| Board | Status |
|---|---|
| `tdeck_max` | LilyGo T-Deck Max: builds; first hardware tests |

A new board is a folder in `boards/` (see `tdeck_max`) and its variant sources.
