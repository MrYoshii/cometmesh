// SPDX-License-Identifier: GPL-3.0-or-later
// ===================== Books: audiobook playback engine (T-Deck Max) =====================
// Included by UITask.cpp after the Lua audio section, whose sink (ES8311 over I2S) and
// minimp3 decoder it shares; the session it plays comes from BooksApp.inc. A track is
// either a server URL (streamed) or a file on the SD card (downloaded for offline).
//
// Two tasks. The reader (core 0, beside Wi-Fi) fetches the tracks, over HTTP(S) with a
// Range request or from the card, skips their ID3 tags and fills a ring buffer in PSRAM
// several seconds deep. The decoder (core 1, above the UI) takes MP3 frames from it and
// writes them to the sink, which paces it. With both in one task, a slow TLS read or a
// reconnect starved the codec's few milliseconds of DMA and the audio stuttered.
// Every change of position (play, seek, pause/resume) restarts both at a time: a time
// inside a track maps to a byte offset in proportion (exact for CBR, close for VBR),
// and minimp3 finds the next frame from there. A track's size comes from the first
// reply's Content-Range; a dropped connection resumes at the exact byte. A stream lets
// go of its connection when the ring is full and asks again at half, and a link that only
// trickles while the ring runs low is dropped and opened afresh.
#if defined(CAP_BOOKS) && CAP_LUA_AUDIO

struct BkaWork {
  mp3dec_t decoder;
  mp3dec_scratch_t scratch;
  uint8_t input[16 * 1024];
  mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
  int16_t out[512];
};

// The ring between the two tasks: one writer, one reader, so plain indices will do.
static const size_t   kBkaRing = 1024 * 1024;   // ~2 min at 64 kbit/s, ~30 s at 256
// A stream does not hold its connection open on a full ring: a connection left waiting
// (TCP window shut) came back at a trickle on the Max and the ring ran dry. The reader
// lets go at full and asks again (a Range request from the exact byte) at half.
static const size_t   kBkaRefill = kBkaRing / 2;
static uint8_t*       s_bka_ring = nullptr;     // PSRAM, kept once allocated
static volatile size_t s_bka_head = 0, s_bka_tail = 0;   // written / read, ever-growing
static volatile bool  s_bka_eof = false;        // the reader has put in everything
static volatile bool  s_bka_reader_alive = false;
static volatile uint32_t s_bka_fetched = 0;     // bytes the reader has put in (for the 10 s log)

static TaskHandle_t   s_bka_task = nullptr;     // the decoder
// Both tasks stay between plays, waiting for the next one: a seek used to end them and
// start them again, and a freshly freed stack only comes back to the heap once the idle
// task has cleaned up, so the restart failed ("not enough memory") while a second try
// worked. They end for good (bkaReleaseTasks) once the player is closed and quiet.
static TaskHandle_t   s_bka_reader_task = nullptr;
static volatile bool  s_bka_quit = false;
static volatile bool  s_bka_net_busy = false;   // the reader holds (or opens) a connection
static volatile bool  s_bka_stop = false;       // ask both tasks to end
static volatile bool  s_bka_alive = false;      // the decoder is running
static volatile bool  s_bka_buffering = false;
static volatile bool  s_bka_ended = false;
static volatile double s_bka_pos = 0;
static double         s_bka_start_t = 0;        // the task's start position
static char           s_bka_err[64] = "";

static bool booksAudioRunning()   { return s_bka_alive; }
static bool booksAudioNetBusy()   { return s_bka_net_busy; }
static bool booksAudioBuffering() { return s_bka_alive && s_bka_buffering; }
static double booksAudioPos()     { return s_bka_pos; }
static bool booksAudioEnded()     { return s_bka_ended; }
static const char* booksAudioError() { return s_bka_err; }

static int bkaTrackAt(double t) {
  for (int i = s_bka_track_count - 1; i >= 0; --i) if (t >= s_bka_tracks[i].start) return i;
  return 0;
}

