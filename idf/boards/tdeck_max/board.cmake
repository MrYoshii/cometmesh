# SPDX-License-Identifier: GPL-3.0-or-later
# LilyGo T-Deck Max: ESP32-S3 (16 MB flash, 8 MB QSPI PSRAM), 3.1" e-paper (GDEQ031T10),
# SX1262, A7682E modem, ES8311 codec, CST328 touch, TCA8418 keyboard, XL9555 expander.
#
# Read by idf/CMakeLists.txt before project(). Everything here mirrors the PlatformIO env
# LilyGo_TDeck_Max_companion_radio_touch, which stays the reference until this build
# replaces it.

set(COMET_BOARD_NAME "LilyGo T-Deck Max")
set(COMET_BOARD_ENV  "LilyGo_TDeck_Max_companion_radio_touch")   # update channel / OTA name
set(COMET_PARTITIONS "variants/lilygo_tdeck_max/partitions_tdeck_max.csv")

# Repo-relative include dirs, searched in this order: the Max variant first (target.h,
# TDeckMaxBoard.h), then the Pro variant for the display and touch drivers it reuses,
# then the Pager variant for the shared ES8311 codec and DRV2605 drivers.
set(COMET_BOARD_INCLUDES
  variants/lilygo_tdeck_max
  variants/lilygo_tdeck_pro
  variants/lilygo_tlora_pager)

# Board sources, repo-relative globs.
set(COMET_BOARD_SOURCES
  variants/lilygo_tdeck_max/*.cpp
  variants/lilygo_tdeck_pro/TDeckProDisplay.cpp
  variants/lilygo_tdeck_pro/TDeckProTouch.cpp
  variants/lilygo_tlora_pager/Es8311Codec.cpp
  variants/lilygo_tlora_pager/PagerHaptics.cpp
  variants/lilygo_tlora_pager/PagerImu.cpp)

# App sources this board leaves out (regexes on the full path).
set(COMET_BOARD_EXCLUDES
  "/helpers/input/TDeckTouch\\.cpp$")

set(COMET_BOARD_DEFS
  HAS_TDECK_PRO=1 HAS_TDECK_MAX=1 HAS_TOUCH_UI=1 HAS_PAGER_KEYBOARD=1 BOARD_HAS_PSRAM=1
  ARDUINO_USB_CDC_ON_BOOT=1 ARDUINO_USB_MODE=1 ESP32_CPU_FREQ=240
  SD_SPI_FAST_HZ=20000000

  # LoRa: SX1262
  LORA_FREQ=869.618 LORA_BW=62.5 LORA_SF=8 LORA_TX_POWER=22
  USE_SX1262=1 RADIO_CLASS=CustomSX1262 WRAPPER_CLASS=CustomSX1262Wrapper
  P_LORA_NSS=3 P_LORA_RESET=4 P_LORA_DIO_1=5 P_LORA_BUSY=6
  P_LORA_SCLK=36 P_LORA_MISO=47 P_LORA_MOSI=33
  SX126X_DIO2_AS_RF_SWITCH=true SX126X_DIO3_TCXO_VOLTAGE=2.4f
  SX126X_CURRENT_LIMIT=140 SX126X_RX_BOOSTED_GAIN=1

  # E-paper on the shared SPI bus
  PIN_TFT_SCLK=36 PIN_TFT_MISO=47 PIN_TFT_MOSI=33 PIN_TFT_CS=34 PIN_TFT_DC=35
  PIN_TFT_RST=9 PIN_TFT_BUSY=37 PIN_TFT_LEDA_CTL=41 TDECK_PRO_FRONTLIGHT_CHANNEL=0
  DISPLAY_CLASS=TDeckProDisplay DISPLAY_ROTATION=0 DISPLAY_SCALE_X=1.0 DISPLAY_SCALE_Y=1.0

  # Touch (reset is XL9555 line 7), keyboard, I2C, button, SD
  PIN_TOUCH_RST=-1 PIN_TOUCH_INT=12 PIN_TOUCH_ADDR=0x1A
  PIN_BOARD_SDA=13 PIN_BOARD_SCL=14 PIN_KB_ADDR=0x34 KB_INT=15 KB_BACKLIGHT=42
  PIN_USER_BTN=0 PIN_SD_CS=48

  # GPS (module-perspective names: PIN_GPS_TX is the ESP32's receive pin)
  ENV_INCLUDE_GPS=1 ENV_SKIP_GPS_DETECT=1 PIN_GPS_TX=2 PIN_GPS_RX=16 GPS_BAUD_RATE=38400

  # ES8311 speaker path
  PIN_I2S_MCLK=38 PIN_I2S_BCK=39 PIN_I2S_WS=18 PIN_I2S_DOUT=40 PIN_I2S_SDIN=17

  # No environment sensors on this board
  ENV_INCLUDE_AHTX0=0 ENV_INCLUDE_BME280=0 ENV_INCLUDE_BMP280=0 ENV_INCLUDE_SHTC3=0
  ENV_INCLUDE_SHT4X=0 ENV_INCLUDE_LPS22HB=0 ENV_INCLUDE_INA3221=0 ENV_INCLUDE_INA219=0
  ENV_INCLUDE_INA226=0 ENV_INCLUDE_INA260=0 ENV_INCLUDE_MLX90614=0 ENV_INCLUDE_VL53L0X=0
  ENV_INCLUDE_BME680=0 ENV_INCLUDE_BMP085=0

  # UI
  UI_LVGL=1 LV_CONF_PATH=lv_conf_tdeck_pro.h)
