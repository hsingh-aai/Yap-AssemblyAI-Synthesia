# Jack, on a screen

The device in your hand runs the AssemblyAI Voice Agent API over a plain
WebSocket. This runs the same Jack on a booth monitor with a Synthesia
interactive avatar, so there is a face to talk to while the thing you are
holding does the talking.

## Why it is not on the device

Synthesia's interactive avatar joins a LiveKit room as a participant and
lip-syncs the agent's speech in real time. The frontend is an ordinary
LiveKit client, which means a WebRTC video subscriber. An ESP32-S3 cannot be
one: LiveKit's ESP32 SDK is audio-only, ESP-IDF-only and still a developer
preview, and decoding live video onto a 466 px panel is not on the table
regardless. The device keeps its own mouth-frame avatar, which is the only
kind it can have; see the main README.

## Where it runs

On **LiveKit Cloud** (project `test-harnoor`). Synthesia's avatar worker is
hosted on their side and dials *into* your room from the internet, so the
LiveKit server has to be publicly reachable. Cloud already is. A self-hosted
`livekit-server` works too, but only behind a public tunnel, and that also
exposes its credentials to the internet — so don't run it with the `--dev`
defaults (`devkey` / `secret`) if you go that way.

## What is different from the quickstart

* **Everything is AssemblyAI, on one key.** STT via
  `livekit-plugins-assemblyai`, with AssemblyAI's endpointing model deciding
  when a turn ends. The LLM via the LLM Gateway, which speaks OpenAI's
  dialect so the `openai` plugin drives it with just a `base_url`. And the
  voice via `assemblyai.TTS` — first audio in about 0.1 s in a live room.
  No OpenAI key, no Cartesia, no LiveKit Inference.
* **Pick a non-reasoning model.** `claude-haiku-5-5` answered Jack's prompt
  with `reasoning_tokens=120` and an **empty string**: it spent its whole
  budget thinking. `gemini-3.5-flash-lite` answers in 0.9 s with none, and is
  what the firmware's Notetaker already uses.
* **The greeting goes straight to the voice** with `session.say()`. It is a
  fixed line from the stored agent, so asking the LLM to repeat it cost a
  round trip — and failed outright behind the gateway, because Gemini rejects
  a request with no user turn (`at least one contents field is required`).
  The quickstart's OpenAI Realtime model tolerated that; Gemini does not.
* **Jack's persona is fetched from the stored AssemblyAI agent**, off the
  event loop. The device references that agent by id and so does this, so
  the two cannot drift apart. Fetching it on the loop stalled audio for
  277 ms on the first run, and LiveKit said so.
* `agent_name` is `pocketvoice-jack` in both `agent.py` and `server.py`.
  Those must match or the worker never joins the room.
* The quickstart's `realtime_preflight.py` shim is gone: it only existed to
  give Realtime models the head start LiveKit already gives cascaded ones.

## Restarting the worker leaves orphans

When the worker is killed mid-session, LiveKit Cloud re-dispatches the
abandoned room's job to the next worker that registers — and each job pulls
in its own Synthesia avatar session, which spends interactive-avatar quota.
After a restart, check and clear them:

```bash
lk room list
lk room delete <name>
```

## Running it

```bash
cd avatar
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
```

`lk cloud auth`, then `lk app env -w -d lk.env` writes the three LiveKit
values to a side file. Copy them into `.env` by hand; pointing `lk app env`
straight at `.env` risks it rewriting the keys already there.

Then, in two terminals:

```bash
python agent.py dev
python server.py
```

Open <http://localhost:8080> and press Start. Do not use
`python agent.py console` — it runs a mock room and the avatar never appears.

## Keys

`.env` is gitignored and `.env.example` is the committed copy with the values
stripped. `SYNTHESIA_API_KEY` is server-side only and must never reach the
browser. `AVATAR_ID` is Kenji, the "Aristotle" face from Synthesia's RAG quickstart; Jenny, the one the minimal quickstart
ships with; a custom avatar has to be converted with
`POST /api/interactive-avatars/avatars` and polled until its status is
`completed`.

Synthesia's own voices are not available through this API, which is fine:
the voice is AssemblyAI's.

[qs]: https://github.com/synthesia-ai/interactive-avatar-quickstarts
