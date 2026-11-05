#!/usr/bin/env python3
"""Draw a jack-o'-lantern opening and shutting its mouth, as an mp4.

A stand-in for the Synthesia render, and deliberately fed through exactly the
same pipeline: this writes a video, make_avatar.py turns a video into mouth
sprites, so the day the real render arrives nothing changes but the filename.
It is also a reasonable thing to ship - Jack is a three-hundred-year-old
pumpkin, and a pumpkin is easier to draw convincingly than a face.

    ./tools/pumpkin.py --out /tmp/pumpkin.mp4
"""
import argparse, math, os, shutil, subprocess, sys, tempfile

S = 300
INK = (0x13, 0x12, 0x10)

def draw(t):
    """One frame. t is mouth openness, 0 shut to 1 wide."""
    px = bytearray(INK * (S * S))

    def put(x, y, c):
        if 0 <= x < S and 0 <= y < S:
            i = (y * S + x) * 3
            px[i:i + 3] = bytes(c)

    def tri(ax, ay, bx, by, cx_, cy_, c):
        xs, ys = (ax, bx, cx_), (ay, by, cy_)
        for y in range(max(0, min(ys)), min(S, max(ys) + 1)):
            for x in range(max(0, min(xs)), min(S, max(xs) + 1)):
                d1 = (x-bx)*(ay-by) - (ax-bx)*(y-by)
                d2 = (x-cx_)*(by-cy_) - (bx-cx_)*(y-cy_)
                d3 = (x-ax)*(cy_-ay) - (cx_-ax)*(y-ay)
                if not ((d1 < 0 or d2 < 0 or d3 < 0) and (d1 > 0 or d2 > 0 or d3 > 0)):
                    put(x, y, c)

    cx = cy = S // 2
    R = 132
    # Body, with ridges: the shading is what stops it reading as a circle.
    for y in range(cy - R, cy + R + 1):
        half = int(math.sqrt(max(0, R * R - (y - cy) ** 2)))
        for x in range(cx - half, cx + half + 1):
            u = (x - cx) / R
            ridge = 0.78 + 0.22 * math.cos(u * math.pi * 2.6)
            shade = 1.0 - 0.28 * ((y - cy) / R) ** 2
            k = ridge * shade
            put(x, y, (int(228 * k), int(104 * k), int(22 * k)))
    # Stem.
    for y in range(cy - R - 20, cy - R + 8):
        for x in range(cx - 11, cx + 11):
            put(x, y, (74, 92, 40))

    glow = (255, 214, 92)
    tri(cx - 74, cy - 44, cx - 24, cy - 44, cx - 49, cy - 2, glow)   # eyes
    tri(cx + 24, cy - 44, cx + 74, cy - 44, cx + 49, cy - 2, glow)
    tri(cx - 15, cy + 22, cx + 15, cy + 22, cx, cy - 8, glow)        # nose

    # The mouth: a lens that opens downward, lit from inside, with two teeth
    # left standing so there is something for the eye to measure the gap by.
    my_, halfw = cy + 64, 62
    halfh = 5 + t * 30
    for x in range(cx - halfw, cx + halfw + 1):
        u = (x - cx) / halfw
        h = halfh * math.sqrt(max(0.0, 1 - u * u))
        for y in range(int(my_ - h), int(my_ + h) + 1):
            put(x, y, glow)
    for sx in (cx - 30, cx + 30):                                    # teeth
        for x in range(sx - 9, sx + 9):
            u = (x - cx) / halfw
            h = halfh * math.sqrt(max(0.0, 1 - u * u))
            for y in range(int(my_ - h), int(my_ - h) + max(4, int(h * 0.8))):
                put(x, y, (150, 60, 12))
    return bytes(px)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="/tmp/pumpkin.mp4")
    ap.add_argument("--frames", type=int, default=24)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="pumpkin-")
    try:
        for i in range(a.frames):
            t = i / (a.frames - 1)
            with open(os.path.join(tmp, f"f{i:03d}.ppm"), "wb") as f:
                f.write(b"P6\n%d %d\n255\n" % (S, S))
                f.write(draw(t))
        r = subprocess.run(["ffmpeg", "-nostdin", "-loglevel", "error", "-y",
                            "-framerate", "10", "-i", os.path.join(tmp, "f%03d.ppm"),
                            "-pix_fmt", "yuv420p", a.out], capture_output=True)
        if r.returncode:
            sys.exit(r.stderr.decode()[:400])
        print(f"# {a.frames} frames -> {a.out}")
        print("# mouth box for make_avatar.py: --mouth 86,176,128,76 --open-is bright")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

if __name__ == "__main__":
    main()
