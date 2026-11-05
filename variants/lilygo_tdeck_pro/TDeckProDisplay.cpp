// SPDX-License-Identifier: GPL-3.0-or-later
#include "TDeckProDisplay.h"

#include <Arduino.h>
#if defined(HAS_TDECK_MAX)
#include "esp32-hal-i2c.h"
#endif
#include <esp_heap_caps.h>
#include <cstring>
#include <cmath>

TDeckProDisplay::BusyHook TDeckProDisplay::_busy_hook = nullptr;
// The CST3530 signals each report with a short LOW pulse on INT and goes back to
// sleep, so polling the current pin level misses almost every touch. Latch the
// falling edge here (as the factory Hynitron driver does) and consume it on the
// next poll. Both T-Deck Pro touch revisions and the Max use this shared driver.
static volatile bool s_cst3530_irq_pending = false;
static void IRAM_ATTR cst3530IntIsr() { s_cst3530_irq_pending = true; }
#if defined(HAS_TDECK_MAX)
// Front capacitive pads (CST3530 keys). Measured on the Max: a held pad does not
// report "down" continuously. The controller sends a down, then an up 50-100 ms
// later, then a fresh down a few ms after that, with quiet gaps of a second or
// more in between. So a pad only counts as released once no down report has
// followed an up for kReleaseMs; until then the touch is one press. The UI gets a
// press edge per real press, and for the heart pad (id 0) one release with its
// held time (tap vs hold).
static constexpr uint32_t kReleaseMs = 250;
static portMUX_TYPE s_key_mux = portMUX_INITIALIZER_UNLOCKED;
static bool     s_key_down[3]  = { false, false, false };   // logically held (debounced)
static bool     s_key_up_seen[3] = { false, false, false }; // an up report is waiting out kReleaseMs
static uint32_t s_key_up_ms[3] = { 0, 0, 0 };
static uint32_t s_key_down_ms[3] = { 0, 0, 0 };
static bool     s_key_press_pending = false;
static uint8_t  s_key_press_id = 0;
static bool     s_key0_release_pending = false;
static bool     s_key0_release_long = false;
static void maxKeyReport(uint8_t kid, bool kdown) {   // from the touch poll, under s_key_mux
  const uint32_t now = millis();
  if (kdown) {
    if (!s_key_down[kid]) {                 // a fresh press
      s_key_down[kid] = true;
      s_key_down_ms[kid] = now;
      s_key_press_pending = true;
      s_key_press_id = kid;
    }
    s_key_up_seen[kid] = false;             // still held: cancels a pending up
  } else if (s_key_down[kid] && !s_key_up_seen[kid]) {
    s_key_up_seen[kid] = true;
    s_key_up_ms[kid] = now;
  }
}
static void maxKeyFinalize() {                         // under s_key_mux
  const uint32_t now = millis();
  for (uint8_t k = 0; k < 3; ++k) {
    if (!s_key_down[k] || !s_key_up_seen[k] || now - s_key_up_ms[k] < kReleaseMs) continue;
    s_key_down[k] = false;
    s_key_up_seen[k] = false;
    if (k == 0) {
      s_key0_release_long = s_key_up_ms[0] - s_key_down_ms[0] >= 600;
      s_key0_release_pending = true;
    }
  }
}
#endif

TDeckProDisplay::TDeckProDisplay()
    : DisplayDriver(WIDTH, HEIGHT),
      _canvas(nullptr),
      _epd(Panel(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST, PIN_TFT_BUSY)),
      _cst328(WIDTH, HEIGHT, &Wire, PIN_TOUCH_RST, PIN_TOUCH_INT) {}

bool TDeckProDisplay::begin() {
  const uint8_t selects[] = { PIN_TFT_CS, P_LORA_NSS, PIN_SD_CS };
  for (uint8_t pin : selects) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }

  _canvas.setPsram(true);
  _canvas.setColorDepth(16);
  if (!_canvas.createSprite(WIDTH, HEIGHT)) return false;
  _canvas.fillScreen(0xFFFF);

  _mono = (uint8_t*)ps_malloc(MONO_BYTES);
  _sent = (uint8_t*)ps_malloc(MONO_BYTES);
  if (!_mono) _mono = (uint8_t*)malloc(MONO_BYTES);
  if (!_sent) _sent = (uint8_t*)malloc(MONO_BYTES);
  if (!_mono || !_sent) return false;
  memset(_mono, 0xFF, MONO_BYTES);
  memset(_sent, 0xFF, MONO_BYTES);

  SPI.begin(PIN_TFT_SCLK, PIN_TFT_MISO, PIN_TFT_MOSI, PIN_TFT_CS);
  _epd.init(115200, true, 2, false, SPI,
            SPISettings(2000000, MSBFIRST, SPI_MODE0));
  _epd.setRotation(0);
  _epd.epd2.setBusyCallback(&TDeckProDisplay::busyCallback);

