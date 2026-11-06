// SPDX-License-Identifier: GPL-3.0-or-later
// ============================ Books: e-reader (T-Deck Max) ============================
// Included by UITask.cpp (it uses the page chrome, fonts, palette and keyboard plumbing
// defined there). Two halves:
//   * the shelf: books on the device (SPIFFS /bk), EPUB and plain text, with the
//     reading position of each, and an Audiobookshelf client that lists a server's
//     libraries and downloads an item's EPUB onto the shelf;
//   * the reader: one chapter at a time as plain text (EpubBook.h), cut into pages
//     that fit the screen exactly, turned by tapping the left or right edge, the
//     keyboard (space / n / b, arrows) or the right front pad.
// Network and download work runs on a short-lived core-0 worker; the UI polls it.
// Audiobooks are listed but not played yet (the audio player is the next step).
#if defined(HAS_TDECK_MAX) && defined(ESP32) && defined(MULTI_TRANSPORT_COMPANION)
#include "EpubBook.h"
#include <ArduinoJson.h>
#define CAP_BOOKS 1

static const char kBkDir[]       = "/bk";
static const char kBkIndex[]     = "/bk/index.json";
static const char kBkAbsConfig[] = "/bk/abs.json";
static const int  kBkMaxBooks    = 40;
static const int  kBkMaxItems    = 50;     // one page of a server library
static const int  kBkMaxLibs     = 12;
static const size_t kBkTxtChunk  = 24 * 1024;
static const size_t kBkJsonCap   = 768 * 1024;

struct BkBook {
  char file[32];        // "/bk/b1a2b3c4.epub"
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
enum class BkView : uint8_t { Shelf, Server, Reader };
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
enum class BkJob : uint8_t { None, Login, Libraries, Items, Download, Progress };
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
  SPIFFS.mkdir(kBkDir);
  File f = SPIFFS.open(kBkIndex, FILE_WRITE);
  if (!f) return;
  serializeJson(doc, f);
  f.close();
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
  File f = SPIFFS.open(kBkIndex, FILE_READ);
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
        if (!b.file[0] || !SPIFFS.exists(b.file)) continue;
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
  // Untracked book files. A subdirectory listing works on the host; on the device's
  // flat SPIFFS the root listing carries the "/bk/" prefix in each path.
  auto consider = [&](const char* path) {
    if (s_bk_count >= kBkMaxBooks || !booksIsBookName(path)) return;
    char full[40];
    if (strncmp(path, "/bk/", 4) == 0) bkCopy(full, sizeof full, path);
    else if (path[0] != '/') snprintf(full, sizeof full, "/bk/%s", path);
    else return;
    if (booksHasFile(full)) return;
    BkBook& b = s_bk_books[s_bk_count++];
    memset(&b, 0, sizeof b);
    bkCopy(b.file, sizeof b.file, full);
    EpubBook::titleFromFileName(full, b.title, sizeof b.title);
  };
  File dir = SPIFFS.open(kBkDir);
  if (dir && dir.isDirectory()) {
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
      if (!e.isDirectory()) {
        const char* p = e.path();
        consider(strncmp(p, "/bk/", 4) == 0 ? p : e.name());
      }
      e.close();
    }
    dir.close();
  } else {
    if (dir) dir.close();
    File root = SPIFFS.open("/");
    if (root) {
      for (File e = root.openNextFile(); e; e = root.openNextFile()) {
        if (strstr(e.path(), "/bk/")) consider(strstr(e.path(), "/bk/"));
        e.close();
      }
      root.close();
    }
  }
}

