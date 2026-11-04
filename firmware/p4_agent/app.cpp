// Pocket Voice - Phase 4: a voice agent you can talk to.
//
// Tap the screen and talk to Jack the Roaster over AssemblyAI's Voice Agent
// API. Mic audio goes up as base64 PCM16, the agent's reply comes back the
// same way and plays through the ES8311 speaker.
//
// Half duplex, and not by choice: M5Unified puts the mic on I2S_NUM_1 and the
// speaker on I2S_NUM_0 but wires both to the same BCK (17) and WS (15) pins,
// at different rates. Two peripherals cannot drive those pins at once, so the
// firmware hands the pins over explicitly - Mic.end() before the speaker
// starts, Speaker.end() before the mic comes back. That rules out listening
// while the agent talks, so barge-in is a tap on the screen instead of a
// voice interruption. (Any WebRTC stack would hit the same wall here.)
//
// Board: M5Stack StopWatch Dev Kit (C152), ESP32-S3R8.

#include <M5Unified.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <mbedtls/base64.h>

#include "secrets.h"

// Deck palette.
static constexpr uint32_t kInk     = 0x131210;
static constexpr uint32_t kPaper   = 0xF3EFE6;
static constexpr uint32_t kMuted   = 0xB8B2A7;
static constexpr uint32_t kAccent  = 0xA99CFF;
static constexpr uint32_t kAccent2 = 0x3F2BC1;
static constexpr uint32_t kError   = 0xE86A5C;
static constexpr uint32_t kLive    = 0xE8625C;

static constexpr char kAgentHost[] = "agents.assemblyai.com";
static constexpr char kAgentPath[] = "/v1/ws";

// Jack the Roaster, from the Halloween station repo. Not a secret - stored
// agents are referenced by id and the persona lives on AssemblyAI's side.
static constexpr char kAgentId[] = "agent_921917929cb944e19960ffdfbd53f217";

// audio/pcm on this API is PCM16 mono at 24 kHz, both directions.
static constexpr uint32_t kRate      = 24000;
static constexpr size_t   kMicChunk  = 1200;   // 50 ms
static constexpr int      kMicBufs   = 3;
static int16_t mic_buf[kMicBufs][kMicChunk];

// Replies arrive as ~11 ms fragments. Collect them in PSRAM and hand the
// speaker 100 ms at a time: one playRaw per fragment would thrash the queue.
static constexpr size_t kReplyMax  = kRate * 30;   // 30 s ceiling
static constexpr size_t kPlayBlock = kRate / 10;   // 100 ms
static int16_t* reply_pcm = nullptr;
static volatile size_t reply_len = 0;      // samples decoded
static size_t reply_played = 0;            // samples handed to the speaker
static volatile bool reply_complete = false;

static constexpr int kFace = 466;
static int cx, cy, radius;

// ------------------------------------------------------------------ logging

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

enum PageId : uint8_t { PAGE_AGENT = 0, PAGE_STATUS, PAGE_LOG, PAGE_COUNT };
static const char* kPageName[PAGE_COUNT] = {"JACK", "STATUS", "LOG"};

enum Phase : uint8_t { IDLE, CONNECTING, LISTENING, SPEAKING };
static const char* kPhaseName[] = {"idle", "connecting", "listening", "speaking"};

static uint8_t g_page  = PAGE_AGENT;
static uint8_t g_phase = IDLE;
static bool    g_session = false;    // socket open and ready
static bool    g_dirty = false;
static uint32_t g_last_draw = 0;

static String g_user;     // what Jack heard from you
static String g_agent;    // what Jack said back
static int    g_replies = 0;

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

// --------------------------------------------------------------- text band

static constexpr int kTextLines = 6;
static constexpr int kLineH     = 30;

static int lineY(int i) {
  return cy - (kTextLines * kLineH) / 2 + i * kLineH + kLineH / 2 - 10;
}

static void clearTextBand() {
  for (int i = 0; i < kTextLines; ++i) {
    const int yc = lineY(i);
    const int w  = (int)(2.0f * chordHalf(yc - cy)) - 20;
    M5.Display.fillRect(cx - w / 2, yc - kLineH / 2, w, kLineH, kInk);
  }
}

