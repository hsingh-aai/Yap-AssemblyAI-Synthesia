// Pocket Voice - Phase 3: live captions.
//
// Press the yellow button and talk. Audio streams to AssemblyAI's realtime
// endpoint 50 ms at a time and the words appear on the face as you say them,
// partial turns in grey firming up to white when the turn closes. Press again
// to stop.
//
// This is the mode the deck calls Blurt. Phase 2 proved the round trip with
// the pre-recorded endpoints; this replaces the record-then-upload-then-poll
// shape with one socket carrying audio up and text down.
//
// Board: M5Stack StopWatch Dev Kit (C152), ESP32-S3R8.

#include <M5Unified.h>
#include <WiFi.h>
#include <WebSocketsClient.h>

#include "secrets.h"

// Deck palette.
static constexpr uint32_t kInk     = 0x131210;
static constexpr uint32_t kPaper   = 0xF3EFE6;
static constexpr uint32_t kMuted   = 0xB8B2A7;
static constexpr uint32_t kAccent  = 0xA99CFF;
static constexpr uint32_t kAccent2 = 0x3F2BC1;
static constexpr uint32_t kError   = 0xE86A5C;
static constexpr uint32_t kLive    = 0xE8625C;

// Streaming endpoint. The API key goes in an Authorization header with no
// Bearer prefix; the token query parameter is only for browsers, which cannot
// set headers.
static constexpr char kStreamHost[] = "streaming.assemblyai.com";
static constexpr char kStreamPath[] =
    "/v3/ws?sample_rate=16000&encoding=pcm_s16le&format_turns=true";

// Audio. 50 ms per frame is the low end of the recommended 50-1000 ms range,
// which is what keeps the captions feeling immediate.
static constexpr uint32_t kSampleRate  = 16000;
static constexpr size_t   kStreamChunk = 800;      // samples == 50 ms
static constexpr int      kBufs        = 3;
static int16_t mic_buf[kBufs][kStreamChunk];

// The panel reports 468x468 but the visible glass is a 466 px circle: centre
// on the panel, size to the glass.
static constexpr int kFace = 466;
static int cx, cy, radius;

// ------------------------------------------------------------------ logging

// Serial and the Log page carry the same lines. Serial only works because
// setup() sets cfg.serial_baudrate - with USBMode=hwcdc nothing else ever
// calls Serial.begin(), so HWCDC stays uninitialised and drops every print.
static constexpr int kLogLines = 11;
static char g_log[kLogLines][52];
static int  g_log_count = 0;
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

static float chordHalf(int dy) {
  const float d = (float)radius * radius - (float)dy * dy;
  return d > 1.0f ? sqrtf(d) : 1.0f;
}

static float fitScale(const char* s, int dy, float cap = 1.0f) {
  M5.Display.setTextSize(1.0f);
  const float room = 2.0f * chordHalf(dy) - 24.0f;
  const int   w    = M5.Display.textWidth(s);
  if (w <= 0) return cap;
  const float scale = room / (float)w;
  return scale > cap ? cap : (scale < 0.45f ? 0.45f : scale);
}

// ------------------------------------------------------------------- state

enum PageId : uint8_t { PAGE_CAPTIONS = 0, PAGE_STATUS, PAGE_LOG, PAGE_COUNT };
static const char* kPageName[PAGE_COUNT] = {"CAPTIONS", "STATUS", "LOG"};

static uint8_t g_page = PAGE_CAPTIONS;
static bool    g_streaming = false;
static bool    g_connected = false;

static String g_final;     // turns the server has closed
static String g_partial;   // the turn still being spoken
static int    g_turns = 0;

// ---------------------------------------------------------------- chrome

static void faceClear() {
  M5.Display.fillScreen(kInk);
  M5.Display.drawCircle(cx, cy, radius - 4, kAccent);
}

static void caption(const char* name) {
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.setTextSize(1.0f);
  M5.Display.setTextColor(kMuted, kInk);
  M5.Display.drawString(name, cx, cy - radius + 30);
}

static void pageDots() {
  const int gap = 18;
  const int y   = cy + radius - 26;
  int x = cx - ((PAGE_COUNT - 1) * gap) / 2;
  for (int i = 0; i < PAGE_COUNT; ++i, x += gap) {
    if (i == g_page) M5.Display.fillCircle(x, y, 4, kAccent);
    else             M5.Display.drawCircle(x, y, 4, kAccent2);
  }
}

