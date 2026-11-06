// SPDX-License-Identifier: GPL-3.0-or-later
// ============================ Books: e-reader (T-Deck Max) ============================
// Included by UITask.cpp (it uses the page chrome, fonts, palette and keyboard plumbing
// defined there). Two halves:
//   * the shelf: books on the SD card (/books), EPUB and plain text, with the
//     reading position of each, and an Audiobookshelf client that lists a server's
//     libraries and downloads an item's EPUB onto the shelf;
//   * the reader: one chapter at a time as plain text (EpubBook.h), cut into pages
//     that fit the screen exactly, turned by tapping the left or right edge, the
//     keyboard (n / b, Enter / Backspace, arrows) or the right front pad.
// Network and download work runs on a short-lived core-0 worker; the UI polls it.
// Audiobooks are listed but not played yet (the audio player is the next step).
#if defined(HAS_TDECK_MAX) && defined(ESP32) && defined(MULTI_TRANSPORT_COMPANION)
#include "EpubBook.h"
#include <ArduinoJson.h>
#define CAP_BOOKS 1

// Books live on the SD card: internal flash (SPIFFS) holds the chats and is far too
// small for them. Only the Audiobookshelf sign-in stays in flash, so it survives a
// card swap. The host build has no card and keeps the books in its SPIFFS folder.
static const char kBkDir[]       = "/books";
static const char kBkIndex[]     = "/books/.shelf.json";
static const char kBkAbsDir[]    = "/bk";
static const char kBkAbsConfig[] = "/bk/abs.json";
static volatile bool s_bk_sd_busy = false;   // UI: a book or the shelf index is open (holds off SD unmount)
static volatile bool s_bk_dl_busy = false;   // worker: a download is being written
static const int  kBkMaxBooks    = 40;
static const int  kBkMaxItems    = 50;     // one page of a server library
static const int  kBkMaxLibs     = 12;
static const size_t kBkTxtChunk  = 24 * 1024;
static const size_t kBkJsonCap   = 768 * 1024;
static size_t s_bk_body_cap = kBkJsonCap;   // reply limit; a download raises it for its one request

struct BkBook {
  char file[96];        // "/books/Title-1a2b3c.epub"
  char title[96];
  char author[64];
  char item_id[48];     // Audiobookshelf library item, "" for a local file
  uint16_t chapter;     // reading position: chapter + byte offset in its text
  uint32_t offset;
  uint16_t chapters;
  uint8_t pct;
  uint32_t seq;         // last-read order (higher = more recent)
};
struct BkAbsItem { char id[48]; char title[80]; char author[48]; char fmt[8]; bool audio; bool local; };
struct BkAbsLib  { char id[48]; char name[40]; bool books; };

static BkBook*    s_bk_books = nullptr;    // PSRAM [kBkMaxBooks]
static int        s_bk_count = 0;
static uint32_t   s_bk_seq = 0;
static int        s_bk_font = 2;           // index into booksFont()

// Audiobookshelf account (abs.json): no password is kept, only the tokens.
static char s_abs_server[96] = "";
static char s_abs_user[48] = "";
static char s_abs_token[600] = "";
static char s_abs_refresh[600] = "";
static char s_abs_lib[48] = "";
static BkAbsLib*  s_abs_libs = nullptr;    // PSRAM
static int        s_abs_lib_count = 0;
static BkAbsItem* s_abs_items = nullptr;   // PSRAM
static int        s_abs_item_count = 0;
static int        s_abs_total = 0;
static int        s_abs_page = 0;

// ---- UI state ----
enum class BkView : uint8_t { Shelf, Server, Reader, Player };
static lv_obj_t*   s_bk_root = nullptr;
static lv_obj_t*   s_bk_body = nullptr;     // the view's content (rebuilt per view)
static lv_timer_t* s_bk_timer = nullptr;
static BkView      s_bk_view = BkView::Shelf;
static lv_obj_t*   s_bk_status = nullptr;   // server view: one line of state
static lv_obj_t*   s_bk_user_ta = nullptr;
static lv_obj_t*   s_bk_pass_ta = nullptr;
static lv_obj_t*   s_bk_server_ta = nullptr;
static int         s_bk_delete_idx = -1;
static char        s_bk_page_title[64] = "Books";
static lv_coord_t  s_bk_body_w = 0, s_bk_body_h = 0;   // set by booksShowView (layout is lazy)

// ---- reader state ----
static EpubBook::Book* s_bk_epub = nullptr;
static File        s_bk_file;
static bool        s_bk_is_txt = false;
static uint32_t    s_bk_file_size = 0;
static int         s_bk_open = -1;          // index into s_bk_books
static int         s_bk_chapter = 0;
static int         s_bk_chapter_count = 0;
static char*       s_bk_text = nullptr;
static size_t      s_bk_text_len = 0;
static uint32_t*   s_bk_pages = nullptr;
static int         s_bk_page_count = 0;
static int         s_bk_page_cap = 0;
static int         s_bk_page = 0;
static char*       s_bk_page_buf = nullptr;
static size_t      s_bk_page_buf_cap = 0;
static lv_obj_t*   s_bk_text_lbl = nullptr;
static lv_obj_t*   s_bk_foot = nullptr;
static lv_obj_t*   s_bk_menu = nullptr;
static lv_coord_t  s_bk_text_w = 0, s_bk_text_h = 0;

// ---- worker ----
enum class BkJob : uint8_t { None, Login, Libraries, Items, Download, Progress, OpenSession, SessionSync,
                              AudioProgress, AudioPull };
static volatile BkJob s_bk_job = BkJob::None;
static volatile bool  s_bk_job_done = false;
static volatile bool  s_bk_job_ok = false;
static volatile uint32_t s_bk_dl_got = 0, s_bk_dl_total = 0;
static char  s_bk_job_msg[96] = "";
static char  s_bk_job_arg[64] = "";        // item id / library id
static char  s_bk_job_arg2[96] = "";       // title for a download
static char  s_bk_job_user[48] = "", s_bk_job_pass[64] = "";
static float s_bk_job_progress = 0.f;
static TaskHandle_t s_bk_task = nullptr;

// ---- audiobooks: the session (BkJob::OpenSession fills it; BooksAudio.inc plays it) ----
struct BkaTrack   { char url[176]; double start; double dur; uint32_t size; bool local; };   // url: server path, or a card path when local
struct BkaChapter { double start; double end; char title[56]; };
static const int kBkaMaxTracks = 400;
static const int kBkaMaxChapters = 400;
static BkaTrack*   s_bka_tracks = nullptr;      // PSRAM [kBkaMaxTracks]
static int         s_bka_track_count = 0;
static BkaChapter* s_bka_chapters = nullptr;    // PSRAM [kBkaMaxChapters]
static int         s_bka_chapter_count = 0;
static char        s_bka_item[48] = "";         // library item of the loaded session
static char        s_bka_session[64] = "";      // Audiobookshelf playback session id
static char        s_bka_title[96] = "";
static char        s_bka_author[64] = "";
static double      s_bka_duration = 0;
static double      s_bka_time = 0;              // where playback is / resumes (UI side)
static uint32_t    s_bka_listened_ms = 0;       // played since the last sync
static uint32_t    s_bka_last_sync_ms = 0;
static double      s_bka_sync_time = 0;         // worker: the position being synced
static uint32_t    s_bka_sync_listened = 0;     // worker: seconds listened in that span
static bool        s_bka_sync_close = false;    // worker: close the session after the sync
static lv_obj_t*   s_bka_lbl_time = nullptr;
static lv_obj_t*   s_bka_lbl_chapter = nullptr;
static lv_obj_t*   s_bka_lbl_state = nullptr;
static lv_obj_t*   s_bka_bar = nullptr;
static lv_obj_t*   s_bka_play_lbl = nullptr;
static lv_timer_t* s_bka_timer = nullptr;       // runs while a session is loaded, page open or not
static volatile bool s_bka_sd_busy = false;     // the engine is reading a track from the card
static lv_obj_t*   s_bka_dl_lbl = nullptr;      // the player's download button label
static lv_obj_t*   s_bka_vol_lbl = nullptr;     // the player's volume readout
static void        booksVolumeStep(int d);

// The playback engine (BooksAudio.inc, after the shared audio sink it uses).
static bool        booksAudioPlayFrom(double t);   // (re)start at t seconds into the book
static void        booksAudioStop();               // stop; the position stays in booksAudioPos()
static bool        booksAudioRunning();            // playing or buffering
static bool        booksAudioBuffering();
static double      booksAudioPos();
static bool        booksAudioEnded();
static const char* booksAudioError();               // "" while fine
struct BkAbsItem;
static void        booksStartAudiobook(const BkAbsItem& it);
static void        booksOpenPlayer();
static void        booksPlayerRefresh();

// The bigger reading faces are Montserrat's ASCII set; accented Latin (ä, ö, ü, é…)
// comes from the matching extras_lat face, then the 16 px chain for anything else.
static const lv_font_t* booksFont(int i) {
  static lv_font_t big[3], lat[3];
  static bool built = false;
  if (!built) {
    built = true;
    const lv_font_t* prim[3] = { &lv_font_montserrat_18, &lv_font_montserrat_20, &lv_font_montserrat_24 };
#if CAP_UI_SIZE
    const lv_font_t* ext[3] = { &extras_lat_18, &extras_lat_20, &extras_lat_24 };
#endif
    for (int k = 0; k < 3; ++k) {
      big[k] = *prim[k];
#if CAP_UI_SIZE
      lat[k] = *ext[k];
      lat[k].fallback = &g_font_16;
      big[k].fallback = &lat[k];
#else
      (void)lat;
      big[k].fallback = &g_font_16;
#endif
    }
  }
  switch (i) {
    case 0:  return &g_font_14;
    case 1:  return &g_font_16;
    case 3:  return &big[1];
    case 4:  return &big[2];
    default: return &big[0];
  }
}
static const int kBkFontCount = 5;
static const int kBkLineSpace = 3;

static void* bkAlloc(size_t n) {
  void* p = heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(n ? n : 1);
}
static void bkFree(void* p) { if (p) free(p); }
static const EpubBook::Hooks kBkHooks = { bkAlloc, bkFree };

struct BkJsonAllocator : ArduinoJson::Allocator {
  void* allocate(size_t n) override { return bkAlloc(n); }
  void deallocate(void* p) override { bkFree(p); }
  void* reallocate(void* p, size_t n) override {
    void* q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : realloc(p, n);
  }
};
static BkJsonAllocator s_bk_json_alloc;

static void bkCopy(char* dst, size_t cap, const char* src) {
  if (!dst || !cap) return;
  snprintf(dst, cap, "%s", src ? src : "");
}

// ======================= storage: the shelf index + account =======================
static fs::FS* booksFs() {
#if defined(WADA_WEB)
  return &SPIFFS;
#elif CAP_SD
  if (sdAdoptLiveMount()) return &SD;
  if (!sdRuntimeLifecycleBusy() && fmSdTryMount()) return &SD;
  return nullptr;
#else
  return nullptr;
#endif
}

static void booksSaveIndex() {
  JsonDocument doc(&s_bk_json_alloc);
  doc["font"] = s_bk_font;
  JsonArray arr = doc["books"].to<JsonArray>();
  for (int i = 0; i < s_bk_count; ++i) {
    const BkBook& b = s_bk_books[i];
    JsonObject o = arr.add<JsonObject>();
    o["f"] = b.file; o["t"] = b.title; o["a"] = b.author; o["id"] = b.item_id;
    o["c"] = b.chapter; o["o"] = b.offset; o["n"] = b.chapters; o["p"] = b.pct; o["s"] = b.seq;
  }
  fs::FS* fs = booksFs();
  if (!fs) return;
  const bool was_busy = s_bk_sd_busy;   // the reader may already hold the card
  s_bk_sd_busy = true;
  fs->mkdir(kBkDir);
  File f = fs->open(kBkIndex, FILE_WRITE);
  if (f) { serializeJson(doc, f); f.close(); }
  s_bk_sd_busy = was_busy;
}

static bool booksHasFile(const char* path) {
  for (int i = 0; i < s_bk_count; ++i) if (strcmp(s_bk_books[i].file, path) == 0) return true;
  return false;
}

static bool booksIsBookName(const char* path) {
  const char* dot = strrchr(path, '.');
  return dot && (strcasecmp(dot, ".epub") == 0 || strcasecmp(dot, ".txt") == 0);
}

// Read the index, drop entries whose file is gone, add book files nobody indexed
// (copied onto the device by hand).
static void booksLoadIndex() {
  if (!s_bk_books) s_bk_books = (BkBook*)psAlloc(sizeof(BkBook) * kBkMaxBooks);
  if (!s_bk_books) return;
  s_bk_count = 0;
  fs::FS* fs = booksFs();
  if (!fs) return;
  s_bk_sd_busy = true;
  File f = fs->open(kBkIndex, FILE_READ);
  if (f) {
    JsonDocument doc(&s_bk_json_alloc);
    if (!deserializeJson(doc, f)) {
      s_bk_font = doc["font"] | 1;
      if (s_bk_font < 0 || s_bk_font >= kBkFontCount) s_bk_font = 1;
      for (JsonObject o : doc["books"].as<JsonArray>()) {
        if (s_bk_count >= kBkMaxBooks) break;
        BkBook& b = s_bk_books[s_bk_count];
        memset(&b, 0, sizeof b);
        bkCopy(b.file, sizeof b.file, o["f"] | "");
        if (!b.file[0] || !fs->exists(b.file)) continue;
        bkCopy(b.title, sizeof b.title, o["t"] | "");
        bkCopy(b.author, sizeof b.author, o["a"] | "");
        bkCopy(b.item_id, sizeof b.item_id, o["id"] | "");
        b.chapter = o["c"] | 0; b.offset = o["o"] | 0; b.chapters = o["n"] | 0;
        b.pct = o["p"] | 0; b.seq = o["s"] | 0;
        if (b.seq > s_bk_seq) s_bk_seq = b.seq;
        ++s_bk_count;
      }
    }
    f.close();
  }
  // Book files nobody indexed yet (copied onto the card by hand).
  File dir = fs->open(kBkDir);
  if (dir && dir.isDirectory()) {
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
      const char* name = e.name();
      const char* base = strrchr(name, '/');
      base = base ? base + 1 : name;
      if (!e.isDirectory() && base[0] != '.' && booksIsBookName(base) && s_bk_count < kBkMaxBooks) {
        char full[96];
        snprintf(full, sizeof full, "%s/%s", kBkDir, base);
        if (!booksHasFile(full)) {
          BkBook& b = s_bk_books[s_bk_count++];
          memset(&b, 0, sizeof b);
          bkCopy(b.file, sizeof b.file, full);
          EpubBook::titleFromFileName(full, b.title, sizeof b.title);
        }
      }
      e.close();
    }
  }
  if (dir) dir.close();
  s_bk_sd_busy = false;
}

