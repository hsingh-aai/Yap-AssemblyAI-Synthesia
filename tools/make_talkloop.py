#!/usr/bin/env python3
"""Cut a talking loop from an avatar recording, for the device.

    ./tools/make_talkloop.py --video kenji.mp4 --region 100,148,100,76 \\
        --mouth 112,160,76,52

Swapping cut-out mouths works on a drawn face and fails on a real one. When a
person opens their mouth their cheeks, moustache and chin move too, so every
cut-out carries a different moment's face around it, and stepping between
them shimmers however well the cut-outs are chosen (tools/ladderpop.py put
the change around the mouth at 5 levels a step, and aligning the head first
did not lower it - the face was deforming, not moving).

So this keeps real video instead: ~1.6 s of consecutive frames of the lower
face, 40 ms apart, which the device plays forwards then backwards while the
agent is talking. Consecutive frames cannot pop. The lips do not form the
agent's words, but they move the way a person's do, and start and stop with
the voice.

It picks the stretch of the clip with the most mouth movement and the least
head movement, starting on a shut frame so the resting face and the loop
agree. Every frame is stored with the per-frame gap area, so the device can
find a shut frame to settle on when the voice stops.
"""
import argparse, os, shutil, subprocess, sys, tempfile

def ffmpeg(*args):
    r = subprocess.run(["ffmpeg", "-nostdin", "-loglevel", "error", *args], capture_output=True)
    if r.returncode:
        sys.exit(f"ffmpeg failed: {r.stderr.decode()[:400]}")
    return r.stdout