static void booksSaveAbs() {
  JsonDocument doc(&s_bk_json_alloc);
  doc["server"] = s_abs_server; doc["user"] = s_abs_user;
  doc["token"] = s_abs_token; doc["refresh"] = s_abs_refresh; doc["lib"] = s_abs_lib;
  SPIFFS.mkdir(kBkDir);
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
    cap = size > 0 ? (size_t)size + 1 : 64 * 1024;
    if (cap > kBkJsonCap) cap = kBkJsonCap;
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
        if (cap >= kBkJsonCap) break;
        size_t ncap = cap * 2 > kBkJsonCap ? kBkJsonCap : cap * 2;
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

static void bkJobDownload() {
  char file[32];
  uint32_t h = 2166136261u;   // FNV-1a of the item id: a stable, short SPIFFS name
  for (const char* p = s_bk_job_arg; *p; ++p) { h ^= (uint8_t)*p; h *= 16777619u; }
  snprintf(file, sizeof file, "/bk/b%08lx.epub", (unsigned long)h);
  char path[120];
  snprintf(path, sizeof path, "/api/items/%s/ebook", s_bk_job_arg);
  SPIFFS.mkdir(kBkDir);
  File f = SPIFFS.open(file, FILE_WRITE);
  if (!f) { snprintf(s_bk_job_msg, sizeof s_bk_job_msg, "Cannot write to the device storage"); return; }
  const int code = bkAuthed("GET", path, nullptr, nullptr, nullptr, &f);
  f.flush();
  const size_t written = s_bk_dl_got;   // File::size() of a file still open for writing reads 0 on some VFS
  f.close();
  if (code != 200 || written == 0) {
    SPIFFS.remove(file);
    if (code == HTTPC_ERROR_STREAM_WRITE) snprintf(s_bk_job_msg, sizeof s_bk_job_msg, "Device storage is full");
    else bkJobMsgForCode(code);
    return;
  }
  bkCopy(s_bk_job_msg, sizeof s_bk_job_msg, file);   // the UI adds it to the shelf
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

static void booksTaskFn(void*) {
  switch (s_bk_job) {
    case BkJob::Login:     bkJobLogin();     break;
    case BkJob::Libraries: bkJobLibraries(); break;
    case BkJob::Items:     bkJobItems();     break;
    case BkJob::Download:  bkJobDownload();  break;
    case BkJob::Progress:  bkJobProgress();  break;
    default: break;
  }
  s_bk_job_done = true;
  s_bk_task = nullptr;
  vTaskDelete(nullptr);
}

static bool booksStartJob(BkJob job) {
  if (s_bk_job != BkJob::None) return false;
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

// ================================= page chrome =================================
static void booksShowView(BkView v);
static void booksCloseReader(bool to_shelf);

static void booksClosePage() {
  if (s_bk_view == BkView::Reader) booksCloseReader(false);
  if (s_bk_timer) { lv_timer_del(s_bk_timer); s_bk_timer = nullptr; }
  appPageEnd(booksClosePage);
  if (s_bk_root) { appPageDeleteRootAsync(s_bk_root); s_bk_root = nullptr; }
  s_bk_body = s_bk_status = s_bk_user_ta = s_bk_pass_ta = s_bk_server_ta = nullptr;
  s_bk_text_lbl = s_bk_foot = s_bk_menu = nullptr;
}

static bool booksReaderOpen() { return s_bk_root && s_bk_view == BkView::Reader && s_bk_text_lbl; }

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
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, (void*)ud);
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
  lv_obj_t* a = booksButton(body, TR("On device"), booksTabCb, (void*)(intptr_t)BkView::Shelf, bw, 30);
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
  SPIFFS.remove(s_bk_books[i].file);
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
  if (lv_event_get_code(e) == LV_EVENT_CLICKED) booksOpenReader(idx);
}

static void booksBuildShelf() {
  booksTabs(s_bk_body, BkView::Shelf);
  if (s_bk_count == 0) {
    lv_obj_t* l = booksLabel(s_bk_body,
        TR("No books yet.\n\nGet them from your Audiobookshelf server (the tab above), "
           "or copy .epub and .txt files to the folder /bk on the device."),
        &g_font_14, COLOR_SUB);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, s_bk_body_w - 24);
    lv_obj_set_pos(l, 12, 50);
    return;
  }
  lv_obj_t* list = booksList(s_bk_body, 40);
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

static void booksLibCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
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
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= s_abs_item_count) return;
  const BkAbsItem& it = s_abs_items[i];
  const int local = booksFindItem(it.id);
  if (local >= 0) { booksOpenReader(local); return; }
  if (strcasecmp(it.fmt, "epub") != 0) {
    if (g_lv.task) g_lv.task->showAlert(it.fmt[0] ? TR("Only EPUB books can be read here for now")
                                                  : TR("Audiobooks are coming in the next version"), 2200);
    return;
  }
  if (s_bk_count >= kBkMaxBooks) { if (g_lv.task) g_lv.task->showAlert(TR("The shelf is full"), 1600); return; }
  bkCopy(s_bk_job_arg, sizeof s_bk_job_arg, it.id);
  bkCopy(s_bk_job_arg2, sizeof s_bk_job_arg2, it.title);
  if (booksStartJob(BkJob::Download)) booksSetStatus(TR("Downloading\xe2\x80\xa6"));
}

