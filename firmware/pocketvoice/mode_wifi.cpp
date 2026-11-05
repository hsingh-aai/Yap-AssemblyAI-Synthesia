// Wi-Fi: pick a network and type its password.
//
// Everything else in this firmware needs the network, so this is the one mode
// that has to work before any of the others can. It is also the only one a
// recipient has to use before the device is theirs, which is why the picker
// is reachable from the mode ring at any time and not only on first boot: a
// device that joins one network and can never be told about another is a
// device that works in exactly one building.

#include "pv.h"

static constexpr int kListRows = 6;
static constexpr int kListH    = 36;

static int listY(int i) { return cy + (i * 2 - (kListRows - 1)) * kListH / 2; }

// Four bars, drawn short to tall. Signal strength is the only thing on this
// screen that tells you which of two identically named networks is the one in
// this room.
static void drawBars(int x, int y, int32_t rssi) {
  const int lit = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
  for (int i = 0; i < 4; ++i) {
    const int h = 4 + i * 4;
    M5.Display.fillRect(x + i * 5, y - h, 3, h, i < lit ? kAccent : 0x3A3832);
  }
}

static void drawLock(int x, int y) {
  M5.Display.drawRoundRect(x + 2, y - 11, 6, 8, 3, kMuted);   // shackle
  M5.Display.fillRoundRect(x, y - 7, 10, 8, 2, kMuted);       // body
}

static void drawRow(int i, const WifiNet& net) {
  const int y = listY(i);
  const int w = (int)(2.0f * chordHalf(y - cy)) - 18;
  const int left = cx - w / 2;

  M5.Display.fillRect(left, y - kListH / 2, w, kListH, kInk);

  const int marks = 36;                       // room kept for lock + bars
  M5.Display.setTextDatum(middle_left);
  M5.Display.setFont(&fonts::FreeSans12pt7b);
  M5.Display.setTextSize(1.0f);
  M5.Display.setTextColor(kPaper, kInk);
  M5.Display.setClipRect(left, y - kListH / 2, w - marks - 8, kListH);
  M5.Display.drawString(net.ssid.c_str(), left + 4, y);
  M5.Display.clearClipRect();

  if (!net.open) drawLock(left + w - marks, y + 4);
  drawBars(left + w - 20, y + 5, net.rssi);
  M5.Display.drawFastHLine(left, y + kListH / 2, w, 0x24231F);
}

// Returns the index into g_nets, -1 if the user backed out, -2 to rescan.
static int chooseNet() {
  int page = 0;
  const int pages = (g_net_count + kListRows - 1) / kListRows;
  bool redraw = true;

  while (true) {
    M5.update();

    if (redraw) {
      faceClear();
      caption("WI-FI");
      const int first = page * kListRows;
      for (int i = 0; i < kListRows && first + i < g_net_count; ++i) {
        drawRow(i, g_nets[first + i]);
      }
      if (pages > 1) {
        char buf[16];
        snprintf(buf, sizeof buf, "%d / %d", page + 1, pages);
        M5.Display.setTextDatum(middle_center);
        M5.Display.setFont(&fonts::FreeSans9pt7b);
        M5.Display.setTextColor(kAccent, kInk);
        M5.Display.drawString(buf, cx, cy - radius + 56);
      }
      hint(pages > 1 ? "B: more   A: rescan" : "A: rescan   tap a network", kMuted);
      redraw = false;
    }

    if (homeChord()) return -1;
    if (M5.BtnA.wasPressed()) return -2;
    if (M5.BtnB.wasPressed()) {
      if (pages > 1) { page = (page + 1) % pages; redraw = true; }
      else           return -1;
    }

    int tx, ty;
    if (tapped(&tx, &ty)) {
      for (int i = 0; i < kListRows; ++i) {
        const int y = listY(i);
        if (ty < y - kListH / 2 || ty >= y + kListH / 2) continue;
        const int at = page * kListRows + i;
        if (at < g_net_count) return at;
      }
    }
    delay(10);
  }
}

static void say(const char* msg, const char* under, uint32_t color) {
  faceClear();
  caption("WI-FI");
  title(msg, color);
  if (under) statusLine(under, kMuted);
}

// Blocks until the user has either joined something or walked away.
void wifiPick() {
  while (true) {
    say("Scanning", nullptr, kPaper);
    wifiBegin();
    if (wifiScan() == 0) {
      say("Nothing here", "no networks in range", kMuted);
      hint("tap to scan again   B: back", kMuted);
      while (true) {
        M5.update();
        if (homeChord() || M5.BtnB.wasPressed()) return;
        int x, y;
        if (tapped(&x, &y) || M5.BtnA.wasPressed()) break;
        delay(10);
      }
      continue;
    }

    const int sel = chooseNet();
    if (sel == -1) return;
    if (sel == -2) continue;

    const WifiNet net = g_nets[sel];          // copied: a rescan moves the list
    String pass;
    if (!net.open) {
      // Offer the password we already have for this name, so a reconnect
      // after a router reboot is two taps rather than thirty.
      String saved_ssid, saved_pass;
      if (wifiLoadCreds(saved_ssid, saved_pass) && saved_ssid == net.ssid) {
        pass = saved_pass;
      }
      if (!textEntry("WI-FI", net.ssid.c_str(), pass)) {
        if (g_go_home) return;
        continue;
      }
    }

    say("Joining", net.ssid.c_str(), kPaper);
    if (wifiJoin(net.ssid.c_str(), pass.c_str(), 60)) {   // 15 s
      wifiSaveCreds(net.ssid, pass);
      g_last_err[0] = 0;
      say("Connected", WiFi.localIP().toString().c_str(), kAccent);
      hint("tap to carry on", kMuted);
      while (true) {
        M5.update();
        int x, y;
        if (homeChord() || tapped(&x, &y) || M5.BtnA.wasPressed() ||
            M5.BtnB.wasPressed()) return;
        delay(10);
      }
    }

    failf("%s: %s", net.ssid.c_str(), wifiWhy());
    say("Did not join", wifiWhy(), kError);
    hint("tap to try again   B: back", kMuted);
    while (true) {
      M5.update();
      if (homeChord() || M5.BtnB.wasPressed()) return;
      int x, y;
      if (tapped(&x, &y) || M5.BtnA.wasPressed()) break;
      delay(10);
    }
  }
}

// --------------------------------------------------------------- the mode

void wifiIdle() {
  faceClear();
  caption("WI-FI");
  if (wifiUp()) {
    title(WiFi.SSID().c_str());
    statusLine(WiFi.localIP().toString().c_str(), kMuted);
  } else {
    title("Not connected", kMuted);
    statusLine(g_last_err[0] ? g_last_err : "no network", kError);
  }
  hint("tap to choose a network");
}

void wifiAct() { wifiPick(); }