// Open the track at byte `from`. On success: the client streams the body, and *total is
// the whole file's size (from Content-Range, or Content-Length for a 200 reply).
static bool bkaOpen(HTTPClient& http, WiFiClientSecure& tls, WiFiClient& plain, const BkaTrack& tr,
                    uint32_t from, uint32_t* total, int* code_out) {
  char url[300];
  bkUrl(url, sizeof url, tr.url);
  http.setReuse(false);
  http.setConnectTimeout(9000);
  http.setTimeout(15000);
  http.useHTTP10(true);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("comet-books");
  const bool https = strncasecmp(url, "https://", 8) == 0;
  if (https) { tls.setInsecure(); if (!http.begin(tls, url)) return false; }
  else if (!http.begin(plain, url)) return false;
  static const char* kKeep[] = { "Content-Range", "Content-Length" };
  http.collectHeaders(kKeep, 2);
  if (s_abs_token[0]) http.addHeader("Authorization", String("Bearer ") + s_abs_token);
  char range[40];
  snprintf(range, sizeof range, "bytes=%lu-", (unsigned long)from);
  http.addHeader("Range", range);
  http.addHeader("Accept-Encoding", "identity");
  const int code = http.GET();
  if (code_out) *code_out = code;
  if (code != 206 && code != 200) {
    if (code < 0) {   // no HTTP answer at all: say why (connect, DNS, TLS, memory)
      char err[96] = "";
      if (https) tls.lastError(err, sizeof err);
      Serial.printf("[books] open %s: %s, tls=\"%s\", wifi status %d rssi %d, internal free=%u largest=%u\n", url,
                    HTTPClient::errorToString(code).c_str(), err, (int)WiFi.status(), (int)WiFi.RSSI(),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
    http.end();
    return false;
  }
  uint32_t size = 0;
  const String cr = http.header("Content-Range");   // "bytes 100-999/1000"
  const int slash = cr.lastIndexOf('/');
  if (slash > 0) size = (uint32_t)strtoul(cr.c_str() + slash + 1, nullptr, 10);
  // No Content-Range (or not collected): from byte 0 the body length is the size.
  if (!size && (code == 200 || from == 0) && http.getSize() > 0) size = (uint32_t)http.getSize();
  if (code == 200 && from) {   // the server ignored the range: read from the start
    if (total) *total = size;
    return true;
  }
  if (total) *total = size;
  return true;
}

static size_t bkaRingUsed() { return s_bka_head - s_bka_tail; }

// Reader side: blocks while the ring is full; false when asked to stop.
static bool bkaRingPut(const uint8_t* data, size_t n) {
  while (n) {
    while (!s_bka_stop && bkaRingUsed() >= kBkaRing) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_bka_stop) return false;
    size_t room = kBkaRing - bkaRingUsed();
    if (room > n) room = n;
    const size_t at = s_bka_head % kBkaRing;
    const size_t first = room < kBkaRing - at ? room : kBkaRing - at;
    memcpy(s_bka_ring + at, data, first);
    if (room > first) memcpy(s_bka_ring, data + first, room - first);
    __sync_synchronize();
    s_bka_head += room;
    s_bka_fetched += (uint32_t)room;
    data += room; n -= room;
  }
  return true;
}

// Decoder side: up to n bytes, never blocking.
static size_t bkaRingGet(uint8_t* out, size_t n) {
  size_t have = bkaRingUsed();
  if (have > n) have = n;
  const size_t at = s_bka_tail % kBkaRing;
  const size_t first = have < kBkaRing - at ? have : kBkaRing - at;
  memcpy(out, s_bka_ring + at, first);
  if (have > first) memcpy(out + first, s_bka_ring, have - first);
  __sync_synchronize();
  s_bka_tail += have;
  return have;
}

static void bkaReaderRun() {
  int track = bkaTrackAt(s_bka_start_t);
  double t_in_track = s_bka_start_t - s_bka_tracks[track].start;
  uint32_t resume_at = UINT32_MAX;   // a dropped connection: the exact byte to go on from
  int failures = 0;
  uint8_t* chunk = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!chunk) { snprintf(s_bka_err, sizeof s_bka_err, "Not enough memory to play"); goto done; }

  while (!s_bka_stop && track < s_bka_track_count) {
    BkaTrack& tr = s_bka_tracks[track];
    uint32_t from = 0;
    if (resume_at != UINT32_MAX) from = resume_at;
    else if (t_in_track > 0.5 && tr.size && tr.dur > 0) from = (uint32_t)((double)tr.size * (t_in_track / tr.dur));
    HTTPClient http; WiFiClientSecure tls; WiFiClient plain;
    uint32_t total = 0;
    int code = 0;
    File lf;
    s_bka_net_busy = !tr.local;
    if (tr.local) {   // a downloaded track: size known up front, exact offsets
      fs::FS* fs = booksFs();
      lf = fs ? fs->open(tr.url, FILE_READ) : File();
      if (!lf) { snprintf(s_bka_err, sizeof s_bka_err, "Track missing on the SD card"); break; }
      total = tr.size = (uint32_t)lf.size();
      if (resume_at == UINT32_MAX && t_in_track > 0.5 && tr.dur > 0) from = (uint32_t)((double)total * (t_in_track / tr.dur));
      if (from > total) from = total;
      lf.seek(from);
    } else if (!bkaOpen(http, tls, plain, tr, from, &total, &code)) {
      if (code == 401 && failures++ == 0 && bkRefresh()) continue;   // token expired: renew once
      if (++failures >= 5) {
        if (code == 401) snprintf(s_bka_err, sizeof s_bka_err, "Signed out by the server: sign in again");
        else snprintf(s_bka_err, sizeof s_bka_err, "Cannot reach the audio (%d)", code);
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(1500 * failures));
      continue;
    } else if (total && !tr.size) {
      tr.size = total;
      if (from == 0 && t_in_track > 0.5 && resume_at == UINT32_MAX) {   // now the offset can be worked out
        http.end();
        continue;
      }
    }
    WiFiClient* stream = tr.local ? nullptr : http.getStreamPtr();
    uint32_t file_pos = from;
    uint32_t skip = 0;                 // an ID3v2 tag still to skip
    bool checked_id3 = from != 0;
    uint8_t id3[10];
    size_t id3_have = 0;
    uint32_t idle_since = millis();
    bool done_track = false, dropped = false, parked = false;
    // The link watch: what the book needs a second, and what came in the last 3 s.
    const uint32_t need_bps = tr.size && tr.dur > 1 ? (uint32_t)((double)tr.size / tr.dur) : 16000;
    uint32_t win_start = millis(), win_bytes = 0;
    while (!s_bka_stop) {
      int n = 0;
      if (!tr.local) {
        if (kBkaRing - bkaRingUsed() < 4096 + 16) { parked = true; break; }   // full: let go
        const uint32_t now = millis();
        if (now - win_start >= 3000) {
          // Running low and the link brings less than the book plays: a stuck connection
          // (it trickles on rather than closing). A fresh one is quicker than waiting.
          const bool low = bkaRingUsed() < 96 * 1024;
          const bool slow = (uint64_t)win_bytes * 1000 < (uint64_t)need_bps * (now - win_start) * 3 / 4;
          if (low && slow) {
            Serial.printf("[bka] link slow (%u B in %u ms, ring %u KB): reconnecting\n", (unsigned)win_bytes,
                          (unsigned)(now - win_start), (unsigned)(bkaRingUsed() / 1024));
            dropped = true;
            break;
          }
          win_start = now; win_bytes = 0;
        }
      }
      if (tr.local) {
        if ((uint32_t)lf.position() >= total) { done_track = true; break; }
        n = lf.read(chunk, 4096);
        if (n <= 0) { snprintf(s_bka_err, sizeof s_bka_err, "SD card read failed"); s_bka_stop = true; break; }
      } else {
        const int avail = stream ? stream->available() : 0;
        if (avail <= 0) {
          if (total && file_pos >= total) { done_track = true; break; }
          if (!stream || !stream->connected()) {
            if (!total) { done_track = true; break; }    // no size given: the close is the end
            dropped = true; break;
          }
          if (millis() - idle_since > 15000) { dropped = true; break; }
          vTaskDelay(pdMS_TO_TICKS(5));
          continue;
        }
        n = stream->read(chunk, avail < 4096 ? avail : 4096);
        if (n <= 0) continue;
        idle_since = millis();
        win_bytes += (uint32_t)n;
      }
      file_pos += (uint32_t)n;
      const uint8_t* p = chunk;
      size_t len = (size_t)n;
      if (!checked_id3) {   // the first 10 bytes of the file decide whether a tag comes first
        while (len && id3_have < 10) { id3[id3_have++] = *p++; --len; }
        if (id3_have < 10) continue;
        checked_id3 = true;
        if (!memcmp(id3, "ID3", 3)) {
          skip = (((uint32_t)id3[6] & 0x7F) << 21 | ((uint32_t)id3[7] & 0x7F) << 14 |
                  ((uint32_t)id3[8] & 0x7F) << 7 | ((uint32_t)id3[9] & 0x7F));
        } else if (!bkaRingPut(id3, 10)) {
          break;
        }
      }
      if (skip) {
        const size_t k = skip < len ? skip : len;
        p += k; len -= k; skip -= (uint32_t)k;
      }
      if (len && !bkaRingPut(p, len)) break;
      failures = 0;
      resume_at = UINT32_MAX;
    }
    if (tr.local) lf.close(); else http.end();
    s_bka_net_busy = false;
    if (s_bka_stop) break;
    if (parked) {   // the ring is full: wait until half of it has played, then ask again
      resume_at = !checked_id3 ? 0 : file_pos + skip;
      while (!s_bka_stop && bkaRingUsed() > kBkaRefill) vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (dropped) {
      resume_at = !checked_id3 ? 0 : file_pos + skip;
      if (++failures >= 6) { snprintf(s_bka_err, sizeof s_bka_err, "The connection keeps dropping"); break; }
      vTaskDelay(pdMS_TO_TICKS(failures == 1 ? 100 : 1000 * (failures - 1)));
      continue;
    }
    if (done_track) { ++track; t_in_track = 0; resume_at = UINT32_MAX; }
  }

done:
  if (chunk) heap_caps_free(chunk);
  s_bka_net_busy = false;
  s_bka_eof = true;
  s_bka_reader_alive = false;
}

static void bkaReaderFn(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (s_bka_quit) break;
    bkaReaderRun();
  }
  s_bka_reader_task = nullptr;
  vTaskDelete(nullptr);
}