def box(s):
    v = [int(x) for x in s.split(",")]
    if len(v) != 4: raise argparse.ArgumentTypeError("x,y,w,h")
    return v

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", required=True)
    ap.add_argument("--size", type=int, default=300)
    ap.add_argument("--region", type=box, required=True, help="lower face: x,y,w,h")
    ap.add_argument("--mouth", type=box, required=True, help="mouth, to measure: x,y,w,h")
    ap.add_argument("--seconds", type=float, default=1.6)
    ap.add_argument("--fps", type=int, default=25)
    ap.add_argument("--gap-luma", type=int, default=60, dest="gap_luma")
    ap.add_argument("--feather", type=int, default=8)
    ap.add_argument("--out", default="firmware/pocketvoice")
    a = ap.parse_args()
    S = a.size
    rx, ry, rw, rh = a.region
    mx, my, mw, mh = a.mouth
    if rw % 2 or rh % 2: sys.exit("region width and height must be even")
    tmp = tempfile.mkdtemp(prefix="talk-")
    try:
        sq = os.path.join(tmp, "sq.mp4")
        ffmpeg("-i", a.video, "-vf",
               f"crop='min(iw,ih)':'min(iw,ih)',scale={S}:{S},fps={a.fps}", "-an", sq)
        G = ffmpeg("-i", sq, "-vf", "format=gray", "-f", "rawvideo", "-")
        C = ffmpeg("-i", sq, "-vf", "format=rgb565le", "-f", "rawvideo", "-")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    count = len(G) // (S * S)
    N = int(round(a.seconds * a.fps))
    if count < N + 1: sys.exit(f"clip has {count} frames, need {N + 1}")

    inner = [(x, y) for y in range(my + int(mh * .15), my + int(mh * .85))
             for x in range(mx + int(mw * .2), mx + int(mw * .8))]
    def gap(f):
        o = f * S * S
        return sum(1 for x, y in inner if G[o + y * S + x] < a.gap_luma)
    gaps = [gap(f) for f in range(count)]

    # Head movement is judged on the region's border, where the loop meets
    # the still face - that seam is what would show.
    ring = [(x, y) for y in range(ry, ry + rh, 2) for x in range(rx, rx + rw, 2)
            if min(x - rx, rx + rw - 1 - x, y - ry, ry + rh - 1 - y) < 6]
    def seam(i, j):
        oi, oj = i * S * S, j * S * S
        return sum(abs(G[oi + y * S + x] - G[oj + y * S + x]) for x, y in ring) / len(ring)

    shut = sorted(gaps)[max(0, len(gaps) // 5)]       # 20th percentile = shut
    best = None
    for s0 in range(count - N):
        if gaps[s0] > shut:
            continue
        seg = range(s0, s0 + N)
        worst = max(seam(s0, f) for f in seg[::3])
        mean = sum(gaps[f] for f in seg) / N
        motion = (sum((gaps[f] - mean) ** 2 for f in seg) / N) ** 0.5
        score = motion - 2.0 * worst
        if best is None or score > best[0]:
            best = (score, s0, worst, motion)
    if best is None: sys.exit("no shut frame to start a loop from")
    _, s0, worst, motion = best
    seg = list(range(s0, s0 + N))
    print(f"# {count} frames; loop = frames {s0}-{s0 + N - 1} ({N} frames, "
          f"{N * 1000 // a.fps} ms); mouth movement {motion:.1f}; worst seam {worst:.1f}")

    base = C[s0 * S * S * 2:(s0 + 1) * S * S * 2]
    def crop(f):
        o = f * S * S * 2; out = bytearray()
        for y in range(rh):
            row = o + ((ry + y) * S + rx) * 2
            out += C[row:row + rw * 2]
        return bytes(out)
    under = crop(s0)
    frames = [feather(crop(f), under, rw, rh, a.feather) for f in seg]
    write(a.out, S, (rx, ry, rw, rh), base, frames, [gaps[f] for f in seg], a.fps, shut)

def feather(sprite, under, w, h, px):
    out = bytearray(sprite)
    for y in range(h):
        for x in range(w):
            d = min(x, w - 1 - x, y, h - 1 - y)
            if d >= px: continue
            t = d / px; i = (y * w + x) * 2
            a_ = sprite[i] | (sprite[i + 1] << 8); b = under[i] | (under[i + 1] << 8)
            r = round(((a_ >> 11) & 31) * t + ((b >> 11) & 31) * (1 - t))
            g = round(((a_ >> 5) & 63) * t + ((b >> 5) & 63) * (1 - t))
            bl = round((a_ & 31) * t + (b & 31) * (1 - t))
            v = (r << 11) | (g << 5) | bl
            out[i], out[i + 1] = v & 0xFF, v >> 8
    return bytes(out)

def panel_words(raw):        # rgb565le -> the byte order M5GFX's pushImage reads
    return [(raw[i] << 8) | raw[i + 1] for i in range(0, len(raw), 2)]

def c_words(ws, per=12):
    return "\n".join("  " + " ".join(f"0x{w:04x}," for w in ws[i:i + per])
                     for i in range(0, len(ws), per))

def write(out, S, region, base, frames, gaps, fps, shut):
    rx, ry, rw, rh = region
    with open(os.path.join(out, "avatar.h"), "w") as f:
        f.write(f"""// Generated by tools/make_talkloop.py - do not edit by hand.
#pragma once
#include <stdint.h>

static constexpr int kAvatarSize = {S};
static constexpr int kTalkX      = {rx};
static constexpr int kTalkY      = {ry};
static constexpr int kTalkW      = {rw};
static constexpr int kTalkH      = {rh};
static constexpr int kTalkFrames = {len(frames)};
static constexpr int kTalkFps    = {fps};
static constexpr int kTalkShut   = {shut};   // gap area at or below this reads as shut

// RGB565, byte-swapped for the panel; pushImage() these as they are.
extern const uint16_t kAvatarBasePix[kAvatarSize * kAvatarSize];
extern const uint16_t kTalkPix[kTalkFrames][kTalkW * kTalkH];
extern const uint16_t kTalkGap[kTalkFrames];
""")
    with open(os.path.join(out, "avatar_data.cpp"), "w") as f:
        f.write('// Generated by tools/make_talkloop.py - do not edit by hand.\n#include "avatar.h"\n\n')
        f.write(f"const uint16_t kAvatarBasePix[{S * S}] = {{\n" + c_words(panel_words(base)) + "\n};\n\n")
        f.write(f"const uint16_t kTalkPix[{len(frames)}][{rw * rh}] = {{\n")
        for fr in frames:
            f.write("{\n" + c_words(panel_words(fr)) + "\n},\n")
        f.write("};\n\n")
        f.write(f"const uint16_t kTalkGap[{len(gaps)}] = {{ " + ", ".join(str(g) for g in gaps) + " };\n")
    kb = (len(base) + sum(len(x) for x in frames)) / 1024
    print(f"# wrote avatar.h + avatar_data.cpp ({kb:.0f} KB of flash)")
    print("# gap per frame: " + " ".join(str(g) for g in gaps))

if __name__ == "__main__":
    main()
