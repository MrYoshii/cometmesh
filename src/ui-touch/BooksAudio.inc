// SPDX-License-Identifier: GPL-3.0-or-later
// ===================== Books: audiobook playback engine (T-Deck Max) =====================
// Included by UITask.cpp after the Lua audio section, whose sink (ES8311 over I2S) and
// minimp3 decoder it shares; the session it plays comes from BooksApp.inc.
//
// One task does it all: it streams the current track over HTTP(S) with a Range request,
// decodes MP3 frames and writes them to the sink, which paces it. Every change of
// position (play, seek, pause/resume) restarts it at a time: a time inside a track maps
// to a byte offset in proportion (exact for CBR, close for VBR), and minimp3 finds the
// next frame from there. The track's size comes from the first reply's Content-Range.
#if defined(CAP_BOOKS) && CAP_LUA_AUDIO

struct BkaWork {
  mp3dec_t decoder;
  mp3dec_scratch_t scratch;
  uint8_t input[32 * 1024];
  mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
  int16_t out[512];
};

static TaskHandle_t   s_bka_task = nullptr;
static volatile bool  s_bka_stop = false;       // ask the task to end
static volatile bool  s_bka_alive = false;      // the task is running
static volatile bool  s_bka_buffering = false;
static volatile bool  s_bka_ended = false;
static volatile double s_bka_pos = 0;
static double         s_bka_start_t = 0;        // the task's start position
static char           s_bka_err[64] = "";

static bool booksAudioRunning()   { return s_bka_alive; }
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
  http.setUserAgent("wadamesh-books");
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
  if (code != 206 && code != 200) { http.end(); return false; }
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

