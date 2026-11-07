// SPDX-License-Identifier: GPL-3.0-or-later
// The Comet mark, drawn from its geometry: see CometMark.h.

#include "CometMark.h"

#include <math.h>

#include "CometMarkGeom.h"

namespace {

constexpr int kNodes = sizeof kCometNodes / sizeof kCometNodes[0];
constexpr int kDust = sizeof kCometDust / sizeof kCometDust[0];
constexpr int kLinks = sizeof kCometLinks / sizeof kCometLinks[0];
constexpr int kPath = sizeof kCometPath / sizeof kCometPath[0];
constexpr int kArcs = sizeof kCometArcs / sizeof kCometArcs[0];

// Boot scene timeline, ms.
constexpr float kDimFrom = 180.0f, kDimTo = 560.0f;   // the still mark dims to its embers
constexpr float kHopFrom = 620.0f;                    // the message sets off from the tail's end
constexpr float kArrive = (float)kCometBootArriveMs;  // and reaches the head
constexpr float kEmber = 0.28f;                       // how much of the mark stays lit while dimmed

// Idle scene: a message every kIdleRun ms, the waves every kIdleWave ms.
constexpr uint32_t kIdleRun = 5200, kIdleHop = 1100, kIdleWave = 2600;

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
float easeOut(float v) { v = clamp01(v); return 1.0f - (1.0f - v) * (1.0f - v); }

struct Ctx {
  lv_draw_ctx_t* dc;
  float ox, oy, s;   // box origin, px per drawing unit
  const CometMarkPaint* p;
  lv_opa_t opa(float v) const {   // 0..1 times the mark's own opacity
    const float o = clamp01(v) * (float)p->opa;
    return (lv_opa_t)(o > 255.0f ? 255 : (int)o);
  }
  lv_point_t pt(int16_t x16, int16_t y16) const {
    return lv_point_t{ (lv_coord_t)lroundf(ox + x16 * s / 16.0f), (lv_coord_t)lroundf(oy + y16 * s / 16.0f) };
  }
  lv_coord_t len(float units) const { return (lv_coord_t)lroundf(units * s); }
};

void disc(const Ctx& c, lv_point_t at, float r_px, lv_color_t col, lv_opa_t opa) {
  if (opa <= LV_OPA_MIN) return;
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.radius = LV_RADIUS_CIRCLE;
  d.bg_color = col;
  d.bg_opa = opa;
  const lv_coord_t r = (lv_coord_t)LV_MAX(1, lroundf(r_px));
  const lv_area_t a = { (lv_coord_t)(at.x - r), (lv_coord_t)(at.y - r), (lv_coord_t)(at.x + r - 1), (lv_coord_t)(at.y + r - 1) };
  lv_draw_rect(c.dc, &d, &a);
}

void ring(const Ctx& c, lv_point_t at, float r_px, float w_px, lv_color_t col, lv_opa_t opa) {
  if (opa <= LV_OPA_MIN || r_px < 1.0f) return;
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.radius = LV_RADIUS_CIRCLE;
  d.bg_opa = LV_OPA_TRANSP;
  d.border_color = col;
  d.border_opa = opa;
  d.border_width = (lv_coord_t)LV_MAX(1, lroundf(w_px));
  const lv_coord_t r = (lv_coord_t)lroundf(r_px);
  const lv_area_t a = { (lv_coord_t)(at.x - r), (lv_coord_t)(at.y - r), (lv_coord_t)(at.x + r), (lv_coord_t)(at.y + r) };
  lv_draw_rect(c.dc, &d, &a);
}

void line(const Ctx& c, lv_point_t a, lv_point_t b, float w_px, lv_color_t col, lv_opa_t opa) {
  if (opa <= LV_OPA_MIN) return;
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.color = col;
  d.opa = opa;
  d.width = (lv_coord_t)LV_MAX(1, lroundf(w_px));
  d.round_start = d.round_end = 0;
  lv_draw_line(c.dc, &d, &a, &b);
}

void wave(const Ctx& c, lv_point_t head, const CometArc& arc, float w_mul, lv_color_t col, lv_opa_t opa) {
  if (opa <= LV_OPA_MIN) return;
  lv_draw_arc_dsc_t d;
  lv_draw_arc_dsc_init(&d);
  d.color = col;
  d.opa = opa;
  d.width = (lv_coord_t)LV_MAX(c.p->paper ? 2 : 1, lroundf(arc.w * c.s * w_mul / 16.0f));
  d.rounded = 1;
  const uint16_t a0 = (uint16_t)(360 - kCometArcHalfDeg), a1 = (uint16_t)kCometArcHalfDeg;   // LVGL: 0 = right, clockwise
  lv_draw_arc(c.dc, &d, &head, (uint16_t)lroundf(arc.r * c.s / 16.0f + d.width / 2.0f), a0, a1);
}

float distToHead(const CometNode& n) {
  return hypotf((float)(n.x - kCometHeadX), (float)(n.y - kCometHeadY)) / 16.0f;
}

// Where a message hopping along kCometPath is after `f` (0 at the tail's end, 1 at the
// head), in 1/16 units, and the point it hopped from (for its trail). The hops get
// shorter in time as it nears the head: it speeds up.
void hopAt(float f, float* x, float* y, float* px, float* py) {
  static const float w[] = { 1.6f, 1.4f, 1.2f, 1.05f, 0.9f, 0.78f, 0.66f, 0.56f };
  const int hops = kPath;   // kPath nodes, then the head
  float total = 0;
  for (int i = 0; i < hops; ++i) total += w[i < 8 ? i : 7];
  float at = clamp01(f) * total;
  int i = 0;
  while (i < hops - 1 && at > w[i < 8 ? i : 7]) { at -= w[i < 8 ? i : 7]; ++i; }
  const float u = clamp01(at / w[i < 8 ? i : 7]);
  const CometNode& a = kCometNodes[kCometPath[i]];
  const float bx = i + 1 < hops ? kCometNodes[kCometPath[i + 1]].x : kCometHeadX;
  const float by = i + 1 < hops ? kCometNodes[kCometPath[i + 1]].y : kCometHeadY;
  // ease within each hop: the message leaves quickly and lands softly
  const float e = 1.0f - (1.0f - u) * (1.0f - u);
  *x = a.x + (bx - a.x) * e;
  *y = a.y + (by - a.y) * e;
  *px = a.x;
  *py = a.y;
}

}  // namespace

