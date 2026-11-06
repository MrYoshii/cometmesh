// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(HAS_TDECK_PRO) && defined(ESP32)

#include <Arduino.h>
#include <helpers/input/HeltecV4CapTouch.h>
#include <helpers/ui/MomentaryButton.h>

#include "TDeckProDisplay.h"

extern TDeckProDisplay display;

namespace {
bool ready = false;
bool down = false;
bool live = false;
bool tap_pending = false;
bool swipe_pending = false;
bool swiping = false;
uint16_t x = 0, y = 0, start_x = 0, start_y = 0, tap_x = 0, tap_y = 0;
int8_t swipe_x = 0, swipe_y = 0;
uint32_t down_at = 0;
uint8_t release_misses = 0;
constexpr uint8_t RELEASE_DEBOUNCE = 2;
#if defined(HAS_TDECK_MAX)
// The Max refreshes its e-paper without blocking, so the UI loop no longer sits in
// the panel's BUSY callback (which used to poll touch for the Pro). A tap that
// starts and ends inside one long UI pass (a page build, a ~600 ms timer burst)
// was therefore never sampled at all: the tab bar "only reacted sometimes". Poll
// from a small task instead; the UI side only reads the latched results.
TaskHandle_t poll_task = nullptr;
uint32_t last_seen_ms = 0;
volatile bool async_active = false;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
#define TOUCH_LOCK()   portENTER_CRITICAL(&mux)
#define TOUCH_UNLOCK() portEXIT_CRITICAL(&mux)
#else
#define TOUCH_LOCK()
#define TOUCH_UNLOCK()
#endif

void pollUpdate(bool touched, uint16_t next_x, uint16_t next_y);
void poll() {
  uint16_t next_x = 0, next_y = 0;
  const bool touched = display.getTouchPoint(next_x, next_y);   // I2C: outside the lock
  TOUCH_LOCK();
  pollUpdate(touched, next_x, next_y);
  TOUCH_UNLOCK();
}
}

namespace {
void pollUpdate(bool touched, uint16_t next_x, uint16_t next_y) {
  if (touched) {
    release_misses = 0;
#if defined(HAS_TDECK_MAX)
    last_seen_ms = millis();
#endif
    x = next_x;
    y = next_y;
    live = true;
    if (!down) {
      down = true;
      start_x = x;
      start_y = y;
      down_at = millis();
      swiping = false;
    }
    const int dx = (int)x - start_x;
    const int dy = (int)y - start_y;
    if (!swiping && abs(dx) >= 40 && abs(dx) > abs(dy)) swiping = true;
    return;
  }

#if defined(HAS_TDECK_MAX)
  // Time-based release debounce: the task polls every 10 ms, faster than the
  // CST3530 raises INT for a held finger, so "no sample" for a couple of polls is
  // normal mid-touch. Count a lift only after 60 ms without a report.
  if (down && millis() - last_seen_ms < 60) {
    release_misses = 1;   // keeps HeldMs from stretching a tap into a hold
    return;
  }
#else
  if (down && release_misses < RELEASE_DEBOUNCE) {
    ++release_misses;
    return;
  }
#endif
  release_misses = 0;
  live = false;
  if (!down) return;
  down = false;
  const int dx = (int)x - start_x;
  const int dy = (int)y - start_y;
  swiping = false;
  if (abs(dx) >= 40 && abs(dx) > abs(dy) + 8) {
    swipe_x = dx < 0 ? -1 : 1;
    swipe_y = 0;
    swipe_pending = true;
  } else if (abs(dy) >= 40 && abs(dy) > abs(dx) + 8) {
    swipe_x = 0;
    swipe_y = dy < 0 ? -1 : 1;
    swipe_pending = true;
  } else if (millis() - down_at >= 12 && abs(dx) <= 16 && abs(dy) <= 16) {
    tap_x = x;
    tap_y = y;
    tap_pending = true;
  }
}
}

