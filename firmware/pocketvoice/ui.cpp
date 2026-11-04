// Round-face drawing, the log ring, and the shared buffers.

#include "pv.h"
#include <stdarg.h>

int cx, cy, radius;

int16_t mic_buf[kMicBufs][kMicMax];
int16_t* g_reply = nullptr;

void uiBegin() {
  cx = M5.Display.width() / 2;
  cy = M5.Display.height() / 2;
  radius = kFace / 2 - 16;
  M5.Display.setBrightness(180);
}

float chordHalf(int dy) {
  const float d = (float)radius * radius - (float)dy * dy;
  return d > 1.0f ? sqrtf(d) : 1.0f;
}

float fitScale(const char* s, int dy, float cap) {
  M5.Display.setTextSize(1.0f);                 // always measure at unit scale
  const float room = 2.0f * chordHalf(dy) - 24.0f;
  const int   w    = M5.Display.textWidth(s);
  if (w <= 0) return cap;
  const float scale = room / (float)w;
  return scale > cap ? cap : (scale < 0.45f ? 0.45f : scale);
}

void faceClear() {
  M5.Display.fillScreen(kInk);
  M5.Display.drawCircle(cx, cy, radius - 4, kAccent);
}

void caption(const char* name) {
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.setTextSize(1.0f);
  M5.Display.setTextColor(kMuted, kInk);
  M5.Display.drawString(name, cx, cy - radius + 30);
}

void title(const char* msg, uint32_t color) {
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::FreeSansBold18pt7b);
  M5.Display.setTextSize(fitScale(msg, 0));
  M5.Display.setTextColor(color, kInk);
  M5.Display.drawString(msg, cx, cy);
  M5.Display.setTextSize(1.0f);
}

// One line of plain instruction for whatever the device wants you to do next.
void hint(const char* msg, uint32_t color) {
  const int y = cy + radius - 80;
  const int w = (int)(2.0f * chordHalf(y - cy)) - 16;
  M5.Display.fillRect(cx - w / 2, y - 16, w, 32, kInk);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.setTextSize(1.0f);
  M5.Display.setTextColor(color, kInk);
  M5.Display.drawString(msg, cx, y);
}

void statusLine(const char* msg, uint32_t color) {
  const int dy = radius / 3;
  M5.Display.fillRect(0, cy + dy, M5.Display.width(), 40, kInk);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::FreeSans12pt7b);
  M5.Display.setTextSize(fitScale(msg, dy + 18));
  M5.Display.setTextColor(color, kInk);
  M5.Display.drawString(msg, cx, cy + dy + 18);
  M5.Display.setTextSize(1.0f);
}

void levelBar(uint8_t level) {
  const int w = M5.Display.width() / 3;
  const int h = 8;
  const int x = cx - w / 2;
  const int y = cy + radius / 3 + 16;
  M5.Display.fillRect(x, y, w, h, kInk);
  M5.Display.fillRoundRect(x, y, (w * level) / 255, h, h / 2,
                           level > 24 ? kAccent : kAccent2);
}

void drawLevelRing(uint8_t level, int spoke) {
  const float a = (spoke % 60) * 6.0f * 3.14159265f / 180.0f;
  const int r0 = radius - 30;
  const int r1 = r0 - 10 - (level * (radius / 5)) / 255;
  M5.Display.drawLine(cx + (int)(cosf(a) * r0), cy + (int)(sinf(a) * r0),
                      cx + (int)(cosf(a) * r1), cy + (int)(sinf(a) * r1),
                      level > 24 ? kAccent : kAccent2);
}

uint8_t chunkPeak(const int16_t* buf, size_t n) {
  int32_t peak = 0;
  for (size_t i = 0; i < n; ++i) {
    const int32_t v = abs(buf[i]);
    if (v > peak) peak = v;
  }
  const int32_t level = peak / 48;            // ~12000 counts is full scale
  return level > 255 ? 255 : (uint8_t)level;
}

// --------------------------------------------------------------- text band

static int lineY(int i) {
  return cy - (kTextLines * kLineH) / 2 + i * kLineH + kLineH / 2;
}

// Wipes exactly the chord each line occupies. A plain rectangle over the whole
// block would clip the ring where the circle narrows.
void clearTextBand() {
  for (int i = 0; i < kTextLines; ++i) {
    const int yc = lineY(i);
    const int w  = (int)(2.0f * chordHalf(yc - cy)) - 20;
    M5.Display.fillRect(cx - w / 2, yc - kLineH / 2, w, kLineH, kInk);
  }
}

