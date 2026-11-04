#!/usr/bin/env python3
"""Hold one turn with an AssemblyAI voice agent from this Mac, no device.

Proves the parts the firmware depends on: Bearer auth, a stored agent_id,
base64 PCM16 in and out at 24 kHz. Writes the agent's reply to a WAV and plays
it, so you hear exactly what the device's speaker will have to render.

    ./tools/agent_probe.py --say "Hi Jack, I'm a developer advocate."
"""
import argparse
import base64
import json
import os
import re
import subprocess
import sys
import time
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wsmin import WS, TEXT  # noqa: E402

HOST = "agents.assemblyai.com"
PATH = "/v1/ws"
RATE = 24000          # audio/pcm is PCM16 mono at 24 kHz, both directions
JACK = "agent_921917929cb944e19960ffdfbd53f217"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--key-from", default="firmware/p3_captions/secrets.h")
    ap.add_argument("--agent", default=JACK)
    ap.add_argument("--say", default="Hi Jack, my name is Harnoor and I build "
                                     "developer tools for a living.")
    ap.add_argument("--out", default="/tmp/agent_reply.wav")
    args = ap.parse_args()

    m = re.search(r'AAI_API_KEY\s+"([^"]+)"', open(args.key_from).read())
    if not m:
        raise SystemExit(f"no AAI_API_KEY in {args.key_from}")

    wav_in = "/tmp/agent_in.wav"
    subprocess.run(["say", "-o", wav_in, f"--data-format=LEI16@{RATE}",
                    args.say], check=True)
    with wave.open(wav_in) as w:
        pcm = w.readframes(w.getnframes())
    print(f"# you: {args.say}  ({len(pcm)/2/RATE:.1f}s)")

    ws = WS(HOST, PATH, {"Authorization": f"Bearer {m.group(1)}"})
    ws.send_text(json.dumps({"type": "session.update",
                             "session": {"agent_id": args.agent}}))

    reply = bytearray()
    ready = False
    deadline = time.time() + 15
    while not ready and time.time() < deadline:
        op, payload = ws.recv()
        msg = json.loads(payload) if op == TEXT else {}
        if msg.get("type") == "session.ready":
            print(f"[ready] {msg.get('session_id', '')[:12]}")
            ready = True
        elif msg.get("type") in ("session.error", "error"):
            raise SystemExit(f"[error] {msg.get('code')}: {msg.get('message')}")
    if not ready:
        raise SystemExit("never became ready")

    # 50 ms chunks, paced at real time like a live mic would be.
    step = RATE // 20 * 2
    for i in range(0, len(pcm), step):
        ws.send_text(json.dumps({
            "type": "input.audio",
            "audio": base64.b64encode(pcm[i:i + step]).decode(),
        }))
        time.sleep(0.05)

    ws.sock.settimeout(25)
    done = time.time() + 35
    while time.time() < done:
        try:
            op, payload = ws.recv()
        except (ConnectionError, OSError):
            break
        if op != TEXT:
            continue
        msg = json.loads(payload)
        t = msg.get("type")
        if t == "reply.audio":
            reply += base64.b64decode(msg["data"])
        elif t == "transcript.user":
            print(f"[heard] {msg.get('text')}")
        elif t == "transcript.agent":
            print(f"[jack ] {msg.get('text')}")
        elif t == "reply.done":
            print(f"[done ] {msg.get('status')}")
            break
        elif t in ("session.error", "error"):
            print(f"[error] {msg.get('code')}: {msg.get('message')}")
            break

    ws.send_text(json.dumps({"type": "session.end"}))
    ws.close()

    if not reply:
        raise SystemExit("no audio came back")
    with wave.open(args.out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(reply))
    print(f"# reply: {len(reply)/2/RATE:.1f}s -> {args.out}")
    subprocess.run(["afplay", args.out])


if __name__ == "__main__":
    main()