// Waiting for data: silence to an open sink (it paces the wait and keeps the DMA
// from replaying its last buffer as a buzz), else a short sleep.
static void bkaIdle(LuaAudioSinkSession* sink, BkaWork* w) {
  if (sink->open) {
    memset(w->out, 0, 256 * sizeof(int16_t));
    luaAudioSinkWrite(sink, w->out, 256);
  } else {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static void bkaDecoderRun() {
  BkaWork* w = (BkaWork*)heap_caps_malloc(sizeof(BkaWork), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  LuaAudioSinkSession sink;
  uint32_t sink_rate = 0;
  size_t in_start = 0, in_count = 0, out_n = 0;
  uint64_t samples_out = 0;
  bool primed = false;                 // enough buffered to start (or start again)
  // Playback health, logged every 10 s while it plays: how fast the reader fetches
  // against what the book needs, how often the ring ran dry, and the longest wait on
  // the codec (a long one means something held the CPU or the flash cache).
  uint32_t log_at = millis(), underruns = 0, slow_writes = 0, max_write_ms = 0, max_dec_ms = 0;
  uint32_t kbps = 0;
  // The codec's DMA holds ~12 ms (4 x 128 frames at 44.1 kHz): a longer pause between
  // two writes means it ran dry and the sound broke. Counted in microseconds.
  uint32_t last_write_end_us = 0, dry = 0, max_gap_us = 0;
  size_t prime_bytes = 16 * 1024;
  if (!w) { snprintf(s_bka_err, sizeof s_bka_err, "Not enough memory to play"); s_bka_stop = true; goto done; }
  s_wada_mp3_scratch = &w->scratch;
  mp3dec_init(&w->decoder);
  s_bka_pos = s_bka_start_t;

  while (!s_bka_stop) {
    if (millis() - log_at >= 10000) {
      const uint32_t dt = millis() - log_at;
      Serial.printf("[bka] fetch %u KB/s (book %u kbit/s), ring %u KB, underruns %u, slow writes %u (max %u ms), "
                    "decode max %u ms, dry %u (longest gap %u.%u ms), int free %u (largest %u), stack left %u/%u\n",
                    (unsigned)(s_bka_fetched / (dt ? dt : 1)), (unsigned)kbps, (unsigned)(bkaRingUsed() / 1024),
                    (unsigned)underruns, (unsigned)slow_writes, (unsigned)max_write_ms, (unsigned)max_dec_ms,
                    (unsigned)dry, (unsigned)(max_gap_us / 1000), (unsigned)(max_gap_us % 1000 / 100),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                    (unsigned)(s_bka_reader_task ? uxTaskGetStackHighWaterMark(s_bka_reader_task) : 0),
                    (unsigned)uxTaskGetStackHighWaterMark(nullptr));
      s_bka_fetched = 0; log_at = millis();
      underruns = slow_writes = max_write_ms = max_dec_ms = 0;
      dry = max_gap_us = 0;
    }
    if (in_count < sizeof w->input / 2) {   // top up from the ring
      if (in_start) { memmove(w->input, w->input + in_start, in_count); in_start = 0; }
      in_count += bkaRingGet(w->input + in_count, sizeof w->input - in_count);
    }
    const bool eof = s_bka_eof && bkaRingUsed() == 0;
    if (!primed) {   // a second or so in hand before sound starts; more after a stall
      if (eof || bkaRingUsed() + in_count >= prime_bytes) { primed = true; s_bka_buffering = false; }
      else { s_bka_buffering = true; bkaIdle(&sink, w); last_write_end_us = 0; continue; }
    }
    if (in_count < 2048 && !eof) {
      if (bkaRingUsed() == 0) {   // ran dry: buffer up again, deeper, rather than stutter
        primed = false; ++underruns; prime_bytes = 64 * 1024;
      }
      bkaIdle(&sink, w);
      last_write_end_us = 0;   // the gap counts below only CPU stalls, not waits for data
      continue;
    }
    if (!in_count) break;   // everything played
    mp3dec_frame_info_t info = {};
    const uint32_t t_dec = millis();
    const int samples = mp3dec_decode_frame(&w->decoder, w->input + in_start, (int)in_count, w->pcm, &info);
    if (millis() - t_dec > max_dec_ms) max_dec_ms = millis() - t_dec;
    if (info.bitrate_kbps > 0) kbps = (uint32_t)info.bitrate_kbps;
    if (info.frame_bytes <= 0) {
      if (eof) break;
      const size_t keep = in_count > 4 ? 4 : in_count;   // no frame in it: keep the tail, read on
      in_start += in_count - keep; in_count = keep;
      continue;
    }
    in_start += (size_t)info.frame_bytes; in_count -= (size_t)info.frame_bytes;
    if (!in_count) in_start = 0;
    if (samples <= 0 || info.hz < 8000 || info.hz > 48000) continue;
    if (!sink.open || sink_rate != (uint32_t)info.hz) {
      if (sink.open) luaAudioSinkEnd(&sink);
      const char* err = nullptr;
      if (!luaAudioSinkBegin((uint32_t)info.hz, &sink, &err)) {
        snprintf(s_bka_err, sizeof s_bka_err, "Audio unavailable (%s)", err ? err : "?");
        s_bka_stop = true;
        break;
      }
      // A new rate (a track encoded differently): the clock goes on from here.
      if (sink_rate) { s_bka_start_t = s_bka_pos; samples_out = 0; }
      sink_rate = (uint32_t)info.hz;
    }
    for (int i = 0; i < samples; ++i) {
      int32_t v = info.channels == 2 ? ((int32_t)w->pcm[2 * i] + w->pcm[2 * i + 1]) / 2 : w->pcm[i];
      v = (int32_t)((float)v * sink.gain);
      w->out[out_n++] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
      if (out_n == sizeof w->out / sizeof w->out[0]) {
        const uint32_t t_w = millis();
        const uint32_t t_wus = (uint32_t)esp_timer_get_time();
        if (last_write_end_us) {
          const uint32_t gap = t_wus - last_write_end_us;
          if (gap > max_gap_us) max_gap_us = gap;
          if (gap > 11000) ++dry;
        }
        if (!luaAudioSinkWrite(&sink, w->out, out_n)) { snprintf(s_bka_err, sizeof s_bka_err, "Audio output failed"); s_bka_stop = true; break; }
        last_write_end_us = (uint32_t)esp_timer_get_time();
        // 512 frames are ~12 ms of sound at 44.1 kHz: a write blocks for about that
        // when it keeps pace; much longer means the codec's DMA ran dry first.
        const uint32_t wt = millis() - t_w;
        if (wt > max_write_ms) max_write_ms = wt;
        if (wt > 40) ++slow_writes;
        out_n = 0;
      }
    }
    samples_out += (uint64_t)samples;
    s_bka_pos = s_bka_start_t + (double)samples_out / sink_rate;
  }
  if (!s_bka_stop && sink.open && out_n) luaAudioSinkWrite(&sink, w->out, out_n);
  if (!s_bka_stop && !s_bka_err[0]) s_bka_ended = true;

done:
  s_bka_stop = true;                                   // the reader too
  for (int i = 0; i < 500 && s_bka_reader_alive; ++i) vTaskDelay(pdMS_TO_TICKS(10));
  if (sink.open) luaAudioSinkEnd(&sink);
  s_wada_mp3_scratch = nullptr;
  if (w) heap_caps_free(w);
  __atomic_store_n(&s_lua_audio_active, false, __ATOMIC_RELEASE);
  s_bka_buffering = false;
  s_bka_sd_busy = false;
  s_bka_alive = false;
}

static void bkaTaskFn(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (s_bka_quit) break;
    bkaDecoderRun();
  }
  s_bka_task = nullptr;
  vTaskDelete(nullptr);
}

// End both tasks (the player is closed and nothing plays): their stacks go back.
static void bkaReleaseTasks() {
  if (s_bka_alive || s_bka_reader_alive) return;
  if (!s_bka_task && !s_bka_reader_task) return;
  s_bka_quit = true;
  if (s_bka_task) xTaskNotifyGive(s_bka_task);
  if (s_bka_reader_task) xTaskNotifyGive(s_bka_reader_task);
  for (int i = 0; i < 100 && (s_bka_task || s_bka_reader_task); ++i) vTaskDelay(pdMS_TO_TICKS(5));
  s_bka_quit = false;
}

static void booksAudioStop() {
  if (!s_bka_alive && !s_bka_reader_alive) return;
  s_bka_stop = true;
  // The decoder ends within a frame; it waits for the reader, which can sit in a
  // connect for a few seconds.
  for (int i = 0; i < 600 && (s_bka_alive || s_bka_reader_alive); ++i) vTaskDelay(pdMS_TO_TICKS(10));
}

static bool booksAudioPlayFrom(double t) {
  booksAudioStop();
  if (s_bka_alive || s_bka_reader_alive) { snprintf(s_bka_err, sizeof s_bka_err, "Audio is still stopping"); return false; }
  if (!s_bka_ring) s_bka_ring = (uint8_t*)heap_caps_malloc(kBkaRing, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_bka_ring) { snprintf(s_bka_err, sizeof s_bka_err, "Not enough memory to play"); return false; }
  s_bka_head = s_bka_tail = 0;
  s_bka_eof = false;
  s_bka_err[0] = 0;
  s_bka_ended = false;
  if (!s_bka_track_count) { snprintf(s_bka_err, sizeof s_bka_err, "Nothing to play"); return false; }
  if (!s_bka_tracks[0].local && WiFi.status() != WL_CONNECTED) {
    snprintf(s_bka_err, sizeof s_bka_err, "Connect to Wi-Fi first");
    return false;
  }
  if (luaAudioActive()) { snprintf(s_bka_err, sizeof s_bka_err, "An app is playing audio"); return false; }
  if (touchPrefsGetSoundVolume() == 0) { snprintf(s_bka_err, sizeof s_bka_err, "Volume is at 0 (quick panel)"); return false; }
  s_bka_start_t = t < 0 ? 0 : t;
  s_bka_pos = s_bka_start_t;
  s_bka_stop = false;
  s_bka_alive = true;
  s_bka_buffering = true;
  s_bka_sd_busy = s_bka_tracks[0].local;   // reading the card: hold off unmounting it
  __atomic_store_n(&s_lua_audio_active, true, __ATOMIC_RELEASE);   // notification chimes stand aside
  // The reader: TLS and the HTTP client want a deep stack (internal RAM, only while
  // playing), on core 0 with Wi-Fi. The decoder: a small stack (minimp3's scratch is in
  // PSRAM), on core 1 above the UI so a panel refresh cannot starve the codec.
  // Started once, then woken for each play (see s_bka_reader_task).
  if (!s_bka_reader_task)
    xTaskCreatePinnedToCore(bkaReaderFn, "bkaread", 12288, nullptr, 4, &s_bka_reader_task, 0);
  if (!s_bka_task) xTaskCreatePinnedToCore(bkaTaskFn, "bkaudio", 6144, nullptr, 6, &s_bka_task, 1);
  if (!s_bka_reader_task || !s_bka_task) {
    s_bka_alive = false;
    s_bka_sd_busy = false;
    __atomic_store_n(&s_lua_audio_active, false, __ATOMIC_RELEASE);
    snprintf(s_bka_err, sizeof s_bka_err, "Not enough memory to play");
    return false;
  }
  s_bka_reader_alive = true;
  xTaskNotifyGive(s_bka_reader_task);
  xTaskNotifyGive(s_bka_task);
  return true;
}

#endif  // CAP_BOOKS && CAP_LUA_AUDIO