#if defined(PIN_TFT_LEDA_CTL) && (PIN_TFT_LEDA_CTL >= 0)
#if defined(HAS_TDECK_MAX)
  // 12-bit for a much dimmer bottom end. 12 kHz x 12 bit is past what the LEDC
  // divider can reach ("requested frequency and duty resolution can not be
  // achieved"): the channel never came up and the screen stayed dark. 5 kHz fits
  // and is still far above any visible flicker.
  ledcSetup(TDECK_PRO_FRONTLIGHT_CHANNEL, 5000, 12);
#else
  ledcSetup(TDECK_PRO_FRONTLIGHT_CHANNEL, 12000, 8);
#endif
  ledcAttachPin(PIN_TFT_LEDA_CTL, TDECK_PRO_FRONTLIGHT_CHANNEL);
  writeBrightness(0);
#else
  _brightness = 0;
#endif
#if defined(HAS_TDECK_MAX)
  // Every panel update re-applies _brightness (serviceRefresh). Its constructor
  // default of 255 lit the front-light on the boot wordmark's first refresh and
  // left it on until the UI took over ~a minute later. Boot dark instead.
  _brightness = 0;
#endif

  Wire.begin(PIN_BOARD_SDA, PIN_BOARD_SCL, 400000);
  resetTouch();
  _touch_is_cst3530 = probeCst3530();
  if (_touch_is_cst3530) {
    pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
    _touch_ready = initCst3530();
    s_cst3530_irq_pending = false;
    attachInterrupt(digitalPinToInterrupt(PIN_TOUCH_INT), cst3530IntIsr, FALLING);
  } else {
    _touch_ready = _cst328.begin();
    _cst328.setRotation(0);
  }

  _is_on = true;
  Serial.printf("[BOOT] T-Deck Pro e-paper ready touch=%s\n",
                _touch_ready ? (_touch_is_cst3530 ? "CST3530" : "CST328") : "missing");
  return true;
}

void TDeckProDisplay::turnOn() {
#if defined(HAS_TDECK_MAX)
  _sleep_light = 0;
#endif
  _sleeping = false;
  _is_on = true;
  requestRefresh(true);
  writeBrightness(_brightness);
}

void TDeckProDisplay::turnOff() {
#if defined(HAS_TDECK_MAX)
  finishUpdate(true);   // let a running update end before the panel's power goes
#endif
  writeBrightness(0);
  _epd.powerOff();
  _sleeping = true;
  _is_on = false;
}

#if defined(HAS_TDECK_MAX)
void TDeckProDisplay::refreshWhileAsleep() {
  if (!_sleeping) { serviceRefresh(true); return; }
  const uint8_t light = _brightness;
  // Every update re-applies _brightness. Asleep that is 0 (light off), or the dim
  // charging light: the once-a-minute lock clock used to put that out.
  _brightness = _sleep_light;
  _sleeping = false;
  serviceRefresh(true);
  _epd.powerOff();
  _sleeping = true;
  _brightness = light;
}
#endif

void TDeckProDisplay::clear() {
  _canvas.fillScreen(0xFFFF);
  memset(_mono, 0xFF, MONO_BYTES);
  requestRefresh(true);
}

void TDeckProDisplay::startFrame(ColorVal background) {
  _canvas.fillScreen(background);
  _canvas.setTextColor(_color);
}

