// SPDX-License-Identifier: GPL-3.0-or-later
// Preferences::begin(ns, readOnly=true) logs "nvs_open failed: NOT_FOUND" at error level
// whenever the namespace has never been written, which on a device that keeps its
// settings on the SD card is the normal case. prefsBeginRO checks for the namespace
// quietly first, so a missing one is just "no saved values".
#pragma once
#include <Preferences.h>
#include <nvs.h>

static inline bool prefsNamespaceExists(const char* ns) {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
  nvs_close(h);
  return true;
}

static inline bool prefsBeginRO(Preferences& p, const char* ns) {
  return prefsNamespaceExists(ns) && p.begin(ns, true);
}
