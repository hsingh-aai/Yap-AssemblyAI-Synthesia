#!/usr/bin/env python3
"""How much does the mouth 'pop' between neighbouring shapes?

    ./tools/ladderpop.py firmware/pocketvoice

Smooth lips need neighbouring shapes that differ only where the mouth is. This
measures, for each step up the ladder, the mean pixel change in the *outer*
part of the box (moustache, beard, cheeks), which should barely move, and in
the inner part (the mouth), which should. A smooth ladder has small outer
numbers and steady inner ones.
"""
import re, sys
d = sys.argv[1] if len(sys.argv) > 1 else "firmware/pocketvoice"
src = open(f"{d}/avatar_data.cpp").read(); hdr = open(f"{d}/avatar.h").read()
g = lambda n: int(re.search(rf"{n}\s*=\s*(\d+)", hdr).group(1))
W, H, L = g("kMouthW"), g("kMouthH"), g("kMouthLevels")
ms = [int(x, 16) for x in re.findall(r"0x([0-9a-f]{4}),", src.split("kMouthPix", 1)[1])]
def luma(w):
    w = ((w & 0xff) << 8) | (w >> 8)
    return (((w >> 11) & 31) * 8 * 299 + ((w >> 5) & 63) * 4 * 587 + (w & 31) * 8 * 114) // 1000
lv = [[luma(w) for w in ms[k * W * H:(k + 1) * W * H]] for k in range(L)]
inner = {y * W + x for y in range(int(H * .15), int(H * .85)) for x in range(int(W * .2), int(W * .8))}
outer = [i for i in range(W * H) if i not in inner]
print("step   outer change   inner change")
tot = 0
for k in range(L - 1):
    o = sum(abs(lv[k + 1][i] - lv[k][i]) for i in outer) / len(outer)
    n = sum(abs(lv[k + 1][i] - lv[k][i]) for i in inner) / len(inner)
    tot += o
    print(f"{k}->{k + 1}   {o:6.1f}         {n:6.1f}")
print(f"mean outer change per step: {tot / (L - 1):.1f}  (pop; want low)")
