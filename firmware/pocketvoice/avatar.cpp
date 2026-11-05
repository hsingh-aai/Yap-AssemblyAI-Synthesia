// A mouth that moves with the voice.
//
// Jack's replies are written by an LLM about a second before he says them,
// so no pre-rendered clip can ever be saying his words. What a pre-rendered
// clip can give us is a face and a ladder of mouth shapes; this picks one
// twenty times a second from the envelope of the audio that is sounding
// right now. It never forms a real word and it does not need to - what reads
// as speech is the mouth moving when the voice moves and shutting in the
// gaps between words.
//
// Two decisions do most of the work:
//
//  * The base face is a JPEG decoded once, and each mouth is raw RGB565
//    blitted straight to the panel. Re-decoding a 300 px JPEG every frame
//    would cost more than the frame budget; pushing a 128x76 sprite costs
//    about a millisecond.
//  * The mouth is chosen from the playback *clock*, not from the block being
//    queued. pumpSpeaker() runs up to 200 ms ahead of the speaker, and a
//    mouth 200 ms early looks worse than no mouth at all.

#include "pv.h"
#include "avatar.h"

static constexpr uint32_t kFrameMs = 50;        // 20 fps

// After loudenUp() the reply is gain-5 and soft-limited, so the loud end is
// squeezed into very little range. A hand-set ladder keeps the gaps between
// words doing the work, which is where a mouth actually reads.
static const int32_t kStep[kMouthLevels - 1] = {
  1500, 6000, 12000, 19000, 26000,
};

static uint32_t s_t0;        // millis() at which the first sample sounded
static uint32_t s_last;      // last frame drawn
static int32_t  s_env;       // smoothed envelope
static int      s_level;
static int      s_ox, s_oy;  // where the face sits on the round display

static void blit(int level) {
  M5.Display.pushImage(s_ox + kMouthX, s_oy + kMouthY, kMouthW, kMouthH,
                       kMouthPix[level]);
}

void avatarShow() {
  // A 300 px square inscribes in this 434 px circle with room to spare; the
  // small lift keeps the chin clear of the hint line, and the corners that
  // fall outside the glass are background colour anyway.
  s_ox = cx - kAvatarSize / 2;
  s_oy = cy - kAvatarSize / 2 - 24;
  M5.Display.drawJpg(kAvatarBase, kAvatarBaseLen, s_ox, s_oy);
  s_level = 0;
  s_env = 0;
  s_t0 = 0;
  s_last = 0;
}

void avatarStart(uint32_t t0) {
  s_t0 = t0;
  s_env = 0;
}

void avatarRest() {
  s_t0 = 0;
  s_env = 0;
  if (s_level != 0) { s_level = 0; blit(0); }
}

void avatarAnimate(const int16_t* pcm, size_t have, uint32_t rate) {
  if (!s_t0) return;

  const uint32_t now = millis();
  if (now - s_last < kFrameMs) return;
  s_last = now;

  const size_t at = (size_t)((uint64_t)(now - s_t0) * rate / 1000);
  if (at >= have) { avatarRest(); return; }

  const size_t win = rate / 40;                 // 25 ms of audio
  const size_t end = at + win < have ? at + win : have;
  int32_t peak = 0;
  for (size_t i = at; i < end; ++i) {
    const int32_t v = pcm[i] < 0 ? -pcm[i] : pcm[i];
    if (v > peak) peak = v;
  }

  // Fast attack, slow release. A mouth that tracks the samples exactly
  // flickers; one that snaps open and eases shut reads as speech.
  if (peak > s_env) s_env = peak;
  else              s_env -= (s_env - peak) / 3;

  int level = 0;
  while (level < kMouthLevels - 1 && s_env > kStep[level]) ++level;
  if (level != s_level) { s_level = level; blit(level); }
}
