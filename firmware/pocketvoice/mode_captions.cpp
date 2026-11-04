// Captions: live transcription straight onto the face.

#include "pv.h"

static String   s_final;     // turns the server has closed
static String   s_partial;   // the turn still being spoken
static bool     s_dirty;
static bool     s_live;      // the socket is open and the face knows it
static uint32_t s_last_draw;

static void onTurn(const String& text, bool final) {
  if (final) {
    if (text.length()) {
      s_final += text;
      s_final += " ";
      // Keep the running caption bounded, trimming on a word boundary so the
      // text never restarts mid-word.
      if (s_final.length() > 600) {
        const int cut = s_final.indexOf(' ', s_final.length() - 400);
        if (cut > 0) s_final = s_final.substring(cut + 1);
      }
      relayText(text);
    }
    s_partial = "";
  } else {
    s_partial = text;
  }
  s_dirty = true;
}

static bool keepGoing() {
  // sttStream only starts calling this once the socket is open, so this is
  // the first honest moment to stop saying "Connecting". Without it the face
  // sits on the connecting screen until someone speaks a whole turn, which
  // reads as a hang.
  if (!s_live) {
    s_live = true;
    drawTail(String("listening..."), kMuted);
    hint("tap to stop", kLive);
  }
  if (poked()) return false;
  if (s_dirty && millis() - s_last_draw > 160) {
    s_dirty = false;
    s_last_draw = millis();
    // Partial text stays grey, so you can see what the model has committed to
    // and what it is still revising.
    drawTail(s_final + s_partial, kPaper);
    hint("tap to stop", kLive);
  }
  return true;
}

void capIdle() {
  faceClear();
  caption("CAPTIONS");
  title("Live captions");
  hint("tap to start");
}

void capAct() {
  s_final = "";
  s_partial = "";
  s_dirty = false;
  s_live = false;
  s_last_draw = 0;

  faceClear();
  caption("CAPTIONS");
  title("Connecting");
  hint("opening the socket", kMuted);

  if (!sttStream(onTurn, keepGoing)) {
    faceClear();
    caption("CAPTIONS");
    title("No socket", kError);
    hint(g_last_err, kError);
    delay(2000);
    return;
  }
  if (s_final.length()) resultViewer("CAPTIONS", s_final);
}