static void booksSaveAbs() {
  JsonDocument doc(&s_bk_json_alloc);
  doc["server"] = s_abs_server; doc["user"] = s_abs_user;
  doc["token"] = s_abs_token; doc["refresh"] = s_abs_refresh; doc["lib"] = s_abs_lib;
  SPIFFS.mkdir(kBkAbsDir);
  File f = SPIFFS.open(kBkAbsConfig, FILE_WRITE);
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

static void booksLoadAbs() {
  File f = SPIFFS.open(kBkAbsConfig, FILE_READ);
  if (!f) return;
  JsonDocument doc(&s_bk_json_alloc);
  if (!deserializeJson(doc, f)) {
    bkCopy(s_abs_server, sizeof s_abs_server, doc["server"] | "");
    bkCopy(s_abs_user, sizeof s_abs_user, doc["user"] | "");
    bkCopy(s_abs_token, sizeof s_abs_token, doc["token"] | "");
    bkCopy(s_abs_refresh, sizeof s_abs_refresh, doc["refresh"] | "");
    bkCopy(s_abs_lib, sizeof s_abs_lib, doc["lib"] | "");
  }
  f.close();
}

// ============================== network (worker task) ==============================
// One request. body/len: the reply in PSRAM (release with bkFree) unless sink is set,
// when it streams into that file. Returns the HTTP status or a negative error.
static int bkHttp(const char* method, const char* url, const char* payload, bool auth,
                  const char* extra_header, uint8_t** body, size_t* len, File* sink) {
  if (body) *body = nullptr;
  if (len) *len = 0;
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(9000);
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("wadamesh-books");
  http.useHTTP10(true);   // no chunked replies: the body is read straight off the stream
  WiFiClientSecure tls; WiFiClient plain;
  const bool https = strncasecmp(url, "https://", 8) == 0;
  bool began;
  if (https) { tls.setInsecure(); began = http.begin(tls, url); }
  else       { began = http.begin(plain, url); }
  if (!began) return HTTPC_ERROR_CONNECTION_REFUSED;
  http.addHeader("Accept-Encoding", "identity");
  if (auth && s_abs_token[0]) http.addHeader("Authorization", String("Bearer ") + s_abs_token);
  if (extra_header) {
    const char* colon = strchr(extra_header, ':');
    if (colon) http.addHeader(String(extra_header).substring(0, colon - extra_header), String(colon + 1).c_str() + (colon[1] == ' '));
  }
  if (payload) http.addHeader("Content-Type", "application/json");
  const int code = http.sendRequest(method, (uint8_t*)(payload ? payload : ""), payload ? strlen(payload) : 0);
  if (code != 200) { http.end(); return code; }
  const int size = http.getSize();
  s_bk_dl_total = size > 0 ? (uint32_t)size : 0;
  s_bk_dl_got = 0;
  WiFiClient* stream = http.getStreamPtr();
  if (!stream) { http.end(); return HTTPC_ERROR_NO_STREAM; }
  uint8_t* buf = nullptr;
  size_t cap = 0, got = 0;
  uint8_t chunk[1024];
  if (!sink) {
    if (size > 0 && (size_t)size + 1 > s_bk_body_cap) { http.end(); return HTTPC_ERROR_TOO_LESS_RAM; }
    cap = size > 0 ? (size_t)size + 1 : 64 * 1024;
    if (cap > s_bk_body_cap) cap = s_bk_body_cap;
    buf = (uint8_t*)bkAlloc(cap);
    if (!buf) { http.end(); return HTTPC_ERROR_TOO_LESS_RAM; }
  }
  uint32_t idle_since = millis();
  while (size < 0 || got < (size_t)size) {
    const int avail = stream->available();
    if (avail <= 0) {
      if (!stream->connected() && stream->available() <= 0) break;
      if ((uint32_t)(millis() - idle_since) > 20000) break;
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    idle_since = millis();
    const int n = stream->read(chunk, avail < (int)sizeof chunk ? avail : (int)sizeof chunk);
    if (n <= 0) continue;
    if (sink) {
      if (sink->write(chunk, (size_t)n) != (size_t)n) { http.end(); return HTTPC_ERROR_STREAM_WRITE; }
    } else {
      if (got + (size_t)n + 1 > cap) {
        if (cap >= s_bk_body_cap) break;
        size_t ncap = cap * 2 > s_bk_body_cap ? s_bk_body_cap : cap * 2;
        uint8_t* nb = (uint8_t*)heap_caps_realloc(buf, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nb) break;
        buf = nb; cap = ncap;
      }
      memcpy(buf + got, chunk, (size_t)n);
    }
    got += (size_t)n;
    s_bk_dl_got = (uint32_t)got;
  }
  http.end();
  if (size > 0 && got < (size_t)size) { bkFree(buf); return HTTPC_ERROR_CONNECTION_LOST; }
  if (buf) buf[got] = 0;
  if (body) *body = buf; else bkFree(buf);
  if (len) *len = got;
  return 200;
}

static void bkUrl(char* out, size_t cap, const char* path) {
  size_t n = strlen(s_abs_server);
  while (n && s_abs_server[n - 1] == '/') --n;
  snprintf(out, cap, "%.*s%s", (int)n, s_abs_server, path);
}

// Tokens from a /login or /auth/refresh reply. Servers from 2.26 hand out a short-lived
// accessToken plus a refreshToken; older ones a single long-lived token.
static bool bkTakeTokens(const uint8_t* body, size_t len) {
  JsonDocument doc(&s_bk_json_alloc);
  if (deserializeJson(doc, (const char*)body, len)) return false;
  JsonObject user = doc["user"];
  const char* access = user["accessToken"] | (const char*)nullptr;
  if (!access || !access[0]) access = user["token"] | "";
  if (!access[0]) return false;
  bkCopy(s_abs_token, sizeof s_abs_token, access);
  const char* refresh = user["refreshToken"] | (const char*)nullptr;
  if (refresh && refresh[0]) bkCopy(s_abs_refresh, sizeof s_abs_refresh, refresh);
  const char* def_lib = doc["userDefaultLibraryId"] | "";
  if (def_lib[0] && !s_abs_lib[0]) bkCopy(s_abs_lib, sizeof s_abs_lib, def_lib);
  return true;
}

static bool bkRefresh() {
  if (!s_abs_refresh[0]) return false;
  char url[160], hdr[640];
  bkUrl(url, sizeof url, "/auth/refresh");
  snprintf(hdr, sizeof hdr, "x-refresh-token: %s", s_abs_refresh);
  uint8_t* body = nullptr; size_t len = 0;
  const int code = bkHttp("POST", url, nullptr, false, hdr, &body, &len, nullptr);
  const bool ok = code == 200 && body && bkTakeTokens(body, len);
  bkFree(body);
  if (ok) booksSaveAbs();
  return ok;
}

// An authenticated request that renews an expired access token once.
static int bkAuthed(const char* method, const char* path, const char* payload,
                    uint8_t** body, size_t* len, File* sink) {
  char url[200];
  bkUrl(url, sizeof url, path);
  int code = bkHttp(method, url, payload, true, nullptr, body, len, sink);
  if (code == 401 && bkRefresh()) {
    if (sink) sink->seek(0);
    code = bkHttp(method, url, payload, true, nullptr, body, len, sink);
  }
  return code;
}

static void bkJobMsgForCode(int code) {
  if (code == 401) snprintf(s_bk_job_msg, sizeof s_bk_job_msg, "Signed out by the server: log in again");
  else if (code == HTTPC_ERROR_TOO_LESS_RAM)
    snprintf(s_bk_job_msg, sizeof s_bk_job_msg, "The server's reply is too large (%d)", code);
  else if (code == HTTPC_ERROR_CONNECTION_LOST || code == HTTPC_ERROR_READ_TIMEOUT)
    snprintf(s_bk_job_msg, sizeof s_bk_job_msg, "The reply broke off (%d)", code);
  else if (code < 0) snprintf(s_bk_job_msg, sizeof s_bk_job_msg, "Server not reachable (%d)", code);
  else snprintf(s_bk_job_msg, sizeof s_bk_job_msg, "Server answered %d", code);
}

static void bkJobLogin() {
  char url[160];
  bkUrl(url, sizeof url, "/login");
  JsonDocument req(&s_bk_json_alloc);
  req["username"] = s_bk_job_user; req["password"] = s_bk_job_pass;
  char payload[200];
  serializeJson(req, payload, sizeof payload);
  memset(s_bk_job_pass, 0, sizeof s_bk_job_pass);   // never kept
  uint8_t* body = nullptr; size_t len = 0;
  s_abs_lib[0] = 0;
  const int code = bkHttp("POST", url, payload, false, "x-return-tokens: true", &body, &len, nullptr);
  memset(payload, 0, sizeof payload);
  if (code == 200 && body && bkTakeTokens(body, len)) {
    bkCopy(s_abs_user, sizeof s_abs_user, s_bk_job_user);
    booksSaveAbs();
    s_bk_job_ok = true;
  } else if (code == 401) {
    snprintf(s_bk_job_msg, sizeof s_bk_job_msg, "Wrong user name or password");
  } else {
    bkJobMsgForCode(code);
  }
  bkFree(body);
}

static void bkJobLibraries() {
  uint8_t* body = nullptr; size_t len = 0;
  const int code = bkAuthed("GET", "/api/libraries", nullptr, &body, &len, nullptr);
  if (code != 200 || !body) { bkJobMsgForCode(code); bkFree(body); return; }
  JsonDocument doc(&s_bk_json_alloc);
  if (deserializeJson(doc, (const char*)body, len)) {
    snprintf(s_bk_job_msg, sizeof s_bk_job_msg, "Unexpected reply from the server");
  } else {
    if (!s_abs_libs) s_abs_libs = (BkAbsLib*)psAlloc(sizeof(BkAbsLib) * kBkMaxLibs);
    s_abs_lib_count = 0;
    for (JsonObject l : doc["libraries"].as<JsonArray>()) {
      if (!s_abs_libs || s_abs_lib_count >= kBkMaxLibs) break;
      BkAbsLib& o = s_abs_libs[s_abs_lib_count++];
      bkCopy(o.id, sizeof o.id, l["id"] | "");
      bkCopy(o.name, sizeof o.name, l["name"] | "");
      o.books = strcmp(l["mediaType"] | "book", "book") == 0;
    }
    bool have = false;
    for (int i = 0; i < s_abs_lib_count; ++i) if (strcmp(s_abs_libs[i].id, s_abs_lib) == 0) have = true;
    if (!have) {
      s_abs_lib[0] = 0;
      for (int i = 0; i < s_abs_lib_count; ++i)
        if (s_abs_libs[i].books) { bkCopy(s_abs_lib, sizeof s_abs_lib, s_abs_libs[i].id); break; }
    }
    s_bk_job_ok = true;
  }
  bkFree(body);
}

static void bkJobItems() {
  char path[200];
  snprintf(path, sizeof path, "/api/libraries/%s/items?limit=%d&page=%d&minified=1&sort=media.metadata.title",
           s_abs_lib, kBkMaxItems, s_abs_page);
  uint8_t* body = nullptr; size_t len = 0;
  const int code = bkAuthed("GET", path, nullptr, &body, &len, nullptr);
  if (code != 200 || !body) { bkJobMsgForCode(code); bkFree(body); return; }
  JsonDocument filter(&s_bk_json_alloc);
  filter["total"] = true;
  JsonObject r = filter["results"][0].to<JsonObject>();
  r["id"] = true;
  r["media"]["metadata"]["title"] = true;
  r["media"]["metadata"]["authorName"] = true;
  r["media"]["ebookFormat"] = true;
  r["media"]["ebookFileFormat"] = true;
  r["media"]["ebookFile"]["ebookFormat"] = true;
  r["media"]["numAudioFiles"] = true;
  r["media"]["numTracks"] = true;
  r["media"]["duration"] = true;
  JsonDocument doc(&s_bk_json_alloc);
  if (deserializeJson(doc, (const char*)body, len, DeserializationOption::Filter(filter))) {
    snprintf(s_bk_job_msg, sizeof s_bk_job_msg, "Unexpected reply from the server");
  } else {
    if (!s_abs_items) s_abs_items = (BkAbsItem*)psAlloc(sizeof(BkAbsItem) * kBkMaxItems);
    s_abs_item_count = 0;
    s_abs_total = doc["total"] | 0;
    for (JsonObject it : doc["results"].as<JsonArray>()) {
      if (!s_abs_items || s_abs_item_count >= kBkMaxItems) break;
      BkAbsItem& o = s_abs_items[s_abs_item_count++];
      memset(&o, 0, sizeof o);
      bkCopy(o.id, sizeof o.id, it["id"] | "");
      JsonObject m = it["media"];
      bkCopy(o.title, sizeof o.title, m["metadata"]["title"] | "?");
      bkCopy(o.author, sizeof o.author, m["metadata"]["authorName"] | "");
      const char* fmt = m["ebookFormat"] | (const char*)nullptr;
      if (!fmt) fmt = m["ebookFileFormat"] | (const char*)nullptr;
      if (!fmt) fmt = m["ebookFile"]["ebookFormat"] | "";
      bkCopy(o.fmt, sizeof o.fmt, fmt);
      o.audio = (m["numAudioFiles"] | 0) > 0 || (m["numTracks"] | 0) > 0 || (m["duration"] | 0.0) > 0.0;
    }
    s_bk_job_ok = true;
  }
  bkFree(body);
}

// The book comes down into PSRAM and the UI task writes it to the card (booksSaveStep):
// the card shares its SPI bus with the e-paper panel, whose refresh in the UI task
// drives the card's chip select, so a write from this core-0 worker failed mid-file.
static uint8_t* s_bk_dl_buf = nullptr;
static size_t   s_bk_dl_len = 0;

static void bkJobDownload() {
  char path[120];
  snprintf(path, sizeof path, "/api/items/%s/ebook", s_bk_job_arg);
  size_t room = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#if defined(WADA_WEB)
  room = 64u * 1024u * 1024u;
#endif
  room = room > 512u * 1024u ? room - 512u * 1024u : 0;   // leave PSRAM for the rest of the UI
  if (room > 24u * 1024u * 1024u) room = 24u * 1024u * 1024u;
  s_bk_body_cap = room;
  uint8_t* body = nullptr; size_t len = 0;
  const int code = bkAuthed("GET", path, nullptr, &body, &len, nullptr);
  s_bk_body_cap = kBkJsonCap;
  if (code != 200 || !body || len == 0) {
    bkFree(body);
    if (code == HTTPC_ERROR_TOO_LESS_RAM)
      snprintf(s_bk_job_msg, sizeof s_bk_job_msg, "Book too large for memory (%u KB free)", (unsigned)(room / 1024));
    else bkJobMsgForCode(code);
    return;
  }
  s_bk_dl_buf = body;
  s_bk_dl_len = len;
  s_bk_job_ok = true;
}

static void bkJobProgress() {
  char path[120], payload[96];
  snprintf(path, sizeof path, "/api/me/progress/%s", s_bk_job_arg);
  snprintf(payload, sizeof payload, "{\"ebookProgress\":%.4f,\"isFinished\":%s}",
           (double)s_bk_job_progress, s_bk_job_progress >= 0.999f ? "true" : "false");
  const int code = bkAuthed("PATCH", path, payload, nullptr, nullptr, nullptr);
  s_bk_job_ok = code == 200;
}

// A JSON reply read straight off the connection, a buffer at a time, so a reply of any
// size goes through ArduinoJson's filter without being held in memory.
class BkBufStream : public Stream {
 public:
  explicit BkBufStream(WiFiClient* c) : c_(c) {}
  int available() override { return (n_ > i_) ? (int)(n_ - i_) : (fill() ? (int)(n_ - i_) : 0); }
  int read() override { return (i_ < n_ || fill()) ? buf_[i_++] : -1; }
  int peek() override { return (i_ < n_ || fill()) ? buf_[i_] : -1; }
  size_t readBytes(char* out, size_t len) override {
    size_t got = 0;
    while (got < len && (i_ < n_ || fill())) {
      size_t k = n_ - i_;
      if (k > len - got) k = len - got;
      memcpy(out + got, buf_ + i_, k);
      i_ += k; got += k;
    }
    return got;
  }
  size_t write(uint8_t) override { return 0; }
  void flush() override {}
  size_t total() const { return total_; }
 private:
  bool fill() {
    if (!c_) return false;
    const uint32_t t0 = millis();
    for (;;) {
      const int a = c_->available();
      if (a > 0) {
        const int r = c_->read(buf_, a < (int)sizeof buf_ ? (size_t)a : sizeof buf_);
        if (r > 0) { n_ = (size_t)r; i_ = 0; total_ += (size_t)r; return true; }
      }
      if (!c_->connected() && c_->available() <= 0) return false;
      if (millis() - t0 > 20000) return false;
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
  WiFiClient* c_;
  uint8_t buf_[1024];
  size_t n_ = 0, i_ = 0, total_ = 0;
};

// A request to the server whose JSON reply is parsed through a filter as it arrives. Returns the HTTP status (or
// a negative error); a parse error comes back as HTTPC_ERROR_CONNECTION_LOST.
static int bkReqJson(const char* method, const char* path, const char* payload,
                     JsonDocument& filter, JsonDocument& doc) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    char url[200];
    bkUrl(url, sizeof url, path);
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(9000);
    http.setTimeout(20000);
    http.useHTTP10(true);
    http.setUserAgent("wadamesh-books");
    WiFiClientSecure tls; WiFiClient plain;
    const bool https = strncasecmp(url, "https://", 8) == 0;
    if (https) { tls.setInsecure(); if (!http.begin(tls, url)) return HTTPC_ERROR_CONNECTION_REFUSED; }
    else if (!http.begin(plain, url)) return HTTPC_ERROR_CONNECTION_REFUSED;
    http.addHeader("Accept-Encoding", "identity");
    if (s_abs_token[0]) http.addHeader("Authorization", String("Bearer ") + s_abs_token);
    if (payload) http.addHeader("Content-Type", "application/json");
    const int code = http.sendRequest(method, (uint8_t*)(payload ? payload : ""), payload ? strlen(payload) : 0);
    if (code == 401 && attempt == 0) { http.end(); if (bkRefresh()) continue; return 401; }
    if (code != 200) { http.end(); return code; }
    BkBufStream in(http.getStreamPtr());
    const DeserializationError err = deserializeJson(doc, in, DeserializationOption::Filter(filter));
    Serial.printf("[books] %s: %u bytes read, json %s\n", path, (unsigned)in.total(), err.c_str());
    http.end();
    return err ? HTTPC_ERROR_CONNECTION_LOST : 200;
  }
  return 401;
}

// An audiobook: a playback session from the server (direct play: the files as they are,
// so the device decodes them itself, MP3 for now). The reply carries the whole library
// item and runs to megabytes for a long book, so it is read as it arrives through a
// filter that keeps only what the player needs; a reply that breaks off is asked again.
static void bkJobOpenSession() {
  char path[120];
  snprintf(path, sizeof path, "/api/items/%s/play", s_bk_job_arg);
  static const char kBody[] =
      "{\"deviceInfo\":{\"clientName\":\"wadamesh\",\"deviceId\":\"wadamesh-max\"},"
      "\"forceDirectPlay\":true,\"forceTranscode\":false,\"mediaPlayer\":\"wadamesh\","
      "\"supportedMimeTypes\":[\"audio/mpeg\"]}";
  JsonDocument filter(&s_bk_json_alloc);
  filter["id"] = true;
  filter["displayTitle"] = true;
  filter["displayAuthor"] = true;
  filter["duration"] = true;
  filter["currentTime"] = true;
  JsonObject fc = filter["chapters"][0].to<JsonObject>();
  fc["start"] = true; fc["end"] = true; fc["title"] = true;
  JsonObject ft = filter["audioTracks"][0].to<JsonObject>();
  ft["startOffset"] = true; ft["duration"] = true; ft["contentUrl"] = true;
  ft["mimeType"] = true; ft["codec"] = true;
  JsonDocument doc(&s_bk_json_alloc);
  int code = 0;
  for (int attempt = 0; attempt < 3; ++attempt) {
    doc.clear();
    code = bkReqJson("POST", path, kBody, filter, doc);
    if (code != HTTPC_ERROR_CONNECTION_LOST && code != HTTPC_ERROR_READ_TIMEOUT) break;
    vTaskDelay(pdMS_TO_TICKS(800));
  }
  if (code != 200) { bkJobMsgForCode(code); return; }
  if (!s_bka_tracks) s_bka_tracks = (BkaTrack*)psAlloc(sizeof(BkaTrack) * kBkaMaxTracks);
  if (!s_bka_chapters) s_bka_chapters = (BkaChapter*)psAlloc(sizeof(BkaChapter) * kBkaMaxChapters);
  if (!s_bka_tracks || !s_bka_chapters) { snprintf(s_bk_job_msg, sizeof s_bk_job_msg, "Not enough memory"); return; }
  int tracks = 0;
  char codec[16] = "";
  for (JsonObject t : doc["audioTracks"].as<JsonArray>()) {
    if (tracks >= kBkaMaxTracks) break;
    const char* mime = t["mimeType"] | "";
    const char* cdc = t["codec"] | "";
    const bool mp3 = strcasecmp(mime, "audio/mpeg") == 0 || strcasecmp(mime, "audio/mp3") == 0 ||
                     strcasecmp(cdc, "mp3") == 0;
    if (!mp3) { bkCopy(codec, sizeof codec, cdc[0] ? cdc : mime); break; }
    BkaTrack& o = s_bka_tracks[tracks++];
    bkCopy(o.url, sizeof o.url, t["contentUrl"] | "");
    o.start = t["startOffset"] | 0.0;
    o.dur = t["duration"] | 0.0;
    o.size = 0;
    o.local = false;
  }
  if (codec[0] || tracks == 0) {
    snprintf(s_bk_job_msg, sizeof s_bk_job_msg, "%s audio is not supported yet: MP3 only", codec[0] ? codec : "This");
    if (codec[0]) for (char* c = s_bk_job_msg; *c && *c != ' '; ++c) if (*c >= 'a' && *c <= 'z') *c -= 32;
    return;
  }
  s_bka_track_count = tracks;
  s_bka_chapter_count = 0;
  for (JsonObject c : doc["chapters"].as<JsonArray>()) {
    if (s_bka_chapter_count >= kBkaMaxChapters) break;
    BkaChapter& o = s_bka_chapters[s_bka_chapter_count++];
    o.start = c["start"] | 0.0;
    o.end = c["end"] | 0.0;
    bkCopy(o.title, sizeof o.title, c["title"] | "");
  }
  bkCopy(s_bka_session, sizeof s_bka_session, doc["id"] | "");
  bkCopy(s_bka_item, sizeof s_bka_item, s_bk_job_arg);
  bkCopy(s_bka_title, sizeof s_bka_title, doc["displayTitle"] | s_bk_job_arg2);
  bkCopy(s_bka_author, sizeof s_bka_author, doc["displayAuthor"] | "");
  s_bka_duration = doc["duration"] | 0.0;
  if (s_bka_duration <= 0) s_bka_duration = s_bka_tracks[tracks - 1].start + s_bka_tracks[tracks - 1].dur;
  s_bka_time = doc["currentTime"] | 0.0;
  if (s_bka_time >= s_bka_duration - 1) s_bka_time = 0;   // finished before: from the start
  s_bk_job_ok = true;
}

// The listening position back to the server (and, when leaving, the session closed).
static void bkJobSessionSync() {
  if (!s_bka_session[0]) return;
  char path[120], payload[128];
  snprintf(path, sizeof path, "/api/session/%s/%s", s_bka_session, s_bka_sync_close ? "close" : "sync");
  snprintf(payload, sizeof payload, "{\"currentTime\":%.2f,\"timeListened\":%u,\"duration\":%.2f}",
           s_bka_sync_time, (unsigned)s_bka_sync_listened, s_bka_duration);
  const int code = bkAuthed("POST", path, payload, nullptr, nullptr, nullptr);
  s_bk_job_ok = code == 200;
}

// A downloaded audiobook has no server session: its position goes to the user's media
// progress directly (push), and comes from there when the book is opened (pull).
static double s_bka_job_time = 0;
static double s_bka_pulled = -1;
static void bkJobAudioProgress() {
  char path[120], payload[160];
  snprintf(path, sizeof path, "/api/me/progress/%s", s_bk_job_arg);
  const double d = s_bka_duration > 0 ? s_bka_duration : 1;
  snprintf(payload, sizeof payload, "{\"currentTime\":%.2f,\"duration\":%.2f,\"progress\":%.4f,\"isFinished\":%s}",
           s_bka_job_time, s_bka_duration, s_bka_job_time / d, s_bka_job_time >= d - 1 ? "true" : "false");
  s_bk_job_ok = bkAuthed("PATCH", path, payload, nullptr, nullptr, nullptr) == 200;
}
static void bkJobAudioPull() {
  char path[120];
  snprintf(path, sizeof path, "/api/me/progress/%s", s_bk_job_arg);
  uint8_t* body = nullptr; size_t len = 0;
  s_bka_pulled = -1;
  const int code = bkAuthed("GET", path, nullptr, &body, &len, nullptr);
  if (code == 200 && body) {
    JsonDocument doc(&s_bk_json_alloc);
    if (!deserializeJson(doc, (const char*)body, len)) { s_bka_pulled = doc["currentTime"] | -1.0; s_bk_job_ok = true; }
  }
  bkFree(body);
}

static void booksTaskFn(void*) {
  switch (s_bk_job) {
    case BkJob::Login:     bkJobLogin();     break;
    case BkJob::Libraries: bkJobLibraries(); break;
    case BkJob::Items:     bkJobItems();     break;
    case BkJob::Download:  bkJobDownload();  break;
    case BkJob::Progress:  bkJobProgress();  break;
    case BkJob::OpenSession: bkJobOpenSession(); break;
    case BkJob::SessionSync: bkJobSessionSync(); break;
    case BkJob::AudioProgress: bkJobAudioProgress(); break;
    case BkJob::AudioPull: bkJobAudioPull(); break;
    default: break;
  }
  s_bk_job_done = true;
  s_bk_task = nullptr;
  vTaskDelete(nullptr);
}

static bool s_bk_save_active_fwd();
static bool booksStartJob(BkJob job) {
  if (s_bk_job != BkJob::None || s_bk_save_active_fwd()) return false;
  if (WiFi.status() != WL_CONNECTED) {
    if (g_lv.task) g_lv.task->showAlert(TR("Connect to Wi-Fi first"), 1600);
    return false;
  }
  s_bk_job = job; s_bk_job_done = false; s_bk_job_ok = false; s_bk_job_msg[0] = 0;
  s_bk_dl_got = s_bk_dl_total = 0;
  // TLS needs a deep stack; it is internal RAM, so the task lives only for one job.
  if (xTaskCreatePinnedToCore(booksTaskFn, "books", 12288, nullptr, 4, &s_bk_task, 0) != pdPASS) {
    s_bk_job = BkJob::None; s_bk_task = nullptr;
    if (g_lv.task) g_lv.task->showAlert(TR("Not enough memory to connect"), 1800);
    return false;
  }
  return true;
}

static void booksShowView(BkView v);
// ---- audiobooks on the card: /books/audio/<id hash>/ holds the MP3 tracks, book.json
// (what the player needs: tracks, chapters, durations) and pos.json (where it was, and
// whether the server has heard about it yet). ----
struct BkAudioBook { char dir[40]; char item[48]; char title[80]; char author[48]; };
static const int kBkMaxAudio = 30;
static BkAudioBook* s_bk_aud = nullptr;   // PSRAM [kBkMaxAudio]
static int s_bk_aud_count = 0;
static bool s_bka_local = false;          // the loaded book plays from the card
static char s_bka_dir[40] = "";           // its folder there
static bool s_bka_dirty = false;          // listened offline, not reported yet

static void bkAudioDirFor(const char* item, char* out, size_t cap) {
  uint32_t h = 2166136261u;
  for (const char* p = item; *p; ++p) { h ^= (uint8_t)*p; h *= 16777619u; }
  snprintf(out, cap, "/books/audio/%08lx", (unsigned long)h);
}

// Every downloaded audiobook on the card (read when the shelf is built).
static void booksScanAudio() {
  s_bk_aud_count = 0;
  fs::FS* fs = booksFs();
  if (!fs) return;
  if (!s_bk_aud) s_bk_aud = (BkAudioBook*)psAlloc(sizeof(BkAudioBook) * kBkMaxAudio);
  if (!s_bk_aud) return;
  const bool was_busy = s_bk_sd_busy;
  s_bk_sd_busy = true;
  File root = fs->open("/books/audio");
  // FAT lists the book folders; a flat store (SPIFFS on the host) lists their files.
  auto adopt = [&](const char* name) {   // name: the folder under /books/audio
    if (s_bk_aud_count >= kBkMaxAudio || !name[0]) return;
    char dir[40];
    snprintf(dir, sizeof dir, "/books/audio/%s", name);
    for (int k = 0; k < s_bk_aud_count; ++k) if (strcmp(s_bk_aud[k].dir, dir) == 0) return;
    char path[80];
    snprintf(path, sizeof path, "%s/book.json", dir);
    File m = fs->open(path, FILE_READ);
    if (!m) return;
    JsonDocument filter(&s_bk_json_alloc);
    filter["id"] = true; filter["title"] = true; filter["author"] = true;
    JsonDocument doc(&s_bk_json_alloc);
    if (!deserializeJson(doc, m, DeserializationOption::Filter(filter))) {
      BkAudioBook& b = s_bk_aud[s_bk_aud_count++];
      bkCopy(b.dir, sizeof b.dir, dir);
      bkCopy(b.item, sizeof b.item, doc["id"] | "");
      bkCopy(b.title, sizeof b.title, doc["title"] | name);
      bkCopy(b.author, sizeof b.author, doc["author"] | "");
    }
    m.close();
  };
  if (root && root.isDirectory()) {
    for (File d = root.openNextFile(); d && s_bk_aud_count < kBkMaxAudio; d = root.openNextFile()) {
      const char* full = d.path();
      if (d.isDirectory()) {
        const char* name = strrchr(d.name(), '/');
        adopt(name ? name + 1 : d.name());
      } else if (full && strstr(full, "/books/audio/") && strstr(full, "/book.json")) {
        char name[24];
        const char* a = strstr(full, "/books/audio/") + 13;
        const char* b = strchr(a, '/');
        const size_t n = b && (size_t)(b - a) < sizeof name ? (size_t)(b - a) : 0;
        memcpy(name, a, n); name[n] = 0;
        adopt(name);
      }
      d.close();
    }
  }
  if (root) root.close();
  s_bk_sd_busy = was_busy;
}

static int booksFindAudio(const char* item) {
  for (int i = 0; i < s_bk_aud_count; ++i) if (strcmp(s_bk_aud[i].item, item) == 0) return i;
  return -1;
}

static void booksSaveAudioPos() {
  if (!s_bka_local || !s_bka_dir[0]) return;
  fs::FS* fs = booksFs();
  if (!fs) return;
  char path[64];
  snprintf(path, sizeof path, "%s/pos.json", s_bka_dir);
  const bool was_busy = s_bk_sd_busy;
  s_bk_sd_busy = true;
  File f = fs->open(path, FILE_WRITE);
  if (f) { f.printf("{\"t\":%.2f,\"dirty\":%s}", s_bka_time, s_bka_dirty ? "true" : "false"); f.close(); }
  s_bk_sd_busy = was_busy;
}

// Load a downloaded audiobook as the session (no server session: progress goes to
// /api/me/progress when Wi-Fi is there).
static bool booksLoadLocalAudio(int idx) {
  if (idx < 0 || idx >= s_bk_aud_count) return false;
  fs::FS* fs = booksFs();
  if (!fs) return false;
  if (!s_bka_tracks) s_bka_tracks = (BkaTrack*)psAlloc(sizeof(BkaTrack) * kBkaMaxTracks);
  if (!s_bka_chapters) s_bka_chapters = (BkaChapter*)psAlloc(sizeof(BkaChapter) * kBkaMaxChapters);
  if (!s_bka_tracks || !s_bka_chapters) return false;
  const BkAudioBook& b = s_bk_aud[idx];
  char path[80];
  snprintf(path, sizeof path, "%s/book.json", b.dir);
  const bool was_busy = s_bk_sd_busy;
  s_bk_sd_busy = true;
  File m = fs->open(path, FILE_READ);
  JsonDocument doc(&s_bk_json_alloc);
  const bool ok = m && !deserializeJson(doc, m);
  if (m) m.close();
  double pos = 0;
  bool dirty = false;
  snprintf(path, sizeof path, "%s/pos.json", b.dir);
  File p = fs->open(path, FILE_READ);
  if (p) {
    JsonDocument pd(&s_bk_json_alloc);
    if (!deserializeJson(pd, p)) { pos = pd["t"] | 0.0; dirty = pd["dirty"] | false; }
    p.close();
  }
  s_bk_sd_busy = was_busy;
  if (!ok) return false;
  int n = 0;
  for (JsonObject t : doc["tracks"].as<JsonArray>()) {
    if (n >= kBkaMaxTracks) break;
    BkaTrack& o = s_bka_tracks[n++];
    snprintf(o.url, sizeof o.url, "%s/%s", b.dir, t["f"] | "");
    o.start = t["start"] | 0.0;
    o.dur = t["dur"] | 0.0;
    o.size = 0;
    o.local = true;
  }
  if (!n) return false;
  s_bka_track_count = n;
  s_bka_chapter_count = 0;
  for (JsonObject c : doc["chapters"].as<JsonArray>()) {
    if (s_bka_chapter_count >= kBkaMaxChapters) break;
    BkaChapter& o = s_bka_chapters[s_bka_chapter_count++];
    o.start = c["s"] | 0.0; o.end = c["e"] | 0.0;
    bkCopy(o.title, sizeof o.title, c["t"] | "");
  }
  bkCopy(s_bka_item, sizeof s_bka_item, b.item);
  bkCopy(s_bka_title, sizeof s_bka_title, b.title);
  bkCopy(s_bka_author, sizeof s_bka_author, b.author);
  bkCopy(s_bka_dir, sizeof s_bka_dir, b.dir);
  s_bka_session[0] = 0;
  s_bka_local = true;
  s_bka_duration = doc["duration"] | 0.0;
  s_bka_time = pos;
  s_bka_dirty = dirty;
  return true;
}

// ---- downloading the loaded (streamed) audiobook to the card: its own task, so the
// shelf's worker stays free for syncs ----
static TaskHandle_t  s_bkd_task = nullptr;
static volatile bool s_bkd_active = false;
static volatile bool s_bkd_cancel = false;
static volatile int  s_bkd_track = 0;        // the track being written
static volatile int  s_bkd_tracks = 0;
static volatile bool s_bkd_done = false;     // finished (s_bkd_msg says how)
static volatile bool s_bkd_ok = false;
static char s_bkd_msg[64] = "";
static char s_bkd_dir[40] = "";
static char s_bkd_item[48] = "";

// The card is written by the UI task only, like an EPUB download: written from this
// task, the card's writes failed part-way (read as "card full"). So the task fetches
// into a ring in PSRAM and asks the UI task, through s_bkd_fstate, to open, fill,
// close and rename the files; booksDownloadPump() does that on a 50 ms timer.
// BkdRetry: a write to the card failed (on the Max a busy card can fail the driver's
// status check and with it the open file); the file is closed and the worker goes on
// from what the card holds, the way it resumes a dropped connection.
enum BkdFile : uint8_t { BkdIdle, BkdOpenReq, BkdOpen, BkdSkip, BkdCloseReq, BkdClosed, BkdFailed, BkdRetry };
static const size_t kBkdRing = 256 * 1024;
static uint8_t* s_bkd_ring = nullptr;
static volatile size_t s_bkd_head = 0, s_bkd_tail = 0;
static volatile uint8_t s_bkd_fstate = BkdIdle;
static char s_bkd_fpath[72] = "";     // the file to open (a .part, or book.json)
static char s_bkd_final[72] = "";     // on close: rename to this ("" = keep the name)
static bool s_bkd_fappend = false;    // open to continue (a .part) instead of anew
static volatile uint32_t s_bkd_fsize = 0;   // the size the open file had
static uint32_t s_bkd_fwritten = 0;          // written to it since (UI task)
static uint8_t s_bkd_write_fails = 0;        // write failures in a row (UI task)
static File s_bkd_file;                // UI task only
static lv_timer_t* s_bkd_timer = nullptr;

static bool bkdWait(uint8_t a, uint8_t b, uint32_t ms) {   // until the UI side moved on
  const uint32_t t0 = millis();
  while (s_bkd_fstate != a && s_bkd_fstate != b) {
    if (s_bkd_fstate == BkdFailed || s_bkd_fstate == BkdRetry) return false;   // the UI side gave up on the file
    if (s_bkd_cancel || millis() - t0 > ms) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return true;
}

static bool bkdPut(const uint8_t* d, size_t n) {   // into the ring, waiting while it is full
  while (n) {
    while (s_bkd_head - s_bkd_tail >= kBkdRing) {
      if (s_bkd_cancel || s_bkd_fstate == BkdFailed || s_bkd_fstate == BkdRetry) return false;
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    size_t room = kBkdRing - (s_bkd_head - s_bkd_tail);
    if (room > n) room = n;
    const size_t at = s_bkd_head % kBkdRing;
    const size_t first = room < kBkdRing - at ? room : kBkdRing - at;
    memcpy(s_bkd_ring + at, d, first);
    if (room > first) memcpy(s_bkd_ring, d + first, room - first);
    __sync_synchronize();
    s_bkd_head += room;
    d += room; n -= room;
  }
  return true;
}

// Open (or skip) a file through the UI task; *have: its size when continuing.
static uint8_t bkdOpen(const char* path, const char* final_name, bool append, uint32_t* have) {
  bkCopy(s_bkd_fpath, sizeof s_bkd_fpath, path);
  bkCopy(s_bkd_final, sizeof s_bkd_final, final_name);
  s_bkd_fappend = append;
  s_bkd_head = s_bkd_tail = 0;
  __sync_synchronize();
  s_bkd_fstate = BkdOpenReq;
  if (!bkdWait(BkdOpen, BkdSkip, 15000) && s_bkd_fstate != BkdFailed && s_bkd_fstate != BkdRetry) return BkdFailed;
  if (have) *have = s_bkd_fsize;
  return s_bkd_fstate;
}

static bool bkdClose(bool rename_it) {   // drained, closed, renamed by the UI task
  if (s_bkd_fstate == BkdFailed || s_bkd_fstate == BkdRetry) return false;   // already closed
  if (!rename_it) s_bkd_final[0] = 0;
  __sync_synchronize();
  s_bkd_fstate = BkdCloseReq;
  return bkdWait(BkdClosed, BkdFailed, 60000) && s_bkd_fstate == BkdClosed;
}

static bool bkaOpen(HTTPClient& http, WiFiClientSecure& tls, WiFiClient& plain, const BkaTrack& tr,
                    uint32_t from, uint32_t* total, int* code_out);   // BooksAudio.inc
static void bkdTaskFn(void*) {
  uint8_t* buf = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_bkd_ring) s_bkd_ring = (uint8_t*)heap_caps_malloc(kBkdRing, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  const int n = s_bkd_tracks;
  bool ok = buf && s_bkd_ring;
  if (!ok) snprintf(s_bkd_msg, sizeof s_bkd_msg, "Not enough memory to download");
  for (int i = 0; ok && i < n && !s_bkd_cancel; ++i) {
    s_bkd_track = i;
    char path[64], part[72];
    snprintf(path, sizeof path, "%s/%03d.mp3", s_bkd_dir, i + 1);
    snprintf(part, sizeof part, "%s.part", path);
    bool track_ok = false;
    bool append = true;
    for (int attempt = 0; attempt < 6 && !s_bkd_cancel && !track_ok; ++attempt) {
      if (attempt) vTaskDelay(pdMS_TO_TICKS(1500 * attempt));
      uint32_t have = 0;
      const uint8_t st = bkdOpen(part, path, append, &have);
      if (st == BkdSkip) { track_ok = true; break; }   // finished in an earlier attempt
      if (st == BkdRetry) { --attempt; vTaskDelay(pdMS_TO_TICKS(500)); continue; }   // the card did not open it (the UI caps these)
      if (st != BkdOpen) { ok = false; break; }        // the UI side said why
      // Fetched the way the player streams (a Range request, which the server and any
      // proxy in front of it answer at once), continuing a .part where it stopped.
      HTTPClient http; WiFiClientSecure tls; WiFiClient plain;
      uint32_t total = 0;
      int code = 0;
      if (!bkaOpen(http, tls, plain, s_bka_tracks[i], have, &total, &code)) {
        bkdClose(false);
        if (code == 401) bkRefresh();
        snprintf(s_bkd_msg, sizeof s_bkd_msg, "Track %d failed (%d)", i + 1, code);
        Serial.printf("[books] download track %d: open failed %d (from %u)\n", i + 1, code, (unsigned)have);
        continue;
      }
      if (code == 200 && have) {   // the range was ignored: start the file again
        http.end();
        bkdClose(false);
        append = false;
        continue;
      }
      WiFiClient* stream = http.getStreamPtr();
      uint32_t pos = have, idle_since = millis();
      s_bk_dl_total = total; s_bk_dl_got = pos;
      bool put_failed = false;
      while (!s_bkd_cancel) {
        if (total && pos >= total) break;
        const int avail = stream ? stream->available() : 0;
        if (avail <= 0) {
          if (!stream || !stream->connected()) break;
          if (millis() - idle_since > 30000) break;
          vTaskDelay(pdMS_TO_TICKS(5));
          continue;
        }
        const int r = stream->read(buf, avail < 4096 ? avail : 4096);
        if (r <= 0) continue;
        if (!bkdPut(buf, (size_t)r)) { put_failed = true; break; }
        pos += (uint32_t)r;
        s_bk_dl_got = pos;
        idle_since = millis();
      }
      http.end();
      if (s_bkd_fstate == BkdRetry) {   // the card failed a write: go on from what it holds
        Serial.printf("[books] download track %d: card write failed at %u, retrying\n", i + 1, (unsigned)pos);
        append = true;
        --attempt;   // not the link's fault: the UI side caps these
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      }
      const bool complete = (total && pos >= total) || (!total && pos > 0 && !put_failed);
      if (!bkdClose(complete && !s_bkd_cancel)) {
        if (s_bkd_fstate == BkdRetry && !s_bkd_cancel) {   // the last writes failed: fetch them again
          append = true; --attempt; vTaskDelay(pdMS_TO_TICKS(500));
          continue;
        }
        ok = false; break;                                 // the UI said why
      }
      append = true;
      if (complete) track_ok = true;
      else Serial.printf("[books] download track %d: broke off at %u of %u, retrying\n", i + 1, (unsigned)pos, (unsigned)total);
    }
    if (!ok) break;
    if (!track_ok) {
      if (!s_bkd_msg[0]) snprintf(s_bkd_msg, sizeof s_bkd_msg, "Track %d failed", i + 1);
      ok = false;
      break;
    }
    s_bkd_msg[0] = 0;
  }
  if (ok && s_bkd_cancel) { snprintf(s_bkd_msg, sizeof s_bkd_msg, "Download stopped"); ok = false; }
  if (ok) {   // the manifest last: its presence means "complete"
    JsonDocument doc(&s_bk_json_alloc);
    doc["id"] = s_bkd_item; doc["title"] = s_bka_title; doc["author"] = s_bka_author;
    doc["duration"] = s_bka_duration;
    JsonArray ta = doc["tracks"].to<JsonArray>();
    for (int i = 0; i < n; ++i) {
      JsonObject t = ta.add<JsonObject>();
      char fname[12];
      snprintf(fname, sizeof fname, "%03d.mp3", i + 1);
      t["f"] = fname; t["start"] = s_bka_tracks[i].start; t["dur"] = s_bka_tracks[i].dur;
    }
    JsonArray ca = doc["chapters"].to<JsonArray>();
    for (int i = 0; i < s_bka_chapter_count; ++i) {
      JsonObject c = ca.add<JsonObject>();
      c["s"] = s_bka_chapters[i].start; c["e"] = s_bka_chapters[i].end; c["t"] = s_bka_chapters[i].title;
    }
    const size_t len = measureJson(doc);
    char* js = (char*)bkAlloc(len + 1);
    char path[64];
    snprintf(path, sizeof path, "%s/book.json", s_bkd_dir);
    ok = js && bkdOpen(path, "", false, nullptr) == BkdOpen;
    if (ok) {
      serializeJson(doc, js, len + 1);
      ok = bkdPut((const uint8_t*)js, len) && bkdClose(false);
    }
    bkFree(js);
    if (!ok && !s_bkd_msg[0]) snprintf(s_bkd_msg, sizeof s_bkd_msg, "Cannot write to the SD card");
  }
  if (ok) snprintf(s_bkd_msg, sizeof s_bkd_msg, "On the SD card: plays offline");
  if (buf) heap_caps_free(buf);
  s_bkd_ok = ok;
  s_bkd_done = true;
  s_bkd_active = false;
  s_bkd_task = nullptr;
  vTaskDelete(nullptr);
}

// The UI half of the download (50 ms timer): opens, writes, closes, renames.
static void booksDownloadPump(lv_timer_t*) {
  const uint8_t st = s_bkd_fstate;
  fs::FS* fs = (st == BkdOpenReq || st == BkdOpen || st == BkdCloseReq) ? booksFs() : nullptr;
  if (st == BkdOpenReq) {
    if (!fs) { snprintf(s_bkd_msg, sizeof s_bkd_msg, "Insert an SD card to download"); s_bkd_fstate = BkdFailed; return; }
    s_bk_sd_busy = true;
    fs->mkdir("/books"); fs->mkdir("/books/audio"); fs->mkdir(s_bkd_dir);
    if (s_bkd_final[0] && fs->exists(s_bkd_final)) {   // done before (a resumed download)
      File e = fs->open(s_bkd_final, FILE_READ);
      const size_t have = e ? e.size() : 0;
      if (e) e.close();
      if (have > 0) { s_bk_sd_busy = false; s_bkd_fstate = BkdSkip; return; }
    }
    // What is there already, read before opening (an appending handle's size is not
    // reliable everywhere).
    uint32_t have = 0;
    if (s_bkd_fappend && fs->exists(s_bkd_fpath)) {
      File r = fs->open(s_bkd_fpath, FILE_READ);
      if (r) { have = (uint32_t)r.size(); r.close(); }
    }
    s_bkd_file = fs->open(s_bkd_fpath, have ? FILE_APPEND : FILE_WRITE);
    if (!s_bkd_file) {
      s_bk_sd_busy = false;
      if (s_bkd_write_fails && s_bkd_write_fails < 5) {   // just after a failed write: try again
        ++s_bkd_write_fails;
        s_bkd_fstate = BkdRetry;
        return;
      }
      snprintf(s_bkd_msg, sizeof s_bkd_msg, "Cannot write to the SD card");
      s_bkd_fstate = BkdFailed;
      return;
    }
    s_bkd_fsize = have;
    s_bkd_fwritten = 0;
    __sync_synchronize();
    s_bkd_fstate = BkdOpen;
    return;
  }
  if (st != BkdOpen && st != BkdCloseReq) return;
  // Up to 32 KB a tick: well above the link's rate, short enough to keep the UI going.
  size_t budget = 32 * 1024;
  while (budget && s_bkd_head != s_bkd_tail) {
    const size_t at = s_bkd_tail % kBkdRing;
    size_t n = s_bkd_head - s_bkd_tail;
    if (n > kBkdRing - at) n = kBkdRing - at;
    if (n > budget) n = budget;
    errno = 0;
    const size_t w = s_bkd_file.write(s_bkd_ring + at, n);
    if (w != n) {
      Serial.printf("[books] download write %u of %u at %u: errno %d (failure %u)\n", (unsigned)w, (unsigned)n,
                    (unsigned)(s_bkd_fsize + s_bkd_fwritten), errno, (unsigned)s_bkd_write_fails + 1);
      s_bkd_file.close();
      s_bk_sd_busy = false;
      bool full = false;
#if CAP_SD && !defined(WADA_WEB)
      const uint64_t free_b = SD.totalBytes() > SD.usedBytes() ? SD.totalBytes() - SD.usedBytes() : 0;
      full = free_b < 1024u * 1024u;
#endif
      if (full) {
        snprintf(s_bkd_msg, sizeof s_bkd_msg, "The SD card is full");
        s_bkd_fstate = BkdFailed;
      } else if (++s_bkd_write_fails <= 5) {   // reopened and continued from what the card holds
        snprintf(s_bkd_msg, sizeof s_bkd_msg, "SD card busy, retrying");
        s_bkd_fstate = BkdRetry;
      } else {
        snprintf(s_bkd_msg, sizeof s_bkd_msg, "Writing to the SD card failed");
        s_bkd_fstate = BkdFailed;
      }
      return;
    }
    // Every 256 KB onto the card for good (FAT records a file's size only then), so a
    // failed write later costs at most that much again.
    if ((s_bkd_fwritten + n) / (256u * 1024u) != s_bkd_fwritten / (256u * 1024u)) s_bkd_file.flush();
    s_bkd_fwritten += n;
    if (s_bkd_write_fails && s_bkd_fwritten >= 1024u * 1024u) s_bkd_write_fails = 0;   // going well again
    __sync_synchronize();
    s_bkd_tail += n;
    budget -= n;
  }
  if (st == BkdCloseReq && s_bkd_head == s_bkd_tail) {
    s_bkd_file.close();
    if (fs && s_bkd_final[0]) { fs->remove(s_bkd_final); fs->rename(s_bkd_fpath, s_bkd_final); }
    s_bk_sd_busy = false;
    __sync_synchronize();
    s_bkd_fstate = BkdClosed;
  }
}

static void booksAudioPause();
static void booksAudioDownloadStart() {
  if (s_bkd_active || !s_bka_item[0] || s_bka_local) return;
  if (!booksFs()) { if (g_lv.task) g_lv.task->showAlert(TR("Insert an SD card to download"), 1800); return; }
  if (WiFi.status() != WL_CONNECTED) { if (g_lv.task) g_lv.task->showAlert(TR("Connect to Wi-Fi first"), 1600); return; }
  if (s_bk_job == BkJob::Download) { if (g_lv.task) g_lv.task->showAlert(TR("A book is still downloading"), 1600); return; }
  // One connection at a time: a stream beside the download starved both.
  if (booksAudioRunning()) {
    booksAudioPause();
    if (g_lv.task) g_lv.task->showAlert(TR("Paused while it downloads"), 1600);
  }
  bkAudioDirFor(s_bka_item, s_bkd_dir, sizeof s_bkd_dir);
  bkCopy(s_bkd_item, sizeof s_bkd_item, s_bka_item);
  s_bkd_tracks = s_bka_track_count;
  s_bkd_track = 0;
  s_bkd_cancel = false;
  s_bkd_write_fails = 0;
  s_bkd_done = false;
  s_bkd_msg[0] = 0;
  s_bkd_fstate = BkdIdle;
  s_bkd_active = true;
  if (!s_bkd_timer) s_bkd_timer = lv_timer_create(booksDownloadPump, 50, nullptr);
  if (xTaskCreatePinnedToCore(bkdTaskFn, "bkdl", 12288, nullptr, 3, &s_bkd_task, 0) != pdPASS) {
    s_bkd_active = false;
    if (g_lv.task) g_lv.task->showAlert(TR("Not enough memory to download"), 1800);
  }
}

// Remove a downloaded audiobook's folder.
static int s_bk_aud_delete = -1;
static void booksDeleteAudioConfirmed() {
  const int i = s_bk_aud_delete;
  s_bk_aud_delete = -1;
  fs::FS* fs = booksFs();
  if (i < 0 || i >= s_bk_aud_count || !fs) return;
  if (s_bka_local && strcmp(s_bka_dir, s_bk_aud[i].dir) == 0) {
    if (booksAudioRunning()) booksAudioStop();
    s_bka_item[0] = 0; s_bka_local = false; s_bka_track_count = 0;
  }
  s_bk_sd_busy = true;
  // Listing while deleting is not safe on FAT: collect a batch of names, delete them,
  // and go again until the folder is empty.
  for (int round = 0; round < 32; ++round) {
    File d = fs->open(s_bk_aud[i].dir);
    char names[64][16];
    int k = 0;
    if (d && d.isDirectory()) {
      for (File e = d.openNextFile(); e && k < 64; e = d.openNextFile()) {
        const char* nm = strrchr(e.name(), '/');
        bkCopy(names[k++], sizeof names[0], nm ? nm + 1 : e.name());
        e.close();
      }
    }
    if (d) d.close();
    if (!k) break;
    for (int j = 0; j < k; ++j) {
      char path[64];
      snprintf(path, sizeof path, "%s/%s", s_bk_aud[i].dir, names[j]);
      fs->remove(path);
    }
  }
  fs->rmdir(s_bk_aud[i].dir);
  s_bk_sd_busy = false;
  booksShowView(BkView::Shelf);
}

static void booksOpenLocalAudio(int i);
static void booksAudioRowCb(lv_event_t* e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= s_bk_aud_count) return;
  if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
    s_bk_aud_delete = i;
    static char msg[160];
    snprintf(msg, sizeof msg, "%s\n\n%s", TR("Remove this audiobook from the SD card?"), s_bk_aud[i].title);
    showConfirm(msg, TR("Remove"), booksDeleteAudioConfirmed);
    return;
  }
  if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
  booksOpenLocalAudio(i);
}

static void booksOpenLocalAudio(int i) {
  if (s_bka_item[0] && strcmp(s_bka_item, s_bk_aud[i].item) == 0 && s_bka_local) { booksOpenPlayer(); return; }
  if (s_bkd_active) { if (g_lv.task) g_lv.task->showAlert(TR("An audiobook is still downloading"), 1600); return; }
  if (booksAudioRunning()) { s_bka_time = booksAudioPos(); booksAudioStop(); }
  if (!booksLoadLocalAudio(i)) { if (g_lv.task) g_lv.task->showAlert(TR("This audiobook could not be read"), 1800); return; }
  s_bka_listened_ms = 0;
  s_bka_last_sync_ms = millis();
  if (WiFi.status() == WL_CONNECTED && s_abs_token[0] && s_bk_job == BkJob::None) {
    bkCopy(s_bk_job_arg, sizeof s_bk_job_arg, s_bka_item);
    if (s_bka_dirty) { s_bka_job_time = s_bka_time; booksStartJob(BkJob::AudioProgress); }   // offline listening first
    else booksStartJob(BkJob::AudioPull);
  }
  booksOpenPlayer();
  if (!booksAudioPlayFrom(s_bka_time) && g_lv.task) g_lv.task->showAlert(booksAudioError(), 2000);
  booksPlayerRefresh();
}

// ================================= page chrome =================================
static void booksShowView(BkView v);
static void booksCloseReader(bool to_shelf);

static bool booksSaveStep();
static void booksClosePage() {
  while (!booksSaveStep()) {}   // a download still being written finishes first
  if (s_bk_view == BkView::Reader) booksCloseReader(false);
  if (s_bk_timer) { lv_timer_del(s_bk_timer); s_bk_timer = nullptr; }
  appPageEnd(booksClosePage);
  if (s_bk_root) { appPageDeleteRootAsync(s_bk_root); s_bk_root = nullptr; }
  s_bk_body = s_bk_status = s_bk_user_ta = s_bk_pass_ta = s_bk_server_ta = nullptr;
  s_bk_text_lbl = s_bk_foot = s_bk_menu = nullptr;
  s_bka_lbl_time = s_bka_lbl_chapter = s_bka_lbl_state = s_bka_bar = s_bka_play_lbl = s_bka_dl_lbl = nullptr;
  s_bka_vol_lbl = nullptr;
}

static bool booksReaderOpen() { return s_bk_root && s_bk_view == BkView::Reader && s_bk_text_lbl; }
// The keyboard goes to Books while a book or the player is up.
static bool booksWantsKeys() { return booksReaderOpen() || (s_bk_root && s_bk_view == BkView::Player); }

// Back from the reader goes to the shelf; from the shelf or the server tab it closes Books.
static bool booksBackStep() {
  if (!s_bk_root) return false;
  if (s_bk_view == BkView::Reader) { booksCloseReader(true); return true; }
  if (s_bk_view == BkView::Player) { booksShowView(BkView::Shelf); return true; }   // it plays on
  return false;
}

static lv_obj_t* booksLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
  return l;
}

static lv_obj_t* booksButton(lv_obj_t* parent, const char* text, lv_event_cb_t cb, void* ud,
                             lv_coord_t w, lv_coord_t h) {
  lv_obj_t* b = lv_btn_create(parent);
  lv_obj_set_size(b, w, h);
  styleButton(b);
  lv_obj_set_style_pad_all(b, 2, LV_PART_MAIN);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, &g_font_14, LV_PART_MAIN);
  lv_obj_center(l);
  return b;
}

