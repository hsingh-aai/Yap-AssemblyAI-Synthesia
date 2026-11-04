// Pocket Voice - Phase 2: "Blurt", the first real AssemblyAI round trip.
//
// Hold the yellow button, talk, let go. The device records 16 kHz mono PCM
// into PSRAM, uploads it to AssemblyAI, polls for the transcript and prints
// the text on the round face.
//
// This is the pre-recorded path on purpose: it is the smallest thing that
// proves mic -> network -> AssemblyAI -> display on this hardware. Live
// streaming (the real Blurt experience) is the next step and reuses the same
// mic buffer and the same screen.
//
// The face is paged: the blue button walks Blurt -> Transcript -> Status ->
// Log and back. A long transcript splits into sub-pages of its own, so the
// blue button reads it to the end before moving on. Status and Log exist
// because a device with no keyboard still has to be able to tell you why it
// failed.
//
// Board: M5Stack StopWatch Dev Kit (C152), ESP32-S3R8.

#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <stdarg.h>

#include "secrets.h"

// Deck palette.
static constexpr uint32_t kInk     = 0x131210;
static constexpr uint32_t kPaper   = 0xF3EFE6;
static constexpr uint32_t kMuted   = 0xB8B2A7;
static constexpr uint32_t kAccent  = 0xA99CFF;
static constexpr uint32_t kAccent2 = 0x3F2BC1;
static constexpr uint32_t kError   = 0xE86A5C;

static constexpr char kApiHost[] = "https://api.assemblyai.com";

// Audio. 16 kHz mono is what we send; 12 s is a comfortable ceiling for a
// demo utterance and costs 384 KB of the 8 MB PSRAM.
static constexpr uint32_t kSampleRate = 16000;
static constexpr uint32_t kMaxSeconds = 12;
static constexpr size_t   kMaxSamples = kSampleRate * kMaxSeconds;
static constexpr size_t   kChunk      = 512;

static int16_t* pcm = nullptr;   // PSRAM, kMaxSamples entries
static size_t   pcm_len = 0;     // samples captured in the last take

// The driver reports this panel as 468x468 (confirmed on the device), but the
// visible glass is a 466 px circle. Take the centre from the panel, which is
// what the framebuffer is addressed in, and the radius from the glass, so
// nothing is drawn onto the two columns that never make it past the bezel.
static constexpr int kFace = 466;

static int cx, cy, radius;

// ------------------------------------------------------------------ logging

// Serial and the Log page carry the same lines. Serial is the relay to a
// laptop; the ring is what you read when the device is on its own.
//
// Serial only works because setup() sets cfg.serial_baudrate. With
// USBMode=hwcdc the core's main.cpp skips its own Serial.begin() (it is
// guarded on !ARDUINO_USB_MODE) and M5Unified's serial_baudrate defaults to 0,
// so without that line nothing ever begins HWCDC, tx_ring_buf stays null and
// every print is silently dropped.
static constexpr int kLogLines = 11;
static char g_log[kLogLines][52];
static int  g_log_count = 0;     // total lines ever, for ring ordering

static char g_last_err[48] = "";

static void logf(const char* fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);

  Serial.println(buf);
  strlcpy(g_log[g_log_count % kLogLines], buf, sizeof g_log[0]);
  ++g_log_count;
}

static void failf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_last_err, sizeof g_last_err, fmt, ap);
  va_end(ap);
  logf("[err] %s", g_last_err);
}

// ---------------------------------------------------------------- geometry

// Half-width of the circle's chord at vertical offset `dy` from the centre.
static float chordHalf(int dy) {
  const float d = (float)radius * radius - (float)dy * dy;
  return d > 1.0f ? sqrtf(d) : 1.0f;
}

