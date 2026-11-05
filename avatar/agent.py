"""Jack, on a screen, with a Synthesia interactive avatar.

The device in your hand runs the AssemblyAI Voice Agent API over a plain
WebSocket, and that is the right shape for an ESP32. It is the wrong shape
for a face: Synthesia's interactive avatar joins a LiveKit room as a
participant and lip-syncs the agent's speech in real time, which needs a
WebRTC video client. The StopWatch cannot be one - LiveKit's ESP32 SDK is
audio-only, ESP-IDF-only and still a developer preview - so the avatar lives
on the booth screen beside the device rather than on it.

Adapted from Synthesia's minimal quickstart (MIT, (c) 2026 Synthesia).
The whole pipeline is AssemblyAI on one key:

  * Listening is AssemblyAI's STT plugin. The quickstart already sets
    turn_detection to "stt", which is what AssemblyAI's endpointing model is
    for - it ends a turn on the words as well as the silence.
  * Thinking is AssemblyAI's LLM Gateway, which speaks OpenAI's dialect, so
    the openai plugin drives it with nothing but a base_url.
  * Speaking is AssemblyAI's TTS (assemblyai-tts-1).
  * Jack's persona is fetched from the stored AssemblyAI agent at startup,
    so the thing on the screen and the thing in your hand cannot drift
    apart. Edit the agent, restart, both change.

Run: python agent.py dev   (`console` mode uses a mock room - no avatar)
"""

import asyncio
import json
import logging
import os
import urllib.error
import urllib.request
from pathlib import Path

from dotenv import load_dotenv
from livekit.agents import (
    Agent,
    AgentSession,
    JobContext,
    WorkerOptions,
    cli,
    metrics,
    room_io,
)
from livekit.plugins import assemblyai, openai, silero, synthesia

# Module-level on purpose: LiveKit plugins must register on the main thread.
try:
    from livekit.plugins import ai_coustics
except (ImportError, OSError):
    ai_coustics = None

load_dotenv(dotenv_path=Path(__file__).parent / ".env")

log = logging.getLogger("jack")

AVATAR_ID = os.environ["AVATAR_ID"]
# Fast and non-reasoning, which on this gateway is not a preference but a
# requirement: claude-haiku-5-5 answered this exact prompt in 1.9 s with
# reasoning_tokens=120, finish_reason=max_tokens and an empty string. It
# spent the whole budget thinking and said nothing. gemini-3.5-flash-lite
# answers in 0.9 s with reasoning_tokens=0 - and is what the firmware's
# Notetaker already uses, for the same reason. The gateway speaks OpenAI's
# dialect, so the openai plugin drives it with nothing but a base_url.
AAI_GATEWAY = "https://llm-gateway.assemblyai.com/v1"
LLM_MODEL = os.getenv("LLM_MODEL", "gemini-3.5-flash-lite")
# AssemblyAI's own TTS, which means the whole pipeline - listening, thinking
# and speaking - is one vendor and one key, with nothing LiveKit-Cloud-only
# in it. The default is the plugin's own voice; override to taste.
TTS_VOICE = os.getenv("TTS_VOICE", "49c443a4-8a1d-5f7e-b70b-98bdbd9823c3")

# Only used if the agent cannot be read - better a dull Jack than no Jack.
FALLBACK = """\
You are Jack the Roaster, a pumpkin-headed ghoul who has been practising his
jokes for three hundred years. Roast the guest's job the moment they mention
it. Keep it quick, playful and never mean. This is a spoken conversation, so
never mention typing or markdown, and answer in one to three short sentences."""