static void booksBuildServer() {
  booksTabs(s_bk_body, BkView::Server);
  const lv_coord_t w = s_bk_body_w;
  s_bk_status = booksLabel(s_bk_body, "", &g_font_12, COLOR_SUB);
  lv_label_set_long_mode(s_bk_status, LV_LABEL_LONG_DOT);
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
  lv_obj_set_width(s_bk_status, w - 96);
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

static void booksTimerCb(lv_timer_t*) {
  if (s_bk_job == BkJob::None) return;
  if (!s_bk_job_done) {
    if (s_bk_job == BkJob::Download && s_bk_view == BkView::Server) {
      char st[64];
      if (s_bk_dl_total) snprintf(st, sizeof st, "%s %u%% (%u KB)", TR("Downloading"),
                                  (unsigned)(s_bk_dl_got * 100ull / s_bk_dl_total), (unsigned)(s_bk_dl_total / 1024));
      else snprintf(st, sizeof st, "%s %u KB", TR("Downloading"), (unsigned)(s_bk_dl_got / 1024));
      booksSetStatus(st);
    }
    return;
  }
  const BkJob job = s_bk_job;
  const bool ok = s_bk_job_ok;
  s_bk_job = BkJob::None;
  if (job == BkJob::Progress) return;   // fire and forget
  if (job == BkJob::Download && ok) {
    booksAddDownloaded(s_bk_job_msg);
    if (g_lv.task) g_lv.task->showAlert(TR("Downloaded to the device"), 1400);
  }
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
  if (s_bk_text) { bkFree(s_bk_text); s_bk_text = nullptr; }
  s_bk_text_len = 0; s_bk_page_count = 0; s_bk_open = -1;
  s_bk_text_lbl = s_bk_foot = s_bk_menu = nullptr;
  if (to_shelf) booksShowView(BkView::Shelf);
}

static void booksMenuClose() {
  if (s_bk_menu && lv_obj_is_valid(s_bk_menu)) lv_obj_del(s_bk_menu);
  s_bk_menu = nullptr;
}

static void booksMenuCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  const int act = (int)(intptr_t)lv_event_get_user_data(e);
  if (act == 1 || act == 2) {   // font smaller / bigger: keep the reading position
    const int nf = s_bk_font + (act == 2 ? 1 : -1);
    if (nf < 0 || nf >= kBkFontCount) return;
    const uint32_t off = s_bk_page_count ? s_bk_pages[s_bk_page] : 0;
    s_bk_font = nf;
    lv_obj_set_style_text_font(s_bk_text_lbl, booksFont(s_bk_font), LV_PART_MAIN);
    booksPaginate();
    s_bk_page = booksPageForOffset(off);
    booksRenderPage();
    booksSaveIndex();
    return;
  }
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

// Keyboard: space / n / j / Enter / right / down turn forward; b / p / k / Backspace /
// left / up turn back; + and - change the size; m opens the menu.
static bool booksReaderKey(int key) {
  if (!booksReaderOpen()) return false;
  switch (key) {
    case ' ': case 'n': case 'N': case 'j': case 'J': case LV_KEY_ENTER: case '\r':
    case LV_KEY_RIGHT: case LV_KEY_DOWN:
      booksTurn(1); return true;
    case 'b': case 'B': case 'p': case 'P': case 'k': case 'K': case LV_KEY_BACKSPACE:
    case LV_KEY_LEFT: case LV_KEY_UP:
      booksTurn(-1); return true;
    case '+': case '=': {
        const uint32_t off = s_bk_page_count ? s_bk_pages[s_bk_page] : 0;
        if (s_bk_font + 1 < kBkFontCount) { ++s_bk_font; lv_obj_set_style_text_font(s_bk_text_lbl, booksFont(s_bk_font), LV_PART_MAIN); booksPaginate(); s_bk_page = booksPageForOffset(off); booksRenderPage(); booksSaveIndex(); }
      } return true;
    case '-': {
        const uint32_t off = s_bk_page_count ? s_bk_pages[s_bk_page] : 0;
        if (s_bk_font > 0) { --s_bk_font; lv_obj_set_style_text_font(s_bk_text_lbl, booksFont(s_bk_font), LV_PART_MAIN); booksPaginate(); s_bk_page = booksPageForOffset(off); booksRenderPage(); booksSaveIndex(); }
      } return true;
    case 'm': case 'M':
      booksMenuOpen(); return true;
    default:
      return false;
  }
}

static void booksBuildReader() {
  const lv_coord_t w = s_bk_body_w, h = s_bk_body_h;
  const lv_coord_t foot_h = 16;
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
  lv_obj_align(s_bk_foot, LV_ALIGN_BOTTOM_MID, 0, -2);
  lv_obj_add_flag(s_bk_body, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_bk_body, booksPageTapCb, LV_EVENT_CLICKED, nullptr);
}

static void booksOpenReader(int idx) {
  if (idx < 0 || idx >= s_bk_count) return;
  BkBook& b = s_bk_books[idx];
  s_bk_file = SPIFFS.open(b.file, FILE_READ);
  if (!s_bk_file) { if (g_lv.task) g_lv.task->showAlert(TR("Cannot open this book"), 1600); return; }
  s_bk_file_size = (uint32_t)s_bk_file.size();
  const char* dot = strrchr(b.file, '.');
  s_bk_is_txt = dot && strcasecmp(dot, ".txt") == 0;
  if (s_bk_is_txt) {
    s_bk_chapter_count = (int)((s_bk_file_size + kBkTxtChunk - 1) / kBkTxtChunk);
  } else {
    s_bk_epub = new EpubBook::Book();
    if (!s_bk_epub->open(bkFileRead, &s_bk_file, s_bk_file_size, kBkHooks)) {
      delete s_bk_epub; s_bk_epub = nullptr; s_bk_file.close();
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

// ================================ view switching ================================
static void booksShowView(BkView v) {
  if (!s_bk_root) return;
  booksMenuClose();
  if (s_bk_body) { lv_obj_del(s_bk_body); s_bk_body = nullptr; }
  s_bk_status = s_bk_user_ta = s_bk_pass_ta = s_bk_server_ta = nullptr;
  s_bk_text_lbl = s_bk_foot = nullptr;
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
  else if (v == BkView::Server) booksBuildServer();
  else booksBuildShelf();
  if (g_statusbar.root) lv_obj_move_foreground(g_statusbar.root);
}

static void openBooksPage() {
  if (s_bk_root) return;
  booksLoadIndex();
  booksLoadAbs();
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
