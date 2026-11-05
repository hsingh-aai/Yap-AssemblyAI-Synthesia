// Pocket Voice - one firmware, every mode.
//
// The whole interface is two gestures. The blue button walks the modes; a tap
// on the screen (or the yellow button) does whatever the face says. There is
// no menu to get lost in, which matters for a device handed to a hundred
// strangers at a party.
//
// Board: M5Stack StopWatch Dev Kit (C152), ESP32-S3R8.

#include "pv.h"

const Mode kModes[] = {
  {"Blurt",     "say it, send it to the Mac", blurtIdle, blurtAct, blurtTick, blurtLeave},
  {"Notetaker", "listen, then summarise",    notesIdle,  notesAct, nullptr,   nullptr},
  {"Jack",      "a voice agent to talk to",  jackIdle,   jackAct,  nullptr,   nullptr},
  {"Wi-Fi",     "pick a network, type a key", wifiIdle,  wifiAct,  nullptr,   nullptr},
  {"Status",    "wi-fi, memory, last error", statusIdle, nullptr,  nullptr,   nullptr},
  {"Log",       "what just happened",        logIdle,    nullptr,  nullptr,   nullptr},
};
const int kModeCount = (int)(sizeof(kModes) / sizeof(kModes[0]));

static int g_mode = 0;

// One dot per mode along the bottom arc, plus the only other thing you can
// do. With no labels and two buttons, this is what tells you where you are.
static void modeDots() {
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.setTextSize(1.0f);
  M5.Display.setTextColor(kMuted, kInk);
  M5.Display.drawString("B: next mode", cx, cy + radius - 54);

  const int gap = 16;
  const int y   = cy + radius - 28;
  int x = cx - ((kModeCount - 1) * gap) / 2;
  for (int i = 0; i < kModeCount; ++i, x += gap) {
    if (i == g_mode) M5.Display.fillCircle(x, y, 4, kAccent);
    else             M5.Display.drawCircle(x, y, 4, kAccent2);
  }
}

static void showMode() {
  kModes[g_mode].idle();
  modeDots();
}

// Put down whatever the current mode holds and open the first one. Waits for
// both buttons to come up first; otherwise the chord is still held when
// loop() runs again and it fires a second time.
static void goHome() {
  if (kModes[g_mode].leave) kModes[g_mode].leave();
  while (M5.BtnA.isPressed() || M5.BtnB.isPressed()) { M5.update(); delay(10); }
  M5.update();
  g_go_home = false;
  g_mode = 0;
  logf("[home]");
  showMode();
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_mic = true;
  cfg.internal_spk = true;
  // Nothing else begins Serial on this board: the core guards its own
  // Serial.begin() on !ARDUINO_USB_MODE and we build with USBMode=hwcdc.
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);

  uiBegin();

  // Allocated once rather than per run: the agent is the only mode that has
  // to hold audio, and it holds a whole reply at a time.
  g_reply = (int16_t*)ps_malloc(kReplyMax * sizeof(int16_t));
  if (!g_reply) {
    faceClear();
    title("No PSRAM", kError);
    while (true) delay(1000);
  }

  logf("[boot] %dx%d touch=%d psram=%uk", M5.Display.width(),
       M5.Display.height(), M5.Touch.isEnabled(),
       (unsigned)(ESP.getFreePsram() / 1024));

  M5.Mic.begin();
  if (!wifiConnect()) {
    // Nothing to transcribe with. Start on the one mode that can fix that
    // rather than on a mode that would only fail.
    for (int i = 0; i < kModeCount; ++i) {
      if (!strcmp(kModes[i].name, "Wi-Fi")) { g_mode = i; break; }
    }
  }
#if PV_SELFTEST & 1
  jackSelfTest();
#endif
#if PV_SELFTEST & 2
  sttSelfTest();
#endif
  showMode();
}

void loop() {
  M5.update();

  if (homeChord()) { goHome(); return; }

  if (M5.BtnB.wasPressed()) {
    // Let the mode put down whatever it was holding - Blurt keeps a warmed
    // socket open while it sits idle, and nobody else should inherit it.
    if (kModes[g_mode].leave) kModes[g_mode].leave();
    g_mode = (g_mode + 1) % kModeCount;
    showMode();
    return;
  }

  if (poked()) {
    if (kModes[g_mode].act) {
      kModes[g_mode].act();      // owns the face until the user is done
      if (g_go_home) { goHome(); return; }
      showMode();
    } else {
      showMode();                // Status and Log just refresh
    }
    return;
  }

  if (kModes[g_mode].tick) kModes[g_mode].tick();

  static uint32_t next_report = 0;
  if (millis() > next_report) {
    next_report = millis() + 5000;
    Serial.printf("[hb] wifi=%d rssi=%d touch=%d heap=%u psram=%u mode=%s\n",
                  wifiUp(), (int)WiFi.RSSI(), M5.Touch.isEnabled(),
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
                  kModes[g_mode].name);
  }
  delay(10);
}
