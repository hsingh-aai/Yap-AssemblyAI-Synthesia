// An on-screen keyboard for a round display.
//
// A Wi-Fi password is the one thing this device cannot be told any other way:
// there is no serial console in a stranger's hands, no app, and no second
// network to provision over. So it needs real text entry on a 466 px circle
// with two buttons and a finger.
//
// A circle is widest across its equator, which is exactly where a keyboard
// wants to be. Five rows centred on the middle each get about 400 px of
// chord, so ten columns land on 40 px keys - the same size a phone gives you.
// Stacked towards the bottom instead, the last row would have 290 px and the
// keys would be 29 px, which is the difference between typing and poking.
// Every row therefore sizes itself to its own chord rather than to a shared
// grid, and the geometry stops being in the way.

#include "pv.h"

static constexpr int kRows   = 5;
static constexpr int kRowH   = 40;
static constexpr int kKeyGap = 3;
static constexpr int kMaxLen = 63;        // a WPA2 passphrase, exactly

// What a key does when it is pressed. Everything that is not one of these
// puts a character in the buffer.
enum { kChar, kShift, kBack, kLayer, kSpace, kDone };

// Three layers over the same five rows. Row 3 always gains a shift key on the
// left and a delete key on the right, and row 4 is always the function row,
// so only the first four strings change between layers.
static const char* kLower[4] = {
  "1234567890",
  "qwertyuiop",
  "asdfghjkl",
  "zxcvbnm",
};
static const char* kUpper[4] = {
  "1234567890",
  "QWERTYUIOP",
  "ASDFGHJKL",
  "ZXCVBNM",
};
static const char* kSyms[4] = {
  "!@#$%^&*()",
  "-_=+[]{}<>",
  ";:'\"`~\\|",
  ",./?",
};

static bool s_shift;
static bool s_syms;

static const char* rowChars(int r) {
  const char* const* layer = s_syms ? kSyms : (s_shift ? kUpper : kLower);
  return layer[r];
}

static int rowSlots(int r) {
  if (r < 3)  return (int)strlen(rowChars(r));
  if (r == 3) return 2 + (int)strlen(rowChars(3));
  return 5;                                  // toggle, space x3, done
}

// One key, by row and slot. `label` wins over `ch` when it is set.
static void keyAt(int r, int s, char* ch, int* act, const char** label) {
  *ch = 0;
  *act = kChar;
  *label = nullptr;

  if (r < 3) { *ch = rowChars(r)[s]; return; }

  if (r == 3) {
    const int n = (int)strlen(rowChars(3));
    // The shift key always shows the case you are currently typing in, and
    // from the symbol layer it is also the way back to letters.
    if (s == 0)     { *act = kShift; *label = s_shift && !s_syms ? "ABC" : "abc"; return; }
    if (s == n + 1) { *act = kBack;  *label = "del"; return; }
    *ch = rowChars(3)[s - 1];
    return;
  }

  if (s == 0) { *act = kLayer; *label = s_syms ? "abc" : "?123"; return; }
  if (s == 4) { *act = kDone;  *label = "done"; return; }
  *act = kSpace;
  *label = "space";
}

static int rowY(int r) { return cy + (r - 2) * kRowH; }

static void slotRect(int r, int s, int* x, int* w) {
  const int rw = (int)(2.0f * chordHalf(rowY(r) - cy)) - 18;
  const int n  = rowSlots(r);
  const int kw = (rw - kKeyGap * (n - 1)) / n;
  *x = cx - rw / 2 + s * (kw + kKeyGap);
  *w = kw;
}

static void drawKey(int r, int s, int x, int w) {
  char ch;
  int act;
  const char* label;
  keyAt(r, s, &ch, &act, &label);

  const int y  = rowY(r) - kRowH / 2;
  const int h  = kRowH - kKeyGap;
  const bool lit = (act == kDone) || (act == kShift && s_shift && !s_syms) ||
                   (act == kLayer && s_syms);

  M5.Display.fillRoundRect(x, y, w, h, 6, lit ? kAccent2 : 0x24231F);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.setTextSize(1.0f);
  M5.Display.setTextColor(act == kChar ? kPaper : kAccent, lit ? kAccent2 : 0x24231F);

  char one[2] = {ch, 0};
  M5.Display.drawString(label ? label : one, x + w / 2, y + h / 2);
}