size_t layoutPage(const String& t, size_t from, bool render, uint32_t color,
                  size_t grey_from) {
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
    if (fit == 0) {   // a single word wider than the line still has to go in
      fit = i + 1;
      while (fit < t.length() &&
             M5.Display.textWidth(t.substring(i, fit + 1)) <= maxw) ++fit;
    }
    if (render) {
      M5.Display.setTextColor(i >= grey_from ? kMuted : color, kInk);
      M5.Display.drawString(t.substring(i, fit).c_str(), cx, yc);
    }
    i = fit;
  }
  return i;
}

int countPages(const String& t, size_t* starts, int max_pages) {
  starts[0] = 0;
  int n = 1;
  size_t at = 0;
  while (n < max_pages) {
    const size_t next = layoutPage(t, at, false);
    if (next >= t.length() || next == at) break;
    starts[n++] = next;
    at = next;
  }
  return n;
}

// Live text scrolls: what matters is the newest words, so show the last page.
void drawTail(const String& t, uint32_t color) {
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

// ------------------------------------------------------------------- log

static constexpr int kLogLines = 11;
static char g_log[kLogLines][52];
static int  g_log_count = 0;
char g_last_err[48] = "";

// Serial and the Log mode carry the same lines. Serial only works because
// setup() sets cfg.serial_baudrate: with USBMode=hwcdc nothing else ever calls
// Serial.begin(), so HWCDC stays uninitialised and drops every print.
void logf(const char* fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  Serial.println(buf);
  strlcpy(g_log[g_log_count % kLogLines], buf, sizeof g_log[0]);
  ++g_log_count;
}

void relayText(const String& text) {
  if (!text.length()) return;
  Serial.print("[say] ");
  for (size_t i = 0; i < text.length(); ++i) {
    const char c = text[i];
    // Exactly the convention Blurt's loopback listener reads: \n is a
    // newline, \\ is a backslash, and a line break is the frame.
    if (c == '\n')      Serial.print("\\n");
    else if (c == '\\') Serial.print("\\\\");
    else if (c == '\r') continue;
    else                 Serial.write(c);
  }
  Serial.println();
  // The on-screen log still gets the short form; it only has room for that.
  logf("[say] %s", text.substring(0, 40).c_str());
}

void failf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_last_err, sizeof g_last_err, fmt, ap);
  va_end(ap);
  logf("[err] %s", g_last_err);
}

void drawLogBody() {
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
}

void drawStatusBody() {
  const bool up = wifiUp();
  static const char* labels[] = {"wi-fi", "ssid", "ip", "rssi",
                                 "touch", "heap", "psram", "last"};
  const String values[8] = {
    up ? "up" : "down",
    up ? WiFi.SSID() : String("-"),
    up ? WiFi.localIP().toString() : String("-"),
    up ? String(WiFi.RSSI()) + " dBm" : String("-"),
    M5.Touch.isEnabled() ? "on" : "off",
    String(ESP.getFreeHeap() / 1024) + "k",
    String(ESP.getFreePsram() / 1024) + "k",
    g_last_err[0] ? String(g_last_err) : String("ok"),
  };

  const int side = (int)(radius * 1.35f);
  const int left = cx - side / 2;
  const int top  = cy - (8 * 26) / 2;
  M5.Display.setTextSize(1.0f);
  for (int i = 0; i < 8; ++i) {
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

// ----------------------------------------------------------------- input

bool poked() {
  if (M5.BtnA.wasPressed()) return true;
  if (M5.Touch.isEnabled()) {
    const auto n = M5.Touch.getCount();
    for (size_t i = 0; i < n; ++i) {
      if (M5.Touch.getDetail(i).wasPressed()) return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------- results

void resultViewer(const char* cap, const String& text) {
  size_t starts[12];
  const int pages = countPages(text, starts, 12);
  int page = 0;
  bool redraw = true;

  while (true) {
    M5.update();
    if (redraw) {
      faceClear();
      caption(cap);
      layoutPage(text, starts[page], true);
      if (pages > 1) {
        char buf[16];
        snprintf(buf, sizeof buf, "%d / %d", page + 1, pages);
        M5.Display.setTextDatum(middle_center);
        M5.Display.setFont(&fonts::FreeSans9pt7b);
        M5.Display.setTextSize(1.0f);
        M5.Display.setTextColor(kAccent, kInk);
        M5.Display.drawString(buf, cx, cy + radius - 92);
      }
      hint(pages > 1 ? "B: next page   tap: again" : "tap to go again");
      redraw = false;
    }
    if (M5.BtnB.wasPressed()) {
      if (pages > 1) { page = (page + 1) % pages; redraw = true; }
      else return;
    }
    if (poked()) return;
    delay(10);
  }
}
