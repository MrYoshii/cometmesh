// SPDX-License-Identifier: GPL-3.0-or-later
// EpubBook: a ZIP built here (stored entries and one deflated chapter), container.xml ->
// OPF -> spine order, metadata, chapter text without the <head>, relative and
// URL-encoded hrefs, and refusal of something that is not an EPUB.

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "ui-touch/EpubBook.h"

namespace {

struct Zip {
  std::vector<uint8_t> data, cd;
  int count = 0;
  static void put16(std::vector<uint8_t>& v, uint32_t x) { v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); }
  static void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, x & 0xFFFF); put16(v, x >> 16); }
  // deflated: the raw DEFLATE stream of body (made by Python's zlib, wbits=-15).
  void add(const std::string& name, const std::string& body,
           const std::vector<uint8_t>* deflated = nullptr) {
    std::vector<uint8_t> payload(body.begin(), body.end());
    uint16_t method = 0;
    if (deflated) { payload = *deflated; method = 8; }
    const uint32_t crc = 0;   // EpubBook does not check it
    const uint32_t off = (uint32_t)data.size();
    put32(data, 0x04034b50); put16(data, 20); put16(data, 0); put16(data, method);
    put16(data, 0); put16(data, 0); put32(data, crc);
    put32(data, (uint32_t)payload.size()); put32(data, (uint32_t)body.size());
    put16(data, (uint32_t)name.size()); put16(data, 0);
    data.insert(data.end(), name.begin(), name.end());
    data.insert(data.end(), payload.begin(), payload.end());
    put32(cd, 0x02014b50); put16(cd, 20); put16(cd, 20); put16(cd, 0); put16(cd, method);
    put16(cd, 0); put16(cd, 0); put32(cd, crc);
    put32(cd, (uint32_t)payload.size()); put32(cd, (uint32_t)body.size());
    put16(cd, (uint32_t)name.size()); put16(cd, 0); put16(cd, 0); put16(cd, 0); put16(cd, 0);
    put32(cd, 0); put32(cd, off);
    cd.insert(cd.end(), name.begin(), name.end());
    ++count;
  }
  std::vector<uint8_t> finish() {
    std::vector<uint8_t> out = data;
    const uint32_t cd_off = (uint32_t)out.size();
    out.insert(out.end(), cd.begin(), cd.end());
    put32(out, 0x06054b50); put16(out, 0); put16(out, 0);
    put16(out, count); put16(out, count);
    put32(out, (uint32_t)cd.size()); put32(out, cd_off); put16(out, 0);
    return out;
  }
};

size_t readMem(void* ctx, uint32_t off, uint8_t* buf, size_t len) {
  auto* v = static_cast<std::vector<uint8_t>*>(ctx);
  if (off >= v->size()) return 0;
  if (off + len > v->size()) len = v->size() - off;
  memcpy(buf, v->data() + off, len);
  return len;
}

const EpubBook::Hooks kHooks = {malloc, free};

const char kChapter1[] =
    "<html><head><title>HEADTITLE</title></head><body><h1>Chapter One</h1>"
    "<p>It was a bright cold day.</p><p>The clocks struck thirteen.</p></body></html>";
