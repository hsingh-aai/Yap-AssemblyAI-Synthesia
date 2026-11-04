// Wi-Fi: scan, pick, join.

#include "pv.h"

bool wifiUp() { return WiFi.status() == WL_CONNECTED; }

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
// unambiguous: a venue running Guest and Employee SSIDs will hand you the
// wrong one over a 1 dBm difference.
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
  if (!best.length()) {
    // No match means the configured name is wrong, not that the join failed.
    failf("no ssid like %s", prefix.c_str());
    return String();
  }
  logf("[wifi] picked %s (%d)", best.c_str(), best_rssi);
  return best;
}

void wifiConnect() {
  faceClear();
  title("Wi-Fi");
  statusLine("scanning");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);         // modem sleep shows up directly as caption lag
  WiFi.setAutoReconnect(true);

  const String ssid = findSsid();
  if (!ssid.length()) { statusLine(g_last_err, kError); delay(1500); return; }
  statusLine(ssid.c_str(), kAccent);

  WiFi.begin(ssid.c_str(), WIFI_PASS);
  for (int i = 0; i < 60 && !wifiUp(); ++i) {
    drawLevelRing(200, i);
    delay(500);
  }
  if (!wifiUp()) { failf("no wi-fi: %s", ssid.c_str()); return; }
  logf("[wifi] %s -> %s", ssid.c_str(), WiFi.localIP().toString().c_str());
}
