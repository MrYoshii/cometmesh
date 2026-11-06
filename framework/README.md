# Comet's Arduino framework

Comet builds against Arduino-ESP32 **2.0.17** (ESP-IDF **v4.4.7**), the version
wadamesh uses, so the code and its libraries stay as they are. What changes is the
configuration the framework's prebuilt ESP-IDF libraries were compiled with, which
a firmware build cannot change: the Wi-Fi driver, lwIP and mbedTLS are compiled
into the framework, not into Comet.

`defconfig.comet` lists Comet's settings. `.github/workflows/framework.yml` runs
Espressif's `esp32-arduino-lib-builder` (release/v4.4) with them for the
ESP32-S3, drops the rebuilt `tools/sdk/esp32s3` into an Arduino-ESP32 2.0.17
tree, and publishes that as a release asset (`framework-N`), which the board
environments in `platformio.ini` then use through `platform_packages`.

| Setting | Stock 2.0.17 | Comet | Why |
|---|---|---|---|
| `LWIP_TCP_WND_DEFAULT` / `LWIP_TCP_SND_BUF_DEFAULT` | 5760 | 65535 | ~130 KB/s cap on transfers over the internet |
| `LWIP_TCP_RECVMBOX_SIZE` / `TCPIP_RECVMBOX_SIZE` | 6 / 32 | 64 / 64 | room for the larger window |
| Wi-Fi dynamic RX buffers | 32 | 64 | in PSRAM |
| `SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` | off | on | big globals can move to PSRAM |

The TCP numbers follow Espressif's ESP32-S3 throughput example
(`esp-idf/examples/wifi/iperf/sdkconfig.defaults.esp32s3`). Kept as they are: the 8
static RX buffers and the block-ack windows (more would cost internal RAM), the
static TX buffers (dynamic ones are not selectable with `SPIRAM_USE_MALLOC`, which
Arduino needs), and the Wi-Fi IRAM optimisations (throughput bought with internal
RAM).

Already on in the stock framework and kept: Wi-Fi and lwIP buffers in PSRAM
(`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`). mbedTLS allocates internally in the stock
build; Comet already redirects it to PSRAM at runtime
(`mbedtls_platform_set_calloc_free` in `src/main.cpp`).
