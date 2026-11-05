// Wi-Fi: scan, remember, join.
//
// A board on a bench joins the network named in secrets.h. A board in
// someone's hand has never heard of their network, so it also has to be able
// to scan, ask, and remember - see mode_wifi.cpp for that half. This file
// owns the three things both halves need: one scan, one join, and the two
// strings in flash that survive a reboot.

#include "pv.h"
#include <Preferences.h>

WifiNet g_nets[kMaxNets];
int     g_net_count = 0;

bool wifiUp() { return WiFi.status() == WL_CONNECTED; }

// ------------------------------------------------------------ remembering

// Plain text in NVS. This is the recipient's own password on the recipient's
// own device, so the exposure is a board someone else picks up - worth the
// flash-encryption note in the README before the 100-unit build, not worth a
// key-derivation ceremony that the device would have to store the key for.
static constexpr char kNvs[] = "pocketvoice";

void wifiSaveCreds(const String& ssid, const String& pass) {
  Preferences p;
  if (!p.begin(kNvs, false)) { failf("nvs write failed"); return; }
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
  logf("[wifi] saved %s", ssid.c_str());
}

bool wifiLoadCreds(String& ssid, String& pass) {
  Preferences p;
  if (!p.begin(kNvs, true)) return false;
  ssid = p.getString("ssid", "");
  pass = p.getString("pass", "");
  p.end();
  return ssid.length() > 0;
}

// ------------------------------------------------------------------ scan

int wifiScan() {
  const int n = WiFi.scanNetworks();
  g_net_count = 0;

  for (int i = 0; i < n; ++i) {
    const String  ssid = WiFi.SSID(i);
    const int32_t rssi = WiFi.RSSI(i);
    if (!ssid.length()) continue;             // hidden network, nothing to show

    // A mesh answers once per node it has. Keep the loudest copy of each
    // name, or the list is three screens of the same house.
    int at = -1;
    for (int j = 0; j < g_net_count; ++j) {
      if (g_nets[j].ssid == ssid) { at = j; break; }
    }
    if (at >= 0) {
      if (rssi > g_nets[at].rssi) g_nets[at].rssi = rssi;
      continue;
    }
    if (g_net_count >= kMaxNets) continue;

    g_nets[g_net_count].ssid = ssid;
    g_nets[g_net_count].rssi = rssi;
    g_nets[g_net_count].open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    ++g_net_count;
  }
  WiFi.scanDelete();

  // Strongest first, which is both the order you want to read and the order
  // that makes "the closest match wins" fall out of the prefix search below.
  for (int i = 1; i < g_net_count; ++i) {
    WifiNet k = g_nets[i];
    int j = i - 1;
    while (j >= 0 && g_nets[j].rssi < k.rssi) { g_nets[j + 1] = g_nets[j]; --j; }
    g_nets[j + 1] = k;
  }

  logf("[wifi] %d seen, %d distinct", n, g_net_count);
  return g_net_count;
}

// ------------------------------------------------------------------ join

// Why the last attempt ended. The status code alone cannot tell a wrong
// password from an out-of-range AP - both arrive as WL_DISCONNECTED - but the
// disconnect event carries the reason, and that is the difference between a
// user retyping and a user giving up.
static volatile uint8_t s_why = 0;
static bool             s_hooked = false;

static void onWifiEvent(WiFiEvent_t e, WiFiEventInfo_t info) {
  if (e == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    s_why = info.wifi_sta_disconnected.reason;
  }
}

const char* wifiWhy() {
  switch (s_why) {
    case WIFI_REASON_NO_AP_FOUND:              return "out of range";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:        return "wrong password";
    case WIFI_REASON_ASSOC_FAIL:               return "the router said no";
    case 0:                                    return "timed out";
    default:                                   return "no answer";
  }
}

void wifiBegin() {
  if (!s_hooked) { WiFi.onEvent(onWifiEvent); s_hooked = true; }
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);         // modem sleep shows up directly as caption lag
  WiFi.setAutoReconnect(true);
}

bool wifiJoin(const char* ssid, const char* pass, int tries) {
  wifiBegin();
  s_why = 0;
  WiFi.disconnect();
  WiFi.begin(ssid, pass && pass[0] ? pass : nullptr);
  for (int i = 0; i < tries && !wifiUp(); ++i) {
    drawLevelRing(200, i);
    delay(250);
  }
  if (!wifiUp()) return false;
  logf("[wifi] %s -> %s", ssid, WiFi.localIP().toString().c_str());
  return true;
}

// ------------------------------------------------------- compiled-in path

// The prefix in secrets.h is the bench shortcut: a board flashed on a desk
// joins the desk's network without anyone touching the screen. A board that
// has never been told anything skips straight to the picker.
static bool haveCompiled() {
  const char* p = WIFI_SSID_PREFIX;
  return p[0] && strcmp(p, "your-network") != 0;
}

static String findSsid() {
  String want = String(WIFI_SSID_PREFIX);
  want.toLowerCase();
  for (int i = 0; i < g_net_count; ++i) {     // already strongest-first
    String lower = g_nets[i].ssid;
    lower.toLowerCase();
    if (lower.startsWith(want)) return g_nets[i].ssid;
  }
  return String();
}

// Returns whether we got on a network. It never blocks waiting for a human:
// a first boot in a room with no known network used to sit in a "tap to
// choose" screen inside setup(), which meant loop() never ran - no heartbeat,
// no log mode, no way to see what had happened without the screen in front of
// you. Boot now always finishes, and app.cpp simply opens on the Wi-Fi mode,
// whose idle face already says "tap to choose a network".
bool wifiConnect() {
  faceClear();
  caption("WI-FI");
  title("Wi-Fi");
  wifiBegin();

  String ssid, pass;
  const bool remembered = wifiLoadCreds(ssid, pass);
  if (remembered) {
    statusLine(ssid.c_str(), kAccent);
    if (wifiJoin(ssid.c_str(), pass.c_str(), 32)) return true;   // 8 s
    logf("[wifi] saved %s: %s", ssid.c_str(), wifiWhy());
  }

  if (haveCompiled()) {
    statusLine("scanning", kMuted);
    wifiScan();
    const String found = findSsid();
    if (found.length()) {
      statusLine(found.c_str(), kAccent);
      if (wifiJoin(found.c_str(), WIFI_PASS, 32)) {
        // Only ever write the bench network into a board that has not been
        // given one. Otherwise carrying someone's device past this desk
        // would quietly replace the network they chose.
        if (!remembered) wifiSaveCreds(found, WIFI_PASS);
        return true;
      }
      logf("[wifi] %s: %s", found.c_str(), wifiWhy());
    } else {
      logf("[wifi] nothing like %s", WIFI_SSID_PREFIX);
    }
  }

  failf("no wi-fi");
  return false;
}
