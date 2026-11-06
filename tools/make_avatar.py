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
    ap.add_argument("--gap-luma", type=int, default=60, dest="gap_luma",
                    help="darker than this counts as the open mouth's gap")
    ap.add_argument("--align", type=int, default=12,
                    help="largest head shift, in px, to correct for")
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
        # Whole frames, once, as grayscale (to measure) and as RGB565 (to keep).
        G = ffmpeg("-i", square, "-vf", "format=gray", "-f", "rawvideo", "-")
        C = ffmpeg("-i", square, "-vf", "format=rgb565le", "-f", "rawvideo", "-")
        count = len(G) // (S * S)
        if count < a.levels:
            sys.exit(f"only {count} frames; need at least {a.levels}")

        # Where the mouth is, and the inside of it.
        inner_xy = [(x, y) for y in range(int(mh * 0.15), int(mh * 0.85))
                    for x in range(int(mw * 0.2), int(mw * 0.8))]
        def gap_area(f, dx=0, dy=0):
            o = f * S * S
            if a.open_is == "dark":
                return sum(1 for x, y in inner_xy
                           if G[o + (my + dy + y) * S + mx + dx + x] < a.gap_luma)
            return sum(1 for x, y in inner_xy
                       if G[o + (my + dy + y) * S + mx + dx + x] > 255 - a.gap_luma)

        # The resting face: the frame with the smallest gap.
        ref = min(range(count), key=gap_area)

        # A real face moves its head while it talks. Picking only frames where
        # it happened to keep still gave tiny openings - the head moves most
        # when the mouth opens widest - so instead every frame is lined up
        # with the resting face before its mouth is cut out. The match is done
        # on the face *around* the mouth (cheeks, moustache, chin), which moves
        # with the head and not with the lips.
        M = a.align
        ctx = [(x, y) for y in range(my - M, my + mh + M, 2)
               for x in range(mx - M, mx + mw + M, 2)
               if not (mx + mw * 0.2 <= x < mx + mw * 0.8 and my + mh * 0.15 <= y < my + mh * 0.85)
               and 0 <= x < S and 0 <= y < S]
        rbase = [G[ref * S * S + y * S + x] for x, y in ctx]
        def sad(f, dx, dy):
            o = f * S * S
            tot = 0
            for (x, y), r in zip(ctx, rbase):
                xx, yy = x + dx, y + dy
                if 0 <= xx < S and 0 <= yy < S:
                    tot += abs(G[o + yy * S + xx] - r)
                else:
                    tot += 64
            return tot / len(ctx)
        lim = a.align // 2
        def align(f):
            best = min(((sad(f, dx, dy), dx, dy)
                        for dx in range(-lim, lim + 1, 2) for dy in range(-lim, lim + 1, 2)))
            _, bx, by = best
            best = min(((sad(f, dx, dy), dx, dy)
                        for dx in (bx - 1, bx, bx + 1) for dy in (by - 1, by, by + 1)))
            return best
        fits = {}
        for f in range(count):
            r, dx, dy = align(f)
            if (0 <= mx + dx and mx + dx + mw <= S and 0 <= my + dy and my + dy + mh <= S):
                fits[f] = (r, dx, dy, gap_area(f, dx, dy))
        if ref not in fits:
            fits[ref] = (0.0, 0, 0, gap_area(ref))

        # Keep the frames that line up well, then spread the shapes evenly from
        # shut to the widest the clip reliably reaches.
        good = sorted(fits, key=lambda f: fits[f][0])[:max(a.levels * 4, int(len(fits) * 0.6))]
        areas = sorted(fits[f][3] for f in good)
        top = areas[int(len(areas) * 0.95) - 1] if len(areas) > 20 else areas[-1]
        print(f"# {count} frames, {len(good)} line up well; gap area 0 -> {top} px; "
              f"residual of the kept frames <= {fits[good[-1]][0]:.1f}")
        picks = [ref]
        for k in range(1, a.levels):
            target = top * k / (a.levels - 1)
            cand = min((f for f in good if f not in picks),
                       key=lambda f: abs(fits[f][3] - target) + 3.0 * fits[f][0])
            picks.append(cand)
        picks.sort(key=lambda f: fits[f][3])
        print("# gap area per level (px): " + " ".join(str(fits[f][3]) for f in picks))
        print("# alignment residual per level: " + " ".join(f"{fits[f][0]:.1f}" for f in picks))
        print("# head shift per level (px): " + " ".join(f"{fits[f][1]:+d},{fits[f][2]:+d}" for f in picks))

        base = C[ref * S * S * 2:(ref + 1) * S * S * 2]
        def crop(f, dx, dy):
            o = f * S * S * 2
            out = bytearray()
            for y in range(mh):
                row = o + ((my + dy + y) * S + mx + dx) * 2
                out += C[row:row + mw * 2]
            return bytes(out)
        under = crop(ref, 0, 0)
        mouths = [feather(crop(f, fits[f][1], fits[f][2]), under, mw, mh, a.feather)
                  for f in picks]
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