static size_t layoutPage(const String& t, size_t from, bool render,
                         uint32_t color = kPaper) {
  M5.Display.setFont(&fonts::FreeSans12pt7b);
  M5.Display.setTextSize(1.0f);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(color, kInk);

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

// Shows the tail of `t`, which is the part that just arrived.
static void drawTail(const String& t, uint32_t color) {
  size_t start = 0, at = 0;
  for (int guard = 0; guard < 64; ++guard) {
    const size_t next = layoutPage(t, at, false);
    if (next >= t.length() || next == at) break;
    start = next;
    at = next;
  }
  clearTextBand();
  if (t.length()) layoutPage(t, start, true, color);
}

// ------------------------------------------------------------- agent page

// One line of plain instruction for whatever the device wants you to do next.
static void hint(const char* msg, uint32_t color = kAccent) {
  const int y = cy + radius - 58;
  const int w = (int)(2.0f * chordHalf(y - cy)) - 16;
  M5.Display.fillRect(cx - w / 2, y - 16, w, 32, kInk);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.setTextSize(1.0f);
  M5.Display.setTextColor(color, kInk);
  M5.Display.drawString(msg, cx, y);
}

static void stateDot() {
  const uint32_t c = g_phase == SPEAKING  ? kLive
                   : g_phase == LISTENING ? kAccent
                   : g_phase == CONNECTING ? kAccent2 : kInk;
  M5.Display.fillCircle(cx - 44, cy - radius + 62, 6, c);
  M5.Display.setTextDatum(middle_left);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.setTextSize(1.0f);
  M5.Display.setTextColor(g_phase == IDLE ? kInk : c, kInk);
  M5.Display.drawString(g_phase == SPEAKING ? "JACK" : "YOU", cx - 30,
                        cy - radius + 62);
}

static void drawAgentBody() {
  switch (g_phase) {
    case IDLE:
      clearTextBand();
      title("Talk to Jack");
      hint("tap the screen to start");
      break;
    case CONNECTING:
      clearTextBand();
      title("Connecting");
      hint("waking Jack up", kMuted);
      break;
    case LISTENING:
      drawTail(g_user.length() ? g_user : String("listening..."),
               g_user.length() ? kPaper : kMuted);
      hint("just talk  -  tap to end");
      break;
    case SPEAKING:
      drawTail(g_agent.length() ? g_agent : String("..."), kPaper);
      hint("tap to interrupt", kLive);
      break;
  }
  stateDot();
}

static void drawAgentPage() {
  faceClear();
  caption(kPageName[PAGE_AGENT]);
  drawAgentBody();
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
  static const char* labels[] = {"wi-fi", "ip", "rssi", "session",
                                 "phase", "touch", "replies", "last"};
  String values[8] = {
    up ? "up" : "down",
    up ? WiFi.localIP().toString() : String("-"),
    up ? String(WiFi.RSSI()) + " dBm" : String("-"),
    g_session ? "ready" : "closed",
    String(kPhaseName[g_phase]),
    M5.Touch.isEnabled() ? "on" : "off",
    String(g_replies),
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
    case PAGE_STATUS: drawStatusPage(); break;
    case PAGE_LOG:    drawLogPage();    break;
    default:          drawAgentPage();  break;
  }
}

static void redrawIfDirty() {
  if (!g_dirty || millis() - g_last_draw < 160) return;
  g_dirty = false;
  g_last_draw = millis();
  if (g_page == PAGE_AGENT) drawAgentBody();
}

// -------------------------------------------------------------------- json

// Messages arrive ~100/s, so this works on the raw buffer rather than
// building Strings for fields we are going to throw away.
static const char* findVal(const char* msg, const char* key, size_t* len) {
  char needle[40];
  snprintf(needle, sizeof needle, "\"%s\":\"", key);
  const char* p = strstr(msg, needle);
  if (!p) return nullptr;
  p += strlen(needle);
  const char* q = p;
  while (*q && *q != '"') {               // values here carry no escapes
    if (*q == '\\' && q[1]) ++q;
    ++q;
  }
  *len = (size_t)(q - p);
  return p;
}

static String fieldStr(const char* msg, const char* key) {
  size_t n = 0;
  const char* p = findVal(msg, key, &n);
  if (!p) return String();
  String out;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    if (p[i] == '\\' && i + 1 < n) {      // \" and \' are all we expect
      ++i;
      out += p[i];
      continue;
    }
    out += p[i];
  }
  return out;
}

// --------------------------------------------------------------- websocket

static WebSocketsClient ws;