void TDeckProDisplay::setTextSize(int size) { _canvas.setTextSize(size); }
void TDeckProDisplay::setColor(ColorVal color) { _color = color; _canvas.setTextColor(color); }
void TDeckProDisplay::setCursor(int x, int y) { _canvas.setCursor(x, y); }
void TDeckProDisplay::print(const char* text) { _canvas.print(text ? text : ""); }
void TDeckProDisplay::fillRect(int x, int y, int w, int h) { _canvas.fillRect(x, y, w, h, _color); }
void TDeckProDisplay::drawRect(int x, int y, int w, int h) { _canvas.drawRect(x, y, w, h, _color); }
void TDeckProDisplay::drawXbm(int x, int y, const uint8_t* bits, int w, int h) {
  _canvas.drawXBitmap(x, y, bits, w, h, _color);
}
uint16_t TDeckProDisplay::getTextWidth(const char* text) { return (uint16_t)_canvas.textWidth(text ? text : ""); }

void TDeckProDisplay::endFrame() {
  canvasToMono();
  requestRefresh(true);
  serviceRefresh(true);
}

bool TDeckProDisplay::isDark(uint16_t color) {
  const uint16_t red = (uint16_t)(((color >> 11) & 0x1F) * 255u / 31u);
  const uint16_t green = (uint16_t)(((color >> 5) & 0x3F) * 255u / 63u);
  const uint16_t blue = (uint16_t)((color & 0x1F) * 255u / 31u);
#if defined(HAS_TDECK_MAX)
  // A little more of each anti-aliased glyph edge counts as ink: the Max's e-paper
  // look sets its text a shade heavier, which reads better on the one-bit panel.
  return ((red * 54u + green * 183u + blue * 19u) >> 8) < 176u;
#else
  return ((red * 54u + green * 183u + blue * 19u) >> 8) < 144u;
#endif
}