// Largest scale at which `s` still fits the chord at vertical offset `dy`.
// Round faces lose width fast towards the top and bottom, so a string that
// fits across the middle will not fit under it.
static float fitScale(const char* s, int dy, float cap = 1.0f) {
  M5.Display.setTextSize(1.0f);              // measure at unit scale
  const float room = 2.0f * chordHalf(dy) - 24.0f;
  const int   w    = M5.Display.textWidth(s);
  if (w <= 0) return cap;
  const float scale = room / (float)w;
  return scale > cap ? cap : (scale < 0.45f ? 0.45f : scale);
}

// ---------------------------------------------------------------- chrome

enum PageId : uint8_t {
  PAGE_BLURT = 0,
  PAGE_TRANSCRIPT,
  PAGE_STATUS,
  PAGE_LOG,
  PAGE_COUNT
};

static const char* kPageName[PAGE_COUNT] = {"BLURT", "TRANSCRIPT", "STATUS", "LOG"};

static uint8_t g_page  = PAGE_BLURT;
static int     g_tpage = 0;          // sub-page within the transcript

static void faceClear() {
  M5.Display.fillScreen(kInk);
  M5.Display.drawCircle(cx, cy, radius - 4, kAccent);
}

// Small caption at the top of the circle naming the page you are on.
static void caption(const char* name) {
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.setTextSize(1.0f);
  M5.Display.setTextColor(kMuted, kInk);
  M5.Display.drawString(name, cx, cy - radius + 30);
}

// One dot per page along the bottom arc, the current one filled. With no
// labels and one button, this is the only thing telling you where you are.
static void pageDots() {
  const int n   = PAGE_COUNT;
  const int gap = 18;
  const int y   = cy + radius - 26;
  int x = cx - ((n - 1) * gap) / 2;
  for (int i = 0; i < n; ++i, x += gap) {
    if (i == g_page) M5.Display.fillCircle(x, y, 4, kAccent);
    else             M5.Display.drawCircle(x, y, 4, kAccent2);
  }
}

// Status line just under the centre of the face.
static void status(const char* msg, uint32_t color = kMuted) {
  const int dy = radius / 3;
  M5.Display.fillRect(0, cy + dy, M5.Display.width(), 40, kInk);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::FreeSans12pt7b);
  M5.Display.setTextSize(fitScale(msg, dy + 18));
  M5.Display.setTextColor(color, kInk);
  M5.Display.drawString(msg, cx, cy + dy + 18);
  M5.Display.setTextSize(1.0f);
  logf("[status] %s", msg);
}

static void title(const char* msg, uint32_t color = kPaper) {
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::FreeSansBold18pt7b);
  M5.Display.setTextSize(fitScale(msg, 0));
  M5.Display.setTextColor(color, kInk);
  M5.Display.drawString(msg, cx, cy);
  M5.Display.setTextSize(1.0f);
}

// Live level ring while recording: one spoke per chunk around the rim.
static void drawLevelRing(uint8_t level, int spoke) {
  const float a = (spoke % 60) * 6.0f * 3.14159265f / 180.0f;
  const int r0 = radius - 30;
  const int r1 = r0 - 10 - (level * (radius / 5)) / 255;
  M5.Display.drawLine(cx + (int)(cosf(a) * r0), cy + (int)(sinf(a) * r0),
                      cx + (int)(cosf(a) * r1), cy + (int)(sinf(a) * r1),
                      level > 24 ? kAccent : kAccent2);
}

// ------------------------------------------------------- transcript paging

// Text is wrapped to the circle's chord line by line, so the block narrows
// towards the top and bottom the way the glass does.
static constexpr int kTextLines = 9;
static constexpr int kLineH     = 30;
static constexpr int kMaxTPages = 8;

static String g_text;                        // last transcript
static size_t g_tstart[kMaxTPages] = {0};    // character offset of each page
static int    g_tpages = 1;