// A list row: title, a second line, optional right-hand tag.
static lv_obj_t* booksRow(lv_obj_t* list, const char* title, const char* sub, const char* tag,
                          lv_event_cb_t cb, intptr_t ud) {
  lv_obj_t* row = lv_obj_create(list);
  lv_obj_remove_style_all(row);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_ver(row, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(row, 8, LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
  lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_color(row, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
  lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_CONTROL), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  const lv_coord_t tag_w = tag && tag[0] ? 46 : 0;
  lv_obj_t* t = booksLabel(row, title, &g_font_semi_14, COLOR_TEXT);
  lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
  lv_obj_set_size(t, lv_pct(100), lv_font_get_line_height(&g_font_semi_14));
  lv_obj_set_style_pad_right(t, tag_w, LV_PART_MAIN);
  if (sub && sub[0]) {
    lv_obj_t* s = booksLabel(row, sub, &g_font_12, COLOR_SUB);
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s, lv_pct(100), lv_font_get_line_height(&g_font_12));
    lv_obj_set_style_pad_right(s, tag_w, LV_PART_MAIN);
    lv_obj_set_y(s, lv_font_get_line_height(&g_font_semi_14) + 2);
  }
  if (tag_w) {
    lv_obj_t* g = booksLabel(row, tag, &g_font_12, COLOR_SUB);
    lv_obj_align(g, LV_ALIGN_RIGHT_MID, 0, 0);
  }
  if (cb) {
    // A short click opens; a long press (remove, where a row offers it) must not also
    // open on release, which a plain CLICKED would.
    lv_obj_add_event_cb(row, cb, LV_EVENT_SHORT_CLICKED, (void*)ud);
    lv_obj_add_event_cb(row, cb, LV_EVENT_LONG_PRESSED, (void*)ud);
  }
  return row;
}

static lv_obj_t* booksList(lv_obj_t* parent, lv_coord_t y) {
  lv_obj_t* list = lv_obj_create(parent);
  lv_obj_remove_style_all(list);
  lv_obj_set_size(list, lv_pct(100), s_bk_body_h - y);
  lv_obj_set_pos(list, 0, y);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
  return list;
}

// The two-tab header of the shelf and server views.
static void booksTabCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  booksShowView((BkView)(intptr_t)lv_event_get_user_data(e));
}
static void booksTabs(lv_obj_t* body, BkView active) {
  const lv_coord_t w = s_bk_body_w;
  const lv_coord_t bw = (w - 18) / 2;
  lv_obj_t* a = booksButton(body, TR("SD card"), booksTabCb, (void*)(intptr_t)BkView::Shelf, bw, 30);
  lv_obj_set_pos(a, 6, 4);
  lv_obj_t* b = booksButton(body, "Audiobookshelf", booksTabCb, (void*)(intptr_t)BkView::Server, bw, 30);
  lv_obj_set_pos(b, 12 + bw, 4);
  lv_obj_t* on = active == BkView::Shelf ? a : b;
  lv_obj_set_style_border_width(on, 3, LV_PART_MAIN);
  lv_obj_set_style_border_side(on, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_color(on, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
}

// =================================== the shelf ===================================
static void booksOpenReader(int idx);

static int booksSortedIndex(int k) {   // most recently read first; never-opened ones last
  static int order[kBkMaxBooks];
  if (k == 0) {
    for (int i = 0; i < s_bk_count; ++i) order[i] = i;
    for (int i = 1; i < s_bk_count; ++i)
      for (int j = i; j > 0 && s_bk_books[order[j]].seq > s_bk_books[order[j - 1]].seq; --j) {
        const int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
      }
  }
  return order[k];
}

static void booksDeleteConfirmed() {
  const int i = s_bk_delete_idx;
  s_bk_delete_idx = -1;
  if (i < 0 || i >= s_bk_count) return;
  if (fs::FS* fs = booksFs()) fs->remove(s_bk_books[i].file);
  for (int k = i; k + 1 < s_bk_count; ++k) s_bk_books[k] = s_bk_books[k + 1];
  --s_bk_count;
  booksSaveIndex();
  booksShowView(BkView::Shelf);
}

static void booksShelfRowCb(lv_event_t* e) {
  const int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= s_bk_count) return;
  if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
    s_bk_delete_idx = idx;
    static char msg[160];
    snprintf(msg, sizeof msg, "%s\n\n%s", TR("Remove this book from the device?"), s_bk_books[idx].title);
    showConfirm(msg, TR("Remove"), booksDeleteConfirmed);
    return;
  }
  if (lv_event_get_code(e) == LV_EVENT_SHORT_CLICKED) booksOpenReader(idx);
}