bool heltecV4CapTouchBegin() { ready = true; return true; }
int heltecV4CapTouchCheck() {
#if defined(HAS_TDECK_MAX)
  if (async_active) return BUTTON_EVENT_NONE;   // the poll task owns the controller
#endif
  if (ready) poll();
  return BUTTON_EVENT_NONE;
}
bool heltecV4CapTouchPopTap(uint16_t* out_x, uint16_t* out_y) {
  TOUCH_LOCK();
  const bool had = tap_pending;
  tap_pending = false;
  const uint16_t tx = tap_x, ty = tap_y;
  TOUCH_UNLOCK();
  if (!had) return false;
  if (out_x) *out_x = tx;
  if (out_y) *out_y = ty;
  return true;
}
bool heltecV4CapTouchGetLive(uint16_t* out_x, uint16_t* out_y) {
  TOUCH_LOCK();
  const bool l = live;
  const uint16_t lx = x, ly = y;
  TOUCH_UNLOCK();
  if (!l) return false;
  if (out_x) *out_x = lx;
  if (out_y) *out_y = ly;
  return true;
}
uint32_t heltecV4CapTouchHeldMs() {
  // A debounce miss keeps LVGL's pointer stable, but must not extend a short
  // physical tap into a hold. Only a fresh positive controller sample counts.
  TOUCH_LOCK();
  const bool held = down && live && release_misses == 0 && down_at != 0;
  const uint32_t at = down_at;
  TOUCH_UNLOCK();
  if (!held) return 0;
  return millis() - at;
}
bool heltecV4CapTouchPopSwipe(int8_t* out_x, int8_t* out_y) {
  TOUCH_LOCK();
  const bool had = swipe_pending;
  swipe_pending = false;
  const int8_t sx = swipe_x, sy = swipe_y;
  TOUCH_UNLOCK();
  if (!had) return false;
  if (out_x) *out_x = sx;
  if (out_y) *out_y = sy;
  return true;
}
#if defined(HAS_TDECK_MAX)
static volatile bool s_paused = false;   // controller held in reset (screen locked)
void maxTouchSetPaused(bool paused) {
  s_paused = paused;
  if (paused) {   // forget any touch in flight
    TOUCH_LOCK();
    down = live = tap_pending = swipe_pending = swiping = false;
    release_misses = 0;
    TOUCH_UNLOCK();
  }
}
static void touchPollTaskFn(void* arg) {
  const TickType_t period = pdMS_TO_TICKS((uint32_t)(uintptr_t)arg);
  for (;;) {
    if (s_paused) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }   // nothing to read, nothing to burn
    poll();
    vTaskDelay(period ? period : 1);
  }
}
bool heltecV4CapTouchStartBackgroundPoll(uint32_t period_ms) {
  if (poll_task || !ready) return false;
  if (period_ms < 5) period_ms = 5;
  if (period_ms > 50) period_ms = 50;
  // 3 KB: poll() is one 9-50 byte I2C read; Wire's own lock serialises it with the
  // keyboard, gauge and codec traffic of the UI task.
  if (xTaskCreatePinnedToCore(touchPollTaskFn, "touch_poll", 3072, (void*)(uintptr_t)period_ms,
                              1, &poll_task, 0) != pdPASS) {
    poll_task = nullptr;
    return false;
  }
  async_active = true;
  return true;
}
bool heltecV4CapTouchIsAsyncPolling() { return async_active; }
#else
bool heltecV4CapTouchStartBackgroundPoll(uint32_t) { return false; }
bool heltecV4CapTouchIsAsyncPolling() { return false; }
#endif
bool heltecV4CapTouchIsSwiping() { return swiping; }
void heltecV4CapTouchSetRotation(uint8_t) {}
void heltecV4CapTouchSetPointRotation(uint8_t) {}
void heltecV4CapTouchSetSlowPoll(bool) {}
const char* heltecV4CapTouchDebug() { return "T-Deck Pro CST328/CST3530"; }
void heltecV4CapTouchGetRaw(uint16_t* out_x, uint16_t* out_y) {
  if (out_x) *out_x = x;
  if (out_y) *out_y = y;
}

#endif