# SPDX-License-Identifier: GPL-3.0-or-later
# Comet firmware, ESP-IDF 5.5 build (arduino-esp32 3.3 as an IDF component).
#
#   idf/fetch-deps.sh                 once, and after changing a pin
#   idf/build.sh <board> [idf.py args] e.g. idf/build.sh tdeck_max
#
# One project for every board: idf/boards/<board>/ holds the board's defines, sources and
# sdkconfig fragment; the build directory is per board (build/idf-<board>).
cmake_minimum_required(VERSION 3.16)

if(NOT COMET_BOARD)
  set(COMET_BOARD tdeck_max)
endif()
set(COMET_BOARD "${COMET_BOARD}" CACHE STRING "Comet board (idf/boards/<name>)")
get_filename_component(COMET_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(COMET_BOARD_DIR "${CMAKE_CURRENT_LIST_DIR}/boards/${COMET_BOARD}")
if(NOT EXISTS "${COMET_BOARD_DIR}/board.cmake")
  message(FATAL_ERROR "Unknown board '${COMET_BOARD}' (no idf/boards/${COMET_BOARD}/board.cmake)")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/deps/idf/arduino-esp32")
  message(FATAL_ERROR "Dependencies missing: run idf/fetch-deps.sh")
endif()

include("${COMET_BOARD_DIR}/board.cmake")

set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${COMET_BOARD_DIR}/sdkconfig.defaults")
# Test builds: COMET_SDKCONFIG_EXTRA=<file> layers one more fragment on top (build.sh
# gives such a build its own directory through COMET_BUILD_SUFFIX).
if(DEFINED ENV{COMET_SDKCONFIG_EXTRA})
  list(APPEND SDKCONFIG_DEFAULTS "$ENV{COMET_SDKCONFIG_EXTRA}")
endif()
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/deps/idf")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ---- Board and firmware identity, visible to every component (as on PlatformIO). ----
if(NOT COMET_FW_TAG)
  set(COMET_FW_TAG "dev")
endif()
set(COMET_DEFS
  ${COMET_BOARD_DEFS}
  NDEBUG ESP32_PLATFORM MC_VENDORED_TOUCH_APP CORE_DEBUG_LEVEL=1
  RADIOLIB_STATIC_ONLY=1 RADIOLIB_GODMODE=1
  RADIOLIB_EXCLUDE_CC1101=1 RADIOLIB_EXCLUDE_RF69=1 RADIOLIB_EXCLUDE_SX1231=1
  RADIOLIB_EXCLUDE_SI443X=1 RADIOLIB_EXCLUDE_RFM2X=1 RADIOLIB_EXCLUDE_SX128X=1
  RADIOLIB_EXCLUDE_AFSK=1 RADIOLIB_EXCLUDE_AX25=1 RADIOLIB_EXCLUDE_HELLSCHREIBER=1
  RADIOLIB_EXCLUDE_MORSE=1 RADIOLIB_EXCLUDE_APRS=1 RADIOLIB_EXCLUDE_BELL=1
  RADIOLIB_EXCLUDE_RTTY=1 RADIOLIB_EXCLUDE_SSTV=1
  LV_CONF_INCLUDE_SIMPLE=1 LV_MEMCPY_MEMSET_STD=1
  MAX_CONTACTS=2000 MAX_GROUP_CHANNELS=40
  MULTI_TRANSPORT_COMPANION=1 WADAMESH_FORK_BUILD=1 TCP_PORT=5000 WS_PORT=8765
  BLE_PIN_CODE=123456 ENABLE_PRIVATE_KEY_IMPORT=1 ENABLE_PRIVATE_KEY_EXPORT=1
  ENABLE_ADVERT_ON_BOOT=0
  COMET_IDF_BUILD=1
  # AsyncTCP's service task feeds the task watchdog it failed to join, which IDF 5 logs
  # on every event ("esp_task_wdt_reset(707): task not found"). The web server's task
  # needs no watchdog of its own.
  CONFIG_ASYNC_TCP_USE_WDT=0)
foreach(_d ${COMET_DEFS})
  idf_build_set_property(COMPILE_DEFINITIONS "${_d}" APPEND)
endforeach()
idf_build_set_property(COMPILE_DEFINITIONS "FIRMWARE_OTA_ENV=\"${COMET_BOARD_ENV}\"" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "ADMIN_PASSWORD=\"password\"" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "FIRMWARE_RELEASE_TAG=\"${COMET_FW_TAG}\"" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "FIRMWARE_VERSION=\"${COMET_FW_TAG}\"" APPEND)

# Board info for the components (custom build properties).
idf_build_set_property(COMET_ROOT "${COMET_ROOT}")
idf_build_set_property(COMET_BOARD "${COMET_BOARD}")
idf_build_set_property(COMET_BOARD_INCLUDES "${COMET_BOARD_INCLUDES}")
idf_build_set_property(COMET_BOARD_SOURCES "${COMET_BOARD_SOURCES}")
idf_build_set_property(COMET_BOARD_EXCLUDES "${COMET_BOARD_EXCLUDES}")

# The shared code and the vendored libraries trip IDF's -Werror=all; keep warnings,
# never fail on them (the PlatformIO build does not use -Werror either).
idf_build_get_property(_co COMPILE_OPTIONS)
list(REMOVE_ITEM _co "-Werror=all")
idf_build_set_property(COMPILE_OPTIONS "${_co}")
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17" APPEND)

project(comet)