const std::vector<uint8_t> kChapter1Deflated = {
  0x25, 0x8e, 0x41, 0x0a, 0xc2, 0x30, 0x10, 0x45, 0xaf, 0x32, 0x27, 0x30, 0x74, 0x3f, 0x0c, 0x88,
  0x16, 0x2c, 0x08, 0x6e, 0x72, 0x81, 0x34, 0x19, 0x9c, 0xd0, 0xd8, 0x84, 0x64, 0x44, 0x7a, 0x7b,
  0xdb, 0x66, 0xfb, 0xdf, 0xe3, 0xf1, 0x51, 0xf4, 0x93, 0x08, 0x85, 0x5d, 0x20, 0xd4, 0xa8, 0x89,
  0xe9, 0x31, 0x5e, 0xef, 0x76, 0xb2, 0xcf, 0x11, 0x4d, 0x1f, 0xd0, 0x74, 0x3c, 0xe7, 0xb0, 0xed,
  0xea, 0x40, 0x37, 0x71, 0x45, 0xb9, 0xc2, 0x6b, 0xe5, 0x9d, 0x0d, 0x84, 0x85, 0x26, 0x85, 0x9f,
  0x6b, 0xe0, 0x60, 0xae, 0xf1, 0x2d, 0x0a, 0x3e, 0xa7, 0x00, 0xc1, 0x6d, 0x17, 0x34, 0xe5, 0xe0,
  0x56, 0x18, 0x7c, 0xca, 0x7e, 0x69, 0xd0, 0xb4, 0x7e, 0xfd, 0x02, 0x2a, 0xb1, 0x2a, 0xf3, 0xda,
  0x0d, 0xd3, 0xe3, 0xe6, 0xbc, 0xf3, 0x07};

}  // namespace

int main() {
  Zip z;
  z.add("mimetype", "application/epub+zip");
  z.add("META-INF/container.xml",
        "<?xml version='1.0'?><container><rootfiles>"
        "<rootfile media-type='application/oebps-package+xml' full-path='OEBPS/content.opf'/>"
        "</rootfiles></container>");
  z.add("OEBPS/content.opf",
        "<package><metadata><dc:title>The Time &amp; Space</dc:title>"
        "<dc:creator opf:role=\"aut\">H. G. Wells</dc:creator></metadata>"
        "<manifest>"
        "<item href=\"text/ch%201.xhtml\" id=\"c1\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"c2\" media-type=\"application/xhtml+xml\" href=\"text/../text/ch2.xhtml#top\"/>"
        "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "</manifest><spine>"
        "<itemref idref=\"c1\"/><itemref idref=\"nav\" linear=\"no\"/><itemref idref=\"c2\"/>"
        "</spine></package>");
  z.add("OEBPS/text/ch 1.xhtml", kChapter1, &kChapter1Deflated);
  z.add("OEBPS/text/ch2.xhtml", "<html><body><p>Second &mdash; chapter.</p></body></html>");
  z.add("OEBPS/nav.xhtml", "<html><body>NAV</body></html>");
  std::vector<uint8_t> file = z.finish();

  EpubBook::Book book;
  assert(book.open(readMem, &file, (uint32_t)file.size(), kHooks));
  assert(strcmp(book.title(), "The Time & Space") == 0);
  assert(strcmp(book.author(), "H. G. Wells") == 0);
  assert(book.chapterCount() == 2);   // linear="no" is skipped

  size_t n = 0;
  char* t = book.chapterText(0, &n);
  assert(t && n == strlen(t));
  assert(strstr(t, "HEADTITLE") == nullptr);
  assert(strncmp(t, "Chapter One", 11) == 0);
  assert(strstr(t, "It was a bright cold day.\n\nThe clocks struck thirteen."));
  book.freeText(t);

  t = book.chapterText(1, &n);
  assert(t && strstr(t, "Second \xE2\x80\x94 chapter."));
  book.freeText(t);
  assert(book.chapterText(2, &n) == nullptr);

  // Not an EPUB: a ZIP without container.xml, and plain bytes.
  Zip bad;
  bad.add("hello.txt", "hi");
  std::vector<uint8_t> badfile = bad.finish();
  EpubBook::Book b2;
  assert(!b2.open(readMem, &badfile, (uint32_t)badfile.size(), kHooks));
  std::vector<uint8_t> junk(100, 'x');
  assert(!b2.open(readMem, &junk, (uint32_t)junk.size(), kHooks));

  char title[64];
  EpubBook::titleFromFileName("/bk/the_time-machine.epub", title, sizeof title);
  assert(strcmp(title, "the time machine") == 0);
  return 0;
}
