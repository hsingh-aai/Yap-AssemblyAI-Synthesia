// The realtime STT socket, shared by Captions and Notetaker.

#include "pv.h"
#include <WebSocketsClient.h>

static constexpr char kSttHost[] = "streaming.assemblyai.com";
static constexpr char kSttPath[] =
    "/v3/ws?sample_rate=16000&encoding=pcm_s16le&format_turns=true";

// 50 ms per frame is the low end of the recommended 50-1000 ms range, which
// is what keeps captions feeling immediate.
static constexpr size_t kSttChunk = kSttRate / 20;   // 800 samples

static WebSocketsClient ws;
static bool g_open = false;      // Begin has arrived; audio may flow
static bool g_opening = false;   // handshake in flight
static bool g_ended = false;     // server acknowledged our Terminate
static bool g_trace = false;
static void (*g_onTurn)(const String&, bool) = nullptr;

static void onEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      g_open = false;
      g_opening = false;
      break;
    case WStype_TEXT: {
      const char* buf = (const char*)payload;
      const size_t len = length;
      const String t = jsonStr(buf, len, "type");
      if (g_trace) logf("[stt] rx %s (%u B)", t.c_str(), (unsigned)len);
      if (t == "Begin") {
        g_open = true;
        g_opening = false;
      } else if (t == "Termination") {
        g_ended = true;
      } else if (t == "Turn") {
        if (g_onTurn) g_onTurn(jsonStr(buf, len, "transcript"),
                               jsonTrue(buf, len, "end_of_turn"));
      } else if (t == "Error") {
        failf("stt: %s", jsonStr(buf, len, "error").substring(0, 36).c_str());
      }
      break;
    }
    case WStype_ERROR:
      failf("stt socket error");
      break;
    default:
      break;
  }
}

// Start the handshake and return immediately. The expensive part of opening
// this socket is the TLS handshake, and it does not care whether anyone is
// looking - so a mode that knows it is about to be used can pay for it while
// the user is still reading the screen.
bool sttOpen() {
  if (g_open || g_opening) return true;
  if (!wifiUp()) return false;

  g_onTurn = nullptr;
  g_ended = false;
  ws.onEvent(onEvent);
  // No Bearer prefix on this API, unlike the Voice Agent API.
  ws.setExtraHeaders("Authorization: " AAI_API_KEY);
  ws.beginSSL(kSttHost, 443, kSttPath);
  g_opening = true;
  return true;
}

// Call often while idle: this is what drives the handshake to completion.
void sttPump() {
  if (g_open || g_opening) ws.loop();
}

void sttClose() {
  if (g_open) ws.sendTXT("{\"type\":\"Terminate\"}");
  ws.disconnect();
  g_open = false;
  g_opening = false;
  g_onTurn = nullptr;
}

bool sttStream(void (*onTurn)(const String&, bool), bool (*keepGoing)()) {
  if (!wifiUp()) { failf("wi-fi down"); return false; }

  // Already warm costs nothing here; cold pays the handshake now.
  const uint32_t t0 = millis();
  const bool warm = g_open;
  sttOpen();
  g_onTurn = onTurn;
  g_ended = false;

  const uint32_t deadline = millis() + 12000;
  while (!g_open && millis() < deadline) {
    ws.loop();
    M5.update();
    if (homeChord()) { sttClose(); return false; }
    delay(5);
  }
  if (!g_open) {
    failf("no stt socket");
    sttClose();
    return false;
  }
  logf("[stt] ready in %ums (%s)", (unsigned)(millis() - t0),
       warm ? "warm" : "cold");

  // Prime both mic slots, then ride record()'s own back-pressure: it returns
  // only when a slot frees, which is exactly when the oldest buffer is full.
  // Three buffers means two are always queued while the third is on the wire.
  M5.Mic.record(mic_buf[0], kSttChunk, kSttRate);
  M5.Mic.record(mic_buf[1], kSttChunk, kSttRate);

  int cur = 0;
  uint32_t frames = 0;
  while (true) {
    ws.loop();
    M5.update();
    if (!keepGoing()) break;

    const int next = (cur + 2) % kMicBufs;
    if (!M5.Mic.record(mic_buf[next], kSttChunk, kSttRate)) {
      failf("mic stalled");
      break;
    }
    if (!ws.sendBIN((uint8_t*)mic_buf[cur], kSttChunk * sizeof(int16_t))) {
      failf("stt send failed");
      break;
    }
    ++frames;
    cur = (cur + 1) % kMicBufs;
    ws.loop();
  }

  // Wait for the server to acknowledge, not for a fixed 400 ms. The last turn
  // usually lands in a fraction of that, and the user is standing there.
  const uint32_t t_end = millis();
  ws.sendTXT("{\"type\":\"Terminate\"}");
  while (!g_ended && millis() - t_end < 600) { ws.loop(); delay(5); }
  ws.disconnect();
  g_open = false;
  g_opening = false;
  g_onTurn = nullptr;
  logf("[stt] %u frames (%.1fs) close %ums", (unsigned)frames, frames * 0.05f,
       (unsigned)(millis() - t_end));
  return true;
}

// Opens the realtime socket, streams a few seconds of mic and reports what
// came back. Answers "is Captions a network problem or a device one" without
// anyone holding the board.
static uint32_t s_test_turns;
static uint32_t s_test_deadline;

static void testTurn(const String& text, bool final) {
  ++s_test_turns;
  logf("[selftest] turn%s %s", final ? "*" : "", text.substring(0, 28).c_str());
}

static bool testKeepGoing() {
  return millis() < s_test_deadline;
}

void sttSelfTest() {
  logf("[selftest] stt heap=%u", (unsigned)ESP.getFreeHeap());
  if (!wifiUp()) { logf("[selftest] no wifi"); return; }

  // The mic is the other half of this: a silent capture looks exactly like a
  // dead socket from the screen, so measure it before blaming the network.
  M5.Mic.record(mic_buf[0], kSttChunk, kSttRate);
  M5.Mic.record(mic_buf[1], kSttChunk, kSttRate);
  uint8_t peak = 0;
  for (int i = 0; i < 20; ++i) {
    M5.Mic.record(mic_buf[2], kSttChunk, kSttRate);
    const uint8_t p = chunkPeak(mic_buf[2], kSttChunk);
    if (p > peak) peak = p;
  }
  logf("[selftest] mic peak=%u (1s)", peak);

  g_trace = true;
  s_test_turns = 0;
  s_test_deadline = millis() + 8000;
  const bool ok = sttStream(testTurn, testKeepGoing);
  g_trace = false;
  logf("[selftest] stt ok=%d turns=%u err=%s", ok, (unsigned)s_test_turns,
       g_last_err);
}