// Lays out one page starting at character `from`, optionally drawing it.
// Returns the offset the next page starts at.
static size_t layoutPage(const String& t, size_t from, bool render) {
  M5.Display.setFont(&fonts::FreeSans12pt7b);
  M5.Display.setTextSize(1.0f);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(kPaper, kInk);

  const int top = cy - (kTextLines * kLineH) / 2;
  size_t i = from;

  for (int line = 0; line < kTextLines; ++line) {
    while (i < t.length() && t[i] == ' ') ++i;
    if (i >= t.length()) return t.length();

    const int yc   = top + line * kLineH + kLineH / 2;
    const int maxw = (int)(2.0f * chordHalf(yc - cy)) - 28;

    // Longest run of whole words that still fits this line's chord.
    size_t fit = 0, probe = i;
    while (probe < t.length()) {
      size_t ws = probe;
      while (ws < t.length() && t[ws] != ' ') ++ws;
      if (M5.Display.textWidth(t.substring(i, ws)) > maxw) break;
      fit = ws;
      probe = ws + 1;
    }
    // A single word wider than the line still has to go somewhere.
    if (fit == 0) {
      fit = i + 1;
      while (fit < t.length() &&
             M5.Display.textWidth(t.substring(i, fit + 1)) <= maxw) ++fit;
    }

    if (render) M5.Display.drawString(t.substring(i, fit).c_str(), cx, yc);
    i = fit;
  }
  return i;
}

static void repaginate() {
  g_tpages = 1;
  g_tstart[0] = 0;
  if (!g_text.length()) return;

  size_t at = 0;
  while (g_tpages < kMaxTPages) {
    size_t next = layoutPage(g_text, at, false);
    if (next >= g_text.length() || next == at) break;
    g_tstart[g_tpages++] = next;
    at = next;
  }
  logf("[text] %u chars, %d page(s)", (unsigned)g_text.length(), g_tpages);
}

// ------------------------------------------------------------------- pages

static void drawBlurtPage() {
  faceClear();
  caption(kPageName[PAGE_BLURT]);
  title("AssemblyAI");
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.setTextSize(1.0f);
  M5.Display.setTextColor(kAccent, kInk);
  M5.Display.drawString("hold A and speak", cx, cy - 44);
  status(g_last_err[0] ? g_last_err : "ready",
         g_last_err[0] ? kError : kMuted);
  pageDots();
}

static void drawTranscriptPage() {
  faceClear();
  caption(kPageName[PAGE_TRANSCRIPT]);

  if (!g_text.length()) {
    title("No transcript", kMuted);
    status("hold A and speak");
  } else {
    if (g_tpage >= g_tpages) g_tpage = 0;
    layoutPage(g_text, g_tstart[g_tpage], true);
    if (g_tpages > 1) {
      char buf[16];
      snprintf(buf, sizeof buf, "%d / %d", g_tpage + 1, g_tpages);
      M5.Display.setTextDatum(middle_center);
      M5.Display.setFont(&fonts::FreeSans9pt7b);
      M5.Display.setTextSize(1.0f);
      M5.Display.setTextColor(kAccent, kInk);
      M5.Display.drawString(buf, cx, cy + radius - 52);
    }
  }
  pageDots();
}

// Left-aligned rows inside the square that fits the circle.
static void rows(const char* const* labels, const String* values, int n) {
  const int side = (int)(radius * 1.35f);
  const int left = cx - side / 2;
  const int top  = cy - (n * 26) / 2;

  M5.Display.setTextSize(1.0f);
  for (int i = 0; i < n; ++i) {
    const int y = top + i * 26;
    M5.Display.setTextDatum(top_left);
    M5.Display.setFont(&fonts::FreeSans9pt7b);
    M5.Display.setTextColor(kMuted, kInk);
    M5.Display.drawString(labels[i], left, y);
    M5.Display.setTextDatum(top_right);
    M5.Display.setTextColor(kPaper, kInk);
    M5.Display.drawString(values[i].c_str(), left + side, y);
  }
}