static void title(const char* msg, uint32_t color = kPaper) {
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::FreeSansBold18pt7b);
  M5.Display.setTextSize(fitScale(msg, 0));
  M5.Display.setTextColor(color, kInk);
  M5.Display.drawString(msg, cx, cy);
  M5.Display.setTextSize(1.0f);
}

// --------------------------------------------------------- caption layout

static constexpr int kTextLines = 8;
static constexpr int kLineH     = 30;

static int lineY(int i) {
  return cy - (kTextLines * kLineH) / 2 + i * kLineH + kLineH / 2;
}

// Wipes exactly the chord each line occupies. A plain rectangle over the whole
// block would clip the ring where the circle narrows.
static void clearTextBand() {
  for (int i = 0; i < kTextLines; ++i) {
    const int yc = lineY(i);
    const int w  = (int)(2.0f * chordHalf(yc - cy)) - 20;
    M5.Display.fillRect(cx - w / 2, yc - kLineH / 2, w, kLineH, kInk);
  }
}

// Lays out one page of `t` starting at `from`, wrapping each line to the
// circle's chord at that line's height. Returns where the next page starts.
static size_t layoutPage(const String& t, size_t from, bool render,
                         uint32_t color = kPaper, size_t grey_from = SIZE_MAX) {
  M5.Display.setFont(&fonts::FreeSans12pt7b);
  M5.Display.setTextSize(1.0f);
  M5.Display.setTextDatum(middle_center);

  size_t i = from;
  for (int line = 0; line < kTextLines; ++line) {
    while (i < t.length() && t[i] == ' ') ++i;
    if (i >= t.length()) return t.length();

    const int yc   = lineY(line);
    const int maxw = (int)(2.0f * chordHalf(yc - cy)) - 28;

    size_t fit = 0, probe = i;
    while (probe < t.length()) {
      size_t ws = probe;
      while (ws < t.length() && t[ws] != ' ') ++ws;
      if (M5.Display.textWidth(t.substring(i, ws)) > maxw) break;
      fit = ws;
      probe = ws + 1;
    }
    if (fit == 0) {  // one word wider than the line still has to go somewhere
      fit = i + 1;
      while (fit < t.length() &&
             M5.Display.textWidth(t.substring(i, fit + 1)) <= maxw) ++fit;
    }
    if (render) {
      // A line that starts inside the in-flight turn is drawn grey, so you can
      // see the difference between what the model has committed to and what it
      // is still revising.
      M5.Display.setTextColor(i >= grey_from ? kMuted : color, kInk);
      M5.Display.drawString(t.substring(i, fit).c_str(), cx, yc);
    }
    i = fit;
  }
  return i;
}

// Captions scroll: what matters is the newest text, so show the last page.
static void drawCaptions() {
  const String all = g_final + g_partial;
  size_t start = 0, at = 0;
  for (int guard = 0; guard < 64; ++guard) {
    size_t next = layoutPage(all, at, false);
    if (next >= all.length() || next == at) break;
    start = next;
    at = next;
  }
  clearTextBand();
  if (all.length()) layoutPage(all, start, true, kPaper, g_final.length());
}

// ------------------------------------------------------------------- pages

static void liveDot() {
  M5.Display.fillCircle(cx - 34, cy - radius + 62, 6,
                        g_streaming ? (g_connected ? kLive : kAccent2) : kInk);
  M5.Display.setTextDatum(middle_left);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.setTextSize(1.0f);
  M5.Display.setTextColor(g_streaming ? kLive : kInk, kInk);
  M5.Display.drawString("LIVE", cx - 20, cy - radius + 62);
}

static void drawCaptionsPage() {
  faceClear();
  caption(kPageName[PAGE_CAPTIONS]);
  if (!g_streaming && !g_final.length() && !g_partial.length()) {
    title("Live captions");
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSans9pt7b);
    M5.Display.setTextColor(kAccent, kInk);
    M5.Display.drawString("press A to start", cx, cy + 54);
  } else {
    drawCaptions();
  }
  liveDot();
  pageDots();
}

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
  static const char* labels[] = {"wi-fi", "ip", "rssi", "socket",
                                 "turns", "heap", "last"};
  String values[7] = {
    up ? "up" : "down",
    up ? WiFi.localIP().toString() : String("-"),
    up ? String(WiFi.RSSI()) + " dBm" : String("-"),
    g_connected ? "open" : (g_streaming ? "opening" : "closed"),
    String(g_turns),
    String(ESP.getFreeHeap() / 1024) + "k",
    g_last_err[0] ? String(g_last_err) : String("ok"),
  };
  rows(labels, values, 7);
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
    case PAGE_STATUS: drawStatusPage();   break;
    case PAGE_LOG:    drawLogPage();      break;
    default:          drawCaptionsPage(); break;
  }
}