static void booksNowPlayingCb(lv_event_t* e);
// The loaded audiobook, first on the shelf: one tap back to its player.
static void booksNowPlayingRow(lv_obj_t* list) {
  if (!s_bka_item[0]) return;
  char sub[96];
  snprintf(sub, sizeof sub, "%s%s%s", booksAudioRunning() ? TR("Playing") : TR("Paused"),
           s_bka_author[0] ? " \xc2\xb7 " : "", s_bka_author);
  booksRow(list, s_bka_title, sub, LV_SYMBOL_AUDIO, booksNowPlayingCb, 0);
}

// Audiobooks downloaded to the card (the loaded one is already the first row).
static void booksAudioRowCb(lv_event_t* e);
static void booksAudioRows(lv_obj_t* list) {
  for (int i = 0; i < s_bk_aud_count; ++i) {
    if (s_bka_item[0] && strcmp(s_bka_item, s_bk_aud[i].item) == 0) continue;
    booksRow(list, s_bk_aud[i].title, s_bk_aud[i].author, LV_SYMBOL_AUDIO, booksAudioRowCb, i);
  }
}

static void booksBuildShelf() {
  booksTabs(s_bk_body, BkView::Shelf);
  booksScanAudio();
  if (!booksFs() && !s_bka_item[0]) {
    lv_obj_t* l = booksLabel(s_bk_body,
        TR("No SD card.\n\nBooks are stored on the SD card: insert one, then open Books again."),
        &g_font_14, COLOR_SUB);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, s_bk_body_w - 24);
    lv_obj_set_pos(l, 12, 50);
    return;
  }
  if (s_bk_count == 0 && !s_bka_item[0] && s_bk_aud_count == 0) {
    lv_obj_t* l = booksLabel(s_bk_body,
        TR("No books yet.\n\nGet them from your Audiobookshelf server (the tab above), "
           "or copy .epub and .txt files to the folder /books on the SD card."),
        &g_font_14, COLOR_SUB);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, s_bk_body_w - 24);
    lv_obj_set_pos(l, 12, 50);
    return;
  }
  lv_obj_t* list = booksList(s_bk_body, 40);
  booksNowPlayingRow(list);
  booksAudioRows(list);
  for (int k = 0; k < s_bk_count; ++k) {
    const int i = booksSortedIndex(k);
    const BkBook& b = s_bk_books[i];
    char sub[96], tag[8] = "";
    if (b.chapters) snprintf(tag, sizeof tag, "%u%%", (unsigned)b.pct);
    snprintf(sub, sizeof sub, "%s%s%s", b.author[0] ? b.author : "",
             b.author[0] && b.item_id[0] ? " \xc2\xb7 " : "", b.item_id[0] ? "Audiobookshelf" : "");
    booksRow(list, b.title[0] ? b.title : b.file, sub, tag, booksShelfRowCb, i);
  }
}