static void drawStatusPage() {
  faceClear();
  caption(kPageName[PAGE_STATUS]);

  const bool up = WiFi.status() == WL_CONNECTED;
  static const char* labels[] = {"wi-fi", "ssid", "ip", "rssi",
                                 "mic", "heap", "psram", "last"};
  String values[8] = {
    up ? "up" : "down",
    up ? WiFi.SSID() : String("-"),
    up ? WiFi.localIP().toString() : String("-"),
    up ? String(WiFi.RSSI()) + " dBm" : String("-"),
    M5.Mic.isEnabled() ? "on" : "off",
    String(ESP.getFreeHeap() / 1024) + "k",
    String(ESP.getFreePsram() / 1024) + "k",
    g_last_err[0] ? String(g_last_err) : String("ok"),
  };
  rows(labels, values, 8);
  pageDots();
}

static void drawLogPage() {
  faceClear();
  caption(kPageName[PAGE_LOG]);

  const int shown = g_log_count < kLogLines ? g_log_count : kLogLines;
  const int first = g_log_count - shown;
  const int side  = (int)(radius * 1.4f);
  const int left  = cx - side / 2;
  const int top   = cy - (shown * 22) / 2;

  M5.Display.setTextDatum(top_left);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.setTextSize(0.8f);
  M5.Display.setTextColor(kMuted, kInk);
  M5.Display.setClipRect(left, top - 4, side, shown * 22 + 8);
  for (int i = 0; i < shown; ++i) {
    M5.Display.drawString(g_log[(first + i) % kLogLines], left, top + i * 22);
  }
  M5.Display.clearClipRect();
  M5.Display.setTextSize(1.0f);
  pageDots();
}

static void drawPage() {
  switch (g_page) {
    case PAGE_TRANSCRIPT: drawTranscriptPage(); break;
    case PAGE_STATUS:     drawStatusPage();     break;
    case PAGE_LOG:        drawLogPage();        break;
    default:              drawBlurtPage();      break;
  }
}

// Blue button: walk the transcript's own pages first, then move on.
static void nextPage() {
  if (g_page == PAGE_TRANSCRIPT && g_tpage + 1 < g_tpages) {
    ++g_tpage;
  } else {
    g_page = (g_page + 1) % PAGE_COUNT;
    g_tpage = 0;
  }
  drawPage();
}

static void showPage(uint8_t p) {
  g_page = p;
  g_tpage = 0;
  drawPage();
}

// ---------------------------------------------------------------- capture

static uint8_t chunkPeak(const int16_t* buf, size_t n) {
  int32_t peak = 0;
  for (size_t i = 0; i < n; ++i) {
    int32_t v = abs(buf[i]);
    if (v > peak) peak = v;
  }
  int32_t level = peak / 48;
  return level > 255 ? 255 : (uint8_t)level;
}

// Records for as long as the yellow button is held, up to kMaxSeconds.
static void recordWhileHeld() {
  faceClear();
  title("Listening");
  status("release to send", kAccent);

  pcm_len = 0;
  int spoke = 0;
  while (M5.BtnA.isPressed() && pcm_len + kChunk <= kMaxSamples) {
    if (M5.Mic.record(pcm + pcm_len, kChunk, kSampleRate)) {
      drawLevelRing(chunkPeak(pcm + pcm_len, kChunk), spoke++);
      pcm_len += kChunk;
    }
    M5.update();
  }
  // record() queues two deep, so the last chunks are still being filled when
  // the button comes up. Let them land before we treat pcm_len as captured.
  for (int i = 0; i < 60 && M5.Mic.isRecording(); ++i) delay(5);

  logf("[rec] %u samples (%.1fs)", (unsigned)pcm_len,
       pcm_len / (float)kSampleRate);
}