static void startSpeaking() {
  if (g_phase == SPEAKING) return;
  // Hand the shared I2S clock pins to the speaker.
  M5.Mic.end();
  M5.Speaker.begin();
  M5.Speaker.setVolume(200);
  reply_len = 0;
  reply_played = 0;
  reply_complete = false;
  g_agent = "";
  g_phase = SPEAKING;
  g_dirty = true;
}

static void startListening() {
  M5.Speaker.stop();
  M5.Speaker.end();
  M5.Mic.begin();
  g_user = "";
  g_phase = LISTENING;
  g_dirty = true;
}

static void onText(char* msg) {
  const String type = fieldStr(msg, "type");

  if (type == "session.ready") {
    g_session = true;
    logf("[agent] ready %s", fieldStr(msg, "session_id").substring(0, 10).c_str());
    startListening();
    return;
  }
  if (type == "reply.audio") {
    size_t n = 0;
    const char* b64 = findVal(msg, "data", &n);
    if (!b64 || !reply_pcm) return;
    if (g_phase != SPEAKING) startSpeaking();
    uint8_t* dst = (uint8_t*)(reply_pcm + reply_len);
    const size_t room = (kReplyMax - reply_len) * sizeof(int16_t);
    size_t out = 0;
    if (mbedtls_base64_decode(dst, room, &out, (const unsigned char*)b64, n) == 0) {
      reply_len += out / sizeof(int16_t);
    }
    return;
  }
  if (type == "reply.started") { startSpeaking(); return; }
  if (type == "reply.done") {
    reply_complete = true;
    ++g_replies;
    logf("[jack] %s", g_agent.substring(0, 40).c_str());
    return;
  }
  if (type == "transcript.agent") {
    g_agent = fieldStr(msg, "text");
    g_dirty = true;
    return;
  }
  if (type == "transcript.user" || type == "transcript.user.delta") {
    const String t = fieldStr(msg, "text");
    if (t.length()) { g_user = t; g_dirty = true; }
    if (type == "transcript.user") logf("[you ] %s", t.substring(0, 40).c_str());
    return;
  }
  if (type == "session.error" || type == "error") {
    failf("%s", fieldStr(msg, "message").substring(0, 40).c_str());
    g_dirty = true;
    return;
  }
}

