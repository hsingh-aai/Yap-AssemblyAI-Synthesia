#!/usr/bin/env python3
"""Replay the device's mouth driver on real audio and score its jitter."""
import math, struct, sys, wave

def load(path):
    w = wave.open(path); rate = w.getframerate()
    raw = w.readframes(w.getnframes())
    pcm = list(struct.unpack(f"<{len(raw)//2}h", raw))
    return pcm, rate

def louden(pcm, gain=5, knee=22000, full=32767):      # mode_jack.cpp loudenUp()
    out = []
    for s in pcm:
        v = s * gain; a = abs(v)
        if a > knee:
            over, room = a - knee, full - knee
            a = int(knee + over * room / (over + room))
        out.append(a if v >= 0 else -a)
    return out

def drive(pcm, rate, *, fps, win_ms, measure, attack, release, levels,
          steps=None, gate=None, top=None, slew=None):
    """Returns the mouth level chosen at each frame."""
    frame = 1.0 / fps; t = 0.0; env = 0.0; lvl = 0; out = []
    while True:
        at = int(t * rate)
        if at >= len(pcm): break
        w = pcm[at: at + int(rate * win_ms / 1000)]
        if measure == "peak":
            x = max(abs(s) for s in w)
        else:
            x = math.sqrt(sum(s * s for s in w) / len(w))
        env += (x - env) * (attack if x > env else release)
        if steps:                                    # hand-set ladder
            want = 0
            while want < levels - 1 and env > steps[want]: want += 1
        else:                                        # gated, log-spaced
            if env <= gate: want = 0
            else:
                f = math.log(env / gate) / math.log(top / gate)
                want = max(1, min(levels - 1, round(f * (levels - 1))))
        if slew is not None:
            want = max(lvl - slew, min(lvl + slew, want))
        lvl = want; out.append(lvl); t += frame
    return out

def score(seq, fps, levels):
    ch = [abs(b - a) for a, b in zip(seq, seq[1:]) if b != a]
    bounce = sum(1 for a, b, c in zip(seq, seq[1:], seq[2:]) if a == c != b)
    secs = len(seq) / fps
    return dict(changes_s=len(ch) / secs,
                big_jumps=sum(1 for j in ch if j > 1) / max(1, len(ch)),
                max_jump=max(ch or [0]) / (levels - 1),
                bounce_s=bounce / secs,
                open=sum(1 for v in seq if v > 0) / len(seq))

if __name__ == "__main__":
    pcm, rate = load(sys.argv[1]); pcm = louden(pcm)
    cur = drive(pcm, rate, fps=20, win_ms=25, measure="peak", attack=1.0,
                release=1/3, levels=6, steps=[1500, 6000, 12000, 19000, 26000])
    print("CURRENT DEVICE:", {k: round(v, 2) for k, v in score(cur, 20, 6).items()})
    print("first 3 s of levels:", "".join(str(v) for v in cur[:60]))