// 44-byte canonical RIFF/WAVE header for 16-bit mono PCM.
static void wavHeader(uint8_t* h, uint32_t data_bytes) {
  const uint32_t byte_rate = kSampleRate * 2;
  memcpy(h, "RIFF", 4);
  *(uint32_t*)(h + 4) = 36 + data_bytes;
  memcpy(h + 8, "WAVEfmt ", 8);
  *(uint32_t*)(h + 16) = 16;          // fmt chunk size
  *(uint16_t*)(h + 20) = 1;           // PCM
  *(uint16_t*)(h + 22) = 1;           // mono
  *(uint32_t*)(h + 24) = kSampleRate;
  *(uint32_t*)(h + 28) = byte_rate;
  *(uint16_t*)(h + 32) = 2;           // block align
  *(uint16_t*)(h + 34) = 16;          // bits per sample
  memcpy(h + 36, "data", 4);
  *(uint32_t*)(h + 40) = data_bytes;
}

// ------------------------------------------------------------------- http

// Pulls one JSON string value out of a response stream without buffering the
// whole body. The transcript response carries per-word timings we don't need,
// so scanning beats parsing here.
static String scanField(Stream& in, const char* key, uint32_t timeout_ms = 15000) {
  String needle = String("\"") + key + "\":";
  size_t match = 0;
  String value;
  bool capturing = false, in_string = false, escaped = false;
  uint32_t deadline = millis() + timeout_ms;

  while (millis() < deadline) {
    if (!in.available()) { delay(2); continue; }
    char c = in.read();

    if (!capturing) {
      match = (c == needle[match]) ? match + 1 : (c == needle[0] ? 1 : 0);
      if (match == needle.length()) { capturing = true; }
      continue;
    }
    if (!in_string) {
      if (c == '"') { in_string = true; continue; }
      if (c == ',' || c == '}') return String();  // null or non-string value
      continue;
    }
    if (escaped) {
      switch (c) {                       // enough of JSON string escapes for text
        case 'n': value += '\n'; break;
        case 't': value += ' ';  break;
        case 'u': for (int i = 0; i < 4 && in.available(); ++i) in.read(); break;
        default:  value += c;    break;
      }
      escaped = false;
      continue;
    }
    if (c == '\\') { escaped = true; continue; }
    if (c == '"') return value;
    value += c;
  }
  return String();
}

static WiFiClientSecure& tls() {
  static WiFiClientSecure client;
  static bool ready = false;
  if (!ready) {
    // Demo shortcut: no certificate pinning. Before the 100-unit build this
    // should carry AssemblyAI's root CA so the device can't be MITM'd on
    // conference wifi.
    client.setInsecure();
    client.setTimeout(20);
    ready = true;
  }
  return client;
}

// POST the WAV to /v2/upload, return the upload_url it hands back.
static String uploadAudio() {
  const uint32_t data_bytes = pcm_len * 2;
  uint8_t header[44];
  wavHeader(header, data_bytes);

  // HTTPClient posts a single contiguous buffer, so the header and the samples
  // get joined in PSRAM rather than streamed separately.
  uint8_t* body = (uint8_t*)ps_malloc(sizeof(header) + data_bytes);
  if (!body) { failf("no psram for body"); return String(); }
  memcpy(body, header, sizeof(header));
  memcpy(body + sizeof(header), pcm, data_bytes);

  HTTPClient post;
  if (!post.begin(tls(), String(kApiHost) + "/v2/upload")) {
    free(body);
    failf("upload connect failed");
    return String();
  }
  post.addHeader("authorization", AAI_API_KEY);
  post.addHeader("content-type", "application/octet-stream");
  post.setTimeout(30000);
  int rc = post.POST(body, sizeof(header) + data_bytes);
  free(body);

  if (rc != 200) {
    failf("upload http %d", rc);
    post.end();
    return String();
  }
  String url = scanField(post.getStream(), "upload_url");
  post.end();
  logf("[upload] ok %u KB", (unsigned)((sizeof(header) + data_bytes) / 1024));
  return url;
}

