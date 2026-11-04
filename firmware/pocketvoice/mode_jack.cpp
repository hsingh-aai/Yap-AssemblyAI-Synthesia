// Jack: a voice agent you can actually talk to.
//
// Half duplex, and not by choice - see the README. The mic and speaker share
// the I2S clock pins, so the firmware hands them over explicitly and barge-in
// is a screen tap rather than a voice interruption.

#include "pv.h"
#include <WebSocketsClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/base64.h>

static constexpr char kAgentHost[] = "agents.assemblyai.com";
static constexpr char kAgentPath[] = "/v1/ws";

// Jack the Roaster, from the Halloween station repo. Not a secret: stored
// agents are referenced by id and the persona lives on AssemblyAI's side.
static constexpr char kAgentId[] = "agent_921917929cb944e19960ffdfbd53f217";

static constexpr size_t kMicChunk  = kAgentRate / 20;   // 50 ms
static constexpr size_t kPlayBlock = kAgentRate / 10;   // 100 ms

// One virtual channel, named explicitly. With the default channel of -1
// M5Unified hands each request to the next *free* channel and mixes them, so
// consecutive blocks of one sentence play on top of each other rather than
// after one another. That is what made Jack unintelligible.
static constexpr int kSpkCh = 0;

// Each channel queues two requests, so the speaker is never more than 200 ms
// ahead of the network. Hold the first block back until there is a little
// more than that buffered, or the first jitter on the wire is a dropout.
static constexpr size_t kPrebuffer = kAgentRate * 2 / 5;  // 400 ms

enum Phase : uint8_t { CONNECTING, LISTENING, SPEAKING };
static uint8_t  s_phase;
static bool     s_ready;
static String   s_user, s_agent;
static bool     s_dirty;
static uint32_t s_last_draw;
static int      s_replies;

// Replies arrive as ~11 ms fragments, ~100 a second. Collect them in PSRAM
// and hand the speaker 100 ms at a time: one playRaw per fragment would
// thrash the queue.
static volatile size_t s_reply_len;
static size_t          s_played;
static volatile bool   s_reply_done;
static size_t          s_max_chans;   // above 1 means blocks are being mixed

static WebSocketsClient ws;

// Jack arrives peaking at -0.2 dBFS with an RMS near -20: a few loud
// consonants sitting over quiet speech. Plain gain only shears the peaks off,
// so lift the whole signal and round the tops over instead. Measured on a
// real reply, the knee keeps every single sample off the rail:
//
//     gain    loudness    samples at full scale
//     x1.5     +3.2 dB    0.00%
//     x2.5     +6.6 dB    0.00%
//     x5.0     +9.7 dB    0.00%
//
// kGain is the only knob; drop it to back off the squash. With the master
// volume at maximum as well, this is about +14 dB over where it started.
static constexpr int32_t kGain = 5;
static constexpr int32_t kKnee = 22000;     // counts; soft above here
static constexpr int32_t kFull = 32767;

static void loudenUp(int16_t* pcm, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    const int32_t v = (int32_t)pcm[i] * kGain;
    const int32_t a = v < 0 ? -v : v;
    int32_t out;
    if (a <= kKnee) {
      out = a;
    } else {
      // Fold the span above the knee into the room left below full scale, so
      // it approaches 32767 asymptotically and never crosses it. 64-bit
      // because the numerator overflows an int32 for a loud enough sample.
      const int64_t over = a - kKnee;
      const int64_t room = kFull - kKnee;
      out = (int32_t)(kKnee + (over * room) / (over + room));
    }
    pcm[i] = (int16_t)(v < 0 ? -out : out);
  }
}

static void startSpeaking() {
  if (s_phase == SPEAKING) return;
  M5.Mic.end();                    // hand the shared I2S clock pins over
  M5.Speaker.begin();
  M5.Speaker.setVolume(255);   // master, maximum
  s_reply_len = 0;
  s_played = 0;
  s_reply_done = false;
  s_agent = "";
  s_phase = SPEAKING;
  s_dirty = true;
}

static void startListening() {
  M5.Speaker.stop();
  M5.Speaker.end();
  M5.Mic.begin();
  s_user = "";
  s_phase = LISTENING;
  s_dirty = true;
}