def load_persona() -> tuple[str, str]:
    """Read Jack's prompt and greeting from the stored AssemblyAI agent."""
    key, agent_id = os.environ["ASSEMBLYAI_API_KEY"], os.environ["AAI_AGENT_ID"]
    req = urllib.request.Request(
        f"https://agents.assemblyai.com/v1/agents/{agent_id}",
        headers={"Authorization": f"Bearer {key}"},
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            d = json.load(r)
        log.info("persona: %s", d.get("name", "?"))
        return d["system_prompt"], d.get("greeting", "")
    except (urllib.error.URLError, KeyError, ValueError) as e:
        log.warning("could not read agent %s (%s); using the fallback persona",
                    agent_id, e)
        return FALLBACK, ""


def quail_audio_input() -> room_io.AudioInputOptions:
    """Optional Voice Focus, using the room's LiveKit Cloud credentials."""
    if os.getenv("QUAIL_ENABLED", "false").strip().lower() != "true":
        return room_io.AudioInputOptions()
    if ai_coustics is None:
        log.warning("Quail unavailable; install requirements-quail.txt.")
        return room_io.AudioInputOptions()
    try:
        processor = ai_coustics.audio_enhancement(
            model=ai_coustics.EnhancerModel.QUAIL_VF_L,
        )
    except Exception as exc:  # noqa: BLE001 - optional cleanup must not stop the agent
        log.warning("Quail unavailable (%s); continuing without it.", type(exc).__name__)
        return room_io.AudioInputOptions()
    # Match Voice Focus's native 16 kHz / 240-sample processing blocks.
    return room_io.AudioInputOptions(
        sample_rate=16000, frame_size_ms=15, noise_cancellation=processor,
    )


def prewarm(proc) -> None:
    proc.userdata["vad"] = silero.VAD.load()


async def entrypoint(ctx: JobContext) -> None:
    await ctx.connect()

    # Don't start an avatar for an empty room. A browser that opens the room
    # and then fails - mic permission denied is the usual one - leaves the
    # agent here alone, and on a one-concurrent-session plan an avatar idling
    # in that room locks the face for everyone else until someone notices.
    # That happened twice while building this. Wait for a person; give up if
    # none arrives.
    try:
        await asyncio.wait_for(ctx.wait_for_participant(), timeout=30)
    except asyncio.TimeoutError:
        log.info("nobody joined %s within 30 s; not starting an avatar", ctx.room.name)
        ctx.shutdown(reason="no participant")
        return

    # Off the event loop: a blocking fetch here stalled the agent's audio loop
    # for 277 ms on the first run, and LiveKit said so.
    instructions, greeting = await asyncio.to_thread(load_persona)

    session = AgentSession(
        # AssemblyAI does the listening, and also the end-of-turn call below.
        stt=assemblyai.STT(),
        # A plain cascaded LLM, which is also why the quickstart's preflight
        # shim is gone: it existed to give Realtime models the eager-transcript
        # head start that LiveKit already does for cascaded ones.
        llm=openai.LLM(
            model=LLM_MODEL,
            base_url=AAI_GATEWAY,
            api_key=os.environ["ASSEMBLYAI_API_KEY"],
        ),
        tts=assemblyai.TTS(voice=TTS_VOICE),
        vad=ctx.proc.userdata["vad"],
        turn_handling={
            "turn_detection": "stt",
            "preemptive_generation": {"enabled": True},
            "interruption": {"min_duration": 0.75, "min_words": 1,
                             "false_interruption_timeout": 2.0},
        },
    )

    @session.on("metrics_collected")
    def on_metrics(ev) -> None:
        metrics.log_metrics(ev.metrics)

    # Must attach before session.start().
    avatar = synthesia.AvatarSession(
        synthesia.AvatarConfig(avatar_ids=[AVATAR_ID]),
        join_timeout=60.0,
    )
    for attempt in range(3):
        try:
            await avatar.start(session, room=ctx.room)
            break
        except synthesia.SynthesiaError as e:
            if not e.retryable or attempt == 2:
                raise
            await asyncio.sleep(2 * (attempt + 1))

    await session.start(
        agent=Agent(instructions=instructions),
        room=ctx.room,
        room_options=room_io.RoomOptions(audio_input=quail_audio_input()),
    )

    # The greeting is a fixed line from the stored agent, so it goes straight
    # to the voice. Asking the LLM to repeat it cost a round trip and, behind
    # the gateway, failed outright: Gemini rejects a request with no user turn
    # ("at least one contents field is required"), which the quickstart's
    # OpenAI Realtime model tolerated.
    if greeting:
        session.say(greeting, allow_interruptions=False)
    else:
        session.generate_reply(
            user_input="(a guest has just walked up)",
            instructions="Greet the guest in one short sentence.",
            allow_interruptions=False,
        )


if __name__ == "__main__":
    # agent_name makes dispatch explicit: the worker joins only rooms whose
    # token asks for it (server.py's RoomAgentDispatch), not every room.
    cli.run_app(
        WorkerOptions(
            entrypoint_fnc=entrypoint,
            prewarm_fnc=prewarm,
            agent_name="pocketvoice-jack",
        )
    )