// POST /v2/transcript, return the job id.
static String createTranscript(const String& audio_url) {
  HTTPClient http;
  if (!http.begin(tls(), String(kApiHost) + "/v2/transcript")) {
    failf("create connect failed");
    return String();
  }
  http.addHeader("authorization", AAI_API_KEY);
  http.addHeader("content-type", "application/json");
  http.setTimeout(20000);

  String body = String("{\"audio_url\":\"") + audio_url + "\"}";
  int rc = http.POST(body);
  if (rc != 200) {
    failf("create http %d", rc);
    http.end();
    return String();
  }
  String id = scanField(http.getStream(), "id");
  http.end();
  logf("[create] id=%s", id.c_str());
  return id;
}

// GET /v2/transcript/<id> until it is done. Returns the text, or "" on error.
static String pollTranscript(const String& id) {
  for (int attempt = 0; attempt < 40; ++attempt) {
    HTTPClient http;
    if (!http.begin(tls(), String(kApiHost) + "/v2/transcript/" + id)) {
      failf("poll connect failed");
      return String();
    }
    http.addHeader("authorization", AAI_API_KEY);
    http.setTimeout(20000);

    int rc = http.GET();
    if (rc != 200) {
      failf("poll http %d", rc);
      http.end();
      return String();
    }

    // status comes before text in the response, so one pass over the stream
    // gets both.
    Stream& s = http.getStream();
    String st = scanField(s, "status");
    if (st == "completed") {
      String text = scanField(s, "text");
      http.end();
      return text.length() ? text : String("(silence)");
    }
    if (st == "error") {
      http.end();
      failf("transcript error");
      return String();
    }
    http.end();

    char buf[32];
    snprintf(buf, sizeof buf, "transcribing %ds", attempt * 2);
    status(buf);
    delay(2000);
  }
  failf("poll timed out");
  return String();
}

// ------------------------------------------------------------------- flow

static void fail(const char* heading) {
  faceClear();
  caption(kPageName[PAGE_BLURT]);
  title(heading, kError);
  status(g_last_err, kError);
  pageDots();
}

static void blurt() {
  g_last_err[0] = '\0';

  if (WiFi.status() != WL_CONNECTED) {
    failf("wi-fi down");
    fail("No Wi-Fi");
    WiFi.reconnect();                 // stable network, so just nudge it
    return;
  }

  recordWhileHeld();

  if (pcm_len < kSampleRate / 2) {    // under half a second, ignore
    faceClear();
    caption(kPageName[PAGE_BLURT]);
    title("Too short");
    status("hold A and speak");
    pageDots();
    return;
  }

  faceClear();
  title("Sending");
  status("uploading");

  String url = uploadAudio();
  if (!url.length()) { fail("Upload failed"); return; }

  status("queued");
  String id = createTranscript(url);
  if (!id.length()) { fail("API error"); return; }

  String text = pollTranscript(id);
  if (!text.length()) { fail("No result"); return; }

  g_text = text;
  repaginate();
  showPage(PAGE_TRANSCRIPT);

  M5.Power.setVibration(180);
  delay(120);
  M5.Power.setVibration(0);
}

// A live level arc on the Blurt page, so the mic proves itself on the face:
// if this never moves, the ES8311 side is the problem and not the network.
static void drawIdleLevel() {
  static int16_t probe[kChunk];
  if (!M5.Mic.record(probe, kChunk, kSampleRate)) return;
  const uint8_t lvl = chunkPeak(probe, kChunk);
  const int w = M5.Display.width() / 3;
  const int h = 8;
  const int x = cx - w / 2;
  const int y = cy + radius / 3 + 48;
  M5.Display.fillRect(x, y, w, h, kInk);
  M5.Display.fillRoundRect(x, y, (w * lvl) / 255, h, h / 2,
                           lvl > 24 ? kAccent : kAccent2);
}

static const char* authName(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN:           return "open";
    case WIFI_AUTH_WEP:            return "wep";
    case WIFI_AUTH_WPA_PSK:        return "wpa";
    case WIFI_AUTH_WPA2_PSK:       return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK:   return "wpa/2";
    case WIFI_AUTH_WPA2_ENTERPRISE:return "enterprise";
    case WIFI_AUTH_WPA3_PSK:       return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK:  return "wpa2/3";
    default:                       return "other";
  }
}

