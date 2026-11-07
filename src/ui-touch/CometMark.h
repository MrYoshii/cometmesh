// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Comet mark, drawn from its geometry (CometMarkGeom.h, scripts/gen_comet_mark.py)
// at any size: a comet whose tail is a mesh, its head a node sending out radio waves.
//
// One drawing for every screen that shows the mark (the boot splash, the lock screen),
// for every panel: on e-paper it is ink only (no glow, no fades, widths that survive
// the 1-bit threshold), on colour panels it is light and can move.
//
// Scenes:
//   Still  the mark as the pre-LVGL boot screen painted it (comet_mark_rgb.h)
//   Boot   the boot splash: the mesh dims, a message hops in through the tail and
//          lights it on its way, the head fires a wave, then it idles (t from 0)
//   Idle   calm loop for screens that stay up (the lock screen): the waves pulse,
//          now and then a message runs in through the tail

#include <lvgl.h>
#include <stdint.h>

// The drawing's proportions (its box, in drawing units).
static constexpr int kCometMarkW = 208, kCometMarkH = 110;

enum class CometMarkScene : uint8_t { Still, Boot, Idle };

struct CometMarkPaint {
  lv_color_t ink;      // the mark's base colour (white on dark, black on e-paper)
  lv_color_t accent;   // the light that runs through it (colour panels)
  bool paper;          // e-paper: ink only
  uint8_t opa;         // the whole mark's opacity (a fade-out)
};

// box: the area the mark fills; its height sets the scale (the drawing is 208 x 110).
void cometMarkDraw(lv_draw_ctx_t* dc, const lv_area_t& box, const CometMarkPaint& paint,
                   CometMarkScene scene, uint32_t t_ms);

// How far past its box the mark can draw (the glow, the wave), for
// LV_EVENT_REFR_EXT_DRAW_SIZE.
lv_coord_t cometMarkExtDraw(lv_coord_t box_h, CometMarkScene scene);

// When the Boot scene's message reaches the head, in ms: the splash times its words by it.
static constexpr uint32_t kCometBootArriveMs = 1500;
