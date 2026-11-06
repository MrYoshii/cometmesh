// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// EPUB reading for the Books app: a ZIP container (stored or deflated entries),
// META-INF/container.xml -> the OPF package, its metadata (title, author), and
// the spine (reading order) of XHTML chapters. A chapter comes back as plain
// text through ReaderContent::htmlToText, so the reader only ever pages text.
//
// The book is read through a random-access callback, never loaded whole: the
// central directory and one chapter at a time live in memory. Allocation goes
// through the alloc/free hooks so the firmware can put buffers in PSRAM.
// No LVGL, no Arduino: host-tested in test/test_epub_book.cpp.

#include <cstddef>
#include <cstdint>

namespace EpubBook {

// Read len bytes at offset into buf; returns the bytes read.
typedef size_t (*ReadAt)(void* ctx, uint32_t offset, uint8_t* buf, size_t len);

struct Hooks {
  void* (*alloc)(size_t);
  void  (*release)(void*);
};

constexpr size_t kTitleMax = 96;
constexpr size_t kMaxSpine = 400;
constexpr size_t kMaxEntries = 1500;
constexpr size_t kMaxEntryBytes = 2u * 1024u * 1024u;   // one inflated chapter

class Book {
public:
  Book() = default;
  ~Book();
  Book(const Book&) = delete;
  Book& operator=(const Book&) = delete;

  // Parse the container. size is the file size. False when it is not an EPUB we can read.
  bool open(ReadAt read, void* ctx, uint32_t size, const Hooks& hooks);
  void close();

  const char* title() const { return title_; }
  const char* author() const { return author_; }
  int chapterCount() const { return spine_count_; }

  // Chapter i as plain text (NUL-terminated, release with freeText). nullptr on failure.
  char* chapterText(int i, size_t* len_out);
  void freeText(char* text);

  // A whole entry by name, inflated (release with freeText). For tests and covers.
  uint8_t* readEntry(const char* name, size_t* len_out);

private:
  struct Entry {
    uint32_t name_off;     // into names_
    uint16_t name_len;
    uint16_t method;       // 0 stored, 8 deflate
    uint32_t comp_size;
    uint32_t size;
    uint32_t local_off;
  };
  int findEntry(const char* name, size_t name_len) const;
  uint8_t* readEntryAt(int idx, size_t* len_out);
  bool parseOpf(const char* opf_path);

  ReadAt read_ = nullptr;
  void* ctx_ = nullptr;
  uint32_t size_ = 0;
  Hooks hooks_ = {nullptr, nullptr};
  Entry* entries_ = nullptr;
  int entry_count_ = 0;
  char* names_ = nullptr;
  uint16_t* spine_ = nullptr;      // entry index per chapter
  int spine_count_ = 0;
  char title_[kTitleMax] = {0};
  char author_[kTitleMax] = {0};
};

// Strip a file name to a readable title: "the_time_machine.epub" -> "the time machine".
void titleFromFileName(const char* name, char* out, size_t cap);

}  // namespace EpubBook