// Venue SSIDs often carry a location suffix, so match on a prefix and join
// whatever the scan actually found rather than a guessed exact name.
static String findSsid() {
  const String prefix = String(WIFI_SSID_PREFIX);
  String want = prefix;
  want.toLowerCase();

  int n = WiFi.scanNetworks();
  logf("[wifi] %d networks", n);
  String best;
  int best_rssi = -1000;
  for (int i = 0; i < n; ++i) {
    String ssid = WiFi.SSID(i);
    String lower = ssid;
    lower.toLowerCase();
    // The whole scan goes to the log: this radio is 2.4 GHz only, so what it
    // can see here is the real list of candidates, whatever a laptop on 5 or
    // 6 GHz shows.
    logf("[scan] %s %d %s", ssid.c_str(), WiFi.RSSI(i),
         authName(WiFi.encryptionType(i)));
    if (lower.startsWith(want) && WiFi.RSSI(i) > best_rssi) {
      best = ssid;
      best_rssi = WiFi.RSSI(i);
    }
  }
  WiFi.scanDelete();
  if (!best.length()) {
    // No match means the configured name is wrong, not that the join failed.
    // Say so plainly rather than attempting a network that isn't there.
    failf("no ssid like %s", prefix.c_str());
    return String();
  }
  logf("[wifi] picked %s (%d)", best.c_str(), best_rssi);
  return best;
}

static void connectWifi() {
  faceClear();
  title("Wi-Fi");
  status("scanning");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);               // modem sleep adds seconds to the round trip
  WiFi.setAutoReconnect(true);        // the network is stable; stay on it

  String ssid = findSsid();
  if (!ssid.length()) { status(g_last_err, kError); return; }
  status(ssid.c_str(), kAccent);

  WiFi.begin(ssid.c_str(), WIFI_PASS);
  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; ++i) {
    drawLevelRing(200, i);
    delay(500);
  }
  if (WiFi.status() != WL_CONNECTED) {
    failf("no wi-fi: %s", ssid.c_str());
    return;
  }
  logf("[wifi] %s -> %s", ssid.c_str(), WiFi.localIP().toString().c_str());
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_mic = true;
  cfg.internal_spk = true;
  // Without this nothing ever calls Serial.begin() on this board: the core
  // guards its own call on !ARDUINO_USB_MODE and we build with USBMode=hwcdc,
  // so HWCDC stays uninitialised and every print is dropped.
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);

  cx = M5.Display.width() / 2;
  cy = M5.Display.height() / 2;
  radius = kFace / 2 - 16;
  M5.Display.setBrightness(180);

  pcm = (int16_t*)ps_malloc(kMaxSamples * sizeof(int16_t));
  if (!pcm) {
    faceClear();
    title("No PSRAM", kError);
    while (true) delay(1000);
  }

  logf("[boot] board=%d %dx%d psram=%uk", (int)M5.getBoard(),
       M5.Display.width(), M5.Display.height(),
       (unsigned)(ESP.getFreePsram() / 1024));

  M5.Mic.begin();
  connectWifi();
  showPage(PAGE_BLURT);
}

void loop() {
  M5.update();

  if (M5.BtnA.wasPressed()) blurt();        // reports its own errors
  if (M5.BtnB.wasPressed()) nextPage();
  if (g_page == PAGE_BLURT) drawIdleLevel();

  static uint32_t next_report = 0;
  if (millis() > next_report) {
    next_report = millis() + 5000;
    Serial.printf("[hb] wifi=%d rssi=%d mic=%d heap=%u psram=%u page=%s\n",
                  WiFi.status() == WL_CONNECTED, (int)WiFi.RSSI(),
                  M5.Mic.isEnabled(), (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getFreePsram(), kPageName[g_page]);
  }
  delay(10);
}