// ================================ Audiobookshelf ================================
static void booksSetStatus(const char* text) {
  if (s_bk_status && lv_obj_is_valid(s_bk_status)) lv_label_set_text(s_bk_status, text);
}

static void booksLoginCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  const char* server = lv_textarea_get_text(s_bk_server_ta);
  const char* user = lv_textarea_get_text(s_bk_user_ta);
  const char* pass = lv_textarea_get_text(s_bk_pass_ta);
  if (!server[0] || !user[0]) { booksSetStatus(TR("Enter the server address and your user name")); return; }
  if (strncasecmp(server, "http://", 7) != 0 && strncasecmp(server, "https://", 8) != 0)
    snprintf(s_abs_server, sizeof s_abs_server, "http://%s", server);
  else
    bkCopy(s_abs_server, sizeof s_abs_server, server);
  bkCopy(s_bk_job_user, sizeof s_bk_job_user, user);
  bkCopy(s_bk_job_pass, sizeof s_bk_job_pass, pass);
  lv_textarea_set_text(s_bk_pass_ta, "");
  hideKb();
  if (booksStartJob(BkJob::Login)) booksSetStatus(TR("Signing in\xe2\x80\xa6"));
}

static void booksLogoutCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  s_abs_token[0] = s_abs_refresh[0] = 0;
  s_abs_lib_count = s_abs_item_count = 0;
  booksSaveAbs();
  booksShowView(BkView::Server);
}

static lv_obj_t* booksField(lv_obj_t* body, const char* placeholder, const char* text, lv_coord_t y, bool password) {
  lv_obj_t* ta = lv_textarea_create(body);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_password_mode(ta, password);
  lv_obj_set_style_text_font(ta, &g_font_14, LV_PART_MAIN);
  taSetPlaceholder(ta, placeholder);
  if (text && text[0]) lv_textarea_set_text(ta, text);
  lv_obj_set_size(ta, s_bk_body_w - 16, 36);
  lv_obj_set_pos(ta, 8, y);
  attachSettingsTaEvents(ta);
  return ta;
}

// Libraries and items again from the server (a library added there since).
static void booksReloadCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_bk_job != BkJob::None) return;
  s_abs_lib_count = s_abs_item_count = 0;
  s_abs_page = 0;
  booksShowView(BkView::Server);
}

static void booksLibCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= s_abs_lib_count) return;
  bkCopy(s_abs_lib, sizeof s_abs_lib, s_abs_libs[i].id);
  booksSaveAbs();
  s_abs_item_count = 0; s_abs_page = 0;
  booksShowView(BkView::Server);
}

static void booksPageCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  const int d = (int)(intptr_t)lv_event_get_user_data(e);
  const int pages = (s_abs_total + kBkMaxItems - 1) / kBkMaxItems;
  if (s_abs_page + d < 0 || s_abs_page + d >= pages) return;
  s_abs_page += d;
  s_abs_item_count = 0;
  booksShowView(BkView::Server);
}