// -------------------------------------------------------------------- json

// The messages we care about are small and already in RAM, so a scan beats
// pulling in a parser for three fields.
static String jsonStr(const String& src, const char* key) {
  const String needle = String("\"") + key + "\":\"";
  int i = src.indexOf(needle);
  if (i < 0) return String();
  i += needle.length();
  String out;
  while (i < (int)src.length()) {
    const char c = src[i++];
    if (c == '\\') {
      if (i >= (int)src.length()) break;
      const char e = src[i++];
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += ' ';  break;
        case 'u': i += 4;      break;
        default:  out += e;    break;
      }
      continue;
    }
    if (c == '"') break;
    out += c;
  }
  return out;
}

// "end_of_turn": matches only the exact key, so end_of_turn_confidence (which
// shares the prefix) cannot be read by mistake.
static bool jsonTrue(const String& src, const char* key) {
  const String needle = String("\"") + key + "\":";
  const int i = src.indexOf(needle);
  if (i < 0) return false;
  return src.substring(i + needle.length(), i + needle.length() + 4) == "true";
}

// --------------------------------------------------------------- websocket

static WebSocketsClient ws;
static bool     g_dirty = false;
static uint32_t g_last_draw = 0;

// Keep the running caption bounded, trimming on a word boundary so the text
// never restarts mid-word.
static void appendFinal(const String& turn) {
  g_final += turn;
  g_final += " ";
  if (g_final.length() > 600) {
    int cut = g_final.indexOf(' ', g_final.length() - 400);
    if (cut > 0) g_final = g_final.substring(cut + 1);
  }
}

static void onMessage(const String& msg) {
  const String type = jsonStr(msg, "type");

  if (type == "Begin") {
    g_connected = true;
    logf("[ws] session %s", jsonStr(msg, "id").substring(0, 8).c_str());
    g_dirty = true;
    return;
  }
  if (type == "Turn") {
    const String text = jsonStr(msg, "transcript");
    if (jsonTrue(msg, "end_of_turn")) {
      if (text.length()) {
        appendFinal(text);
        ++g_turns;
        logf("[turn] %s", text.c_str());   // this is the relay to the Mac
      }
      g_partial = "";
    } else {
      g_partial = text;
    }
    g_dirty = true;
    return;
  }
  if (type == "Termination") {
    logf("[ws] terminated");
    g_connected = false;
    return;
  }
  if (type == "Error" || msg.indexOf("\"error\"") >= 0) {
    failf("ws: %s", jsonStr(msg, "error").substring(0, 40).c_str());
  }
}

static void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      logf("[ws] connected");
      break;
    case WStype_DISCONNECTED:
      if (g_connected) logf("[ws] closed");
      g_connected = false;
      break;
    case WStype_TEXT:
      onMessage(String((const char*)payload, length));
      break;
    case WStype_ERROR:
      failf("ws error");
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------- session

static void redrawIfDirty() {
  if (!g_dirty || millis() - g_last_draw < 160) return;
  g_dirty = false;
  g_last_draw = millis();
  if (g_page == PAGE_CAPTIONS) {
    drawCaptions();
    liveDot();
  }
}

static void captionSession() {
  if (WiFi.status() != WL_CONNECTED) {
    failf("wi-fi down");
    drawPage();
    return;
  }

  g_final = "";
  g_partial = "";
  g_streaming = true;
  g_connected = false;
  g_last_err[0] = '\0';

  faceClear();
  caption(kPageName[PAGE_CAPTIONS]);
  title("Connecting");
  liveDot();
  pageDots();

  ws.onEvent(onWsEvent);
  ws.setExtraHeaders("Authorization: " AAI_API_KEY);
  ws.beginSSL(kStreamHost, 443, kStreamPath);

  const uint32_t deadline = millis() + 12000;
  while (!g_connected && millis() < deadline) {
    ws.loop();
    delay(10);
  }
  if (!g_connected) {
    failf("no stream socket");
    g_streaming = false;
    ws.disconnect();
    drawPage();
    return;
  }

  faceClear();
  caption(kPageName[PAGE_CAPTIONS]);
  liveDot();
  pageDots();

  // Prime both mic slots, then ride record()'s own back-pressure: it returns
  // only when a slot frees, which is exactly when the oldest buffer is full.
  // Three buffers means two are always queued while the third is on the wire.
  M5.Mic.record(mic_buf[0], kStreamChunk, kSampleRate);
  M5.Mic.record(mic_buf[1], kStreamChunk, kSampleRate);

  int cur = 0;
  uint32_t sent = 0;
  while (g_streaming) {
    ws.loop();
    M5.update();
    if (M5.BtnA.wasPressed()) break;
    if (M5.BtnB.wasPressed()) {
      g_page = (g_page + 1) % PAGE_COUNT;
      drawPage();
    }

    const int next = (cur + 2) % kBufs;
    if (!M5.Mic.record(mic_buf[next], kStreamChunk, kSampleRate)) {
      failf("mic stalled");
      break;
    }
    if (!ws.sendBIN((uint8_t*)mic_buf[cur], kStreamChunk * sizeof(int16_t))) {
      failf("send failed at %us", (unsigned)(sent / 20));
      break;
    }
    ++sent;
    cur = (cur + 1) % kBufs;

    ws.loop();
    redrawIfDirty();
  }

  ws.sendTXT("{\"type\":\"Terminate\"}");
  for (int i = 0; i < 40; ++i) { ws.loop(); delay(10); }
  ws.disconnect();

  g_streaming = false;
  g_connected = false;
  logf("[ws] %u frames (%.1fs), %d turns", (unsigned)sent, sent * 0.05f, g_turns);
  drawPage();
}