static void drawKeys() {
  for (int r = 0; r < kRows; ++r) {
    const int rw = (int)(2.0f * chordHalf(rowY(r) - cy)) - 18;
    M5.Display.fillRect(cx - rw / 2 - 4, rowY(r) - kRowH / 2, rw + 8, kRowH, kInk);

    const int n = rowSlots(r);
    for (int s = 0; s < n; ++s) {
      int x, w;
      slotRect(r, s, &x, &w);
      // The three space slots are one key to look at and three to hit, which
      // is the whole point of a space bar.
      if (r == 4 && s == 1) {
        int x3, w3;
        slotRect(r, 3, &x3, &w3);
        drawKey(r, s, x, x3 + w3 - x);
      } else if (r == 4 && (s == 2 || s == 3)) {
        continue;
      } else {
        drawKey(r, s, x, w);
      }
    }
  }
}

static bool hitKey(int tx, int ty, int* r, int* s) {
  for (int i = 0; i < kRows; ++i) {
    const int y = rowY(i);
    if (ty < y - kRowH / 2 || ty >= y + kRowH / 2) continue;

    const int rw = (int)(2.0f * chordHalf(y - cy)) - 18;
    const int n  = rowSlots(i);
    const int kw = (rw - kKeyGap * (n - 1)) / n;
    const int dx = tx - (cx - rw / 2);
    if (dx < 0) return false;
    const int slot = dx / (kw + kKeyGap);
    if (slot >= n) return false;
    *r = i;
    *s = slot;
    return true;
  }
  return false;
}

// Shown in the clear. The only person who can read this screen is the person
// holding the device, and a password you cannot check is a password you
// retype three times.
static void drawEntry(const String& t) {
  const int y = cy - 124;
  const int w = (int)(2.0f * chordHalf(y - cy)) - 20;
  M5.Display.fillRect(cx - w / 2, y - 18, w, 38, kInk);

  String s = t + "_";
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::FreeSans12pt7b);
  M5.Display.setTextSize(1.0f);
  M5.Display.setTextColor(kPaper, kInk);
  while (s.length() > 1 && M5.Display.textWidth(s) > w - 8) s = s.substring(1);
  M5.Display.drawString(s.c_str(), cx, y);
  M5.Display.drawFastHLine(cx - w / 2, y + 19, w, kAccent2);
}

bool textEntry(const char* cap, const char* prompt, String& out) {
  s_shift = false;
  s_syms  = false;

  faceClear();
  caption(cap);

  const int py = cy - 158;
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.setTextSize(fitScale(prompt, py - cy));
  M5.Display.setTextColor(kMuted, kInk);
  M5.Display.drawString(prompt, cx, py);
  M5.Display.setTextSize(1.0f);

  String buf = out;
  drawEntry(buf);
  drawKeys();
  hint("A: done   B: cancel", kMuted);

  while (true) {
    M5.update();

    if (homeChord()) return false;
    if (M5.BtnB.wasPressed()) return false;
    if (M5.BtnA.wasPressed()) { out = buf; return true; }

    int tx, ty, r, s;
    if (tapped(&tx, &ty) && hitKey(tx, ty, &r, &s)) {
      char ch;
      int act;
      const char* label;
      keyAt(r, s, &ch, &act, &label);

      switch (act) {
        case kChar:
          if (ch && buf.length() < kMaxLen) { buf += ch; drawEntry(buf); }
          break;
        case kSpace:
          if (buf.length() < kMaxLen) { buf += ' '; drawEntry(buf); }
          break;
        case kBack:
          if (buf.length()) { buf.remove(buf.length() - 1); drawEntry(buf); }
          break;
        case kShift:
          if (s_syms) { s_syms = false; s_shift = false; }
          else        { s_shift = !s_shift; }
          drawKeys();
          break;
        case kLayer:
          s_syms = !s_syms;
          drawKeys();
          break;
        case kDone:
          out = buf;
          return true;
      }
    }
    delay(8);
  }
}
