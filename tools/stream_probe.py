#!/usr/bin/env python3
"""Exercise AssemblyAI's realtime STT endpoint from this Mac, with no device.

Speaks the same protocol the p3_captions firmware does - Authorization header,
50 ms pcm_s16le frames, Turn messages back - so when captions misbehave you can
tell a board problem from an account or endpoint problem.

    say -o /tmp/probe.wav --data-format=LEI16@16000 "testing one two three"
    ./tools/stream_probe.py --wav /tmp/probe.wav
"""
import argparse
import json
import os
import re
import sys
import threading
import time
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wsmin import WS, TEXT, BINARY  # noqa: E402

HOST = "streaming.assemblyai.com"
PATH = "/v3/ws?sample_rate=16000&encoding=pcm_s16le&format_turns=true"


def reader(ws, stop):
    while not stop.is_set():
        try:
            op, payload = ws.recv()
        except (ConnectionError, OSError):
            return
        if op != TEXT:
            continue
        msg = json.loads(payload)
        t = msg.get("type")
        if t == "Begin":
            print(f"[begin] model={msg.get('configuration', {}).get('model')}")
        elif t == "Turn":
            if msg.get("transcript"):
                mark = "FINAL" if msg.get("end_of_turn") else "  ..."
                print(f"[{mark}] {msg['transcript']}")
        elif t == "Termination":
            print(f"[end] {msg.get('audio_duration_seconds')}s audio")
            return


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--key-from", default="firmware/p3_captions/secrets.h")
    ap.add_argument("--wav", required=True, help="16 kHz mono 16-bit WAV")
    args = ap.parse_args()

    m = re.search(r'AAI_API_KEY\s+"([^"]+)"', open(args.key_from).read())
    if not m:
        raise SystemExit(f"no AAI_API_KEY in {args.key_from}")

    with wave.open(args.wav) as w:
        assert w.getnchannels() == 1 and w.getsampwidth() == 2, "need 16-bit mono"
        rate = w.getframerate()
        pcm = w.readframes(w.getnframes())
    print(f"# {args.wav}: {len(pcm)/2/rate:.1f}s at {rate} Hz")

    # No Bearer prefix on the streaming API - unlike the Voice Agent API.
    ws = WS(HOST, PATH, {"Authorization": m.group(1)})
    stop = threading.Event()
    threading.Thread(target=reader, args=(ws, stop), daemon=True).start()

    # 50 ms frames paced at real time: a tight loop makes Turns erratic.
    step = rate // 20 * 2
    for i in range(0, len(pcm), step):
        ws.send(BINARY, pcm[i:i + step])
        time.sleep(0.05)
    ws.send_text(json.dumps({"type": "Terminate"}))
    time.sleep(2.0)
    stop.set()
    ws.close()


if __name__ == "__main__":
    main()