// ------------------------------------------------------------------- wifi

static const char* authName(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "wep";
    case WIFI_AUTH_WPA_PSK:         return "wpa";
    case WIFI_AUTH_WPA2_PSK:        return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "wpa/2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "enterprise";
    case WIFI_AUTH_WPA3_PSK:        return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "wpa2/3";
    default:                        return "other";
  }
}

// Matched as a prefix, strongest match wins. Give it enough of the name to be
// unambiguous: a venue running Guest and Employee SSIDs will happily hand you
// the wrong one over a 1 dBm difference.
static String findSsid() {
  const String prefix = String(WIFI_SSID_PREFIX);
  String want = prefix;
  want.toLowerCase();

  const int n = WiFi.scanNetworks();
  logf("[wifi] %d networks", n);
  String best;
  int best_rssi = -1000;
  for (int i = 0; i < n; ++i) {
    const String ssid = WiFi.SSID(i);
    String lower = ssid;
    lower.toLowerCase();
    logf("[scan] %s %d %s", ssid.c_str(), WiFi.RSSI(i),
         authName(WiFi.encryptionType(i)));
    if (lower.startsWith(want) && WiFi.RSSI(i) > best_rssi) {
      best = ssid;
      best_rssi = WiFi.RSSI(i);
    }
  }
  WiFi.scanDelete();
  if (!best.length()) { failf("no ssid like %s", prefix.c_str()); return String(); }
  logf("[wifi] picked %s (%d)", best.c_str(), best_rssi);
  return best;
}

static void connectWifi() {
  faceClear();
  title("Wi-Fi");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // modem sleep shows up directly as caption lag
  WiFi.setAutoReconnect(true);

  const String ssid = findSsid();
  if (!ssid.length()) return;

  WiFi.begin(ssid.c_str(), WIFI_PASS);
  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; ++i) delay(500);
  if (WiFi.status() != WL_CONNECTED) { failf("no wi-fi: %s", ssid.c_str()); return; }
  logf("[wifi] %s -> %s", ssid.c_str(), WiFi.localIP().toString().c_str());
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_mic = true;
  cfg.internal_spk = true;
  cfg.serial_baudrate = 115200;   // nothing else begins Serial with USBMode=hwcdc
  M5.begin(cfg);

  cx = M5.Display.width() / 2;
  cy = M5.Display.height() / 2;
  radius = kFace / 2 - 16;
  M5.Display.setBrightness(180);

  logf("[boot] %dx%d heap=%uk", M5.Display.width(), M5.Display.height(),
       (unsigned)(ESP.getFreeHeap() / 1024));

  M5.Mic.begin();
  connectWifi();
  drawPage();
}

void loop() {
  M5.update();

  if (M5.BtnA.wasPressed()) captionSession();   // blocks until stopped
  if (M5.BtnB.wasPressed()) {
    g_page = (g_page + 1) % PAGE_COUNT;
    drawPage();
  }

  static uint32_t next_report = 0;
  if (millis() > next_report) {
    next_report = millis() + 5000;
    Serial.printf("[hb] wifi=%d rssi=%d heap=%u turns=%d page=%s\n",
                  WiFi.status() == WL_CONNECTED, (int)WiFi.RSSI(),
                  (unsigned)ESP.getFreeHeap(), g_turns, kPageName[g_page]);
  }
  delay(10);
}
