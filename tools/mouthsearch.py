#!/usr/bin/env python3
"""Search mouth-driver configs on a reply WAV:  ./tools/mouthsearch.py reply.wav"""
import itertools, math, sys
sys.path.insert(0, __import__("os").path.dirname(__file__))
from mouthsim import load, louden, drive, score

pcm, rate = load(sys.argv[1]); pcm = louden(pcm)

def truth(fps):                       # what the voice is doing at each frame
    out, t = [], 0.0
    while int(t * rate) < len(pcm):
        w = pcm[int(t*rate): int(t*rate) + rate // 50]
        out.append(math.sqrt(sum(s*s for s in w) / len(w))); t += 1 / fps
    return out

def corr(a, b):
    n = min(len(a), len(b)); a, b = a[:n], b[:n]
    ma, mb = sum(a)/n, sum(b)/n
    va = sum((x-ma)**2 for x in a); vb = sum((y-mb)**2 for y in b)
    return sum((x-ma)*(y-mb) for x, y in zip(a, b)) / math.sqrt(va*vb) if va and vb else 0

rms = sorted(truth(30)); gate = max(400, rms[int(len(rms)*0.15)]); top = rms[int(len(rms)*0.92)]
print(f"loudened RMS: gate {gate:.0f}  top {top:.0f}")

rows = []
for fps, meas, win, att, rel, lv, slew in itertools.product(
        (20, 30), ("peak", "rms"), (20, 30, 40), (0.35, 0.5, 0.7, 1.0),
        (0.15, 0.25, 0.35), (6, 8, 10), (None, 1, 2)):
    seq = drive(pcm, rate, fps=fps, win_ms=win, measure=meas, attack=att, release=rel,
                levels=lv, gate=gate, top=top, slew=slew)
    tr = truth(fps)
    best = max(((corr(seq, tr[:len(tr)-k] if k else tr), k) for k in range(0, 5)),
               key=lambda c: c[0])
    # Correlate level k frames LATE against the voice: seq[k:] vs tr[:-k]
    lagc = max(((corr(seq[k:], tr), k) for k in range(0, 5)), key=lambda c: c[0])
    sc = score(seq, fps, lv)
    ch = [abs(b-a)/(lv-1) for a, b in zip(seq, seq[1:]) if b != a]
    step = sum(ch)/len(ch) if ch else 0
    lag_ms = lagc[1] * 1000 / fps
    rows.append(dict(fps=fps, meas=meas, win=win, att=att, rel=rel, lv=lv, slew=slew,
                     corr=lagc[0], lag_ms=lag_ms, step=step, max_jump=sc["max_jump"],
                     bounce=sc["bounce_s"], opn=sc["open"]))

# Smooth enough that no frame moves more than a third of the range, never
# later than 70 ms behind the voice, then the closest tracking wins, with
# flicker as the tie-break.
ok = [r for r in rows if r["max_jump"] <= 0.34 and r["lag_ms"] <= 70]
ok.sort(key=lambda r: (-(round(r["corr"], 2)), r["bounce"], r["step"]))
fmt = "{fps:>3} {meas:>4} {win:>3}ms att {att:<4} rel {rel:<4} lv {lv:>2} slew {slew!s:>4} | corr {corr:.3f} lag {lag_ms:>3.0f}ms  step {step:.2f}  maxjump {max_jump:.2f}  bounce/s {bounce:.2f}  open {opn:.2f}"
print(f"{len(rows)} configs tried, {len(ok)} pass the smoothness + latency bar. Best:")
for r in ok[:8]: print(" ", fmt.format(**r))
cur = [r for r in rows if r["fps"]==20 and r["meas"]=="peak" and r["win"]==20 and r["att"]==1.0 and r["lv"]==6 and r["slew"] is None and r["rel"]==0.35]
print("nearest-to-current (log-mapped):", fmt.format(**cur[0]))