static int booksFindItem(const char* item_id) {
  for (int i = 0; i < s_bk_count; ++i) if (strcmp(s_bk_books[i].item_id, item_id) == 0) return i;
  return -1;
}

static void booksItemCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= s_abs_item_count) return;
  const BkAbsItem& it = s_abs_items[i];
  const int local = booksFindItem(it.id);
  if (local >= 0) { booksOpenReader(local); return; }
  if (it.audio && strcasecmp(it.fmt, "epub") != 0) { booksStartAudiobook(it); return; }
  if (strcasecmp(it.fmt, "epub") != 0) {
    if (g_lv.task) g_lv.task->showAlert(TR("Only EPUB books can be read here for now"), 2200);
    return;
  }
  if (s_bk_count >= kBkMaxBooks) { if (g_lv.task) g_lv.task->showAlert(TR("The shelf is full"), 1600); return; }
  if (!booksFs()) { if (g_lv.task) g_lv.task->showAlert(TR("Insert an SD card to download books"), 2000); return; }
  bkCopy(s_bk_job_arg, sizeof s_bk_job_arg, it.id);
  bkCopy(s_bk_job_arg2, sizeof s_bk_job_arg2, it.title);
  if (booksStartJob(BkJob::Download)) booksSetStatus(TR("Downloading\xe2\x80\xa6"));
}

static void booksBuildServer() {
  booksTabs(s_bk_body, BkView::Server);
  const lv_coord_t w = s_bk_body_w;
  s_bk_status = booksLabel(s_bk_body, "", &g_font_12, COLOR_SUB);
  lv_label_set_long_mode(s_bk_status, LV_LABEL_LONG_DOT);
  lv_obj_set_height(s_bk_status, lv_font_get_line_height(&g_font_12));   // one line; the alert has the rest
  lv_obj_set_width(s_bk_status, w - 16);
  lv_obj_set_pos(s_bk_status, 8, 40);
  if (!s_abs_token[0]) {   // ---- sign in ----
    lv_label_set_text(s_bk_status, TR("Sign in to your Audiobookshelf server"));
    s_bk_server_ta = booksField(s_bk_body, "http://host:13378", s_abs_server, 60, false);
    s_bk_user_ta = booksField(s_bk_body, TR("User name"), s_abs_user, 102, false);
    s_bk_pass_ta = booksField(s_bk_body, TR("Password"), "", 144, true);
    lv_obj_t* go = booksButton(s_bk_body, TR("Sign in"), booksLoginCb, nullptr, w - 16, 34);
    lv_obj_set_pos(go, 8, 188);
    lv_obj_t* note = booksLabel(s_bk_body, TR("The password is not stored; the device keeps the server's sign-in token."),
                                &g_font_12, COLOR_SUB);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, w - 16);
    lv_obj_set_pos(note, 8, 228);
    return;
  }
  // ---- signed in: library chooser + items ----
  lv_obj_t* out = booksButton(s_bk_body, TR("Sign out"), booksLogoutCb, nullptr, 72, 24);
  lv_obj_align(out, LV_ALIGN_TOP_RIGHT, -6, 38);
  lv_obj_t* re = booksButton(s_bk_body, LV_SYMBOL_REFRESH, booksReloadCb, nullptr, 32, 24);
  lv_obj_align(re, LV_ALIGN_TOP_RIGHT, -84, 38);
  lv_obj_set_width(s_bk_status, w - 132);
  if (s_abs_lib_count == 0) {
    lv_label_set_text(s_bk_status, TR("Loading libraries\xe2\x80\xa6"));
    booksStartJob(BkJob::Libraries);
    return;
  }
  lv_obj_t* list = booksList(s_bk_body, 66);
  if (s_abs_lib_count > 1) {   // library chips as the first rows
    for (int i = 0; i < s_abs_lib_count; ++i) {
      const bool on = strcmp(s_abs_libs[i].id, s_abs_lib) == 0;
      lv_obj_t* r = booksRow(list, s_abs_libs[i].name, nullptr, on ? LV_SYMBOL_OK : "", booksLibCb, i);
      lv_obj_set_style_pad_ver(r, 3, LV_PART_MAIN);
    }
  }
  if (!s_abs_lib[0]) { lv_label_set_text(s_bk_status, TR("No book library on this server")); return; }
  if (s_abs_item_count == 0) {
    lv_label_set_text(s_bk_status, TR("Loading books\xe2\x80\xa6"));
    booksStartJob(BkJob::Items);
    return;
  }
  char st[64];
  const int pages = (s_abs_total + kBkMaxItems - 1) / kBkMaxItems;
  snprintf(st, sizeof st, "%d %s \xc2\xb7 %s %d/%d", s_abs_total, TR("items"), TR("page"), s_abs_page + 1, pages ? pages : 1);
  lv_label_set_text(s_bk_status, st);
  for (int i = 0; i < s_abs_item_count; ++i) {
    const BkAbsItem& it = s_abs_items[i];
    char tag[16];
    if (booksFindItem(it.id) >= 0) snprintf(tag, sizeof tag, "%s", LV_SYMBOL_OK);
    else if (it.fmt[0]) snprintf(tag, sizeof tag, "%s", strcasecmp(it.fmt, "epub") == 0 ? "EPUB" : it.fmt);
    else if (it.audio) snprintf(tag, sizeof tag, "%s", LV_SYMBOL_AUDIO);
    else tag[0] = 0;
    booksRow(list, it.title, it.author, tag, booksItemCb, i);
  }
  if (pages > 1) {
    lv_obj_t* nav = lv_obj_create(list);
    lv_obj_remove_style_all(nav);
    lv_obj_set_size(nav, lv_pct(100), 40);
    lv_obj_t* p = booksButton(nav, LV_SYMBOL_LEFT, booksPageCb, (void*)(intptr_t)-1, 60, 30);
    lv_obj_align(p, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t* n = booksButton(nav, LV_SYMBOL_RIGHT, booksPageCb, (void*)(intptr_t)1, 60, 30);
    lv_obj_align(n, LV_ALIGN_RIGHT_MID, -8, 0);
  }
}

// Worker results, polled while the page is open.
static void booksAddDownloaded(const char* file) {
  if (s_bk_count >= kBkMaxBooks) return;
  BkBook& b = s_bk_books[s_bk_count++];
  memset(&b, 0, sizeof b);
  bkCopy(b.file, sizeof b.file, file);
  bkCopy(b.title, sizeof b.title, s_bk_job_arg2);
  bkCopy(b.item_id, sizeof b.item_id, s_bk_job_arg);
  for (int i = 0; i < s_abs_item_count; ++i)
    if (strcmp(s_abs_items[i].id, s_bk_job_arg) == 0) bkCopy(b.author, sizeof b.author, s_abs_items[i].author);
  booksSaveIndex();
}

// ---- saving a download to the card, a slice per timer tick (UI task: see bkJobDownload) ----
static File   s_bk_save_file;
static size_t s_bk_save_pos = 0;
static bool   s_bk_save_active = false;
static char   s_bk_save_path[96] = "";

static void booksSaveFail(const char* why) {
  if (s_bk_save_file) s_bk_save_file.close();
  if (fs::FS* fs = booksFs()) fs->remove(s_bk_save_path);
  s_bk_sd_busy = false;
  s_bk_save_active = false;
  bkFree(s_bk_dl_buf); s_bk_dl_buf = nullptr; s_bk_dl_len = 0;
  booksSetStatus(why);
  if (g_lv.task) g_lv.task->showAlert(why, 2400);
}

static void booksSaveBegin() {
  // A readable file name for the card: the title, then a short hash of the item id.
  uint32_t h = 2166136261u;
  for (const char* p = s_bk_job_arg; *p; ++p) { h ^= (uint8_t)*p; h *= 16777619u; }
  char base[48];
  size_t w = 0;
  for (const char* p = s_bk_job_arg2; *p && w + 1 < sizeof base; ++p) {
    const unsigned char c = (unsigned char)*p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) base[w++] = (char)c;
    else if ((c == ' ' || c == '-' || c == '_') && w && base[w - 1] != '_') base[w++] = '_';
  }
  while (w && base[w - 1] == '_') --w;
  base[w] = 0;
  snprintf(s_bk_save_path, sizeof s_bk_save_path, "%s/%s%s%06lx.epub", kBkDir, base, w ? "-" : "",
           (unsigned long)(h & 0xFFFFFF));
  s_bk_save_active = true;
  s_bk_save_pos = 0;
  fs::FS* fs = booksFs();
  if (!fs) { booksSaveFail(TR("Insert an SD card to download books")); return; }
#if CAP_SD && !defined(WADA_WEB)
  const uint64_t free_bytes = SD.totalBytes() > SD.usedBytes() ? SD.totalBytes() - SD.usedBytes() : 0;
  if (free_bytes < (uint64_t)s_bk_dl_len + 64u * 1024u) { booksSaveFail(TR("The SD card is full")); return; }
#endif
  s_bk_sd_busy = true;
  fs->mkdir(kBkDir);
  s_bk_save_file = fs->open(s_bk_save_path, FILE_WRITE);
  if (!s_bk_save_file) { booksSaveFail(TR("Cannot write to the SD card")); return; }
  booksSetStatus(TR("Saving\xe2\x80\xa6"));
}

// One slice; true when finished (either way).
static bool booksSaveStep() {
  if (!s_bk_save_active) return true;
  const size_t slice = 32u * 1024u;
  const size_t n = s_bk_dl_len - s_bk_save_pos < slice ? s_bk_dl_len - s_bk_save_pos : slice;
  if (n && s_bk_save_file.write(s_bk_dl_buf + s_bk_save_pos, n) != n) {
    booksSaveFail(TR("Writing to the SD card failed"));
    return true;
  }
  s_bk_save_pos += n;
  if (s_bk_save_pos < s_bk_dl_len) return false;
  s_bk_save_file.close();
  s_bk_sd_busy = false;
  s_bk_save_active = false;
  bkFree(s_bk_dl_buf); s_bk_dl_buf = nullptr; s_bk_dl_len = 0;
  booksAddDownloaded(s_bk_save_path);
  if (g_lv.task) g_lv.task->showAlert(TR("Saved to the SD card"), 1400);
  if (s_bk_root && s_bk_view == BkView::Server) booksShowView(BkView::Server);
  return true;
}

static bool s_bk_save_active_fwd() { return s_bk_save_active; }
static void booksTimerCb(lv_timer_t*) {
  // Reading and listening are long stretches without input: keep the idle timeout from
  // blanking and locking the page while Books is open. Locking by hand (hold Space) still works,
  // and once locked or dark this stops pushing, so the device sleeps as usual.
  if (s_bk_root && g_lv.task && !g_lv.task->isScreenOff() && !g_lv.task->isManualLock())
    g_lv.task->noteUserInput();
  if (s_bk_save_active) { booksSaveStep(); return; }
  if (s_bk_job == BkJob::None) return;
  if (!s_bk_job_done) {
    if (s_bk_job == BkJob::Download && s_bk_view == BkView::Server) {
      char st[64];
      // Short enough for the status line beside the buttons: "45% of 612 KB".
      if (s_bk_dl_total) snprintf(st, sizeof st, "%u%% %s %u KB",
                                  (unsigned)(s_bk_dl_got * 100ull / s_bk_dl_total), TR("of"), (unsigned)(s_bk_dl_total / 1024));
      else snprintf(st, sizeof st, "%u KB", (unsigned)(s_bk_dl_got / 1024));
      booksSetStatus(st);
    }
    return;
  }
  const BkJob job = s_bk_job;
  const bool ok = s_bk_job_ok;
  s_bk_job = BkJob::None;
  if (job == BkJob::Progress || job == BkJob::SessionSync) return;   // fire and forget
  if (job == BkJob::AudioProgress) {
    if (ok && s_bka_local) { s_bka_dirty = false; booksSaveAudioPos(); }
    return;
  }
  if (job == BkJob::AudioPull) {
    // The server's position wins when it is newer (listened elsewhere): only before
    // playback has moved on here.
    if (ok && s_bka_local && s_bka_pulled >= 0 && fabs(s_bka_pulled - s_bka_time) > 5 && !s_bka_dirty) {
      const bool was = booksAudioRunning();
      s_bka_time = s_bka_pulled;
      if (was) booksAudioPlayFrom(s_bka_time);
      booksPlayerRefresh();
    }
    return;
  }
  if (job == BkJob::OpenSession && ok) {
    s_bka_local = false;   // a streamed session now (a card copy had been loaded before)
    s_bka_dir[0] = 0;
    s_bka_dirty = false;
    s_bka_listened_ms = 0;
    s_bka_last_sync_ms = millis();
    if (s_bk_root) booksOpenPlayer();
    if (!booksAudioPlayFrom(s_bka_time) && g_lv.task) g_lv.task->showAlert(booksAudioError(), 2200);
    booksPlayerRefresh();
    return;
  }
  if (job == BkJob::Download && ok) { booksSaveBegin(); return; }
  if (!ok && job == BkJob::Login) s_abs_token[0] = 0;
  if (!ok && s_bk_job_msg[0] && s_bk_root) {
    if (strstr(s_bk_job_msg, "log in again")) { s_abs_token[0] = 0; booksSaveAbs(); }
    if (s_bk_view == BkView::Server) booksShowView(BkView::Server);
    booksSetStatus(s_bk_job_msg);
    if (g_lv.task) g_lv.task->showAlert(s_bk_job_msg, 2200);
    return;
  }
  if (s_bk_root && s_bk_view == BkView::Server) booksShowView(BkView::Server);
}

// =================================== the reader ===================================
static size_t bkFileRead(void* ctx, uint32_t off, uint8_t* buf, size_t len) {
  File* f = static_cast<File*>(ctx);
  if (!f->seek(off)) return 0;
  size_t got = 0;
  while (got < len) {
    const int n = f->read(buf + got, len - got);
    if (n <= 0) break;
    got += (size_t)n;
  }
  return got;
}

// Plain text: fixed windows, each cut at whitespace so a word or a UTF-8 character
// never splits; single line breaks (hard-wrapped files) are joined into paragraphs.
static char* booksTxtChunk(int i, size_t* len_out) {
  *len_out = 0;
  uint32_t start = (uint32_t)i * kBkTxtChunk;
  if (start >= s_bk_file_size) return nullptr;
  uint32_t end = start + kBkTxtChunk + 512;
  if (end > s_bk_file_size) end = s_bk_file_size;
  const size_t n = end - start;
  char* raw = (char*)bkAlloc(n + 1);
  if (!raw) return nullptr;
  const size_t got = bkFileRead(&s_bk_file, start, (uint8_t*)raw, n);
  raw[got] = 0;
  size_t a = 0, b = got;
  if (i > 0) { while (a < got && raw[a] != ' ' && raw[a] != '\n') ++a; }
  if (start + kBkTxtChunk < s_bk_file_size) {
    b = kBkTxtChunk < got ? kBkTxtChunk : got;
    while (b < got && raw[b] != ' ' && raw[b] != '\n') ++b;
  }
  char* out = (char*)bkAlloc(b - a + 1);
  if (!out) { bkFree(raw); return nullptr; }
  size_t w = 0;
  for (size_t k = a; k < b; ++k) {
    char c = raw[k];
    if (c == '\r') continue;
    if (c == '\n') {
      const bool para = (k + 1 < b && (raw[k + 1] == '\n' || raw[k + 1] == '\r')) || (w && out[w - 1] == '\n');
      if (!para) { if (w && out[w - 1] != ' ') out[w++] = ' '; continue; }
    }
    out[w++] = c;
  }
  out[w] = 0;
  bkFree(raw);
  *len_out = w;
  return out;
}