static void onText(const char* buf, size_t len) {
  const String type = jsonStr(buf, len, "type");

  if (type == "session.ready") {
    s_ready = true;
    logf("[jack] ready");
    startListening();
    return;
  }
  if (type == "reply.audio") {
    size_t n = 0;
    const char* b64 = jsonFind(buf, len, "data", &n);
    if (!b64 || !g_reply) return;
    if (s_phase != SPEAKING) startSpeaking();
    uint8_t* dst = (uint8_t*)(g_reply + s_reply_len);
    const size_t room = (kReplyMax - s_reply_len) * sizeof(int16_t);
    size_t out = 0;
    if (mbedtls_base64_decode(dst, room, &out,
                              (const unsigned char*)b64, n) == 0) {
      const size_t got = out / sizeof(int16_t);
      loudenUp(g_reply + s_reply_len, got);
      s_reply_len += got;
    }
    return;
  }
  if (type == "reply.started") { startSpeaking(); return; }
  if (type == "reply.done") {
    s_reply_done = true;
    ++s_replies;
    return;
  }
  if (type == "transcript.agent" || type == "transcript.agent.delta") {
    s_agent = jsonStr(buf, len, "text");
    s_dirty = true;
    return;
  }
  if (type == "transcript.user" || type == "transcript.user.delta") {
    const String t = jsonStr(buf, len, "text");
    if (t.length()) { s_user = t; s_dirty = true; }
    return;
  }
  if (type == "session.error" || type == "error") {
    failf("%s", jsonStr(buf, len, "message").substring(0, 40).c_str());
  }
}

static void onEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      // agent_id must be the only field in `session`: it is mutually
      // exclusive with the inline persona fields.
      char buf[160];
      snprintf(buf, sizeof buf,
               "{\"type\":\"session.update\",\"session\":{\"agent_id\":\"%s\"}}",
               kAgentId);
      ws.sendTXT(buf);
      break;
    }
    case WStype_DISCONNECTED:
      logf("[ws] disconnected (ready=%d)", s_ready);
      s_ready = false;
      break;
    case WStype_TEXT:
      onText((const char*)payload, length);
      break;
    case WStype_ERROR:
      failf("agent socket error: %.40s", (const char*)payload);
      break;
    default:
      logf("[ws] evt %d len %u", (int)type, (unsigned)length);
      break;
  }
}

static void drawBody() {
  if (s_phase == SPEAKING) {
    drawTail(s_agent.length() ? s_agent : String("..."), kPaper);
    hint("tap to interrupt", kLive);
  } else {
    drawTail(s_user.length() ? s_user : String("listening..."),
             s_user.length() ? kPaper : kMuted);
    hint("just talk   tap to end");
  }
}

// Feed the speaker in blocks. Returns true once the whole reply is queued.
//
// playRaw() blocks when the channel's queue is full, so check for room first:
// stalling here would stall ws.loop() and starve the very stream being
// played.
static bool pumpSpeaker() {
  const size_t have = s_reply_len;
  if (!s_played && !s_reply_done && have < kPrebuffer) return false;

  while (have - s_played >= kPlayBlock) {
    if (M5.Speaker.isPlaying(kSpkCh) >= 2) return false;
    if (!M5.Speaker.playRaw(g_reply + s_played, kPlayBlock, kAgentRate,
                            false, 1, kSpkCh, false)) {
      return false;
    }
    s_played += kPlayBlock;
  }
  if (s_reply_done && s_played < have && M5.Speaker.isPlaying(kSpkCh) < 2) {
    if (M5.Speaker.playRaw(g_reply + s_played, have - s_played, kAgentRate,
                           false, 1, kSpkCh, false)) {
      s_played = have;
    }
  }
  return s_reply_done && s_played >= have;
}

void jackIdle() {
  faceClear();
  caption("JACK");
  title("Talk to Jack");
  hint("tap to start");
}

