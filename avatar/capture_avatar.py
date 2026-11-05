#!/usr/bin/env python3
"""Record the Synthesia avatar's video from a live room, for the device.

    .venv/bin/python capture_avatar.py out.mp4 [seconds]

The StopWatch cannot play the interactive avatar - that needs a WebRTC video
client - but it can play mouth sprites cut from any video of a face. So this
borrows the live avatar for a few seconds: it opens a room the same way the
page does (which dispatches agent.py, so agent.py must be running), joins as a
silent participant, subscribes to the avatar's video while Jack says his
greeting, and pipes the frames into ffmpeg. tools/make_avatar.py takes it
from there.

Joining with no microphone matters: the agent hears nothing, so the only
speech in the recording is the greeting, and the mouth genuinely moves.
"""
import asyncio
import os
import pathlib
import subprocess
import sys
import uuid
from datetime import timedelta

from dotenv import load_dotenv
from livekit import api, rtc

load_dotenv(pathlib.Path(__file__).parent / ".env")
AVATAR_IDENTITY = "synthesia-avatar-agent"


def token(room: str) -> str:
    return (
        api.AccessToken()
        .with_identity(f"recorder-{uuid.uuid4().hex[:6]}")
        .with_ttl(timedelta(minutes=5))
        .with_grants(api.VideoGrants(room_join=True, room=room,
                                     can_publish=False, can_subscribe=True))
        .with_room_config(api.RoomConfiguration(
            agents=[api.RoomAgentDispatch(agent_name="pocketvoice-jack")]))
        .to_jwt()
    )


async def main(out: str, seconds: float) -> None:
    name = f"capture-{uuid.uuid4().hex[:8]}"
    room = rtc.Room()
    got_track: asyncio.Future = asyncio.get_running_loop().create_future()

    @room.on("track_subscribed")
    def _on(track, _pub, participant):
        if (track.kind == rtc.TrackKind.KIND_VIDEO
                and participant.identity == AVATAR_IDENTITY
                and not got_track.done()):
            got_track.set_result(track)

    await room.connect(os.environ["LIVEKIT_URL"], token(name))
    print(f"# joined {name}; waiting for the avatar's video", flush=True)
    try:
        track = await asyncio.wait_for(got_track, timeout=60)
        stream = rtc.VideoStream(track, format=rtc.VideoBufferType.RGB24)
        ff = None
        size = None
        frames = 0
        loop = asyncio.get_running_loop()
        deadline = loop.time() + seconds
        async for ev in stream:
            f = ev.frame
            if ff is None:
                size = (f.width, f.height)
                ff = subprocess.Popen(
                    ["ffmpeg", "-nostdin", "-loglevel", "error", "-y",
                     "-f", "rawvideo", "-pix_fmt", "rgb24",
                     "-s", f"{f.width}x{f.height}", "-r", "25", "-i", "-",
                     "-pix_fmt", "yuv420p", out],
                    stdin=subprocess.PIPE)
                print(f"# recording {f.width}x{f.height}", flush=True)
            if (f.width, f.height) == size:   # a mid-stream resize would corrupt the pipe
                ff.stdin.write(bytes(f.data))
                frames += 1
            if loop.time() > deadline:
                break
        ff.stdin.close()
        ff.wait()
        print(f"# {frames} frames -> {out}")
    finally:
        await room.disconnect()
        # Close the room outright, so its avatar session stops spending quota.
        async with api.LiveKitAPI() as lk:
            await lk.room.delete_room(api.DeleteRoomRequest(room=name))
        print(f"# deleted room {name}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("usage: capture_avatar.py out.mp4 [seconds]")
    asyncio.run(main(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 14))
