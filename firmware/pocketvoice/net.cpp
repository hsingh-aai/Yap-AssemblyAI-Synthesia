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

// ------------------------------------------------------- known networks

// secrets.h names a few networks the device may join without being asked:
// the bench, and the hotspot of whoever the unit is shipped to. Matched as
// prefixes, so "Merveen" finds "Merveen’s iPhone" whatever apostrophe the
// phone used.
struct Known { const char* prefix; const char* pass; };
static const Known kKnown[] = WIFI_KNOWN;

static bool startsWithFold(const String& name, const char* prefix) {
  String a = name, b = prefix;
  a.toLowerCase();
  b.toLowerCase();
  return a.startsWith(b);
}

// One scan, then a decision, so neither place pays the other's timeout: the
// saved network if it is in range, otherwise the strongest known one that is.
// The old order - try saved for 8 s, then scan - cost every boot away from
// the saved network 8 s of nothing.
bool wifiConnect() {
  faceClear();
  caption("WI-FI");
  title("Wi-Fi");
  statusLine("scanning", kMuted);
  wifiBegin();
  wifiScan();

  String ssid, pass;
  const bool remembered = wifiLoadCreds(ssid, pass);
  if (remembered) {
    for (int i = 0; i < g_net_count; ++i) {
      if (g_nets[i].ssid != ssid) continue;
      statusLine(ssid.c_str(), kAccent);
      if (wifiJoin(ssid.c_str(), pass.c_str(), 40)) return true;   // 10 s
      logf("[wifi] saved %s: %s", ssid.c_str(), wifiWhy());
      break;
    }
  }

  for (int i = 0; i < g_net_count; ++i) {             // strongest first
    for (const Known& k : kKnown) {
      if (!k.prefix[0] || !strcmp(k.prefix, "your-network")) continue;
      if (!startsWithFold(g_nets[i].ssid, k.prefix)) continue;
      const String& found = g_nets[i].ssid;
      statusLine(found.c_str(), kAccent);
      if (wifiJoin(found.c_str(), k.pass, 40)) {
        // Only fill an empty slot. A network the owner picked by hand stays
        // picked, even if a known one happened to be in range today.
        if (!remembered) wifiSaveCreds(found, k.pass);
        return true;
      }
      logf("[wifi] %s: %s", found.c_str(), wifiWhy());
    }
  }

  failf("no wi-fi");
  return false;
}