void jackAct() {
  if (!wifiUp()) {
    faceClear(); caption("JACK"); title("No Wi-Fi", kError);
    hint("check the status page", kError); delay(2000); return;
  }

  s_phase = CONNECTING;
  s_ready = false;
  s_user = "";
  s_agent = "";
  s_replies = 0;

  faceClear();
  caption("JACK");
  title("Connecting");
  hint("waking Jack up", kMuted);

  {
    // Isolate TLS from the WebSocket layer: if this fails the problem is
    // certificates or heap, not the handshake.
    WiFiClientSecure probe;
    probe.setInsecure();
    const uint32_t t0 = millis();
    const int ok = probe.connect(kAgentHost, 443);
    logf("[tls] connect=%d in %ums heap=%u", ok, (unsigned)(millis() - t0),
         (unsigned)ESP.getFreeHeap());
    probe.stop();
  }

  ws.onEvent(onEvent);
  ws.setExtraHeaders("Authorization: Bearer " AAI_API_KEY);
  ws.beginSSL(kAgentHost, 443, kAgentPath);

  const uint32_t deadline = millis() + 15000;
  while (!s_ready && millis() < deadline) {
    ws.loop();
    M5.update();
    if (poked()) { ws.disconnect(); return; }
    delay(10);
  }
  if (!s_ready) {
    failf("no agent socket");
    ws.disconnect();
    faceClear(); caption("JACK"); title("No answer", kError);
    hint(g_last_err, kError); delay(2500);
    return;
  }

  static uint8_t b64[kMicChunk * 2 * 4 / 3 + 8];
  static char    frame[sizeof(b64) + 64];
  int cur = 0;
  bool primed = false;

  while (true) {
    ws.loop();
    M5.update();

    if (poked()) {
      if (s_phase == SPEAKING) {
        // Our barge-in: the mic is off while the speaker owns the pins, so
        // the server's own interrupt path can never fire here.
        logf("[jack] interrupted");
        M5.Speaker.stop();
        startListening();
        primed = false;
        continue;
      }
      break;
    }

    if (s_dirty && millis() - s_last_draw > 160) {
      s_dirty = false;
      s_last_draw = millis();
      drawBody();
    }

    if (s_phase == SPEAKING) {
      const size_t c = M5.Speaker.getPlayingChannels();
      if (c > s_max_chans) s_max_chans = c;
      if (pumpSpeaker() && !M5.Speaker.isPlaying()) {
        // chans=1 is the fix holding. Anything higher means playRaw found a
        // second channel and two parts of one sentence played at once.
        logf("[jack] reply %ums chans=%u",
             (unsigned)(s_reply_len * 1000 / kAgentRate), (unsigned)s_max_chans);
        s_max_chans = 0;
        startListening();
        primed = false;
      }
      delay(2);
      continue;
    }

    if (!primed) {
      M5.Mic.record(mic_buf[0], kMicChunk, kAgentRate);
      M5.Mic.record(mic_buf[1], kMicChunk, kAgentRate);
      cur = 0;
      primed = true;
    }

    const int next = (cur + 2) % kMicBufs;
    if (!M5.Mic.record(mic_buf[next], kMicChunk, kAgentRate)) {
      failf("mic stalled");
      break;
    }

    size_t n = 0;
    if (mbedtls_base64_encode(b64, sizeof b64, &n,
                              (const unsigned char*)mic_buf[cur],
                              kMicChunk * sizeof(int16_t)) != 0) {
      failf("b64 encode");
      break;
    }
    const int len = snprintf(frame, sizeof frame,
                             "{\"type\":\"input.audio\",\"audio\":\"%.*s\"}",
                             (int)n, (const char*)b64);
    if (!ws.sendTXT((uint8_t*)frame, len)) { failf("agent send failed"); break; }
    cur = (cur + 1) % kMicBufs;
  }

  ws.sendTXT("{\"type\":\"session.end\"}");
  for (int i = 0; i < 30; ++i) { ws.loop(); delay(10); }
  ws.disconnect();

  M5.Speaker.stop();
  M5.Speaker.end();
  M5.Mic.begin();
  logf("[jack] done, %d replies", s_replies);
}

// Opens the agent socket, waits for session.ready, reports, and closes. Does
// no audio: this answers "can this board reach Jack at all".
void jackSelfTest() {
  logf("[selftest] heap=%u", (unsigned)ESP.getFreeHeap());
  if (!wifiUp()) { logf("[selftest] no wifi"); return; }

  {
    WiFiClientSecure probe;
    probe.setInsecure();
    const uint32_t t0 = millis();
    const int ok = probe.connect(kAgentHost, 443);
    logf("[selftest] tls connect=%d in %ums", ok, (unsigned)(millis() - t0));
    probe.stop();
  }

  s_ready = false;
  ws.onEvent(onEvent);
  ws.setExtraHeaders("Authorization: Bearer " AAI_API_KEY);
  ws.beginSSL(kAgentHost, 443, kAgentPath);

  s_phase = CONNECTING;
  s_reply_len = 0;
  s_played = 0;
  s_reply_done = false;

  const uint32_t deadline = millis() + 20000;
  while (!s_ready && millis() < deadline) { ws.loop(); delay(10); }
  logf("[selftest] ready=%d heap=%u", s_ready, (unsigned)ESP.getFreeHeap());

  // Jack greets unprompted, so the whole playback path can be exercised
  // without anyone speaking into the board: let the greeting arrive and
  // render it. getPlayingChannels() above 1 would mean blocks are being
  // mixed on top of each other instead of queued.
  if (s_ready) {
    size_t chans = 0;
    const uint32_t audio_by = millis() + 20000;
    while (millis() < audio_by) {
      ws.loop();
      if (s_phase == SPEAKING) {
        const bool queued = pumpSpeaker();
        const size_t c = M5.Speaker.getPlayingChannels();
        if (c > chans) chans = c;
        if (queued && !M5.Speaker.isPlaying()) break;
      }
      delay(2);
    }
    logf("[selftest] reply %ums max_channels=%u",
         (unsigned)(s_reply_len * 1000 / kAgentRate), (unsigned)chans);
    ws.sendTXT("{\"type\":\"session.end\"}");
  }
  for (int i = 0; i < 30; ++i) { ws.loop(); delay(10); }
  ws.disconnect();

  M5.Speaker.stop();
  M5.Speaker.end();
  M5.Mic.begin();
  logf("[selftest] done");
}
