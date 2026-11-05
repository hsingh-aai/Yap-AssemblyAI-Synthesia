#!/usr/bin/env python3
"""Turn an avatar render into mouth sprites the device can blit.

    ./tools/make_avatar.py --video jack.mp4 --mouth 90,185,120,70

Jack's replies are written by an LLM a second before he says them, so no
pre-rendered clip can ever be saying his words. What a pre-rendered clip
*can* give us is a face and a set of mouth shapes; the device then picks one
per 50 ms from the envelope of the audio that is sounding right now. The
mouth never forms real words, but it moves in time with the voice, which is
the part people actually notice.

So this does not keep the video. It keeps one base portrait plus N mouth
crops chosen to span closed-to-open, and emits them all as raw RGB565 that
the panel blits directly - no decoder anywhere. Two lessons from the first
hardware run went into that:

  * The face used to be a JPEG, and drew nothing. ffmpeg had encoded it with
    1x2 sampling on all three components, which the board's decoder does not
    support, and it fails silently. Raw pixels cost ~170 KB more flash and
    cannot fail that way.
  * The pixels are stored byte-swapped. M5GFX reads a uint16_t image as
    big-endian RGB565, so native little-endian words came out with orange
    turned magenta and blue - exactly what the first photo showed.

Openness is scored without numpy or Pillow: ffmpeg hands us the mouth box as
raw grayscale and an open mouth is darker than a closed one, because the
oral cavity is a hole. Mean luma over a few hundred small frames is cheap in
plain Python.
"""
import argparse, os, shutil, subprocess, sys, tempfile

