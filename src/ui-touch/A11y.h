// A11y.h -- what the screen reader will say for an on-screen object.
//
// Prepared for Comet's screen reader (docs/ACCESSIBILITY.md); the speech itself
// (eSpeak NG) comes after the Pager UI. Until then this only builds the text, and
// builds with -DCOMET_A11Y_TRACE log it on every focus change, so new screens can be
// checked for stops without a good name while they are made.
//
// A description is: name, role and state, place, hint. For example
//   "Brightness, slider, 60 percent, 2 of 5"
//   "Chats, button, 1 of 16. 3 new, Lukas: Bin um 6 am Parkplatz"
// The name is the one given with a11ySetName(), else the text of the labels inside the
// object (icon glyphs skipped). Header-only, LVGL only, so the web host build has it.
#pragma once

#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef A11Y_TR
#define A11Y_TR(s) (s)   // UITask defines it as TR() before including, so the words follow the UI language
#endif

namespace a11y {

struct Info { char* name; char* hint; };

static void infoDeleteCb(lv_event_t* e) {
  Info* in = (Info*)lv_event_get_user_data(e);
  if (!in) return;
  free(in->name);
  free(in->hint);
  free(in);
}
static inline Info* infoOf(lv_obj_t* o) {
  return o ? (Info*)lv_obj_get_event_user_data(o, infoDeleteCb) : nullptr;
}
static inline char* dupOrNull(const char* s) {
  if (!s || !s[0]) return nullptr;
  const size_t n = strlen(s) + 1;
  char* d = (char*)malloc(n);
  if (d) memcpy(d, s, n);
  return d;
}

// A label that is only icon glyphs (Private Use Area, U+E000..U+F8FF: LV_SYMBOL_* and the
// UI icon fonts) names nothing.
static inline bool onlyIcons(const char* t) {
  const unsigned char* p = (const unsigned char*)t;
  bool any = false;
  while (*p) {
    if (*p == ' ') { ++p; continue; }
    if (*p == 0xEE || (*p == 0xEF && p[1] >= 0x80 && p[1] <= 0xA3)) { any = true; p += 3; continue; }
    return false;
  }
  return any;
}

static inline void append(char* out, size_t cap, size_t& len, const char* sep, const char* s) {
  if (!s || !s[0] || len + 1 >= cap) return;
  const int w = snprintf(out + len, cap - len, "%s%s", len ? sep : "", s);
  if (w > 0) len = LV_MIN(cap - 1, len + (size_t)w);
}

// The labels' text under o, in tree order, joined by ", ".
static inline void labelsOf(lv_obj_t* o, char* out, size_t cap, size_t& len, int depth = 0) {
  if (!o || depth > 4 || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
  if (lv_obj_check_type(o, &lv_label_class)) {
    const char* t = lv_label_get_text(o);
    if (t && t[0] && !onlyIcons(t)) append(out, cap, len, ", ", t);
    return;
  }
  const uint32_t n = lv_obj_get_child_cnt(o);
  for (uint32_t i = 0; i < n; ++i) labelsOf(lv_obj_get_child(o, i), out, cap, len, depth + 1);
}

}  // namespace a11y

// Give an object the name (and an optional hint) the reader says for it. Copies both;
// they go with the object. Call again to change them (a value that moves, a new hint).
static inline void a11ySetName(lv_obj_t* o, const char* name, const char* hint = nullptr) {
  if (!o) return;
  a11y::Info* in = a11y::infoOf(o);
  if (!in) {
    in = (a11y::Info*)calloc(1, sizeof(a11y::Info));
    if (!in) return;
    lv_obj_add_event_cb(o, a11y::infoDeleteCb, LV_EVENT_DELETE, in);
  }
  free(in->name); in->name = a11y::dupOrNull(name);
  free(in->hint); in->hint = a11y::dupOrNull(hint);
}
static inline void a11ySetHint(lv_obj_t* o, const char* hint) {
  a11y::Info* in = a11y::infoOf(o);
  if (!in) { a11ySetName(o, nullptr, hint); return; }
  free(in->hint); in->hint = a11y::dupOrNull(hint);
}

// The whole description of o into out. Returns its length.
static inline size_t a11yDescribe(lv_obj_t* o, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (!o || !lv_obj_is_valid(o)) return 0;
  size_t len = 0;
  const a11y::Info* in = a11y::infoOf(o);
  // Name
  if (in && in->name) a11y::append(out, cap, len, ", ", in->name);
  else if (lv_obj_check_type(o, &lv_textarea_class)) {
    const char* ph = lv_textarea_get_placeholder_text(o);
    a11y::append(out, cap, len, ", ", ph);
  } else if (lv_obj_check_type(o, &lv_dropdown_class)) {
    // its own text is the list; the choice is said below
  } else a11y::labelsOf(o, out, cap, len);
  // Role and state
  char v[96];
  if (lv_obj_check_type(o, &lv_switch_class)) {
    a11y::append(out, cap, len, ", ", A11Y_TR("switch"));
    a11y::append(out, cap, len, ", ", lv_obj_has_state(o, LV_STATE_CHECKED) ? A11Y_TR("on") : A11Y_TR("off"));
  } else if (lv_obj_check_type(o, &lv_checkbox_class)) {
    a11y::append(out, cap, len, ", ", A11Y_TR("check box"));
    a11y::append(out, cap, len, ", ", lv_obj_has_state(o, LV_STATE_CHECKED) ? A11Y_TR("checked") : A11Y_TR("not checked"));
  } else if (lv_obj_check_type(o, &lv_slider_class)) {
    const int32_t lo = lv_slider_get_min_value(o), hi = lv_slider_get_max_value(o), val = lv_slider_get_value(o);
    a11y::append(out, cap, len, ", ", A11Y_TR("slider"));
    snprintf(v, sizeof v, "%d %s", hi > lo ? (int)((val - lo) * 100 / (hi - lo)) : 0, A11Y_TR("percent"));
    a11y::append(out, cap, len, ", ", v);
  } else if (lv_obj_check_type(o, &lv_dropdown_class)) {
    a11y::append(out, cap, len, ", ", A11Y_TR("list"));
    lv_dropdown_get_selected_str(o, v, sizeof v);
    a11y::append(out, cap, len, ", ", v);
  } else if (lv_obj_check_type(o, &lv_textarea_class)) {
    a11y::append(out, cap, len, ", ", A11Y_TR("text field"));
    const char* t = lv_textarea_get_text(o);
    a11y::append(out, cap, len, ", ", (t && t[0]) ? t : A11Y_TR("empty"));
  } else if (lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) {
    a11y::append(out, cap, len, ", ", A11Y_TR("button"));
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_CHECKABLE) || lv_obj_has_state(o, LV_STATE_CHECKED))
      a11y::append(out, cap, len, ", ", lv_obj_has_state(o, LV_STATE_CHECKED) ? A11Y_TR("selected") : A11Y_TR("not selected"));
  }
  if (lv_obj_has_state(o, LV_STATE_DISABLED)) a11y::append(out, cap, len, ", ", A11Y_TR("unavailable"));
  // Place among its clickable siblings, for a list or a row of choices
  if (lv_obj_t* par = lv_obj_get_parent(o)) {
    const uint32_t n = lv_obj_get_child_cnt(par);
    int idx = 0, cnt = 0;
    for (uint32_t i = 0; i < n; ++i) {
      lv_obj_t* c = lv_obj_get_child(par, i);
      if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN) || !lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE)) continue;
      ++cnt;
      if (c == o) idx = cnt;
    }
    if (idx > 0 && cnt >= 3) {
      snprintf(v, sizeof v, "%d %s %d", idx, A11Y_TR("of"), cnt);
      a11y::append(out, cap, len, ", ", v);
    }
  }
  // Hint
  if (in && in->hint) a11y::append(out, cap, len, ". ", in->hint);
  return len;
}
