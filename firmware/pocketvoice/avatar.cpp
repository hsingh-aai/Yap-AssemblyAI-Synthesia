// A face that talks while the agent talks.
//
// The first version swapped cut-out mouths by loudness. That works on a drawn
// face and fails on a real one: a person's cheeks, moustache and chin move
// when their mouth does, so each cut-out brought a different moment's face
// with it and stepping between them shimmered, however the cut-outs were
// picked (tools/ladderpop.py measures it). So this plays real video instead:
// ~1.6 s of consecutive frames of the lower face, forwards then backwards
// while the voice is going, so there is never a seam in time. The lips do not
// form the agent's words; they move the way a person's do, and start and
// stop with the voice.
//
// Two rules keep it from looking mechanical:
//
//  * It is clocked from playback, not from the queue. pumpSpeaker() runs up
//    to 200 ms ahead of the speaker, so "is he talking now" is read from the
//    sample actually sounding: (millis() - t0) * rate / 1000 into g_reply.
//  * When the voice stops it keeps playing until it reaches a frame with the
//    mouth shut, then holds there. Freezing mid-word leaves him gaping;
//    cutting straight to shut is a jump.

#include "pv.h"
#include "avatar.h"

static constexpr uint32_t kFrameMs   = 1000 / kTalkFps;  // the recording's own pace
static constexpr uint32_t kWinMs     = 20;               // RMS window
static constexpr float    kSmooth    = 0.35f;            // per-frame envelope smoothing
static constexpr float    kGate      = 400.0f;           // RMS above this is voice
static constexpr uint32_t kHangMs    = 180;              // a gap shorter than this is not a pause

static uint32_t s_t0;        // millis() at which the first sample sounded
static uint32_t s_last;      // last frame drawn
static uint32_t s_voiced_at; // last time the voice was above the gate
static float    s_env;
static int      s_idx;       // frame of the loop on screen
static int      s_dir;       // +1 forwards, -1 backwards
static int      s_ox, s_oy;  // where the face sits on the round display

static void blit(int i) {
  M5.Display.pushImage(s_ox + kTalkX, s_oy + kTalkY, kTalkW, kTalkH, kTalkPix[i]);
}

static void step() {
  if (s_idx + s_dir < 0 || s_idx + s_dir >= kTalkFrames) s_dir = -s_dir;
  s_idx += s_dir;
  blit(s_idx);
}

void avatarShow() {
  // A 300 px square inscribes in this 434 px circle with room to spare; the
  // small lift keeps the chin clear of the hint line, and the corners that
  // fall outside the glass are background anyway.
  s_ox = cx - kAvatarSize / 2;
  s_oy = cy - kAvatarSize / 2 - 24;
  M5.Display.pushImage(s_ox, s_oy, kAvatarSize, kAvatarSize, kAvatarBasePix);
  s_idx = 0;
  s_dir = 1;
  s_env = 0.0f;
  s_t0 = 0;
  s_last = 0;
  s_voiced_at = 0;
}

void avatarStart(uint32_t t0) {
  s_t0 = t0;
  s_env = 0.0f;
}

void avatarRest() {
  s_t0 = 0;
  s_env = 0.0f;
  // Settle on the loop's first frame, which is the resting face itself.
  if (s_idx != 0) { s_idx = 0; s_dir = 1; blit(0); }
}

void avatarAnimate(const int16_t* pcm, size_t have, uint32_t rate) {
  if (!s_t0) return;

  const uint32_t now = millis();
  if (now - s_last < kFrameMs) return;
  s_last = now;

  const size_t at = (size_t)((uint64_t)(now - s_t0) * rate / 1000);
  float rms = 0.0f;
  if (at < have) {
    const size_t win = rate * kWinMs / 1000;
    const size_t end = at + win < have ? at + win : have;
    float sum = 0.0f;
    for (size_t i = at; i < end; ++i) sum += (float)pcm[i] * pcm[i];
    rms = end > at ? sqrtf(sum / (float)(end - at)) : 0.0f;
  }
  s_env += (rms - s_env) * kSmooth;
  if (s_env > kGate) s_voiced_at = now;

  const bool talking = s_voiced_at && now - s_voiced_at < kHangMs;
  if (talking || kTalkGap[s_idx] > kTalkShut) step();
}
