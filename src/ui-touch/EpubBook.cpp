// SPDX-License-Identifier: GPL-3.0-or-later
#include "EpubBook.h"

#include <cstdio>
#include <cstring>

#include "ReaderContent.h"

// The S3 ROM's tinfl (no flash cost); the host test and the web build use miniz,
// which has the same API (see PmTiles.cpp).
#if __has_include("rom/miniz.h")
  #include "rom/miniz.h"
#elif __has_include("miniz.h")
  #include "miniz.h"
#else
  #error "EpubBook needs tinfl (rom/miniz.h or miniz.h)"
#endif

namespace EpubBook {
namespace {

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool ciEq(const char* a, size_t an, const char* b, size_t bn) {
  if (an != bn) return false;
  for (size_t i = 0; i < an; ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x += 32;
    if (y >= 'A' && y <= 'Z') y += 32;
    if (x != y) return false;
  }
  return true;
}

// Case-insensitive search for needle in [hay, hay+len).
const char* ciFind(const char* hay, size_t len, const char* needle) {
  const size_t n = strlen(needle);
  if (n == 0 || len < n) return nullptr;
  for (size_t i = 0; i + n <= len; ++i) {
    size_t k = 0;
    while (k < n) {
      char x = hay[i + k], y = needle[k];
      if (x >= 'A' && x <= 'Z') x += 32;
      if (y >= 'A' && y <= 'Z') y += 32;
      if (x != y) break;
      ++k;
    }
    if (k == n) return hay + i;
  }
  return nullptr;
}

// The next "<name" that is that whole tag name ("<rootfile" must not match "<rootfiles").
const char* findTag(const char* hay, size_t len, const char* open) {
  const size_t n = strlen(open);
  const char* end = hay + len;
  for (const char* p = hay; p < end;) {
    const char* t = ciFind(p, (size_t)(end - p), open);
    if (!t) return nullptr;
    const char c = t + n < end ? t[n] : '>';
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/' || c == '>') return t;
    p = t + n;
  }
  return nullptr;
}

// Value of attribute `name` inside one tag [tag, end). Handles any attribute order.
bool attr(const char* tag, const char* end, const char* name, char* out, size_t cap) {
  const size_t n = strlen(name);
  for (const char* p = tag; p + n + 1 < end; ++p) {
    if (!(p == tag || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r')) continue;
    if (!ciEq(p, n, name, n)) continue;
    const char* q = p + n;
    while (q < end && (*q == ' ' || *q == '\t')) ++q;
    if (q >= end || *q != '=') continue;
    ++q;
    while (q < end && (*q == ' ' || *q == '\t')) ++q;
    if (q >= end || (*q != '"' && *q != '\'')) continue;
    const char quote = *q++;
    size_t w = 0;
    while (q < end && *q != quote && w + 1 < cap) out[w++] = *q++;
    out[w] = 0;
    return true;
  }
  return false;
}

// Decode %XX in place (hrefs in the OPF are URL-encoded; ZIP names are not).
void urlDecode(char* s) {
  char* w = s;
  for (char* r = s; *r; ++r) {
    if (r[0] == '%' && r[1] && r[2]) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = hex(r[1]), lo = hex(r[2]);
      if (hi >= 0 && lo >= 0) { *w++ = (char)(hi * 16 + lo); r += 2; continue; }
    }
    *w++ = *r;
  }
  *w = 0;
}

// Normalise "a/b/../c/./d" -> "a/c/d".
void normalisePath(char* s) {
  char out[460];
  size_t w = 0;
  const char* p = s;
  while (*p) {
    const char* seg = p;
    while (*p && *p != '/') ++p;
    const size_t len = (size_t)(p - seg);
    if (len == 2 && seg[0] == '.' && seg[1] == '.') {
      if (w) { --w; while (w && out[w - 1] != '/') --w; }
    } else if (!(len == 1 && seg[0] == '.') && len) {
      if (w + len + 1 >= sizeof out) break;
      memcpy(out + w, seg, len);
      w += len;
      if (*p == '/') out[w++] = '/';
    }
    if (*p == '/') ++p;
  }
  if (w && out[w - 1] == '/') --w;
  out[w] = 0;
  memcpy(s, out, w + 1);
}

// The text of the first <tag ...>text</tag> (tag may carry a namespace prefix).
void elementText(const char* xml, size_t len, const char* tag, char* out, size_t cap) {
  out[0] = 0;
  char open[32];
  snprintf(open, sizeof open, "<%s", tag);
  const char* p = findTag(xml, len, open);
  if (!p) return;
  const char* end = xml + len;
  const char* gt = (const char*)memchr(p, '>', (size_t)(end - p));
  if (!gt) return;
  const char* lt = (const char*)memchr(gt, '<', (size_t)(end - gt));
  if (!lt) return;
  // Entities and stray whitespace, through the same decoder the chapters use.
  const size_t raw = (size_t)(lt - gt - 1);
  ReaderContent::htmlToText(gt + 1, raw, out, cap, nullptr, nullptr, 0, nullptr);
  for (char* c = out; *c; ++c) if (*c == '\n') *c = ' ';
}

}  // namespace

Book::~Book() { close(); }

void Book::close() {
  if (hooks_.release) {
    if (entries_) hooks_.release(entries_);
    if (names_) hooks_.release(names_);
    if (spine_) hooks_.release(spine_);
  }
  entries_ = nullptr; names_ = nullptr; spine_ = nullptr;
  entry_count_ = 0; spine_count_ = 0;
  title_[0] = 0; author_[0] = 0;
  read_ = nullptr; ctx_ = nullptr; size_ = 0;
}

bool Book::open(ReadAt read, void* ctx, uint32_t size, const Hooks& hooks) {
  close();
  if (!read || !hooks.alloc || !hooks.release || size < 22) return false;
  read_ = read; ctx_ = ctx; size_ = size; hooks_ = hooks;

  // End of central directory: the last 22 bytes, or earlier behind a comment.
  const uint32_t tail_len = size < 65557u ? size : 65557u;
  uint8_t* tail = (uint8_t*)hooks_.alloc(tail_len);
  if (!tail) return false;
  bool ok = read_(ctx_, size - tail_len, tail, tail_len) == tail_len;
  int32_t eocd = -1;
  for (int32_t i = (int32_t)tail_len - 22; ok && i >= 0; --i) {
    if (rd32(tail + i) == 0x06054b50u) { eocd = i; break; }
  }
  uint32_t cd_size = 0, cd_off = 0, cd_count = 0;
  if (eocd >= 0) {
    cd_count = rd16(tail + eocd + 10);
    cd_size  = rd32(tail + eocd + 12);
    cd_off   = rd32(tail + eocd + 16);
  }
  hooks_.release(tail);
  if (eocd < 0 || cd_count == 0 || cd_count > kMaxEntries ||
      (uint64_t)cd_off + cd_size > size) { close(); return false; }

  uint8_t* cd = (uint8_t*)hooks_.alloc(cd_size);
  entries_ = (Entry*)hooks_.alloc(sizeof(Entry) * cd_count);
  names_ = (char*)hooks_.alloc(cd_size);   // names are a subset of the directory bytes
  if (!cd || !entries_ || !names_ || read_(ctx_, cd_off, cd, cd_size) != cd_size) {
    if (cd) hooks_.release(cd);
    close();
    return false;
  }
  uint32_t pos = 0, names_used = 0;
  for (uint32_t n = 0; n < cd_count; ++n) {
    if (pos + 46 > cd_size || rd32(cd + pos) != 0x02014b50u) break;
    const uint16_t nlen = rd16(cd + pos + 28), xlen = rd16(cd + pos + 30), clen = rd16(cd + pos + 32);
    if (pos + 46 + nlen > cd_size) break;
    Entry& e = entries_[entry_count_++];
    e.method = rd16(cd + pos + 10);
    e.comp_size = rd32(cd + pos + 20);
    e.size = rd32(cd + pos + 24);
    e.local_off = rd32(cd + pos + 42);
    e.name_off = names_used;
    e.name_len = nlen;
    memcpy(names_ + names_used, cd + pos + 46, nlen);
    names_used += nlen;
    pos += 46u + nlen + xlen + clen;
  }
  hooks_.release(cd);
  if (entry_count_ == 0) { close(); return false; }

  // container.xml -> the package document.
  size_t clen = 0;
  uint8_t* container = readEntry("META-INF/container.xml", &clen);
  if (!container) { close(); return false; }
  char opf[256] = "";
  const char* rf = findTag((const char*)container, clen, "<rootfile");
  if (rf) {
    const char* gt = (const char*)memchr(rf, '>', clen - (size_t)(rf - (const char*)container));
    if (gt) attr(rf, gt, "full-path", opf, sizeof opf);
  }
  hooks_.release(container);
  if (!opf[0] || !parseOpf(opf)) { close(); return false; }
  return spine_count_ > 0;
}

int Book::findEntry(const char* name, size_t name_len) const {
  for (int i = 0; i < entry_count_; ++i) {
    const Entry& e = entries_[i];
    if (e.name_len == name_len && memcmp(names_ + e.name_off, name, name_len) == 0) return i;
  }
  // Some books differ in case between the OPF and the archive.
  for (int i = 0; i < entry_count_; ++i) {
    const Entry& e = entries_[i];
    if (ciEq(names_ + e.name_off, e.name_len, name, name_len)) return i;
  }
  return -1;
}

uint8_t* Book::readEntry(const char* name, size_t* len_out) {
  const int idx = findEntry(name, strlen(name));
  return idx < 0 ? nullptr : readEntryAt(idx, len_out);
}

uint8_t* Book::readEntryAt(int idx, size_t* len_out) {
  if (len_out) *len_out = 0;
  if (idx < 0 || idx >= entry_count_) return nullptr;
  const Entry& e = entries_[idx];
  if (e.size > kMaxEntryBytes || (e.method != 0 && e.method != 8)) return nullptr;
  uint8_t lh[30];
  if (read_(ctx_, e.local_off, lh, 30) != 30 || rd32(lh) != 0x04034b50u) return nullptr;
  const uint32_t data_off = e.local_off + 30u + rd16(lh + 26) + rd16(lh + 28);
  if ((uint64_t)data_off + e.comp_size > size_) return nullptr;
  uint8_t* out = (uint8_t*)hooks_.alloc(e.size + 1);
  if (!out) return nullptr;
  bool ok = false;
  if (e.method == 0) {
    ok = e.comp_size == e.size && read_(ctx_, data_off, out, e.size) == e.size;
  } else {
    uint8_t* comp = (uint8_t*)hooks_.alloc(e.comp_size ? e.comp_size : 1);
    tinfl_decompressor* d = (tinfl_decompressor*)hooks_.alloc(sizeof(tinfl_decompressor));
    if (comp && d && read_(ctx_, data_off, comp, e.comp_size) == e.comp_size) {
      tinfl_init(d);
      size_t in_len = e.comp_size, out_len = e.size;
      const tinfl_status st = tinfl_decompress(d, comp, &in_len, out, out, &out_len,
                                               TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
      ok = st == TINFL_STATUS_DONE && out_len == e.size;
    }
    if (comp) hooks_.release(comp);
    if (d) hooks_.release(d);
  }
  if (!ok) { hooks_.release(out); return nullptr; }
  out[e.size] = 0;
  if (len_out) *len_out = e.size;
  return out;
}

bool Book::parseOpf(const char* opf_path) {
  size_t len = 0;
  char* xml = (char*)readEntry(opf_path, &len);
  if (!xml) return false;
  elementText(xml, len, "dc:title", title_, sizeof title_);
  if (!title_[0]) elementText(xml, len, "title", title_, sizeof title_);
  elementText(xml, len, "dc:creator", author_, sizeof author_);

  char dir[256];
  snprintf(dir, sizeof dir, "%s", opf_path);
  char* slash = strrchr(dir, '/');
  if (slash) slash[1] = 0; else dir[0] = 0;

  spine_ = (uint16_t*)hooks_.alloc(sizeof(uint16_t) * kMaxSpine);
  if (!spine_) { hooks_.release(xml); return false; }
  const char* end = xml + len;
  const char* spine = findTag(xml, len, "<spine");
  const char* manifest = findTag(xml, len, "<manifest");
  if (!spine || !manifest) { hooks_.release(xml); return false; }
  for (const char* p = spine; p < end && spine_count_ < (int)kMaxSpine;) {
    const char* ref = findTag(p, (size_t)(end - p), "<itemref");
    if (!ref) break;
    const char* gt = (const char*)memchr(ref, '>', (size_t)(end - ref));
    if (!gt) break;
    p = gt + 1;
    char idref[96];
    if (!attr(ref, gt, "idref", idref, sizeof idref)) continue;
    char linear[8];
    if (attr(ref, gt, "linear", linear, sizeof linear) && ciEq(linear, strlen(linear), "no", 2)) continue;
    // The manifest item with that id.
    for (const char* m = manifest; m < spine;) {
      const char* item = findTag(m, (size_t)(spine - m), "<item");
      if (!item) break;
      const char* igt = (const char*)memchr(item, '>', (size_t)(end - item));
      if (!igt) break;
      m = igt + 1;
      char id[96];
      if (!attr(item, igt, "id", id, sizeof id) || strcmp(id, idref) != 0) continue;
      char href[200];
      if (!attr(item, igt, "href", href, sizeof href)) break;
      char* hash = strchr(href, '#');
      if (hash) *hash = 0;
      urlDecode(href);
      char full[460];
      snprintf(full, sizeof full, "%s%s", dir, href);
      normalisePath(full);
      const int idx = findEntry(full, strlen(full));
      if (idx >= 0) spine_[spine_count_++] = (uint16_t)idx;
      break;
    }
  }
  hooks_.release(xml);
  return spine_count_ > 0;
}

char* Book::chapterText(int i, size_t* len_out) {
  if (len_out) *len_out = 0;
  if (i < 0 || i >= spine_count_) return nullptr;
  size_t len = 0;
  char* html = (char*)readEntryAt(spine_[i], &len);
  if (!html) return nullptr;
  // Skip <head> (its <title> would print as the first line of every chapter).
  const char* body = ciFind(html, len, "<body");
  const size_t skip = body ? (size_t)(body - html) : 0;
  const size_t cap = len - skip + 16;
  char* text = (char*)hooks_.alloc(cap);
  if (!text) { hooks_.release(html); return nullptr; }
  const size_t n = ReaderContent::htmlToText(html + skip, len - skip, text, cap, nullptr, nullptr, 0, nullptr);
  hooks_.release(html);
  if (len_out) *len_out = n;
  return text;
}

void Book::freeText(char* text) {
  if (text && hooks_.release) hooks_.release(text);
}

void titleFromFileName(const char* name, char* out, size_t cap) {
  if (!out || cap == 0) return;
  const char* base = strrchr(name, '/');
  base = base ? base + 1 : name;
  size_t w = 0;
  const char* dot = strrchr(base, '.');
  for (const char* p = base; *p && (!dot || p < dot) && w + 1 < cap; ++p)
    out[w++] = (*p == '_' || *p == '-') ? ' ' : *p;
  out[w] = 0;
}

}  // namespace EpubBook
