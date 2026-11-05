#!/usr/bin/env python3
"""Turn one of your Synthesia avatars into an interactive one and use it.

    .venv/bin/python convert_avatar.py av_xxxxxxxx

A Personal avatar can't be used by the LiveKit plugin as-is; Synthesia has to
convert it first. This asks for the conversion, waits until it is done, and
writes the result into .env as AVATAR_ID. Restart agent.py to pick it up.

It polls by asking again rather than by a status endpoint: Synthesia documents
that repeating the request for the same source returns the existing
interactive avatar instead of making a second one, so the request that starts
the job is also the safe way to watch it.
"""
import json
import pathlib
import re
import sys
import time
import urllib.error
import urllib.request

URL = "https://developers.synthesia.io/api/interactive-avatars/avatars"
ENV = pathlib.Path(__file__).parent / ".env"


def env_value(name):
    for line in ENV.read_text().splitlines():
        if line.startswith(name + "="):
            return line.split("=", 1)[1].strip()
    sys.exit(f"{name} missing from {ENV}")


def request(key, source):
    req = urllib.request.Request(
        URL, method="POST",
        data=json.dumps({"sourceId": source}).encode(),
        headers={"Authorization": key, "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        sys.exit(f"HTTP {e.code}: {e.read(500).decode()}")


def main():
    if len(sys.argv) != 2 or not re.fullmatch(r"av_[A-Za-z0-9_-]{1,64}", sys.argv[1]):
        sys.exit("usage: convert_avatar.py av_xxxxxxxx  (Synthesia: avatar ••• > Copy ID)")
    source, key = sys.argv[1], env_value("SYNTHESIA_API_KEY")

    deadline = time.time() + 15 * 60
    while True:
        d = request(key, source)
        status = str(d.get("status", "")).lower()
        print(f"  {time.strftime('%H:%M:%S')}  status={status or '?'}")
        if status in ("completed", "complete", "ready", "succeeded"):
            break
        if status in ("failed", "error", "rejected"):
            sys.exit(f"conversion failed: {json.dumps(d)[:400]}")
        if time.time() > deadline:
            sys.exit("still not done after 15 minutes - run this again later")
        time.sleep(15)

    new_id = d.get("id") or d.get("avatarId")
    if not new_id:
        sys.exit(f"done, but no id in the response: {json.dumps(d)[:400]}")
    text = ENV.read_text()
    text = re.sub(r"(?m)^AVATAR_ID=.*$", f"AVATAR_ID={new_id}", text)
    ENV.write_text(text)
    print(f"AVATAR_ID={new_id} written to .env - restart agent.py")


if __name__ == "__main__":
    main()
