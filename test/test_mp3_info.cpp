// SPDX-License-Identifier: GPL-3.0-or-later
// Mp3Info: ID3v2 size, constant-bitrate length, Xing/Info frame counts (VBR), MPEG-2
// mono frames, and a stray sync pattern that is not a frame.
#include <assert.h>
#include <math.h>
#include <string.h>

#include <vector>

#include "ui-touch/Mp3Info.h"

static void frames(std::vector<uint8_t>& v, const uint8_t hdr[4], size_t bytes, int count) {
  for (int k = 0; k < count; ++k) {
    const size_t at = v.size();
    v.resize(at + bytes, 0);
    memcpy(&v[at], hdr, 4);
  }
}

int main() {
  // ID3v2: synchsafe size + 10, + 10 more with a footer.
  const uint8_t id3[10] = {'I', 'D', '3', 4, 0, 0, 0, 0, 0x02, 0x01};   // 257-byte body
  assert(Mp3Info::id3v2Size(id3, 10) == 267);
  const uint8_t id3f[10] = {'I', 'D', '3', 4, 0, 0x10, 0, 0, 0x02, 0x01};
  assert(Mp3Info::id3v2Size(id3f, 10) == 277);
  const uint8_t none[10] = {0xFF, 0xFB, 0x90, 0, 0, 0, 0, 0, 0, 0};
  assert(Mp3Info::id3v2Size(none, 10) == 0);

  // MPEG-1 layer III, 128 kbit/s, 44.1 kHz, stereo: 417-byte frames, CBR by size.
  const uint8_t cbr[4] = {0xFF, 0xFB, 0x90, 0x00};
  std::vector<uint8_t> a;
  frames(a, cbr, 417, 8);
  const double d = Mp3Info::duration(a.data(), a.size(), 417u * 1000u);
  assert(fabs(d - 417.0 * 1000 * 8 / 128000) < 1e-6);

  // The same with a Xing header in the first frame: 1000 frames of 1152 samples.
  std::vector<uint8_t> b = a;
  memcpy(&b[4 + 32], "Xing", 4);
  b[4 + 32 + 7] = 1;                         // flags: frame count present
  b[4 + 32 + 10] = 0x03; b[4 + 32 + 11] = 0xE8;   // 1000
  assert(fabs(Mp3Info::duration(b.data(), b.size(), 999999) - 1000.0 * 1152 / 44100) < 1e-6);

  // MPEG-2 layer III, 64 kbit/s, 22.05 kHz, mono: 208-byte frames, "Info" at 4 + 9.
  const uint8_t m2[4] = {0xFF, 0xF3, 0x80, 0xC0};
  Mp3Info::Frame f;
  assert(Mp3Info::parseHeader(m2, &f) && f.version == 2 && f.hz == 22050 && f.kbps == 64 && f.bytes == 208 && f.mono);
  std::vector<uint8_t> c;
  frames(c, m2, 208, 4);
  memcpy(&c[4 + 9], "Info", 4);
  c[4 + 9 + 7] = 1;
  c[4 + 9 + 11] = 100;
  assert(fabs(Mp3Info::duration(c.data(), c.size(), 1) - 100.0 * 576 / 22050) < 1e-6);

  // Junk with a lone FF FB first: skipped, the real frames after it count.
  std::vector<uint8_t> j = {0xFF, 0xFB, 0x90, 0x00, 1, 2, 3};
  const size_t lead = j.size();
  frames(j, cbr, 417, 4);
  const double dj = Mp3Info::duration(j.data(), j.size(), (uint32_t)j.size());
  assert(fabs(dj - (double)(j.size() - lead) * 8 / 128000) < 1e-6);

  // Nothing that looks like audio.
  std::vector<uint8_t> z(500, 0x55);
  assert(Mp3Info::duration(z.data(), z.size(), 500) == 0);
  return 0;
}
