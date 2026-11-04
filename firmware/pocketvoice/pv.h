// Pocket Voice - shared declarations.
//
// One firmware, every mode. The three phase sketches each grew their own copy
// of the round-screen layout code and the log ring; this is that code once,
// with the modes reduced to what actually differs between them.

#pragma once

#include <M5Unified.h>
#include <WiFi.h>

#include "secrets.h"

// Deck palette.
static constexpr uint32_t kInk     = 0x131210;
static constexpr uint32_t kPaper   = 0xF3EFE6;
static constexpr uint32_t kMuted   = 0xB8B2A7;
static constexpr uint32_t kAccent  = 0xA99CFF;
static constexpr uint32_t kAccent2 = 0x3F2BC1;
static constexpr uint32_t kError   = 0xE86A5C;
static constexpr uint32_t kLive    = 0xE8625C;

static constexpr char kApiHost[] = "https://api.assemblyai.com";

// The panel reports 468x468 but the visible glass is a 466 px circle: centre
// on the panel, size to the glass.
static constexpr int kFace = 466;
extern int cx, cy, radius;

// ---------------------------------------------------------------- display

void  uiBegin();
float chordHalf(int dy);
float fitScale(const char* s, int dy, float cap = 1.0f);
void  faceClear();
void  caption(const char* name);
void  title(const char* msg, uint32_t color = kPaper);
void  hint(const char* msg, uint32_t color = kAccent);
void  statusLine(const char* msg, uint32_t color = kMuted);
void  levelBar(uint8_t level);
void  drawLevelRing(uint8_t level, int spoke);

// Text wrapped to the circle's chord, line by line: a round face loses width
// fast towards the top and bottom, so a string that fits across the middle
// will not fit under it.
static constexpr int kTextLines = 8;
static constexpr int kLineH     = 30;
void   clearTextBand();
size_t layoutPage(const String& t, size_t from, bool render,
                  uint32_t color = kPaper, size_t grey_from = SIZE_MAX);
void   drawTail(const String& t, uint32_t color = kPaper);
int    countPages(const String& t, size_t* starts, int max_pages);

// ------------------------------------------------------------------ log

void logf(const char* fmt, ...);

// Sends a finished transcript up the USB cable, whole. logf() is bounded by
// the on-screen log's width and by its own 192-byte line; a sentence being
// dictated into a Mac has to arrive complete. Newlines are escaped so one
// transcript is always exactly one line for the reader on the other end.
void relayText(const String& text);
void failf(const char* fmt, ...);
extern char g_last_err[48];
void drawLogBody();
void drawStatusBody();

// ----------------------------------------------------------------- input

// Every "do the thing" gesture - screen tap or the yellow button - lands here,
// so an on-screen hint only ever has to name one of them.
bool poked();

// ------------------------------------------------------------------ wifi

bool wifiUp();
void wifiConnect();

// ----------------------------------------------------------------- audio

static constexpr uint32_t kSttRate   = 16000;   // Blurt and captions
static constexpr uint32_t kAgentRate = 24000;   // Voice Agent API
static constexpr size_t   kMicMax    = 1200;    // 50 ms at the higher rate
static constexpr int      kMicBufs   = 3;
extern int16_t mic_buf[kMicBufs][kMicMax];

uint8_t chunkPeak(const int16_t* buf, size_t n);

// PSRAM, allocated once at boot. Only the agent needs a big buffer now:
// every speech mode streams, so nothing else holds audio to send later.
static constexpr size_t kReplyMax = kAgentRate * 30;  // 30 s of agent reply
extern int16_t* g_reply;

// ----------------------------------------------------------------- modes

// A mode owns the whole face while it runs. `idle` paints its resting state,
// `act` runs it until the user ends it, `tick` is optional background work
// (a mic meter) while the mode sits idle.
struct Mode {
  const char* name;
  const char* blurb;
  void (*idle)();
  void (*act)();
  void (*tick)();
};

extern const Mode kModes[];
extern const int  kModeCount;

void blurtIdle();    void blurtAct();  void blurtTick();
void capIdle();      void capAct();
void jackIdle();     void jackAct();
void statusIdle();
void logIdle();

// ------------------------------------------------------------------ json

// Top-level field access over a raw, non-null-terminated frame. See json.cpp
// for why both of those properties matter.
const char* jsonFind(const char* buf, size_t len, const char* key, size_t* vlen);
String      jsonStr(const char* buf, size_t len, const char* key);
bool        jsonTrue(const char* buf, size_t len, const char* key);

// ------------------------------------------------------------ stt stream

// Runs one realtime STT session. `onTurn` gets every turn as it arrives,
// `final` false while the model is still revising it. `keepGoing` is called
// each loop: return false to end the session. Returns false if the socket
// never opened.
bool sttStream(void (*onTurn)(const String& text, bool final),
               bool (*keepGoing)());

// Chat completion through the LLM Gateway. Returns "" on failure and sets
// the error line.
String llmAsk(const char* model, const String& prompt, int max_tokens);

void notesIdle();  void notesAct();

// Blocking viewer for a finished result: pages with B, returns on a poke.
void resultViewer(const char* cap, const String& text);

// Boot-time checks that a socket comes up, so the network path can be
// debugged without a finger on the device. PV_SELFTEST is a bitmask:
// 1 runs the agent check, 2 the realtime STT one, 3 both.
#ifndef PV_SELFTEST
#define PV_SELFTEST 0
#endif
void jackSelfTest();
void sttSelfTest();