static void booksPaginate() {
  s_bk_page_count = 0;
  if (!s_bk_text) return;
  const lv_font_t* f = booksFont(s_bk_font);
  const lv_coord_t lh = lv_font_get_line_height(f) + kBkLineSpace;
  int lines_per_page = (s_bk_text_h + kBkLineSpace) / lh;
  if (lines_per_page < 1) lines_per_page = 1;
  uint32_t pos = 0;
  size_t longest = 0;
  while (pos < s_bk_text_len) {
    while (pos < s_bk_text_len && (s_bk_text[pos] == '\n' || s_bk_text[pos] == ' ')) ++pos;
    if (pos >= s_bk_text_len) break;
    if (s_bk_page_count + 1 >= s_bk_page_cap) {
      const int ncap = s_bk_page_cap ? s_bk_page_cap * 2 : 128;
      uint32_t* np = (uint32_t*)heap_caps_realloc(s_bk_pages, sizeof(uint32_t) * ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!np) break;
      s_bk_pages = np; s_bk_page_cap = ncap;
    }
    s_bk_pages[s_bk_page_count++] = pos;
    const uint32_t start = pos;
    for (int line = 0; line < lines_per_page && pos < s_bk_text_len; ++line) {
      uint32_t adv = _lv_txt_get_next_line(s_bk_text + pos, f, 0, s_bk_text_w, nullptr, LV_TEXT_FLAG_NONE);
      if (adv == 0) adv = 1;
      pos += adv;
    }
    if (pos - start > longest) longest = pos - start;
  }
  if (s_bk_page_count == 0) { s_bk_pages = s_bk_pages ? s_bk_pages : (uint32_t*)bkAlloc(sizeof(uint32_t) * 4); s_bk_page_cap = s_bk_page_cap ? s_bk_page_cap : 4; if (s_bk_pages) { s_bk_pages[0] = 0; s_bk_page_count = 1; } }
  if (s_bk_page_count < s_bk_page_cap) s_bk_pages[s_bk_page_count] = (uint32_t)s_bk_text_len;
  if (longest + 1 > s_bk_page_buf_cap) {
    bkFree(s_bk_page_buf);
    s_bk_page_buf_cap = longest + 64;
    s_bk_page_buf = (char*)bkAlloc(s_bk_page_buf_cap);
  }
}

static int booksPageForOffset(uint32_t off) {
  int p = 0;
  while (p + 1 < s_bk_page_count && s_bk_pages[p + 1] <= off) ++p;
  return p;
}

static bool booksLoadChapter(int ch) {
  if (s_bk_text) { bkFree(s_bk_text); s_bk_text = nullptr; }
  s_bk_text_len = 0;
  if (ch < 0 || ch >= s_bk_chapter_count) return false;
  size_t n = 0;
  s_bk_text = s_bk_is_txt ? booksTxtChunk(ch, &n) : s_bk_epub->chapterText(ch, &n);
  s_bk_text_len = n;
  s_bk_chapter = ch;
  if (!s_bk_text) return false;
  booksPaginate();
  return true;
}

static float booksProgress() {
  if (s_bk_chapter_count <= 0) return 0.f;
  const float in_ch = s_bk_page_count > 1 ? (float)s_bk_page / (float)(s_bk_page_count - 1) : 1.f;
  return ((float)s_bk_chapter + in_ch) / (float)s_bk_chapter_count;
}

static void booksRenderPage() {
  if (!s_bk_text_lbl || !s_bk_text || !s_bk_page_buf || s_bk_page_count == 0) return;
  if (s_bk_page >= s_bk_page_count) s_bk_page = s_bk_page_count - 1;
  if (s_bk_page < 0) s_bk_page = 0;
  const uint32_t a = s_bk_pages[s_bk_page];
  const uint32_t b = s_bk_pages[s_bk_page + 1 < s_bk_page_count ? s_bk_page + 1 : s_bk_page_count];
  size_t n = b > a ? b - a : 0;
  if (n + 1 > s_bk_page_buf_cap) n = s_bk_page_buf_cap - 1;
  memcpy(s_bk_page_buf, s_bk_text + a, n);
  while (n && (s_bk_page_buf[n - 1] == '\n' || s_bk_page_buf[n - 1] == ' ')) --n;
  s_bk_page_buf[n] = 0;
  lv_label_set_text(s_bk_text_lbl, s_bk_page_buf);
  if (s_bk_foot) {
    char f[64];
    snprintf(f, sizeof f, "%d/%d  \xc2\xb7  %d/%d  \xc2\xb7  %d%%", s_bk_chapter + 1, s_bk_chapter_count,
             s_bk_page + 1, s_bk_page_count, (int)(booksProgress() * 100.f + 0.5f));
    lv_label_set_text(s_bk_foot, f);
  }
}

static void booksRememberPosition() {
  if (s_bk_open < 0 || s_bk_open >= s_bk_count || s_bk_page_count == 0) return;
  BkBook& b = s_bk_books[s_bk_open];
  b.chapter = (uint16_t)s_bk_chapter;
  b.offset = s_bk_pages[s_bk_page];
  b.chapters = (uint16_t)s_bk_chapter_count;
  b.pct = (uint8_t)(booksProgress() * 100.f + 0.5f);
  b.seq = ++s_bk_seq;
  booksSaveIndex();
}

static void booksTurn(int dir) {
  if (!booksReaderOpen()) return;
  if (dir > 0) {
    if (s_bk_page + 1 < s_bk_page_count) ++s_bk_page;
    else if (s_bk_chapter + 1 < s_bk_chapter_count) { if (booksLoadChapter(s_bk_chapter + 1)) s_bk_page = 0; }
    else { if (g_lv.task) g_lv.task->showAlert(TR("The end"), 1000); return; }
  } else {
    if (s_bk_page > 0) --s_bk_page;
    else if (s_bk_chapter > 0) { if (booksLoadChapter(s_bk_chapter - 1)) s_bk_page = s_bk_page_count - 1; }
    else return;
  }
  booksRenderPage();
  static uint8_t turns = 0;
  if (++turns >= 5) { turns = 0; booksRememberPosition(); }   // a crash or power loss costs a few pages at most
}

static void booksCloseReader(bool to_shelf) {
  booksRememberPosition();
  if (s_bk_open >= 0 && s_bk_open < s_bk_count && s_bk_books[s_bk_open].item_id[0] && s_abs_token[0] &&
      WiFi.status() == WL_CONNECTED && s_bk_job == BkJob::None) {
    bkCopy(s_bk_job_arg, sizeof s_bk_job_arg, s_bk_books[s_bk_open].item_id);
    s_bk_job_progress = booksProgress();
    booksStartJob(BkJob::Progress);   // reading position back to the server
  }
  if (s_bk_epub) { s_bk_epub->close(); delete s_bk_epub; s_bk_epub = nullptr; }
  if (s_bk_file) s_bk_file.close();
  s_bk_sd_busy = false;
  if (s_bk_text) { bkFree(s_bk_text); s_bk_text = nullptr; }
  s_bk_text_len = 0; s_bk_page_count = 0; s_bk_open = -1;
  s_bk_text_lbl = s_bk_foot = s_bk_menu = nullptr;
  if (to_shelf) booksShowView(BkView::Shelf);
}

static void booksMenuClose() {
  if (s_bk_menu && lv_obj_is_valid(s_bk_menu)) lv_obj_del(s_bk_menu);
  s_bk_menu = nullptr;
}

// Text size: kept for every book (in the shelf index), the reading position held.
static void booksSetFont(int nf) {
  if (nf < 0 || nf >= kBkFontCount || nf == s_bk_font || !s_bk_text_lbl) return;
  const uint32_t off = s_bk_page_count ? s_bk_pages[s_bk_page] : 0;
  s_bk_font = nf;
  lv_obj_set_style_text_font(s_bk_text_lbl, booksFont(s_bk_font), LV_PART_MAIN);
  booksPaginate();
  s_bk_page = booksPageForOffset(off);
  booksRenderPage();
  booksSaveIndex();
}

static void booksSizeBtnCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  booksSetFont(s_bk_font + (int)(intptr_t)lv_event_get_user_data(e));
}

static void booksMenuCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  const int act = (int)(intptr_t)lv_event_get_user_data(e);
  if (act == 1 || act == 2) { booksSetFont(s_bk_font + (act == 2 ? 1 : -1)); return; }
  if (act == 3 || act == 4) {   // previous / next chapter
    const int ch = s_bk_chapter + (act == 4 ? 1 : -1);
    if (ch >= 0 && ch < s_bk_chapter_count && booksLoadChapter(ch)) { s_bk_page = 0; booksRenderPage(); }
    return;
  }
  booksMenuClose();
  if (act == 5) booksCloseReader(true);
}

