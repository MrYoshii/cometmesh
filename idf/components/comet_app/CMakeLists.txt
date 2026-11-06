# SPDX-License-Identifier: GPL-3.0-or-later
# Comet itself: the app (src/), the board's variant sources, and the MeshCore core
# (idf/deps/lib/meshcore), with the same source selection as the PlatformIO build
# (platformio.ini build_src_filter, and meshcomod's build_as_lib.py for the core).
get_filename_component(REPO "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
get_filename_component(CORE "${CMAKE_CURRENT_LIST_DIR}/../../deps/lib/meshcore" ABSOLUTE)
idf_build_get_property(_board_inc COMET_BOARD_INCLUDES)
idf_build_get_property(_board_src COMET_BOARD_SOURCES)
idf_build_get_property(_board_exc COMET_BOARD_EXCLUDES)

# ---- MeshCore core (build_as_lib.py for ESP32, minus the files the app ships itself) ----
file(GLOB _core
  "${CORE}/src/*.cpp"
  "${CORE}/src/helpers/*.cpp"
  "${CORE}/src/helpers/radiolib/*.cpp"
  "${CORE}/src/helpers/esp32/*.cpp"
  "${CORE}/src/helpers/wifi/*.cpp"
  "${CORE}/src/helpers/ui/MomentaryButton.cpp"
  "${CORE}/src/helpers/ui/buzzer.cpp")
file(GLOB_RECURSE _core_sensors "${CORE}/src/helpers/sensors/*.cpp")
list(APPEND _core ${_core_sensors})
list(FILTER _core EXCLUDE REGEX "/(LvglPsramAlloc|helpers/TouchDiagTrace|helpers/esp32/ESPNOWRadio)\\.cpp$")
list(FILTER _core EXCLUDE REGEX "/helpers/esp32/(MultiTransportCompanionInterface|SdNvsPrefs|TCPCompanionServer|TouchPrefsStore|WifiRuntimeStore|WebSocketCompanionServer)\\.cpp$")

# ---- The app ----
file(GLOB _app
  "${REPO}/src/*.cpp" "${REPO}/src/*.c"
  "${REPO}/src/helpers/*.cpp"
  "${REPO}/src/helpers/esp32/*.cpp"
  "${REPO}/src/helpers/input/*.cpp"
  "${REPO}/src/helpers/ui/UIColorPalette.cpp"
  "${REPO}/src/ui-touch/*.cpp" "${REPO}/src/ui-touch/*.c"
  "${REPO}/lib/ed25519/*.c"
  "${REPO}/lib/lua/src/*.c")
foreach(_g ${_board_src})
  file(GLOB _s "${REPO}/${_g}")
  list(APPEND _app ${_s})
endforeach()
foreach(_x ${_board_exc})
  list(FILTER _app EXCLUDE REGEX "${_x}")
endforeach()
# lua.c / luac.c are the stand-alone interpreter and compiler (they have main()).
list(FILTER _app EXCLUDE REGEX "/lib/lua/src/(lua|luac|onelua)\\.c$")

set(_inc "${REPO}/src" "${REPO}/src/ui-touch" "${REPO}/include")
foreach(_i ${_board_inc})
  list(APPEND _inc "${REPO}/${_i}")
endforeach()
list(APPEND _inc "${CORE}/src" "${REPO}/lib/ed25519" "${REPO}/lib/lua/src" "${REPO}/lib/minimp3")

idf_component_register(
  SRCS ${_core} ${_app}
  INCLUDE_DIRS ${_inc}
  REQUIRES arduino-esp32 comet_libs h2zero__esp-nimble-cpp bt nvs_flash
           esp_timer esp_partition spi_flash app_update esp_adc esp_pm espcoredump
           esp_wifi esp_netif lwip mbedtls esp_psram esp_lcd efuse
           esp_driver_spi esp_driver_i2c esp_driver_gpio esp_driver_i2s esp_driver_ledc
           esp_driver_uart esp_driver_usb_serial_jtag fatfs spiffs
           joltwallet__littlefs
  # setup() and loop() are only referenced from arduino-esp32's app_main; keep every
  # object (static constructors, weak overrides) as PlatformIO's link does.
  WHOLE_ARCHIVE)

# Wi-Fi credentials header (scripts/inject_wifi_env.py on PlatformIO): defaults, never
# real credentials in a build that leaves this machine.
set(_ws "${CMAKE_CURRENT_BINARY_DIR}/wifi_secrets.h")
if(DEFINED ENV{WIFI_SSID})
  set(_ssid "$ENV{WIFI_SSID}")
else()
  set(_ssid "YourSSID")
endif()
if(DEFINED ENV{WIFI_PWD})
  set(_pwd "$ENV{WIFI_PWD}")
else()
  set(_pwd "YourPassword")
endif()
file(WRITE "${_ws}.tmp" "#ifndef WIFI_SECRETS_H\n#define WIFI_SECRETS_H\n#define WIFI_SSID \"${_ssid}\"\n#define WIFI_PWD \"${_pwd}\"\n#endif\n")
file(COPY_FILE "${_ws}.tmp" "${_ws}" ONLY_IF_DIFFERENT)
target_compile_options(${COMPONENT_LIB} PRIVATE -w -include "${_ws}")