def ffmpeg(*args):
    r = subprocess.run(["ffmpeg", "-nostdin", "-loglevel", "error", *args],
                       capture_output=True)
    if r.returncode:
        sys.exit(f"ffmpeg failed: {r.stderr.decode()[:400]}")
    return r.stdout

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", required=True, help="the avatar render (mp4)")
    ap.add_argument("--size", type=int, default=300, help="face square, px")
    ap.add_argument("--mouth", required=True,
                    help="mouth box within the face square: x,y,w,h")
    ap.add_argument("--levels", type=int, default=6)
    ap.add_argument("--open-is", choices=("dark", "bright"), default="dark",
                    dest="open_is",
                    help="a human mouth is darker open (the default); a lit "
                         "jack-o-lantern is brighter")
    ap.add_argument("--fps", type=int, default=10, help="sampling rate only")
    ap.add_argument("--feather", type=int, default=6,
                    help="px of each mouth edge blended into the face beneath")
    ap.add_argument("--out", default="firmware/pocketvoice")
    a = ap.parse_args()

    mx, my, mw, mh = (int(v) for v in a.mouth.split(","))
    if mw % 2 or mh % 2:
        sys.exit("mouth width and height must be even")
    S = a.size
    tmp = tempfile.mkdtemp(prefix="avatar-")
    try:
        # Square the face once, so every later crop is in the same frame.
        square = os.path.join(tmp, "sq.mp4")
        ffmpeg("-i", a.video, "-vf",
               f"crop='min(iw,ih)':'min(iw,ih)',scale={S}:{S},fps={a.fps}",
               "-an", square)

        # Score every frame by how dark its mouth box is.
        gray = ffmpeg("-i", square, "-vf", f"crop={mw}:{mh}:{mx}:{my},format=gray",
                      "-f", "rawvideo", "-")
        n = mw * mh
        count = len(gray) // n
        if count < a.levels:
            sys.exit(f"only {count} frames; need at least {a.levels}")
        scores = []
        for i in range(count):
            box = gray[i * n:(i + 1) * n]
            scores.append((sum(box) / n, i))
        scores.sort()
        if a.open_is == "dark":
            scores.reverse()                 # brightest first, i.e. shut first
        print(f"# {count} frames; luma {scores[0][0]:.0f} (shut) "
              f"-> {scores[-1][0]:.0f} (open)")

        # The face is the most-shut frame, so the resting portrait and the
        # mouths agree about everything except the mouth.
        base_idx = scores[0][1]
        base_box = gray[base_idx * n:(base_idx + 1) * n]

        # A real face moves its head while it talks, and a mouth cut from a
        # frame where it had drifted leaves a ridge where the box meets the
        # face. So split the frames into openness bands and, in each band,
        # take the frame whose box *border* best matches the face's - the one
        # where the head was in the same place.
        ring = [i for i in range(n)
                if min(i % mw, mw - 1 - i % mw, i // mw, mh - 1 - i // mw) < 4]
        def drift(idx):
            box = gray[idx * n:(idx + 1) * n]
            return sum(abs(box[i] - base_box[i]) for i in ring) / len(ring)
        picks = []
        for k in range(a.levels):
            band = scores[k * count // a.levels:(k + 1) * count // a.levels] or [scores[-1]]
            best = min(band, key=lambda sc: drift(sc[1]))
            picks.append(best)
        print("# head drift at the box edge, per level: "
              + " ".join(f"{drift(i):.1f}" for _, i in picks))

        base = ffmpeg("-i", square, "-vf",
                      f"select='eq(n\\,{base_idx})',format=rgb565le",
                      "-vframes", "1", "-f", "rawvideo", "-")
        if len(base) != S * S * 2:
            sys.exit(f"base frame is {len(base)} bytes, expected {S * S * 2}")

        rgb = ffmpeg("-i", square, "-vf",
                     f"crop={mw}:{mh}:{mx}:{my},format=rgb565le",
                     "-f", "rawvideo", "-")
        per = mw * mh * 2
        under = bytearray()                  # the face pixels each mouth covers
        for y in range(mh):
            o = ((my + y) * S + mx) * 2
            under += base[o:o + mw * 2]
        mouths = [feather(rgb[i * per:(i + 1) * per], bytes(under), mw, mh, a.feather)
                  for _, i in picks]
        write_header(a.out, S, mx, my, mw, mh, base, mouths)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

def feather(sprite, under, w, h, px):
    """Blend a mouth's outer `px` pixels into the face beneath it.

    The device pushes the sprite opaquely over the face, so any mismatch shows
    as a hard edge. The face is a fixed image, so the blend can be baked in
    here: full sprite in the middle, full face at the border, a ramp between.
    """
    if px <= 0:
        return sprite
    out = bytearray(sprite)
    for y in range(h):
        for x in range(w):
            d = min(x, w - 1 - x, y, h - 1 - y)
            if d >= px:
                continue
            t = d / px
            i = (y * w + x) * 2
            a = sprite[i] | (sprite[i + 1] << 8)
            b = under[i] | (under[i + 1] << 8)
            r = round(((a >> 11) & 31) * t + ((b >> 11) & 31) * (1 - t))
            g = round(((a >> 5) & 63) * t + ((b >> 5) & 63) * (1 - t))
            bl = round((a & 31) * t + (b & 31) * (1 - t))
            v = (r << 11) | (g << 5) | bl
            out[i], out[i + 1] = v & 0xFF, v >> 8
    return bytes(out)


def panel_words(raw):
    """rgb565le bytes -> uint16 words in the byte order the panel reads."""
    return [(raw[i] << 8) | raw[i + 1] for i in range(0, len(raw), 2)]


def c_words(words, per_line=12):
    return "\n".join("  " + " ".join(f"0x{w:04x}," for w in words[i:i + per_line])
                     for i in range(0, len(words), per_line))


def write_header(out, size, mx, my, mw, mh, base, mouths):
    """Emit avatar.h (shape) and avatar_data.cpp (pixels)."""

    with open(os.path.join(out, "avatar.h"), "w") as f:
        f.write(f"""// Generated by tools/make_avatar.py - do not edit by hand.
#pragma once
#include <stdint.h>
#include <stddef.h>

static constexpr int kAvatarSize   = {size};
static constexpr int kMouthLevels  = {len(mouths)};
static constexpr int kMouthX       = {mx};
static constexpr int kMouthY       = {my};
static constexpr int kMouthW       = {mw};
static constexpr int kMouthH       = {mh};

// RGB565, byte-swapped for the panel; pushImage() these as they are.
extern const uint16_t kAvatarBasePix[kAvatarSize * kAvatarSize];
extern const uint16_t kMouthPix[kMouthLevels][kMouthW * kMouthH];
""")

    with open(os.path.join(out, "avatar_data.cpp"), "w") as f:
        f.write('// Generated by tools/make_avatar.py - do not edit by hand.\n'
                '#include "avatar.h"\n\n')
        f.write(f"const uint16_t kAvatarBasePix[{size * size}] = {{\n"
                + c_words(panel_words(base)) + "\n};\n\n")
        f.write(f"const uint16_t kMouthPix[{len(mouths)}][{mw * mh}] = {{\n")
        for m in mouths:
            f.write("{\n" + c_words(panel_words(m)) + "\n},\n")
        f.write("};\n")
    kb = (len(base) + sum(len(m) for m in mouths)) / 1024
    print(f"# wrote avatar.h + avatar_data.cpp  ({kb:.0f} KB of flash)")

if __name__ == "__main__":
    main()
