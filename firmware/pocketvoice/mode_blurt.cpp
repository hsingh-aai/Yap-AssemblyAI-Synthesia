// Blurt: say a thing, and have the text already waiting when you stop.
//
// This was the pre-recorded path - record to PSRAM, upload the WAV, create a
// transcript, poll until it was ready. That was the right first move, because
// it is the smallest thing that proves mic -> network -> display, and the
// wrong thing to hand a stranger at a party. Measured end to end from a Mac on
// fast wifi the round trip is 3.2 s, and the device is slower at every step:
// 300 KB of WAV pushed through TLS off an ESP32 rather than 0.4 s of laptop
// upload, then a poll loop that rounds the answer up to its next two-second
// tick. Ten seconds of standing there watching a spinner.
//
// Streaming deletes the wait rather than shortening it. The words arrive while
// you are still talking, so the tap that ends the sentence also finishes the
// job - and the 12-second recording cap goes with it, since nothing is being
// held in memory any more.

#include "pv.h"

static constexpr size_t kChunk = 512;   // idle mic meter only

static String   s_text;      // turns the server has closed
static String   s_partial;   // the one still being spoken
static bool     s_dirty;
static bool     s_live;      // socket is open and the face knows it
static uint32_t s_last_draw;

// The finished turns plus whatever is still being revised, as one line.
static String shown() {
  if (!s_text.length())    return s_partial;
  if (!s_partial.length()) return s_text;
  return s_text + " " + s_partial;
}

static void onTurn(const String& text, bool final) {
  if (final) {
    if (text.length()) {
      if (s_text.length()) s_text += " ";
      s_text += text;
      // One line per closed turn, so a session that relays nothing can be
      // told apart from a session that heard nothing.
      logf("[blurt] %s", text.substring(0, 40).c_str());
    }
    s_partial = "";
  } else {
    s_partial = text;
  }
  s_dirty = true;
}

static void paint() {
  const String t = shown();
  drawTail(t.length() ? t : String("listening..."), t.length() ? kPaper : kMuted);
  hint("tap to send", kLive);
}

static bool keepGoing() {
  // sttStream calls this only once the socket is open, so the first call is
  // the moment the face can stop saying "Connecting".
  if (!s_live) {
    s_live = true;
    paint();
  }
  if (poked()) return false;
  if (s_dirty && millis() - s_last_draw > 160) {
    s_dirty = false;
    s_last_draw = millis();
    paint();
  }
  return true;
}

void blurtIdle() {
  faceClear();
  caption("BLURT");
  title("Blurt");
  hint("tap, talk, tap again");
}

void blurtTick() {
  // Proves the mic on the face: if this bar never moves, the ES8311 side is
  // the problem and not the network.
  static int16_t probe[kChunk];
  if (!M5.Mic.record(probe, kChunk, kSttRate)) return;
  levelBar(chunkPeak(probe, kChunk));
}

void blurtAct() {
  s_text = "";
  s_partial = "";
  s_dirty = false;
  s_live = false;
  s_last_draw = 0;

  faceClear();
  caption("BLURT");
  title("Connecting");
  hint("opening the socket", kMuted);

  if (!sttStream(onTurn, keepGoing)) {
    faceClear();
    caption("BLURT");
    title("No socket", kError);
    hint(g_last_err, kError);
    delay(2000);
    return;
  }

  if (!s_text.length()) {
    faceClear();
    caption("BLURT");
    title("Nothing heard");
    hint("tap, talk, tap again", kMuted);
    delay(1800);
    return;
  }

  relayText(s_text);                     // up the cable to the Mac
  M5.Power.setVibration(180);
  delay(120);
  M5.Power.setVibration(0);
  resultViewer("BLURT", s_text);
}