static void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  static char msg[2048];
  switch (type) {
    case WStype_CONNECTED: {
      logf("[ws] connected");
      // Bind the session to the stored agent. agent_id must be the only
      // field in `session` - it is mutually exclusive with inline config.
      char buf[160];
      snprintf(buf, sizeof buf,
               "{\"type\":\"session.update\",\"session\":{\"agent_id\":\"%s\"}}",
               kAgentId);
      ws.sendTXT(buf);
      break;
    }
    case WStype_DISCONNECTED:
      if (g_session) logf("[ws] closed");
      g_session = false;
      break;
    case WStype_TEXT: {
      const size_t n = length < sizeof(msg) - 1 ? length : sizeof(msg) - 1;
      memcpy(msg, payload, n);
      msg[n] = '\0';
      onText(msg);
      break;
    }
    case WStype_ERROR:
      failf("ws error");
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------- session

// True on a screen tap or a press of either button - every way of saying
// "do the thing" leads here, so the on-screen hint only has to mention one.
static bool poked() {
  if (M5.BtnA.wasPressed()) return true;
  if (M5.Touch.isEnabled()) {
    const auto n = M5.Touch.getCount();
    for (size_t i = 0; i < n; ++i) {
      if (M5.Touch.getDetail(i).wasPressed()) return true;
    }
  }
  return false;
}

// Feed the speaker in blocks, so one playRaw per 11 ms fragment doesn't
// thrash the queue. Returns true once the whole reply has been handed over.
static bool pumpSpeaker() {
  const size_t have = reply_len;
  while (have - reply_played >= kPlayBlock) {
    if (!M5.Speaker.playRaw(reply_pcm + reply_played, kPlayBlock, kRate,
                            false, 1, -1, false)) {
      return false;                       // queue full, come back next loop
    }
    reply_played += kPlayBlock;
  }
  if (reply_complete && reply_played < have) {
    if (M5.Speaker.playRaw(reply_pcm + reply_played, have - reply_played,
                           kRate, false, 1, -1, false)) {
      reply_played = have;
    }
  }
  return reply_complete && reply_played >= have;
}

static void agentSession() {
  if (WiFi.status() != WL_CONNECTED) { failf("wi-fi down"); drawPage(); return; }

  g_phase = CONNECTING;
  g_session = false;
  g_last_err[0] = '\0';
  g_user = "";
  g_agent = "";
  drawPage();

  ws.onEvent(onWsEvent);
  ws.setExtraHeaders("Authorization: Bearer " AAI_API_KEY);
  ws.beginSSL(kAgentHost, 443, kAgentPath);

  const uint32_t deadline = millis() + 15000;
  while (!g_session && millis() < deadline) {
    ws.loop();
    M5.update();
    if (poked()) { ws.disconnect(); g_phase = IDLE; drawPage(); return; }
    delay(10);
  }
  if (!g_session) {
    failf("no agent socket");
    g_phase = IDLE;
    ws.disconnect();
    drawPage();
    return;
  }

  // Mic ping-pong, same back-pressure trick as the captions build: record()
  // returns only when a slot frees, which is when the oldest buffer is full.
  static uint8_t b64[kMicChunk * 2 * 4 / 3 + 8];
  static char    frame[sizeof(b64) + 64];
  int cur = 0;
  bool primed = false;

  while (true) {
    ws.loop();
    M5.update();

    if (poked()) {
      if (g_phase == SPEAKING) {
        // Our barge-in: the mic is off while the speaker owns the pins, so
        // the server's own interrupt path can never fire here.
        logf("[jack] interrupted");
        M5.Speaker.stop();
        startListening();
        primed = false;
        continue;
      }
      break;                               // tap while listening ends the chat
    }

    if (g_page == PAGE_AGENT) redrawIfDirty();

    if (g_phase == SPEAKING) {
      if (pumpSpeaker() && !M5.Speaker.isPlaying()) {
        startListening();
        primed = false;
      }
      delay(2);
      continue;
    }

    if (g_phase != LISTENING) { delay(5); continue; }

    if (!primed) {
      M5.Mic.record(mic_buf[0], kMicChunk, kRate);
      M5.Mic.record(mic_buf[1], kMicChunk, kRate);
      cur = 0;
      primed = true;
    }

    const int next = (cur + 2) % kMicBufs;
    if (!M5.Mic.record(mic_buf[next], kMicChunk, kRate)) { failf("mic stalled"); break; }

    size_t n = 0;
    if (mbedtls_base64_encode(b64, sizeof b64, &n,
                              (const unsigned char*)mic_buf[cur],
                              kMicChunk * sizeof(int16_t)) != 0) {
      failf("b64 encode");
      break;
    }
    const int len = snprintf(frame, sizeof frame,
                             "{\"type\":\"input.audio\",\"audio\":\"%.*s\"}",
                             (int)n, (const char*)b64);
    if (!ws.sendTXT((uint8_t*)frame, len)) { failf("send failed"); break; }
    cur = (cur + 1) % kMicBufs;
  }

  ws.sendTXT("{\"type\":\"session.end\"}");
  for (int i = 0; i < 30; ++i) { ws.loop(); delay(10); }
  ws.disconnect();

  M5.Speaker.stop();
  M5.Speaker.end();
  M5.Mic.begin();
  g_session = false;
  g_phase = IDLE;
  logf("[agent] done, %d replies", g_replies);
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
  WiFi.setSleep(false);
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

  reply_pcm = (int16_t*)ps_malloc(kReplyMax * sizeof(int16_t));
  if (!reply_pcm) {
    faceClear();
    title("No PSRAM", kError);
    while (true) delay(1000);
  }

  logf("[boot] %dx%d touch=%d heap=%uk", M5.Display.width(),
       M5.Display.height(), M5.Touch.isEnabled(),
       (unsigned)(ESP.getFreeHeap() / 1024));

  M5.Mic.begin();
  connectWifi();
  drawPage();
}

void loop() {
  M5.update();

  if (poked()) agentSession();            // blocks until the chat ends
  if (M5.BtnB.wasPressed()) {
    g_page = (g_page + 1) % PAGE_COUNT;
    drawPage();
  }

  static uint32_t next_report = 0;
  if (millis() > next_report) {
    next_report = millis() + 5000;
    Serial.printf("[hb] wifi=%d rssi=%d touch=%d heap=%u phase=%s page=%s\n",
                  WiFi.status() == WL_CONNECTED, (int)WiFi.RSSI(),
                  M5.Touch.isEnabled(), (unsigned)ESP.getFreeHeap(),
                  kPhaseName[g_phase], kPageName[g_page]);
  }
  delay(10);
}
