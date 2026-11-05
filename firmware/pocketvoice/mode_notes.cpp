// Notetaker: listen for as long as you like, then hand the transcript to the
// LLM Gateway for a summary and action items.

#include "pv.h"

// Fast and non-reasoning: the reasoning models here spend their whole token
// budget thinking and hand back an empty message unless max_tokens is large,
// which is the wrong trade on a device someone is standing in front of.
static constexpr char kModel[] = "gemini-3.5-flash-lite";

static constexpr char kPrompt[] =
    "Summarise this meeting in under 40 words, then list action items as "
    "short lines starting with a dash. Name owners where they are stated. "
    "Plain text only, no markdown.\n\nTranscript: ";

static String   s_text;      // everything said, in order
static String   s_partial;
static int      s_turns;
static bool     s_dirty;
static bool     s_live;
static uint32_t s_last_draw;

static void onTurn(const String& text, bool final) {
  if (final) {
    if (text.length()) {
      s_text += text;
      s_text += " ";
      ++s_turns;
      logf("[note] %s", text.substring(0, 40).c_str());
    }
    s_partial = "";
  } else {
    s_partial = text;
  }
  s_dirty = true;
}

static bool keepGoing() {
  if (!s_live) {                       // socket is up: see mode_captions.cpp
    s_live = true;
    drawTail(String("listening..."), kMuted);
    hint("0 turns   tap to finish", kLive);
  }
  if (homeChord() || poked()) return false;
  if (s_dirty && millis() - s_last_draw > 200) {
    s_dirty = false;
    s_last_draw = millis();
    drawTail(s_text + s_partial, kPaper);
    char buf[40];
    snprintf(buf, sizeof buf, "%d turns   tap to finish", s_turns);
    hint(buf, kLive);
  }
  return true;
}

void notesIdle() {
  faceClear();
  caption("NOTETAKER");
  title("Notetaker");
  hint("tap to start listening");
}

void notesAct() {
  s_text = "";
  s_partial = "";
  s_turns = 0;
  s_dirty = false;
  s_live = false;
  s_last_draw = 0;

  faceClear();
  caption("NOTETAKER");
  title("Connecting");
  hint("opening the socket", kMuted);

  if (!sttStream(onTurn, keepGoing)) {
    if (g_go_home) return;
    faceClear();
    caption("NOTETAKER");
    title("No socket", kError);
    hint(g_last_err, kError);
    delay(2000);
    return;
  }
  if (g_go_home) return;           // left mid-session: keep nothing

  if (s_text.length() < 20) {
    faceClear();
    caption("NOTETAKER");
    title("Too short");
    hint("nothing to summarise", kMuted);
    delay(1800);
    return;
  }

  faceClear();
  caption("NOTETAKER");
  title("Writing notes");
  hint("summarising", kMuted);

  // The gateway takes about a second; the transcript is already bounded by
  // how long someone will stand here talking.
  const String notes = llmAsk(kModel, String(kPrompt) + s_text, 700);
  if (!notes.length()) {
    faceClear();
    caption("NOTETAKER");
    title("No notes", kError);
    hint(g_last_err, kError);
    delay(2500);
    resultViewer("TRANSCRIPT", s_text);   // the words are still worth showing
    return;
  }
  relayText(notes);
  resultViewer("NOTES", notes);
}