static void bkaTaskFn(void*) {
  BkaWork* w = (BkaWork*)heap_caps_malloc(sizeof(BkaWork), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  LuaAudioSinkSession sink;
  uint32_t sink_rate = 0;
  int track = bkaTrackAt(s_bka_start_t);
  double t_in_track = s_bka_start_t - s_bka_tracks[track].start;
  int failures = 0;
  if (!w) { snprintf(s_bka_err, sizeof s_bka_err, "Not enough memory to play"); goto done; }
  s_wada_mp3_scratch = &w->scratch;

  while (!s_bka_stop && track < s_bka_track_count) {
    BkaTrack& tr = s_bka_tracks[track];
    // Where in the file: in proportion to the time, once the size is known.
    uint32_t from = 0;
    if (t_in_track > 0.5 && tr.size && tr.dur > 0)
      from = (uint32_t)((double)tr.size * (t_in_track / tr.dur));
    HTTPClient http; WiFiClientSecure tls; WiFiClient plain;
    uint32_t total = 0;
    int code = 0;
    s_bka_buffering = true;
    if (!bkaOpen(http, tls, plain, tr, from, &total, &code)) {
      if (code == 401 && failures++ == 0 && bkRefresh()) continue;   // token expired: renew once
      if (++failures >= 4) {
        if (code == 401) snprintf(s_bka_err, sizeof s_bka_err, "Signed out by the server: sign in again");
        else snprintf(s_bka_err, sizeof s_bka_err, "Cannot reach the audio (%d)", code);
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(1500 * failures));
      continue;
    }
    if (total && !tr.size) {
      tr.size = total;
      if (from == 0 && t_in_track > 0.5) {   // now the offset can be worked out: reopen there
        http.end();
        continue;
      }
    }
    WiFiClient* stream = http.getStreamPtr();
    mp3dec_init(&w->decoder);
    size_t in_start = 0, in_count = 0;
    uint32_t file_pos = from;                 // bytes of the file received so far
    uint32_t skip = 0;                        // an ID3v2 tag still to skip
    bool checked_id3 = from != 0;
    uint64_t samples_out = 0;
    uint32_t hz = 0;
    size_t out_n = 0;
    uint32_t idle_since = millis();
    bool track_done = false, reconnect = false;
    const double base = tr.start + (from && tr.size ? tr.dur * ((double)from / tr.size) : 0);
    s_bka_pos = base;

    while (!s_bka_stop) {
      // Keep the input topped up from the network (never blocking for long).
      if (in_count < sizeof w->input / 2 && file_pos < (total ? total : UINT32_MAX)) {
        if (in_start) { memmove(w->input, w->input + in_start, in_count); in_start = 0; }
        const int avail = stream ? stream->available() : 0;
        if (avail > 0) {
          size_t room = sizeof w->input - in_count;
          if ((size_t)avail < room) room = (size_t)avail;
          const int n = stream->read(w->input + in_count, room);
          if (n > 0) { in_count += (size_t)n; file_pos += (uint32_t)n; idle_since = millis(); }
        } else if (!stream || !stream->connected()) {
          // Closed. Without a known size that is the end of the track; with one, a
          // close short of it is a dropped connection, picked up again below.
          if (!total) total = file_pos;
          else if (file_pos < total && in_count == 0) { reconnect = true; break; }
        } else if (millis() - idle_since > 15000) {
          reconnect = true; break;
        }
      }
      if (!checked_id3 && in_count >= 10) {
        checked_id3 = true;
        if (!memcmp(w->input + in_start, "ID3", 3)) {
          const uint8_t* h = w->input + in_start;
          skip = 10u + (((uint32_t)h[6] & 0x7F) << 21 | ((uint32_t)h[7] & 0x7F) << 14 |
                        ((uint32_t)h[8] & 0x7F) << 7 | ((uint32_t)h[9] & 0x7F));
        }
      }
      if (skip) {
        const size_t n = skip < in_count ? skip : in_count;
        in_start += n; in_count -= n; skip -= (uint32_t)n;
        if (!in_count) in_start = 0;
        continue;
      }
      const bool all_in = total && file_pos >= total;
      if (in_count < 2048 && !all_in) {    // starved: wait for the network
        s_bka_buffering = in_count == 0;
        if (sink.open && in_count == 0) {   // keep the codec fed with silence while it refills
          memset(w->out, 0, sizeof w->out);
          luaAudioSinkWrite(&sink, w->out, 256);
        } else {
          vTaskDelay(pdMS_TO_TICKS(10));
        }
        continue;
      }
      if (!in_count) { track_done = true; break; }

      mp3dec_frame_info_t info = {};
      const int samples = mp3dec_decode_frame(&w->decoder, w->input + in_start, (int)in_count, w->pcm, &info);
      if (info.frame_bytes <= 0) {
        if (all_in) { track_done = true; break; }
        // No frame in what we have: drop all but the tail and read on.
        const size_t keep = in_count > 4 ? 4 : in_count;
        in_start += in_count - keep; in_count = keep;
        continue;
      }
      in_start += (size_t)info.frame_bytes; in_count -= (size_t)info.frame_bytes;
      if (!in_count) in_start = 0;
      if (samples <= 0 || info.hz < 8000 || info.hz > 48000) continue;
      s_bka_buffering = false;
      if (!sink.open || sink_rate != (uint32_t)info.hz) {
        if (sink.open) luaAudioSinkEnd(&sink);
        const char* err = nullptr;
        if (!luaAudioSinkBegin((uint32_t)info.hz, &sink, &err)) {
          snprintf(s_bka_err, sizeof s_bka_err, "Audio unavailable (%s)", err ? err : "?");
          s_bka_stop = true;
          break;
        }
        sink_rate = (uint32_t)info.hz;
      }
      hz = (uint32_t)info.hz;
      for (int i = 0; i < samples; ++i) {
        int32_t v = info.channels == 2 ? ((int32_t)w->pcm[2 * i] + w->pcm[2 * i + 1]) / 2 : w->pcm[i];
        v = (int32_t)((float)v * sink.gain);
        w->out[out_n++] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        if (out_n == sizeof w->out / sizeof w->out[0]) {
          if (!luaAudioSinkWrite(&sink, w->out, out_n)) { snprintf(s_bka_err, sizeof s_bka_err, "Audio output failed"); s_bka_stop = true; break; }
          out_n = 0;
        }
      }
      samples_out += (uint64_t)samples;
      s_bka_pos = base + (double)samples_out / hz;
      failures = 0;
    }
    http.end();
    if (s_bka_stop) break;
    if (reconnect) {   // dropped mid-track: pick up where it stopped
      t_in_track = s_bka_pos - tr.start;
      if (++failures >= 4) { snprintf(s_bka_err, sizeof s_bka_err, "The connection keeps dropping"); break; }
      vTaskDelay(pdMS_TO_TICKS(1000 * failures));
      continue;
    }
    if (track_done) { ++track; t_in_track = 0; }
  }
  if (!s_bka_stop && !s_bka_err[0] && track >= s_bka_track_count) s_bka_ended = true;

done:
  if (sink.open) luaAudioSinkEnd(&sink);
  s_wada_mp3_scratch = nullptr;
  if (w) heap_caps_free(w);
  __atomic_store_n(&s_lua_audio_active, false, __ATOMIC_RELEASE);
  s_bka_buffering = false;
  s_bka_alive = false;
  s_bka_task = nullptr;
  vTaskDelete(nullptr);
}

static void booksAudioStop() {
  if (!s_bka_alive) return;
  s_bka_stop = true;
  for (int i = 0; i < 300 && s_bka_alive; ++i) vTaskDelay(pdMS_TO_TICKS(10));   // ends within one frame
}

static bool booksAudioPlayFrom(double t) {
  booksAudioStop();
  if (s_bka_alive) { snprintf(s_bka_err, sizeof s_bka_err, "Audio is still stopping"); return false; }
  s_bka_err[0] = 0;
  s_bka_ended = false;
  if (!s_bka_track_count) { snprintf(s_bka_err, sizeof s_bka_err, "Nothing to play"); return false; }
  if (WiFi.status() != WL_CONNECTED) { snprintf(s_bka_err, sizeof s_bka_err, "Connect to Wi-Fi first"); return false; }
  if (luaAudioActive()) { snprintf(s_bka_err, sizeof s_bka_err, "An app is playing audio"); return false; }
  if (touchPrefsGetSoundVolume() == 0) { snprintf(s_bka_err, sizeof s_bka_err, "Volume is at 0 (quick panel)"); return false; }
  s_bka_start_t = t < 0 ? 0 : t;
  s_bka_pos = s_bka_start_t;
  s_bka_stop = false;
  s_bka_alive = true;
  s_bka_buffering = true;
  __atomic_store_n(&s_lua_audio_active, true, __ATOMIC_RELEASE);   // notification chimes stand aside
  // TLS and the HTTP client want a deep stack; internal RAM, so only while playing.
  if (xTaskCreatePinnedToCore(bkaTaskFn, "bkaudio", 12288, nullptr, 5, &s_bka_task, 0) != pdPASS) {
    s_bka_alive = false;
    __atomic_store_n(&s_lua_audio_active, false, __ATOMIC_RELEASE);
    snprintf(s_bka_err, sizeof s_bka_err, "Not enough memory to play");
    return false;
  }
  return true;
}

#endif  // CAP_BOOKS && CAP_LUA_AUDIO