lv_coord_t cometMarkExtDraw(lv_coord_t box_h, CometMarkScene scene) {
  const float s = box_h / (float)kCometBoxH;
  return (lv_coord_t)lroundf((scene == CometMarkScene::Boot ? 70.0f : 22.0f) * s) + 4;
}

void cometMarkDraw(lv_draw_ctx_t* dc, const lv_area_t& box, const CometMarkPaint& paint,
                   CometMarkScene scene, uint32_t t_ms) {
  if (!dc) return;
  Ctx c{ dc, (float)box.x1, (float)box.y1, (float)lv_area_get_height(&box) / (float)kCometBoxH, &paint };
  const lv_point_t head = c.pt(kCometHeadX, kCometHeadY);
  const lv_color_t white = lv_color_white();

  // ---- e-paper: ink only, one still frame whatever the scene ----
  if (paint.paper) {
    const lv_color_t ink = paint.ink;
    for (int i = 0; i < kLinks; ++i) {
      const CometNode& a = kCometNodes[kCometLinks[i][0]];
      const CometNode& b = kCometNodes[kCometLinks[i][1]];
      line(c, c.pt(a.x, a.y), c.pt(b.x, b.y), 1.0f, ink, LV_OPA_COVER);
    }
    for (uint8_t n : kCometHeadLinks) {
      const CometNode& a = kCometNodes[n];
      line(c, head, c.pt(a.x, a.y), LV_MAX(1.0f, 1.4f * c.s), ink, LV_OPA_COVER);
    }
    for (const CometNode& n : kCometNodes) disc(c, c.pt(n.x, n.y), LV_MAX(1.5f, n.r * c.s / 16.0f + 0.3f), ink, LV_OPA_COVER);
    for (const CometNode& n : kCometDust) disc(c, c.pt(n.x, n.y), 1.0f, ink, LV_OPA_COVER);
    for (const CometArc& a : kCometArcs) wave(c, head, a, 1.0f, ink, LV_OPA_COVER);
    disc(c, head, 5.2f * c.s, ink, LV_OPA_COVER);
    ring(c, head, 8.0f * c.s, 1.0f, ink, LV_OPA_COVER);
    return;
  }

  // ---- colour panels ----
  const float t = (float)t_ms;
  // Where the light is: D = the message's distance from the head (units); every node
  // farther out than D has been passed and is lit. lit_all: the mark is fully lit.
  float D = 1e9f, mx = 0, my = 0, mpx = 0, mpy = 0;
  bool msg = false;
  float ember = 1.0f;       // the unlit mark's brightness
  float lit_base = 0.0f;    // the lit mark's steady light (0: lit parts only flash)
  float arrive = -1.0f;     // ms since the message reached the head (<0: not yet)
  float wave_phase = -1.0f; // ms into a pulse of the radio waves (<0: none)
  if (scene == CometMarkScene::Still) {
    ember = 1.0f;
  } else if (scene == CometMarkScene::Boot) {
    ember = 1.0f - (1.0f - kEmber) * easeOut((t - kDimFrom) / (kDimTo - kDimFrom));
    lit_base = 1.0f;
    if (t >= kHopFrom && t < kArrive) {
      const float f = (t - kHopFrom) / (kArrive - kHopFrom);
      hopAt(f, &mx, &my, &mpx, &mpy);
      D = hypotf(mx - kCometHeadX, my - kCometHeadY) / 16.0f;
      msg = true;
    } else if (t >= kArrive) {
      D = -1.0f;
      arrive = t - kArrive;
      // the waves fire with the arrival, then pulse calmly
      wave_phase = arrive < 900.0f ? arrive : fmodf(arrive - 900.0f, 1700.0f);
    }
  } else {   // Idle
    ember = 1.0f;
    lit_base = 0.0f;
    const uint32_t run = t_ms % kIdleRun;
    if (run < kIdleHop) {
      hopAt((float)run / (float)kIdleHop, &mx, &my, &mpx, &mpy);
      D = hypotf(mx - kCometHeadX, my - kCometHeadY) / 16.0f;
      msg = true;
    } else {
      D = -1.0f;
      arrive = (float)(run - kIdleHop);
    }
    wave_phase = (float)(t_ms % kIdleWave);
  }
  const float tw = scene == CometMarkScene::Boot && arrive >= 0.0f ? clamp01(arrive / 600.0f) : 0.0f;   // twinkle once lit

  auto litOf = [&](float dist, float* flash) {
    // passed: lit (ramping in over 6 units); just passed: a white flash fading over 28 units
    const float delta = dist - D;
    *flash = (delta > 0.0f && delta < 28.0f && D < 1e8f) ? (1.0f - delta / 28.0f) : 0.0f;
    if (D <= -1.0f) *flash = 0.0f;
    return delta > 0.0f ? clamp01(delta / 6.0f) : 0.0f;
  };

  // links
  for (int i = 0; i < kLinks; ++i) {
    const CometNode& a = kCometNodes[kCometLinks[i][0]];
    const CometNode& b = kCometNodes[kCometLinks[i][1]];
    const float light = (a.light + b.light) / 510.0f;
    float fa, fb;
    const float lit = LV_MIN(litOf(distToHead(a), &fa), litOf(distToHead(b), &fb)) * lit_base;
    const float flash = LV_MAX(fa, fb);
    const float o = 0.55f * light * (ember * (1.0f - lit) + lit * 1.1f) + 0.5f * flash;
    const lv_color_t col = (lit > 0.0f || flash > 0.0f) ? lv_color_mix(white, paint.accent, (uint8_t)(255.0f * flash))
                                                       : paint.ink;
    line(c, c.pt(a.x, a.y), c.pt(b.x, b.y), LV_MAX(1.0f, 1.1f * c.s), col, c.opa(o));
  }
  for (uint8_t n : kCometHeadLinks) {
    const CometNode& a = kCometNodes[n];
    const float lit = (arrive >= 0.0f ? 1.0f : 0.0f) * lit_base;
    line(c, head, c.pt(a.x, a.y), LV_MAX(1.0f, 1.3f * c.s), lit > 0.0f ? paint.accent : paint.ink,
         c.opa(0.7f * (ember * (1.0f - lit) + lit)));
  }
  // dust: catches the light once the mark is lit
  for (int i = 0; i < kDust; ++i) {
    const CometNode& n = kCometDust[i];
    const float tws = 0.6f + 0.4f * sinf(t / 260.0f + i * 2.1f);
    const float o = (n.light / 255.0f) * (ember + tw * (0.6f * tws));
    disc(c, c.pt(n.x, n.y), LV_MAX(1.0f, n.r * c.s / 16.0f), tw > 0.0f ? paint.accent : paint.ink, c.opa(o));
  }
  // nodes
  for (int i = 0; i < kNodes; ++i) {
    const CometNode& n = kCometNodes[i];
    const float light = n.light / 255.0f;
    float flash;
    const float lit = litOf(distToHead(n), &flash) * (scene == CometMarkScene::Idle ? 0.0f : 1.0f);
    const float twinkle = 1.0f - tw * 0.22f * (0.5f + 0.5f * sinf(t / 210.0f + i * 1.7f));
    const float o = LV_MIN(1.0f, 0.35f + light) * (ember * (1.0f - lit) + lit * twinkle) + flash;
    const lv_color_t col = flash > 0.0f ? lv_color_mix(white, paint.accent, (uint8_t)(255.0f * flash))
                                        : (lit > 0.0f ? paint.accent : paint.ink);
    const float r = n.r * c.s / 16.0f * (1.0f + 0.6f * flash);
    disc(c, c.pt(n.x, n.y), LV_MAX(1.0f, r), col, c.opa(o));
    if (flash > 0.55f) ring(c, c.pt(n.x, n.y), r + (1.0f - flash) * 9.0f * c.s, 1.0f, paint.accent, c.opa((flash - 0.55f) * 1.6f));
  }
  // the message itself: a bright point with a short trail behind it
  if (msg) {
    const lv_point_t m = c.pt((int16_t)mx, (int16_t)my);
    const lv_point_t from = c.pt((int16_t)(mx + (mpx - mx) * 0.45f), (int16_t)(my + (mpy - my) * 0.45f));
    line(c, from, m, LV_MAX(1.0f, 1.6f * c.s), paint.accent, c.opa(0.9f));
    disc(c, m, LV_MAX(1.5f, 2.3f * c.s), white, c.opa(1.0f));
  }

  // the radio waves: steady at their own light, a pulse running outward through them
  for (int i = 0; i < kArcs; ++i) {
    const CometArc& a = kCometArcs[i];
    float pulse = 0.0f;
    if (wave_phase >= 0.0f) {
      const float d = wave_phase - 110.0f * i;   // each wave a beat after the one inside it
      if (d > 0.0f && d < 520.0f) pulse = d < 120.0f ? d / 120.0f : 1.0f - (d - 120.0f) / 400.0f;
    }
    const float steady = (a.light / 255.0f) * (scene == CometMarkScene::Boot ? (arrive >= 0.0f ? 1.0f : ember) : 1.0f);
    const lv_color_t col = (scene == CometMarkScene::Still || (scene == CometMarkScene::Boot && arrive < 0.0f))
                               ? paint.ink : lv_color_mix(white, paint.accent, (uint8_t)(170.0f * pulse));
    wave(c, head, a, 1.0f + 0.35f * pulse, col, c.opa(steady + pulse * 0.6f));
  }

  // the head: a soft glow, then the core; the arrival flashes it and sends a wave out
  float boom = 0.0f;
  if (arrive >= 0.0f && arrive < 420.0f) boom = 1.0f - arrive / 420.0f;
  if (scene == CometMarkScene::Boot && arrive >= 0.0f && arrive < 1100.0f) {
    const float f = arrive / 1100.0f;
    const float r = (7.0f + 120.0f * easeOut(f)) * c.s;
    ring(c, head, r, LV_MAX(1.0f, (2.6f - 1.8f * f) * c.s), paint.accent, c.opa(0.85f * (1.0f - f)));
    if (f > 0.12f) {
      const float g = (f - 0.12f) / 0.88f;
      ring(c, head, (5.0f + 80.0f * easeOut(g)) * c.s, 1.0f, white, c.opa(0.45f * (1.0f - g)));
    }
  }
  const bool lit_head = scene == CometMarkScene::Idle || arrive >= 0.0f;
  const float breathe = lit_head ? 1.0f + 0.08f * sinf(t / 190.0f) : 1.0f;
  lv_draw_rect_dsc_t g;
  lv_draw_rect_dsc_init(&g);
  g.radius = LV_RADIUS_CIRCLE;
  g.bg_color = lit_head ? lv_color_mix(white, paint.accent, 150) : paint.ink;
  g.bg_opa = c.opa(1.0f);
  g.shadow_color = lit_head ? paint.accent : paint.ink;
  g.shadow_width = (lv_coord_t)lroundf((16.0f + 26.0f * boom) * c.s * breathe);
  g.shadow_spread = (lv_coord_t)lroundf((1.5f + 3.0f * boom) * c.s);
  g.shadow_opa = c.opa((lit_head ? 0.75f : 0.45f) + 0.25f * boom);
  const float gr = (5.4f + 2.5f * boom) * c.s;
  const lv_area_t ga = { (lv_coord_t)(head.x - gr), (lv_coord_t)(head.y - gr), (lv_coord_t)(head.x + gr), (lv_coord_t)(head.y + gr) };
  lv_draw_rect(dc, &g, &ga);
  disc(c, head, (3.2f + 1.8f * boom) * c.s, white, c.opa(1.0f));
}
