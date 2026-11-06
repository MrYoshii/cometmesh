// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// LEDC (PWM) on a fixed channel, for both Arduino-ESP32 cores Comet builds with:
// 2.x (PlatformIO) has only the channel API (ledcSetup / ledcAttachPin / ledcWrite by
// channel); 3.x (the ESP-IDF build) replaced it with a pin API (ledcAttachChannel /
// ledcWrite by pin). Callers keep their channel numbers, because channel pairs share a
// timer and the boards' channel plans depend on that (see PagerKeyboard.cpp).

#include <Arduino.h>

inline bool ledcCompatAttach(int pin, int channel, uint32_t freq_hz, uint8_t bits) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  return ledcAttachChannel(pin, freq_hz, bits, channel);
#else
  ledcSetup(channel, freq_hz, bits);
  ledcAttachPin(pin, channel);
  return true;
#endif
}

inline void ledcCompatWrite(int pin, int channel, uint32_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)channel;
  ledcWrite(pin, duty);
#else
  (void)pin;
  ledcWrite(channel, duty);
#endif
}
