// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Feed the task watchdog from code that may run on a task it does not watch.
// ESP-IDF 4.4 returned an error for such a feed silently; IDF 5 logs every one
// ("esp_task_wdt_reset(707): task not found"), so ask first.

#include <esp_task_wdt.h>

inline void wdtFeed() {
  if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
}
