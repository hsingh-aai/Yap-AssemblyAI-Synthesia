// Pocket Voice - Phase 1: hardware hello world
//
// Proves the board end to end before any network code goes on it:
// round AMOLED, the two side buttons, the ES8311 mic, the speaker and
// the vibration motor. Nothing here talks to AssemblyAI yet; Phase 2
// adds that on top of the same mic path.
//
// Board: M5Stack StopWatch Dev Kit (C152), ESP32-S3R8.

#include <M5Unified.h>

// Deck palette.
static constexpr uint32_t kInk     = 0x131210;  // near-black background
static constexpr uint32_t kPaper   = 0xF3EFE6;  // warm off-white text
static constexpr uint32_t kMuted   = 0xB8B2A7;  // secondary text
static constexpr uint32_t kAccent  = 0xA99CFF;  // lavender
static constexpr uint32_t kAccent2 = 0x3F2BC1;  // deep violet

// Mic capture. 16 kHz mono is what the AssemblyAI endpoints want, so we
// run the meter at the same rate Phase 2 will record at.
static constexpr uint32_t kSampleRate = 16000;
static constexpr size_t   kFrame      = 256;
static int16_t            mic_buf[kFrame];

// Waveform history, one bar per column across the face.
static constexpr int kBars = 24;
static uint8_t       bar_level[kBars];
static int           bar_head = 0;

// M5GFX reports this panel as 480x480 but the glass is 466x466 round, with no
// offset configured, so centring on Display.width() runs off the visible area.
static constexpr int kFace = 466;

static int cx, cy, radius;

static void drawFrame() {
  M5.Display.drawCircle(cx, cy, radius - 4, kAccent);
  M5.Display.fillCircle(cx, cy - radius + 14, 5, kAccent);
}

static void drawWordmark() {
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(kPaper, kInk);
  M5.Display.setFont(&fonts::FreeSansBold18pt7b);
  M5.Display.drawString("AssemblyAI", cx, cy - radius / 2);

  M5.Display.setTextColor(kAccent, kInk);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.drawString("POCKET VOICE", cx, cy - radius / 2 + 34);
}

static void drawHints() {
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.setTextColor(kMuted, kInk);
  M5.Display.drawString("A: beep + buzz    B: mic test", cx, cy + radius / 2 + 40);
}

// One peak value per frame, 0..255, mapped with a little headroom so
// normal speech fills most of the bar.
static uint8_t frameLevel() {
  if (!M5.Mic.record(mic_buf, kFrame, kSampleRate)) return 0;
  int32_t peak = 0;
  for (size_t i = 0; i < kFrame; ++i) {
    int32_t v = abs(mic_buf[i]);
    if (v > peak) peak = v;
  }
  int32_t level = peak / 48;  // ~12000 counts reaches full scale
  return level > 255 ? 255 : (uint8_t)level;
}

static void drawWaveform() {
  const int band_w = (radius * 2) / 3;
  const int left   = cx - band_w / 2;
  const int step   = band_w / kBars;
  const int max_h  = radius / 2;

  for (int i = 0; i < kBars; ++i) {
    uint8_t lvl = bar_level[(bar_head + i) % kBars];
    int h = 4 + (lvl * max_h) / 255;
    int x = left + i * step;
    // Clear the column, then paint the bar centred on the face.
    M5.Display.fillRect(x, cy - max_h, step, max_h * 2, kInk);
    M5.Display.fillRoundRect(x, cy - h / 2, step - 2, h, (step - 2) / 2,
                             lvl > 24 ? kAccent : kAccent2);
  }
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_mic = true;
  cfg.internal_spk = true;
  // Nothing else begins Serial on this board: the core guards its own
  // Serial.begin() on !ARDUINO_USB_MODE and we build with USBMode=hwcdc, so
  // without this HWCDC stays uninitialised and every print is dropped.
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);

  cx = M5.Display.width() / 2;
  cy = M5.Display.height() / 2;
  radius = kFace / 2 - 16;

  M5.Display.setBrightness(180);
  M5.Display.fillScreen(kInk);
  drawFrame();
  drawWordmark();
  drawHints();

  // Say hello on all three output channels so a dead one is obvious.
  M5.Speaker.tone(880, 120);
  delay(140);
  M5.Speaker.tone(1320, 160);
  M5.Power.setVibration(180);
  delay(180);
  M5.Power.setVibration(0);

  M5.Mic.begin();

  Serial.printf("board=%d display=%dx%d mic=%d spk=%d\n", (int)M5.getBoard(),
                M5.Display.width(), M5.Display.height(), M5.Mic.isEnabled(),
                M5.Speaker.isEnabled());
}

void loop() {
  M5.update();

  if (M5.BtnA.wasPressed()) {  // yellow, G2
    M5.Speaker.tone(1046, 120);
    M5.Power.setVibration(200);
    delay(120);
    M5.Power.setVibration(0);
  }

  if (M5.BtnB.wasPressed()) {  // blue, G1
    M5.Display.fillScreen(kInk);
    drawFrame();
    drawWordmark();
    drawHints();
  }

  bar_level[bar_head] = frameLevel();
  bar_head = (bar_head + 1) % kBars;
  drawWaveform();

  // Heartbeat, so the board state is readable over USB at any time and not
  // only in the boot banner we always miss.
  static uint32_t next_report = 0;
  if (millis() > next_report) {
    next_report = millis() + 2000;
    Serial.printf("board=%d display=%dx%d mic=%d spk=%d level=%u btnA=%d btnB=%d\n",
                  (int)M5.getBoard(), M5.Display.width(), M5.Display.height(),
                  M5.Mic.isEnabled(), M5.Speaker.isEnabled(),
                  bar_level[(bar_head + kBars - 1) % kBars], M5.BtnA.isPressed(),
                  M5.BtnB.isPressed());
  }
}
