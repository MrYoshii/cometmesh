# SPDX-License-Identifier: GPL-3.0-or-later
# Every third-party Arduino library (idf/deps/lib/*, fetched by idf/fetch-deps.sh) and
# LVGL, compiled as one component, so the libraries' includes of each other resolve the
# way they do under PlatformIO. The MeshCore core is not here: it builds with the app
# (components/comet_app), because its ESP32 board code includes the board's target.h.
#
# Layout rules, as PlatformIO applies them: a library with a src/ folder builds src/
# recursively; one without builds its top-level sources (and utility/). Examples, tests
# and tools never build.
get_filename_component(DEPS "${CMAKE_CURRENT_LIST_DIR}/../../deps/lib" ABSOLUTE)

set(_roots "")
file(GLOB _libs LIST_DIRECTORIES true "${DEPS}/*")
foreach(_l ${_libs})
  get_filename_component(_n "${_l}" NAME)
  if(NOT IS_DIRECTORY "${_l}" OR _n STREQUAL "meshcore")
    continue()
  endif()
  if(_n STREQUAL "Crypto")
    set(_l "${_l}/libraries/Crypto")   # rweather/arduinolibs: one library of many
  endif()
  list(APPEND _roots "${_l}")
endforeach()

set(_srcs "")
set(_inc "${DEPS}")   # for <lvgl/lvgl.h>-style includes
foreach(_r ${_roots})
  get_filename_component(_n "${_r}" NAME)
  if(_n STREQUAL "lvgl")
    file(GLOB_RECURSE _s "${_r}/src/*.c")
    list(APPEND _inc "${_r}")
  elseif(IS_DIRECTORY "${_r}/src")
    file(GLOB_RECURSE _s "${_r}/src/*.c" "${_r}/src/*.cpp")
    list(APPEND _inc "${_r}/src" "${_r}")
  else()
    file(GLOB _s "${_r}/*.c" "${_r}/*.cpp" "${_r}/utility/*.c" "${_r}/utility/*.cpp")
    list(APPEND _inc "${_r}")
    if(IS_DIRECTORY "${_r}/utility")
      list(APPEND _inc "${_r}/utility")
    endif()
  endif()
  list(APPEND _srcs ${_s})
endforeach()
list(FILTER _srcs EXCLUDE REGEX "/(examples?|tests?|extras|fontconvert|docs|demos)/")
# RadioLib: only the Arduino HAL (our radio is built as new Module(..., SPIClass&)).
list(FILTER _srcs EXCLUDE REGEX "/RadioLib/src/hal/(ESP-IDF|RPi|RPiPico|Stm32duino|Tock)/")

idf_component_register(
  SRCS ${_srcs}
  INCLUDE_DIRS ${_inc} "${CMAKE_CURRENT_LIST_DIR}/../../../include"
  REQUIRES arduino-esp32 nvs_flash esp_timer esp_lcd esp_adc efuse esp_psram
           esp_driver_spi esp_driver_i2c esp_driver_gpio esp_driver_ledc esp_driver_i2s
           esp_netif esp_wifi esp_event lwip mbedtls)

# Code we consume as a library: no warnings, never fatal.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