static void booksMenuOpen() {
  if (s_bk_menu) { booksMenuClose(); return; }
  const lv_coord_t w = s_bk_body_w;
  s_bk_menu = lv_obj_create(s_bk_root);
  lv_obj_remove_style_all(s_bk_menu);
  lv_obj_set_size(s_bk_menu, w, 84);
  lv_obj_align(s_bk_menu, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(s_bk_menu, lv_color_hex(COLOR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_bk_menu, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_bk_menu, 2, LV_PART_MAIN);
  lv_obj_set_style_border_side(s_bk_menu, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
  lv_obj_set_style_border_color(s_bk_menu, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
  lv_obj_add_flag(s_bk_menu, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(s_bk_menu, LV_OBJ_FLAG_SCROLLABLE);
  const lv_coord_t bw = (w - 6 * 6) / 5;
  static const char* const kLabels[] = { "A-", "A+", LV_SYMBOL_PREV, LV_SYMBOL_NEXT, LV_SYMBOL_LIST };
  for (int i = 0; i < 5; ++i) {
    lv_obj_t* b = booksButton(s_bk_menu, kLabels[i], booksMenuCb, (void*)(intptr_t)(i + 1), bw, 34);
    lv_obj_set_pos(b, 6 + i * (bw + 6), 8);
  }
  lv_obj_t* hint = booksLabel(s_bk_menu, TR("Text size \xc2\xb7 chapter \xc2\xb7 back to the shelf"), &g_font_12, COLOR_SUB);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void booksPageTapCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !booksReaderOpen()) return;
  lv_indev_t* indev = lv_indev_get_act();
  if (!indev) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);
  if (s_bk_menu) { booksMenuClose(); return; }
  const lv_coord_t w = s_bk_body_w;
  if (p.x < w / 3) booksTurn(-1);
  else if (p.x > w * 2 / 3) booksTurn(1);
  else booksMenuOpen();
}

// Keyboard: n / j / Enter / right / down turn forward; b / p / k / Backspace /
// left / up turn back; + and - change the size; m opens the menu.
static void booksPlayerBtnCb(lv_event_t* e);
static bool booksReaderKey(int key) {
  if (s_bk_root && s_bk_view == BkView::Player) {   // the player: volume and seeking
    switch (key) {
      case '+': case '=': booksVolumeStep(10); return true;
      case '-': booksVolumeStep(-10); return true;
      default: return false;
    }
  }
  if (!booksReaderOpen()) return false;
  switch (key) {
    // Not Space: holding it locks the screen, and its press would turn a page first.
    case 'n': case 'N': case 'j': case 'J': case LV_KEY_ENTER: case '\r':
    case LV_KEY_RIGHT: case LV_KEY_DOWN:
      booksTurn(1); return true;
    case 'b': case 'B': case 'p': case 'P': case 'k': case 'K': case LV_KEY_BACKSPACE:
    case LV_KEY_LEFT: case LV_KEY_UP:
      booksTurn(-1); return true;
    case '+': case '=':
      booksSetFont(s_bk_font + 1); return true;
    case '-':
      booksSetFont(s_bk_font - 1); return true;
    case 'm': case 'M':
      booksMenuOpen(); return true;
    default:
      return false;
  }
}

static void booksBuildReader() {
  const lv_coord_t w = s_bk_body_w, h = s_bk_body_h;
  const lv_coord_t foot_h = 26;   // the size buttons and the position line
  s_bk_text_w = w - 16;
  s_bk_text_h = h - foot_h - 8;
  s_bk_text_lbl = lv_label_create(s_bk_body);
  lv_label_set_long_mode(s_bk_text_lbl, LV_LABEL_LONG_WRAP);   // pages are cut to fit exactly
  lv_obj_set_size(s_bk_text_lbl, s_bk_text_w, s_bk_text_h);
  lv_obj_set_pos(s_bk_text_lbl, 8, 4);
  lv_obj_set_style_text_font(s_bk_text_lbl, booksFont(s_bk_font), LV_PART_MAIN);
  lv_obj_set_style_text_color(s_bk_text_lbl, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
  lv_obj_set_style_text_line_space(s_bk_text_lbl, kBkLineSpace, LV_PART_MAIN);
  lv_obj_clear_flag(s_bk_text_lbl, LV_OBJ_FLAG_CLICKABLE);
  s_bk_foot = booksLabel(s_bk_body, "", &g_font_12, COLOR_SUB);
  lv_obj_align(s_bk_foot, LV_ALIGN_BOTTOM_MID, 0, -6);
  // Text size, one tap away at the bottom corners.
  lv_obj_t* smaller = booksButton(s_bk_body, "A-", booksSizeBtnCb, (void*)(intptr_t)-1, 38, 22);
  lv_obj_align(smaller, LV_ALIGN_BOTTOM_LEFT, 4, -2);
  lv_obj_t* bigger = booksButton(s_bk_body, "A+", booksSizeBtnCb, (void*)(intptr_t)1, 38, 22);
  lv_obj_align(bigger, LV_ALIGN_BOTTOM_RIGHT, -4, -2);
  lv_obj_add_flag(s_bk_body, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_bk_body, booksPageTapCb, LV_EVENT_CLICKED, nullptr);
}

static void booksOpenReader(int idx) {
  if (idx < 0 || idx >= s_bk_count) return;
  BkBook& b = s_bk_books[idx];
  fs::FS* fs = booksFs();
  if (!fs) { if (g_lv.task) g_lv.task->showAlert(TR("Insert the SD card with your books"), 1800); return; }
  s_bk_sd_busy = true;   // until the reader closes the book
  s_bk_file = fs->open(b.file, FILE_READ);
  if (!s_bk_file) {
    s_bk_sd_busy = false;
    if (g_lv.task) g_lv.task->showAlert(TR("Cannot open this book"), 1600);
    return;
  }
  s_bk_file_size = (uint32_t)s_bk_file.size();
  const char* dot = strrchr(b.file, '.');
  s_bk_is_txt = dot && strcasecmp(dot, ".txt") == 0;
  if (s_bk_is_txt) {
    s_bk_chapter_count = (int)((s_bk_file_size + kBkTxtChunk - 1) / kBkTxtChunk);
  } else {
    s_bk_epub = new EpubBook::Book();
    if (!s_bk_epub->open(bkFileRead, &s_bk_file, s_bk_file_size, kBkHooks)) {
      delete s_bk_epub; s_bk_epub = nullptr; s_bk_file.close();
      s_bk_sd_busy = false;
      if (g_lv.task) g_lv.task->showAlert(TR("This EPUB could not be read"), 1800);
      return;
    }
    s_bk_chapter_count = s_bk_epub->chapterCount();
    if (s_bk_epub->title()[0]) bkCopy(b.title, sizeof b.title, s_bk_epub->title());
    if (s_bk_epub->author()[0]) bkCopy(b.author, sizeof b.author, s_bk_epub->author());
  }
  if (s_bk_chapter_count <= 0) { booksCloseReader(false); return; }
  s_bk_open = idx;
  snprintf(s_bk_page_title, sizeof s_bk_page_title, "%s", b.title[0] ? b.title : "Books");
  booksShowView(BkView::Reader);
  int ch = b.chapter < s_bk_chapter_count ? b.chapter : 0;
  if (!booksLoadChapter(ch) && !booksLoadChapter(0)) {
    if (g_lv.task) g_lv.task->showAlert(TR("This chapter could not be read"), 1800);
  }
  s_bk_page = booksPageForOffset(b.offset);
  booksRenderPage();
  booksRememberPosition();
}

// ================================== audiobooks ==================================
// The player page: title, chapter, position, and five buttons. Playback runs on its
// own task (BooksAudio.inc) and goes on with the page closed or the screen off; the
// position goes back to the server every minute and on pause.
static void fmtClock(char* out, size_t cap, double s) {
  if (s < 0) s = 0;
  const unsigned t = (unsigned)s;
  if (t >= 3600) snprintf(out, cap, "%u:%02u:%02u", t / 3600, (t / 60) % 60, t % 60);
  else           snprintf(out, cap, "%u:%02u", t / 60, t % 60);
}

static int booksAudioChapterAt(double t) {
  for (int i = s_bka_chapter_count - 1; i >= 0; --i) if (t >= s_bka_chapters[i].start - 0.5) return i;
  return s_bka_chapter_count ? 0 : -1;
}

static double booksAudioNow() { return booksAudioRunning() ? booksAudioPos() : s_bka_time; }

static void booksAudioSyncSoon(bool close) {
  if (s_bka_local || !s_bka_session[0]) {   // the user's media progress (card copy: also kept there)
    s_bka_time = booksAudioNow();
    if (WiFi.status() != WL_CONNECTED || s_bk_job != BkJob::None || !s_abs_token[0]) {
      s_bka_dirty = true;
      booksSaveAudioPos();
      return;
    }
    bkCopy(s_bk_job_arg, sizeof s_bk_job_arg, s_bka_item);
    s_bka_job_time = s_bka_time;
    s_bka_dirty = true;
    booksSaveAudioPos();
    if (booksStartJob(BkJob::AudioProgress)) { s_bka_listened_ms %= 1000; s_bka_last_sync_ms = millis(); }
    return;
  }
  if (!s_bka_session[0] || s_bk_job != BkJob::None) return;
  if (WiFi.status() != WL_CONNECTED) return;
  s_bka_sync_time = booksAudioNow();
  s_bka_sync_listened = s_bka_listened_ms / 1000;
  s_bka_sync_close = close;
  if (booksStartJob(BkJob::SessionSync)) { s_bka_listened_ms %= 1000; s_bka_last_sync_ms = millis(); }
}

static void booksPlayerRefresh() {
  if (!s_bka_lbl_time || s_bk_view != BkView::Player) return;
  const double now = booksAudioNow();
  char a[16], b[16], line[48];
  fmtClock(a, sizeof a, now);
  fmtClock(b, sizeof b, s_bka_duration);
  snprintf(line, sizeof line, "%s / %s", a, b);
  lv_label_set_text(s_bka_lbl_time, line);
  lv_bar_set_value(s_bka_bar, s_bka_duration > 0 ? (int32_t)(now * 1000.0 / s_bka_duration) : 0, LV_ANIM_OFF);
  const int ch = booksAudioChapterAt(now);
  char chap[96];
  if (ch >= 0) snprintf(chap, sizeof chap, "%d/%d  %s", ch + 1, s_bka_chapter_count, s_bka_chapters[ch].title);
  else chap[0] = 0;
  lv_label_set_text(s_bka_lbl_chapter, chap);
  const char* err = booksAudioError();
  const char* st = err[0] ? err
                 : booksAudioBuffering() ? TR("Buffering\xe2\x80\xa6")
                 : booksAudioRunning() ? TR("Playing")
                 : booksAudioEnded() ? TR("The end")
                 : TR("Paused");
  lv_label_set_text(s_bka_lbl_state, st);
  lv_label_set_text(s_bka_play_lbl, booksAudioRunning() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
  if (s_bka_dl_lbl) {
    char dl[64];
    if (s_bka_local || booksFindAudio(s_bka_item) >= 0) snprintf(dl, sizeof dl, "%s", TR("On the SD card: plays offline"));
    else if (s_bkd_active && strcmp(s_bkd_item, s_bka_item) == 0) {
      if (s_bk_dl_total) snprintf(dl, sizeof dl, "%s %d/%d  %u%%  (%s)", TR("Saving"), s_bkd_track + 1, s_bkd_tracks,
                                  (unsigned)(s_bk_dl_got * 100ull / s_bk_dl_total), TR("tap to stop"));
      else snprintf(dl, sizeof dl, "%s %d/%d  (%s)", TR("Saving"), s_bkd_track + 1, s_bkd_tracks, TR("tap to stop"));
    } else snprintf(dl, sizeof dl, "%s", TR("Download for offline listening"));
    lv_label_set_text(s_bka_dl_lbl, dl);
  }
}

static bool s_bka_user_stopped = false;   // the stop was ours: no second sync for it
static void booksAudioPause() {
  if (!booksAudioRunning()) return;
  s_bka_time = booksAudioPos();
  booksAudioStop();
  s_bka_user_stopped = true;
  booksAudioSyncSoon(false);
}

static void booksAudioSeekTo(double t) {
  if (t < 0) t = 0;
  if (s_bka_duration > 0 && t > s_bka_duration - 1) t = s_bka_duration - 1;
  s_bka_time = t;
  if (booksAudioRunning()) booksAudioPlayFrom(t);
  booksPlayerRefresh();
}

static void booksPlayerBtnCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  const int act = (int)(intptr_t)lv_event_get_user_data(e);
  const double now = booksAudioNow();
  switch (act) {
    case 0: {   // chapter back: to its start, or the one before when just past it
      int ch = booksAudioChapterAt(now);
      if (ch > 0 && now - s_bka_chapters[ch].start < 3) --ch;
      booksAudioSeekTo(ch >= 0 ? s_bka_chapters[ch].start : 0);
      break;
    }
    case 1: booksAudioSeekTo(now - 30); break;
    case 2:
      if (booksAudioRunning()) booksAudioPause();
      else {
        if (s_bka_duration > 0 && s_bka_time >= s_bka_duration - 1) s_bka_time = 0;   // finished: again from the start
        // Downloaded while it streamed: from now on from the card.
        const int li = s_bka_local ? -1 : booksFindAudio(s_bka_item);
        if (li >= 0 && !s_bkd_active) { const double t = s_bka_time; if (booksLoadLocalAudio(li)) s_bka_time = t; }
        if (!booksAudioPlayFrom(s_bka_time) && g_lv.task) g_lv.task->showAlert(booksAudioError(), 2000);
      }
      break;
    case 3: booksAudioSeekTo(now + 30); break;
    case 4: {
      const int ch = booksAudioChapterAt(now);
      if (ch >= 0 && ch + 1 < s_bka_chapter_count) booksAudioSeekTo(s_bka_chapters[ch + 1].start);
      break;
    }
  }
  booksPlayerRefresh();
}

// Volume, right in the player: the system volume (as in the quick panel), applied live.
static void booksVolumeStep(int d) {
  int v = (int)touchPrefsGetSoundVolume() + d;
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  touchPrefsSetSoundVolume((uint8_t)v);
  if (booksAudioRunning()) maxSetVolume(v);
  if (s_bka_vol_lbl) lv_label_set_text_fmt(s_bka_vol_lbl, "%d%%", v);
}
static void booksVolumeBtnCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  booksVolumeStep((int)(intptr_t)lv_event_get_user_data(e));
}

// Download for offline / stop the download; on a card copy it only says so.
static void booksDownloadBtnCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (s_bka_local) return;
  if (s_bkd_active) { s_bkd_cancel = true; return; }
  booksAudioDownloadStart();
  booksPlayerRefresh();
}

static void booksBuildPlayer() {
  const lv_coord_t w = s_bk_body_w;
  lv_obj_t* t = booksLabel(s_bk_body, s_bka_title, &g_font_semi_16, COLOR_TEXT);
  lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(t, w - 20);
  lv_obj_set_pos(t, 10, 8);
  lv_obj_t* a = booksLabel(s_bk_body, s_bka_author, &g_font_14, COLOR_SUB);
  lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
  lv_obj_set_width(a, w - 20);
  lv_obj_align_to(a, t, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);
  s_bka_lbl_chapter = booksLabel(s_bk_body, "", &g_font_14, COLOR_TEXT);
  lv_label_set_long_mode(s_bka_lbl_chapter, LV_LABEL_LONG_DOT);
  lv_obj_set_width(s_bka_lbl_chapter, w - 20);
  lv_obj_set_pos(s_bka_lbl_chapter, 10, 104);
  s_bka_bar = lv_bar_create(s_bk_body);
  lv_obj_set_size(s_bka_bar, w - 20, 8);
  lv_obj_set_pos(s_bka_bar, 10, 132);
  lv_bar_set_range(s_bka_bar, 0, 1000);
  lv_obj_set_style_bg_color(s_bka_bar, lv_color_hex(COLOR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_bka_bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_bka_bar, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(s_bka_bar, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_bka_bar, lv_color_hex(COLOR_TEXT), LV_PART_INDICATOR);
  s_bka_lbl_time = booksLabel(s_bk_body, "", &g_font_16, COLOR_TEXT);
  lv_obj_set_pos(s_bka_lbl_time, 10, 146);
  // Volume on the time row's right: [-] 60% [+]
  lv_obj_t* vp = booksButton(s_bk_body, "+", booksVolumeBtnCb, (void*)(intptr_t)10, 30, 26);
  lv_obj_set_pos(vp, w - 36, 144);
  s_bka_vol_lbl = booksLabel(s_bk_body, "", &g_font_12, COLOR_TEXT);
  lv_obj_set_width(s_bka_vol_lbl, 36);
  lv_obj_set_style_text_align(s_bka_vol_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_pos(s_bka_vol_lbl, w - 76, 150);
  lv_label_set_text_fmt(s_bka_vol_lbl, "%d%%", (int)touchPrefsGetSoundVolume());
  lv_obj_t* vm = booksButton(s_bk_body, "-", booksVolumeBtnCb, (void*)(intptr_t)-10, 30, 26);
  lv_obj_set_pos(vm, w - 110, 144);
  s_bka_lbl_state = booksLabel(s_bk_body, "", &g_font_12, COLOR_SUB);
  lv_label_set_long_mode(s_bka_lbl_state, LV_LABEL_LONG_DOT);
  lv_obj_set_width(s_bka_lbl_state, w - 20);
  lv_obj_set_pos(s_bka_lbl_state, 10, 170);
  static const char* const kLabels[] = { LV_SYMBOL_PREV, "-30", LV_SYMBOL_PLAY, "+30", LV_SYMBOL_NEXT };
  const lv_coord_t bw = (w - 6 * 6) / 5;
  for (int i = 0; i < 5; ++i) {
    lv_obj_t* b = booksButton(s_bk_body, kLabels[i], booksPlayerBtnCb, (void*)(intptr_t)i, bw, 44);
    lv_obj_set_pos(b, 6 + i * (bw + 6), 184);
    if (i == 2) s_bka_play_lbl = lv_obj_get_child(b, 0);
  }
  lv_obj_t* dl = booksButton(s_bk_body, "", booksDownloadBtnCb, nullptr, w - 12, 30);
  lv_obj_set_pos(dl, 6, 238);
  s_bka_dl_lbl = lv_obj_get_child(dl, 0);
  lv_obj_set_style_text_font(s_bka_dl_lbl, &g_font_12, LV_PART_MAIN);
  booksPlayerRefresh();
}

// Every second while a session is loaded: listening time, the minute sync, the end of
// the book; the page itself repaints only every 10 s (each repaint is a panel update).
static void booksAudioTimerCb(lv_timer_t*) {
  static uint32_t s_last = 0, s_last_paint = 0;
  const uint32_t now = millis();
  const uint32_t dt = s_last ? now - s_last : 0;
  s_last = now;
  static bool s_was_running = false;
  // With the Books page closed nobody collects worker results: a finished sync
  // would otherwise hold the worker and refuse the next one.
  if (!s_bk_timer && s_bk_job_done && (s_bk_job == BkJob::SessionSync || s_bk_job == BkJob::Progress))
    s_bk_job = BkJob::None;
  const bool running = booksAudioRunning();
  if (running && !booksAudioBuffering()) s_bka_listened_ms += dt;
  if (running) s_bka_time = booksAudioPos();
  if (running && now - s_bka_last_sync_ms >= 60000u) booksAudioSyncSoon(false);
  if (s_bkd_done) {   // an offline download finished (or stopped)
    s_bkd_done = false;
    if (s_bkd_file) s_bkd_file.close();
    s_bk_sd_busy = false;
    if (s_bkd_timer) { lv_timer_del(s_bkd_timer); s_bkd_timer = nullptr; }
    if (g_lv.task) g_lv.task->showAlert(s_bkd_msg, 2200);
    if (s_bkd_ok) {
      booksScanAudio();
      // The loaded book is now on the card: switch to it while it is not playing.
      const int i = booksFindAudio(s_bkd_item);
      if (i >= 0 && !running && strcmp(s_bka_item, s_bkd_item) == 0) {
        const double t = s_bka_time;
        if (booksLoadLocalAudio(i)) s_bka_time = t;
      }
    }
    s_last_paint = 0;
  }
  if (s_bkd_active && s_bk_root && s_bk_view == BkView::Player && now - s_last_paint >= 3000u) s_last_paint = 0;
  if (s_was_running && !running) {   // ended or failed on its own (a pause synced already)
    if (booksAudioEnded()) s_bka_time = s_bka_duration;
    if (!s_bka_user_stopped) booksAudioSyncSoon(false);
    s_last_paint = 0;
  }
  if (running) s_bka_user_stopped = false;
  s_was_running = running;
  // Waiting for data shows at once (the panel said "Playing" through a silent stall).
  static bool s_was_buffering = false;
  const bool buffering = booksAudioBuffering();
  if (buffering != s_was_buffering) { s_was_buffering = buffering; s_last_paint = 0; }
  if (s_bk_root && s_bk_view == BkView::Player && (s_last_paint == 0 || now - s_last_paint >= 10000u)) {
    s_last_paint = now;
    booksPlayerRefresh();
  }
}

static void booksOpenPlayer() {
  if (!s_bka_timer) s_bka_timer = lv_timer_create(booksAudioTimerCb, 1000, nullptr);
  booksShowView(BkView::Player);
}

static void booksNowPlayingCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
  booksOpenPlayer();
}

// A different audiobook replaces the loaded session.
static void booksStartAudiobook(const BkAbsItem& it) {
  if (s_bka_item[0] && strcmp(s_bka_item, it.id) == 0) { booksOpenPlayer(); return; }
  if (s_bkd_active) {   // the download reads the loaded session's tracks
    if (g_lv.task) g_lv.task->showAlert(TR("An audiobook is still downloading"), 1600);
    return;
  }
  booksScanAudio();
  const int local = booksFindAudio(it.id);
  if (local >= 0) {   // downloaded: play the card copy
    booksOpenLocalAudio(local);
    return;
  }
  if (booksAudioRunning()) { s_bka_time = booksAudioPos(); booksAudioStop(); }
  // The old session is left to the server, which ends idle sessions on its own.
  if (s_bk_job != BkJob::None) {
    if (g_lv.task) g_lv.task->showAlert(TR("One moment, still talking to the server"), 1500);
    return;
  }
  bkCopy(s_bk_job_arg, sizeof s_bk_job_arg, it.id);
  bkCopy(s_bk_job_arg2, sizeof s_bk_job_arg2, it.title);
  if (booksStartJob(BkJob::OpenSession)) booksSetStatus(TR("Opening\xe2\x80\xa6"));
}

// ================================ view switching ================================
static void booksShowView(BkView v) {
  if (!s_bk_root) return;
  booksMenuClose();
  if (s_bk_body) { lv_obj_del(s_bk_body); s_bk_body = nullptr; }
  s_bk_status = s_bk_user_ta = s_bk_pass_ta = s_bk_server_ta = nullptr;
  s_bk_text_lbl = s_bk_foot = nullptr;
  s_bka_lbl_time = s_bka_lbl_chapter = s_bka_lbl_state = s_bka_bar = s_bka_play_lbl = s_bka_dl_lbl = nullptr;
  s_bka_vol_lbl = nullptr;
  s_bk_view = v;
  // The bar first: the reader slims it, the shelf has the tall "< Books" row.
  if (v == BkView::Reader) appPageBeginSlim(s_bk_page_title, booksClosePage);
  else                     appPageBegin("Books", booksClosePage);
  lv_coord_t top = statusBarCurH() - STATUSBAR_H;
  if (top < 0) top = 0;
  s_bk_body = lv_obj_create(s_bk_root);
  lv_obj_remove_style_all(s_bk_body);
  s_bk_body_w = lv_disp_get_hor_res(nullptr);
  s_bk_body_h = lv_disp_get_ver_res(nullptr) - STATUSBAR_H - top;
  lv_obj_set_size(s_bk_body, s_bk_body_w, s_bk_body_h);
  lv_obj_set_pos(s_bk_body, 0, top);
  lv_obj_clear_flag(s_bk_body, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_update_layout(s_bk_body);
  if (v == BkView::Reader) booksBuildReader();
  else if (v == BkView::Player) booksBuildPlayer();
  else if (v == BkView::Server) booksBuildServer();
  else booksBuildShelf();
  if (g_statusbar.root) lv_obj_move_foreground(g_statusbar.root);
}

static void openBooksPage() {
  if (s_bk_root) return;
  booksLoadIndex();
  booksLoadAbs();
  s_abs_lib_count = s_abs_item_count = 0;   // fresh from the server on every open
  s_abs_page = 0;
  appPageBegin("Books", booksClosePage);
  s_bk_root = appPageCreateRoot(COLOR_BG);
  // The reader slims the status bar: size the root for the larger content area.
  lv_obj_set_pos(s_bk_root, 0, STATUSBAR_H);
  lv_obj_set_height(s_bk_root, lv_disp_get_ver_res(nullptr) - STATUSBAR_H);
  lv_obj_update_layout(s_bk_root);
  if (!s_bk_timer) s_bk_timer = lv_timer_create(booksTimerCb, 250, nullptr);
  booksShowView(BkView::Shelf);
}
#endif  // HAS_TDECK_MAX books