#if defined(HAS_TDECK_MAX)
// Max: render OSM raster tiles as line art instead of a full-tile Bayer screen.
// On the plain screen almost every OSM colour (land, farmland, meadows, buildings)
// sits between the black and white points, so the whole map turned into grain and
// streets and labels drowned in it. Here:
//   - line work = any pixel clearly darker than its brightest 3x3 neighbour (road
//     casings, rail, borders, river banks, text) or simply dark -> solid black;
//   - fills fade from white at the light land colours to a light (<= 25%) ordered
//     screen for the darker or strongly coloured areas (forest, water, motorways);
//   - everything lighter than the land colours is white.
// One 8-bit luminance copy of the tile (64 KB for 256x256) lives in PSRAM briefly.
bool TDeckProDisplay::prepareMapTileLineArt(uint16_t* pixels, int width, int height) {
  static constexpr uint8_t bayer[8][8] = {
    { 0, 48, 12, 60,  3, 51, 15, 63 }, {32, 16, 44, 28, 35, 19, 47, 31 },
    { 8, 56,  4, 52, 11, 59,  7, 55 }, {40, 24, 36, 20, 43, 27, 39, 23 },
    { 2, 50, 14, 62,  1, 49, 13, 61 }, {34, 18, 46, 30, 33, 17, 45, 29 },
    {10, 58,  6, 54,  9, 57,  5, 53 }, {42, 26, 38, 22, 41, 25, 37, 21 },
  };
  constexpr int kWhite = 226;     // OSM land (#f2efe9) and lighter stay paper-white
  constexpr int kDark = 110;      // darker than this is ink regardless of context
  constexpr int kEdge = 36;       // this much darker than a neighbour = a drawn line
  constexpr int kMaxInk = 12;     // fills never exceed 12/64 ink (16 read as grain on the device)
  const size_t n = (size_t)width * (size_t)height;
  uint8_t* lum = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  uint8_t* sat = lum ? (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
  if (!lum || !sat) { heap_caps_free(lum); return false; }
  for (size_t i = 0; i < n; ++i) {
    const uint16_t c = pixels[i];
    const int r = (int)(((c >> 11) & 0x1F) * 255u / 31u);
    const int g = (int)(((c >> 5) & 0x3F) * 255u / 63u);
    const int b = (int)((c & 0x1F) * 255u / 31u);
    int mx = r > g ? r : g; if (b > mx) mx = b;
    int mn = r < g ? r : g; if (b < mn) mn = b;
    lum[i] = (uint8_t)((r * 54 + g * 183 + b * 19) >> 8);
    sat[i] = (uint8_t)(mx - mn);
  }
  for (int y = 0; y < height; ++y) {
    const int y0 = y > 0 ? y - 1 : y, y1 = y + 1 < height ? y + 1 : y;
    for (int x = 0; x < width; ++x) {
      const int x0 = x > 0 ? x - 1 : x, x1 = x + 1 < width ? x + 1 : x;
      const size_t i = (size_t)y * width + x;
      const int l = lum[i];
      int nmax = l;
      for (int yy = y0; yy <= y1; ++yy)
        for (int xx = x0; xx <= x1; ++xx) {
          const int v = lum[(size_t)yy * width + xx];
          if (v > nmax) nmax = v;
        }
      bool black = l < kDark || nmax - l >= kEdge;
      if (!black && l < kWhite) {
        int frac = (kWhite - l) * kMaxInk / (kWhite - 120);
        if (frac > kMaxInk) frac = kMaxInk;
        if (sat[i] > 90 && l < 215) frac = kMaxInk;   // strongly coloured: major roads, motorways
        black = bayer[y & 7][x & 7] < frac;
      }
      pixels[i] = black ? 0x0000u : 0xFFFFu;
    }
  }
  heap_caps_free(sat);
  heap_caps_free(lum);
  return true;
}
#endif

void TDeckProDisplay::prepareMapTileRGB565(uint16_t* pixels, int width, int height) {
  if (!pixels || width <= 0 || height <= 0) return;
#if defined(HAS_TDECK_MAX)
  if (prepareMapTileLineArt(pixels, width, height)) return;   // falls back below if PSRAM is short
#endif

  // A fixed Bayer screen preserves mid-tone map regions without carrying
  // diffusion error across independently decoded tiles. Map tiles are 256 px,
  // a multiple of eight, so the pattern also remains continuous at tile edges.
  static constexpr uint8_t bayer[8][8] = {
    { 0, 48, 12, 60,  3, 51, 15, 63 },
    {32, 16, 44, 28, 35, 19, 47, 31 },
    { 8, 56,  4, 52, 11, 59,  7, 55 },
    {40, 24, 36, 20, 43, 27, 39, 23 },
    { 2, 50, 14, 62,  1, 49, 13, 61 },
    {34, 18, 46, 30, 33, 17, 45, 29 },
    {10, 58,  6, 54,  9, 57,  5, 53 },
    {42, 26, 38, 22, 41, 25, 37, 21 },
  };
  constexpr int black_point = 72;
  constexpr int white_point = 246;

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const uint16_t color = pixels[(size_t)y * width + x];
      const int red = (int)(((color >> 11) & 0x1F) * 255u / 31u);
      const int green = (int)(((color >> 5) & 0x3F) * 255u / 63u);
      const int blue = (int)((color & 0x1F) * 255u / 31u);
      int maximum = red > green ? red : green;
      if (blue > maximum) maximum = blue;
      int minimum = red < green ? red : green;
      if (blue < minimum) minimum = blue;

      // Hue differences carry map meaning even when their luminance is close.
      // Darkening saturated fills slightly keeps water, parks, and major roads
      // distinguishable from neutral land after conversion to one bit.
      int tone = ((red * 54 + green * 183 + blue * 19) >> 8) -
                 (maximum - minimum) / 5;
      bool black;
      if (tone <= black_point) {
        black = true;
      } else if (tone >= white_point) {
        black = false;
      } else {
        const int white_level =
            ((tone - black_point) * 64 + (white_point - black_point) / 2) /
            (white_point - black_point);
        black = bayer[y & 7][x & 7] >= white_level;
      }
      pixels[(size_t)y * width + x] = black ? 0x0000u : 0xFFFFu;
    }
  }
}

void TDeckProDisplay::writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels) {
  if (!_mono || !pixels || w <= 0 || h <= 0) return;
  if (x >= 0 && y >= 0 && x + w <= WIDTH && y + h <= HEIGHT)
    _canvas.pushImage(x, y, w, h, const_cast<uint16_t*>(pixels));
  for (int row = 0; row < h; ++row) {
    const int py = y + row;
    if (py < 0 || py >= HEIGHT) continue;
    for (int col = 0; col < w; ++col) {
      const int px = x + col;
      if (px < 0 || px >= WIDTH) continue;
      uint8_t& destination = _mono[(size_t)py * (WIDTH / 8) + ((size_t)px >> 3)];
      const uint8_t mask = (uint8_t)(0x80u >> (px & 7));
      if (isDark(pixels[(size_t)row * w + col])) destination &= (uint8_t)~mask;
      else destination |= mask;
    }
  }
  requestRefresh(false);
}

