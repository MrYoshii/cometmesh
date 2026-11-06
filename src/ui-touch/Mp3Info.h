// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The length of an MP3 file from its first bytes, for audiobook folders copied onto the
// card (Books imports them without a server). An ID3v2 tag is skipped by the caller
// (id3v2Size), then the first MPEG audio frame decides: a Xing/Info or VBRI header gives
// the frame count (exact, VBR included); without one the file is taken as constant
// bitrate and its audio bytes divided by the bitrate.
// No Arduino, no LVGL: host-tested in test/test_mp3_info.cpp.

#include <cstddef>
#include <cstdint>

namespace Mp3Info {

// Bytes of an ID3v2 tag at the start of a file (0 when there is none). head: >= 10 bytes.
inline uint32_t id3v2Size(const uint8_t* head, size_t n) {
  if (n < 10 || head[0] != 'I' || head[1] != 'D' || head[2] != '3') return 0;
  if ((head[6] | head[7] | head[8] | head[9]) & 0x80) return 0;   // not synchsafe: not a tag
  const uint32_t body = ((uint32_t)head[6] << 21) | ((uint32_t)head[7] << 14) |
                        ((uint32_t)head[8] << 7) | (uint32_t)head[9];
  return 10 + body + ((head[5] & 0x10) ? 10 : 0);   // a footer, when flagged
}

struct Frame {
  int version;        // 1, 2 or 25 (MPEG 2.5)
  int layer;          // 1..3
  uint32_t kbps;
  uint32_t hz;
  uint32_t samples;   // per frame
  uint32_t bytes;     // this frame, padding included
  bool mono;
};

inline bool parseHeader(const uint8_t* p, Frame* f) {
  if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
  const int ver_bits = (p[1] >> 3) & 3;     // 0: 2.5, 1: reserved, 2: 2, 3: 1
  const int layer_bits = (p[1] >> 1) & 3;   // 1: III, 2: II, 3: I
  const int br = (p[2] >> 4) & 15, sr = (p[2] >> 2) & 3, pad = (p[2] >> 1) & 1;
  if (ver_bits == 1 || layer_bits == 0 || br == 0 || br == 15 || sr == 3) return false;
  static const uint16_t kRates[3] = {44100, 48000, 32000};
  static const uint16_t kV1[3][15] = {
    {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},   // layer I
    {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},      // layer II
    {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}};      // layer III
  static const uint16_t kV2[3][15] = {
    {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}};
  f->version = ver_bits == 3 ? 1 : ver_bits == 2 ? 2 : 25;
  f->layer = 4 - layer_bits;
  f->kbps = (f->version == 1 ? kV1 : kV2)[f->layer - 1][br];
  f->hz = kRates[sr] >> (f->version == 1 ? 0 : f->version == 2 ? 1 : 2);
  f->mono = ((p[3] >> 6) & 3) == 3;
  if (f->layer == 1) {
    f->samples = 384;
    f->bytes = (12 * f->kbps * 1000 / f->hz + pad) * 4;
  } else {
    f->samples = (f->layer == 3 && f->version != 1) ? 576 : 1152;
    f->bytes = f->samples / 8 * f->kbps * 1000 / f->hz + pad;
  }
  return f->bytes >= 24;
}

inline uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Seconds of audio. head: bytes from audio_start (right after any ID3v2 tag), n of them
// (a few KB is plenty); audio_bytes: the file size minus audio_start. 0 when no frame
// is found in head.
inline double duration(const uint8_t* head, size_t n, uint32_t audio_bytes) {
  for (size_t i = 0; i + 4 <= n; ++i) {
    Frame f;
    if (!parseHeader(head + i, &f)) continue;
    // A real frame is followed by another one: rules out a stray FF Ex in the data.
    if (i + f.bytes + 4 <= n) {
      Frame g;
      if (!parseHeader(head + i + f.bytes, &g) || g.hz != f.hz) continue;
    }
    const size_t side = f.version == 1 ? (f.mono ? 17 : 32) : (f.mono ? 9 : 17);
    const uint8_t* x = head + i + 4 + side;
    if (f.layer == 3 && x + 16 <= head + n && (!__builtin_memcmp(x, "Xing", 4) || !__builtin_memcmp(x, "Info", 4))) {
      const uint32_t flags = be32(x + 4);
      if (flags & 1) {
        const uint32_t frames = be32(x + 8);
        if (frames) return (double)frames * f.samples / f.hz;
      }
    }
    const uint8_t* v = head + i + 4 + 32;
    if (v + 18 <= head + n && !__builtin_memcmp(v, "VBRI", 4)) {
      const uint32_t frames = be32(v + 14);
      if (frames) return (double)frames * f.samples / f.hz;
    }
    const uint32_t bytes = audio_bytes > i ? audio_bytes - (uint32_t)i : 0;
    return (double)bytes * 8.0 / (f.kbps * 1000.0);
  }
  return 0;
}

}  // namespace Mp3Info