void TDeckProDisplay::setDisplayRotation(uint8_t) { setLogicalSize(WIDTH, HEIGHT); }

void TDeckProDisplay::setBrightness(uint8_t brightness) {
  _brightness = brightness;
  if (_sent_valid && !_sleeping) writeBrightness(brightness);
}

void TDeckProDisplay::writeBrightness(uint8_t brightness) {
#if defined(PIN_TFT_LEDA_CTL) && (PIN_TFT_LEDA_CTL >= 0) && defined(HAS_TDECK_MAX)
  // Perceived LED brightness is far from linear in duty, so map the 0-255 level
  // through a ~2.2 gamma onto the 12-bit timer: the low end of the slider gets
  // fine, very dim steps (a bedside reading light) instead of one coarse jump.
  uint32_t duty = 0;
  if (brightness) {
    const float f = (float)brightness / 255.0f;
    duty = (uint32_t)(4095.0f * f * f * sqrtf(f) + 0.5f);   // f^2.5 ~ gamma 2.2-2.5, no powf
    if (duty < 1) duty = 1;
  }
  ledcWrite(TDECK_PRO_FRONTLIGHT_CHANNEL, duty);
#elif defined(PIN_TFT_LEDA_CTL) && (PIN_TFT_LEDA_CTL >= 0)
  ledcWrite(TDECK_PRO_FRONTLIGHT_CHANNEL, brightness);
#else
  (void)brightness;
#endif
}

void TDeckProDisplay::requestRefresh(bool full) {
#if defined(HAS_TDECK_MAX)
  const uint32_t now = millis();
  if (!_refresh_pending) _first_request_ms = now;
  _last_request_ms = now;
#endif
  _refresh_pending = true;
  _full_refresh_pending = _full_refresh_pending || full;
}

#if defined(HAS_TDECK_MAX)
// The Max sends a frame and lets the panel update on its own: the loop (radio,
// touch, keyboard, LVGL) keeps running for the ~0.65 s the panel takes, and the
// next frame goes out as soon as it is done. The SPI traffic (the frame, a few
// commands) stays in this task, so nothing else on the shared bus can collide
// with it; only the wait for BUSY is spread over the loop.
//
// A change is held back until LVGL has been quiet for 120 ms (at most 350 ms
// after the first change), so a tap's pressed look and what the tap opens reach
// the panel as one update rather than two.
void TDeckProDisplay::finishUpdate(bool wait) {
  if (!_epd_busy) return;
  if (wait) {
    const uint32_t t0 = millis();
    while (_epd.epd2.refreshBusy() && millis() - t0 < 3000) busyCallback(nullptr);
  } else if (_epd.epd2.refreshBusy() && millis() - _epd_start_ms < 3000) {
    return;
  }
  digitalWrite(P_LORA_NSS, HIGH);
  digitalWrite(PIN_SD_CS, HIGH);
  _epd.epd2.finishRefresh(_epd_busy_full);
  // The controller diffs against its "previous" RAM: make that the frame now shown.
  _epd.epd2.writeImageAgain(_sent, 0, 0, WIDTH, HEIGHT);
  _epd.epd2.powerOff();
  _epd_busy = false;
  writeBrightness(_brightness);
  _last_refresh_ms = millis();
  Serial.printf("[EPD] done after %lums\n", (unsigned long)(_last_refresh_ms - _epd_start_ms));   // TEST BUILD ONLY
}

void TDeckProDisplay::serviceRefresh(bool force) {
  finishUpdate(force);
  if (_epd_busy) return;                         // still updating; the frame waits
  if (!_refresh_pending || _sleeping || !_mono) return;
  const uint32_t now = millis();
  if (!force) {
    if (_last_refresh_ms && now - _last_refresh_ms < 60) return;
    if (now - _last_request_ms < 120 && now - _first_request_ms < 350) return;
  }

  // An unchanged frame is never sent, also when the next one is due to be a full
  // refresh: after nine partials every redraw that changed no pixel (a label set to
  // the text it had) used to flash the whole panel for a second for nothing. The
  // full refresh waits for the next real change.
  // A full refresh (the black-white flash that clears any ghosting) after 29
  // partials rather than 9: the Max's panel showed no ghosting at that pace, and
  // the flash every few taps read as the screen going black.
  const bool full = _full_refresh_pending || _partial_refreshes >= 29 || _epd.epd2.needsInitialFull();
  if (!_full_refresh_pending && _sent_valid && memcmp(_mono, _sent, MONO_BYTES) == 0) {
    _refresh_pending = false;
    return;
  }

  // TEST BUILD ONLY: where the frame changed, and since when.
  if (_sent_valid) {
    int y0 = HEIGHT, y1 = -1, x0 = WIDTH, x1 = -1;
    for (int y = 0; y < HEIGHT; ++y)
      for (int b = 0; b < WIDTH / 8; ++b) {
        const size_t i = (size_t)y * (WIDTH / 8) + b;
        if (_mono[i] == _sent[i]) continue;
        if (y < y0) y0 = y; if (y > y1) y1 = y;
        if (b * 8 < x0) x0 = b * 8; if (b * 8 + 7 > x1) x1 = b * 8 + 7;
      }
    static uint32_t s_prev = 0;
    Serial.printf("[EPD] %s changed x%d-%d y%d-%d (+%lums, waited %lums)\n", full ? "full" : "part", x0, x1, y0, y1,
                  (unsigned long)(now - s_prev), (unsigned long)(now - _first_request_ms));
    s_prev = now;
  }
  digitalWrite(P_LORA_NSS, HIGH);
  digitalWrite(PIN_SD_CS, HIGH);
  digitalWrite(PIN_TFT_CS, HIGH);
  memcpy(_sent, _mono, MONO_BYTES);
  _sent_valid = true;
  if (full) _epd.epd2.writeImageForFullRefresh(_sent, 0, 0, WIDTH, HEIGHT);
  else      _epd.epd2.writeImage(_sent, 0, 0, WIDTH, HEIGHT);
  _epd.epd2.startRefresh(full);
  _epd_busy = true;
  _epd_busy_full = full;
  _epd_start_ms = millis();
  _refresh_pending = false;
  _full_refresh_pending = false;
  _partial_refreshes = full ? 0 : (uint8_t)(_partial_refreshes + 1);
  if (force) finishUpdate(true);
}
#else
void TDeckProDisplay::serviceRefresh(bool force) {
  if (!_refresh_pending || _sleeping || !_mono) return;
  const uint32_t now = millis();
  if (!force && _last_refresh_ms && now - _last_refresh_ms < 250) return;

#if defined(HAS_TDECK_MAX)
  // An unchanged frame is never sent, also when the next one is due to be a full
  // refresh: after nine partials every redraw that changed no pixel (a label set to
  // the text it had) used to flash the whole panel for a second for nothing. The
  // full refresh waits for the next real change.
  const bool full = _full_refresh_pending || _partial_refreshes >= 9;
  if (!_full_refresh_pending && _sent_valid && memcmp(_mono, _sent, MONO_BYTES) == 0) {
#else
  const bool full = _full_refresh_pending || _partial_refreshes >= 9;
  if (!full && _sent_valid && memcmp(_mono, _sent, MONO_BYTES) == 0) {
#endif
    _refresh_pending = false;
    return;
  }

  digitalWrite(P_LORA_NSS, HIGH);
  digitalWrite(PIN_SD_CS, HIGH);
  digitalWrite(PIN_TFT_CS, HIGH);
  if (full) _epd.setFullWindow();
  else _epd.setPartialWindow(0, 0, WIDTH, HEIGHT);
  _epd.firstPage();
  do {
    _epd.drawInvertedBitmap(0, 0, _mono, WIDTH, HEIGHT, GxEPD_BLACK);
  } while (_epd.nextPage());
  _epd.powerOff();

  memcpy(_sent, _mono, MONO_BYTES);
  _sent_valid = true;
  writeBrightness(_brightness);
  _last_refresh_ms = millis();
  _refresh_pending = false;
  _full_refresh_pending = false;
  _partial_refreshes = full ? 0 : (uint8_t)(_partial_refreshes + 1);
}

#endif

void TDeckProDisplay::busyCallback(const void*) {
  if (_busy_hook) _busy_hook();
  else delay(1);
}

void TDeckProDisplay::canvasToMono() {
  memset(_mono, 0xFF, MONO_BYTES);
  for (int y = 0; y < HEIGHT; ++y) {
    for (int x = 0; x < WIDTH; ++x) {
      if (isDark((uint16_t)_canvas.readPixelValue(x, y)))
        _mono[(size_t)y * (WIDTH / 8) + ((size_t)x >> 3)] &= (uint8_t)~(0x80u >> (x & 7));
    }
  }
}

void TDeckProDisplay::resetTouch() {
#if defined(PIN_TOUCH_RST) && (PIN_TOUCH_RST >= 0)
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(20);
  digitalWrite(PIN_TOUCH_RST, LOW);
  delay(80);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(20);
#endif
}

bool TDeckProDisplay::probeCst3530() {
  const uint8_t command[] = { 0xD0, 0x03, 0x00, 0x00 };
  uint8_t response[7] = {};
  for (uint8_t attempt = 0; attempt < 5; ++attempt) {
    Wire.beginTransmission((uint8_t)PIN_TOUCH_ADDR);
    Wire.write(command, sizeof(command));
    if (Wire.endTransmission() == 0 &&
        Wire.requestFrom((int)PIN_TOUCH_ADDR, (int)sizeof(response)) == sizeof(response)) {
      Wire.readBytes(response, sizeof(response));
      if (response[2] == 0xCA && response[3] == 0xCA) return true;
    }
    const uint8_t wake[] = { 0xD0, 0x00, 0x04, 0x00 };
    Wire.beginTransmission((uint8_t)PIN_TOUCH_ADDR);
    Wire.write(wake, sizeof(wake));
    (void)Wire.endTransmission();
    delay(50);
  }
  return false;
}

bool TDeckProDisplay::writeCst3530Command(uint32_t command) {
  const uint8_t bytes[] = {
    (uint8_t)(command >> 24), (uint8_t)(command >> 16),
    (uint8_t)(command >> 8), (uint8_t)command,
  };
  Wire.beginTransmission((uint8_t)PIN_TOUCH_ADDR);
  Wire.write(bytes, sizeof(bytes));
  return Wire.endTransmission() == 0;
}

bool TDeckProDisplay::initCst3530() {
  bool ok = writeCst3530Command(0xD0000400);
  delay(20);
  ok = writeCst3530Command(0xD0000400) && ok;
  delay(20);
  ok = writeCst3530Command(0xD0000000) && ok;
  ok = writeCst3530Command(0xD0000C00) && ok;
  ok = writeCst3530Command(0xD0000100) && ok;
  return ok;
}

bool TDeckProDisplay::readCst3530(int16_t& x, int16_t& y) {
  const uint8_t read_command[] = { 0xD0, 0x07, 0x00, 0x00 };
  const uint8_t clear_command[] = { 0xD0, 0x00, 0x02, 0xAB };
  uint8_t response[50] = {};

  const bool pending = s_cst3530_irq_pending;
  if (pending) s_cst3530_irq_pending = false;
  if (!pending && digitalRead(PIN_TOUCH_INT) != LOW) return false;
#if defined(HAS_TDECK_MAX)
  // The Max reads touch from its own task (TDeckProTouch.cpp). Through TwoWire that
  // raced the UI task's I2C traffic: requestFrom() fills Wire's ONE shared receive
  // buffer, and another task's transfer could overwrite it before readBytes() --
  // garbage reports (a 0xFF frame was logged) and dropped taps. The HAL calls lock
  // the bus per transfer and read straight into this buffer.
  size_t received = 0;
  if (i2cWrite(0, PIN_TOUCH_ADDR, read_command, sizeof(read_command), 20) != ESP_OK ||
      i2cRead(0, PIN_TOUCH_ADDR, response, 9, 20, &received) != ESP_OK || received != 9) return false;
#else
  Wire.beginTransmission((uint8_t)PIN_TOUCH_ADDR);
  Wire.write(read_command, sizeof(read_command));
  if (Wire.endTransmission() != 0 || Wire.requestFrom((int)PIN_TOUCH_ADDR, 9) != 9) return false;
  size_t received = Wire.readBytes(response, 9);
#endif
  const uint8_t fingers = response[3] & 0x0F;
  const uint8_t keys = (response[3] >> 4) & 0x0F;
#if defined(HAS_TDECK_MAX)
  // Key report (factory cst66xx layout): id in the low nibble of byte 8, state
  // in the high nibble (1 = pressed). Latch the press edge; releases just clear.
  if (keys && response[3] != 0xFF) {   // 0xFF: a garbled read (seen once mid-hold), not a report
    const uint8_t kid = response[8] & 0x0F;
    const bool kdown = ((response[8] >> 4) == 1);
    if (kid < 3) {
      portENTER_CRITICAL(&s_key_mux);
      maxKeyReport(kid, kdown);
      portEXIT_CRITICAL(&s_key_mux);
    }
  }
  {   // TEST ONLY: raw pad reports, to see how the CST3530 reports a held pad
    static uint8_t s_last3 = 0xFF, s_last8 = 0xFF;
    if (response[3] != s_last3 || (keys && response[8] != s_last8)) {
      Serial.printf("[PAD] t=%lu b3=%02X b8=%02X fingers=%u keys=%u down0=%d\n", (unsigned long)millis(),
                    response[3], response[8], (unsigned)fingers, (unsigned)keys, (int)s_key_down[0]);
      s_last3 = response[3]; s_last8 = response[8];
    }
  }
  // A report WITHOUT a key entry says nothing about the pads: the controller
  // interleaves those with the key reports while a pad is held. Treating them as
  // "all up" turned one press into a burst of press/release pairs (the keyboard
  // light flickered and the front-light toggled back off). Only the pad's own
  // up-report releases it.
#endif
  const uint8_t total = (uint8_t)(fingers + keys);
  if (total > 1) {
    size_t extra = (size_t)(total - 1) * 5u;
    if (extra > sizeof(response) - received) extra = sizeof(response) - received;
#if defined(HAS_TDECK_MAX)
    size_t got = 0;
    if (i2cRead(0, PIN_TOUCH_ADDR, response + received, extra, 20, &got) == ESP_OK) received += got;
#else
    if (Wire.requestFrom((int)PIN_TOUCH_ADDR, (int)extra) == extra)
      received += Wire.readBytes(response + received, extra);
#endif
  }
#if defined(HAS_TDECK_MAX)
  (void)i2cWrite(0, PIN_TOUCH_ADDR, clear_command, sizeof(clear_command), 20);
#else
  Wire.beginTransmission((uint8_t)PIN_TOUCH_ADDR);
  Wire.write(clear_command, sizeof(clear_command));
  (void)Wire.endTransmission();
#endif

  if (fingers == 0 || (response[8] >> 4) == 0) return false;
  const size_t index = (size_t)keys * 5u;
  if (index + 7 >= received) return false;
  x = (int16_t)(response[index + 4] | ((uint16_t)(response[index + 7] & 0x0F) << 8));
  y = (int16_t)(response[index + 5] | ((uint16_t)(response[index + 7] & 0xF0) << 4));
  return true;
}

#if defined(HAS_TDECK_MAX)
bool TDeckProDisplay::takeFrontKey0Release(bool& long_press) {
  portENTER_CRITICAL(&s_key_mux);
  maxKeyFinalize();   // releases are decided by time, so the UI loop's poll drives them
  const bool had = s_key0_release_pending;
  s_key0_release_pending = false;
  long_press = s_key0_release_long;
  portEXIT_CRITICAL(&s_key_mux);
  return had;
}
bool TDeckProDisplay::takeFrontKeyPress(uint8_t& key_id) {
  portENTER_CRITICAL(&s_key_mux);
  maxKeyFinalize();
  const bool had = s_key_press_pending;
  s_key_press_pending = false;
  key_id = s_key_press_id;
  portEXIT_CRITICAL(&s_key_mux);
  return had;
}
#endif

bool TDeckProDisplay::getTouchPoint(uint16_t& x, uint16_t& y) {
  if (!_touch_ready) return false;
  int16_t raw_x = 0, raw_y = 0;
  if (_touch_is_cst3530) {
    if (!readCst3530(raw_x, raw_y)) return false;
  } else {
    if (_cst328.getTouches() == 0) return false;
    const CSE_TouchPoint point = _cst328.getPoint(0);
    raw_x = point.x;
    raw_y = point.y;
  }
  if (raw_x < 0 || raw_y < 0 || raw_x >= WIDTH || raw_y >= HEIGHT) return false;
  x = (uint16_t)raw_x;
  y = (uint16_t)raw_y;
  return true;
}