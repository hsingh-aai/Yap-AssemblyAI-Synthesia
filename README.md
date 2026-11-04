# Pocket Voice

Firmware for the AssemblyAI "Pocket Voice" device from the
[Product Day deck](https://claude.ai/artifact/3WTNsW2mpYDmQFju6FMDLS) — all of
AssemblyAI's products in one pocket device, handed out at the event as an
interactive product brochure.

Hardware: **M5Stack StopWatch Dev Kit (C152)** — ESP32-S3R8, 16 MB flash,
8 MB octal PSRAM, 1.75" round AMOLED (CO5300, QSPI; driver reports 468×468,
visible glass is a 466 px circle), CST820B touch,
ES8311 codec with MEMS mic and AW8737A speaker amp, BMI270 IMU, RX8130CE RTC,
M5PM1 power management, M5IOE1 I/O expander, 450 mAh battery.

## The firmware

**`firmware/pocketvoice` is the one to flash.** It carries every mode in one
build, at 43% of flash.

The whole interface is two gestures. The blue button walks the modes; a tap on
the screen — or the yellow button — does whatever the face says. There is no
menu to get lost in, which matters for a device handed to a hundred strangers
at a party. Dots along the bottom arc show where you are.

| Mode | What it does | AssemblyAI product |
|---|---|---|
| **Blurt** | Tap, talk, tap. The text is already there, and goes up the cable to the Mac. | Realtime STT |
| **Captions** | Live words as you speak them, partials grey, finals white. | Realtime STT |
| **Notetaker** | Listens as long as you like, then writes a summary and action items. | Realtime STT + LLM Gateway |
| **Jack** | A voice agent you can hold a conversation with. Tap to interrupt him. | Voice Agent API |
| **Status** | Wi-Fi, IP, RSSI, touch, memory, last error. | — |
| **Log** | The last eleven log lines, same as the serial relay. | — |

Source layout: `pv.h` declares everything shared; `ui.cpp` has the round-face
drawing, the chord text layout and the log ring; `net.cpp` is Wi-Fi; `stt.cpp`
is the realtime socket that Blurt, Captions and Notetaker share; `llm.cpp` is the
gateway call; each `mode_*.cpp` is one mode and `app.cpp` is the carousel.
A mode owns the whole face while it runs.

### How it got here

The phase sketches are kept because each one is the smallest provable step,
and they still build. `pocketvoice` is those four with the triplicated layout
and logging code collapsed into one shared layer — which is why it costs
barely more flash than the agent sketch did alone.

| Phase | Sketch | What it proves |
|---|---|---|
| 1 | `firmware/p1_hello` | Board brought up: round display, both buttons, mic level, speaker, vibration. No network. |
| 2 | `firmware/p2_blurt` | The first AssemblyAI round trip. Hold the yellow button, talk, release — the device records 16 kHz mono PCM to PSRAM, uploads to AssemblyAI, polls for the transcript and prints it on the face. Blue button walks four pages: Blurt, Transcript, Status, Log. |
| 3 | `firmware/p3_captions` | Live captions. One WebSocket to AssemblyAI's realtime endpoint carries 50 ms PCM frames up and Turn messages down; partial turns render grey and firm up to white as the model commits. Press A to start, press again to stop. |
| 4 | `firmware/p4_agent` | A voice agent you can talk to. Tap the screen, talk to Jack the Roaster over the Voice Agent API; mic audio goes up as base64 PCM16 at 24 kHz and the reply plays through the ES8311. Tap again to interrupt him. |
| 5 | not started | Notetaker (LLM Gateway), Speak (TTS), and an avatar for the agent. |

Phase 2 deliberately uses the **pre-recorded** endpoints, not streaming — it is
the smallest thing that proves mic → network → AssemblyAI → display on this
board. Phase 3 keeps the same mic path and the same screen and swaps the
record-upload-poll shape for one socket.

`tools/stream_probe.py` speaks the same realtime protocol from the Mac with no
device involved, so a caption problem can be pinned on the board or on the
account without guessing:

```bash
say -o /tmp/probe.wav --data-format=LEI16@16000 "testing one two three"
```

```bash
./tools/stream_probe.py --wav /tmp/probe.wav
```

`tools/agent_probe.py` does the same for the Voice Agent API — holds one turn
with Jack from the Mac, writes his reply to a WAV and plays it, so you can tell
an account or persona problem from a board problem:

```bash
./tools/agent_probe.py --say "Jack, tell me a joke about firmware."
```

Both share `tools/wsmin.py`, a WebSocket client small enough to need no
dependency.

## Build and flash

```bash
./firmware/build.sh pocketvoice upload   # compile and flash
```

```bash
./firmware/build.sh p2_blurt             # compile a phase sketch
```

`PORT` overrides the serial port (default `/dev/cu.usbmodem1101`).

### Reading the device

Every sketch logs to USB serial *and* to an on-screen Log page, so the device
can explain itself with or without a laptop:

```bash
./tools/relay.py
```

```bash
./tools/relay.py --only "[say]"
```

**`arduino-cli monitor` reads nothing from this board** and neither does `cat`.
The ESP32-S3's USB-Serial-JTAG peripheral only counts itself connected while
the host asserts DTR, and the monitor deliberately leaves the modem lines alone
to avoid tripping the chip's auto-reset, so the board drops every byte it
writes. `tools/relay.py` asserts DTR and RTS by hand, which is the whole
difference.

The boot banner prints the panel size and free heap, and the Wi-Fi scan prints
every visible SSID with its RSSI and auth mode.

### Dictating into the Mac

Blurt mode is the one for this: tap, talk, tap. The transcript goes up the USB
cable, and the relay puts it where you want it.

```bash
./tools/relay.py --copy
```

```bash
./tools/relay.py --type
```

```bash
./tools/relay.py --send --token <token-from-blurt>
```

`--copy` leaves each transcript on the clipboard. `--type` pastes it straight
into whatever app is in front, then puts your previous clipboard back.

`--send` is the good one: it hands the text to Blurt over a Unix socket and
Blurt inserts it, so the words inherit its focus pinning and its
editable-target check instead of a blind Cmd-V. Turn it on in Blurt under
Settings → Advanced → Local insertion, copy the token, and pass it as
`--token` or in `BLURT_INSERT_TOKEN`. The socket is found automatically under
`~/Library/Application Support/`; `--socket` overrides it. A reply of
`[clipboard]` means nothing editable had focus, so Blurt put the text on the
clipboard rather than dropping it.

**`--type` needs Accessibility permission** for whatever program runs the
script (System Settings → Privacy & Security → Accessibility). Without the
grant the paste silently does nothing — that is how macOS reports a missing
permission, so it looks like a bug in the relay.

Finished transcripts go out as `[say] <text>`, separate from `logf`, which is
bounded by the on-screen log's width and by its own 192-byte line. Blurt,
Captions and Notetaker all emit it.

The escaping is deliberately **the same convention Blurt's listener reads**:
`\n` is a newline, `\\` is a backslash, and the line break is the frame. One
rule runs from the microphone to the text field, so `relay.py --send` forwards
the device's bytes untouched. Escaping only newlines is not enough — a
transcript containing `C:\new` would otherwise arrive as `C:`, a line break,
and `ew`.

The audio never reaches the Mac. It goes from the device straight to
AssemblyAI over Wi-Fi; only the finished sentence comes down the cable.

Phase 2 needs credentials. Copy the template and fill it in — `secrets.h` is
gitignored:

```bash
cp firmware/p2_blurt/secrets.h.example firmware/p2_blurt/secrets.h
```

## Toolchain notes

Set up once:

```bash
arduino-cli core install esp32:esp32
arduino-cli lib install M5Unified M5GFX M5PM1 M5IOE1
```

M5Unified 0.2.29 has first-class StopWatch support (`board_M5StopWatch`),
including display power via the M5IOE1 expander, the ES8311 mic and speaker
enable callbacks, and the two side buttons (`BtnA` = yellow/G2,
`BtnB` = blue/G1). Autodetect finds the board; `-DM5GFX_BOARD=board_M5StopWatch`
can force it if that ever fails.

Board options are pinned in `build.sh`: octal PSRAM, 16 MB flash,
`app3M_fat9M_16MB` partitions, hardware CDC/JTAG with CDC on boot.

### Three traps worth knowing

**1. Nothing begins `Serial` on this board unless you ask.** With
`USBMode=hwcdc` the core's `main.cpp` guards its own `Serial.begin()` on
`!ARDUINO_USB_MODE`, and M5Unified's `cfg.serial_baudrate` defaults to `0`, so
`HWCDC::write()` returns early on a null `tx_ring_buf` and **every print is
silently dropped**. Hours went into modem-line and reset theories before the
fix turned out to be one line:

```cpp
cfg.serial_baudrate = 115200;   // before M5.begin(cfg)
```

**2. `.ino` files are stubs; real code lives in `app.cpp`.** arduino-cli 1.5.1
on this machine generates broken function prototypes for `.ino` files — it
drops return types and inserts the prototypes *inside* the first function body,
so any sketch with a helper function fails to compile. Keeping the code in a
`.cpp` beside the `.ino` sidesteps the preprocessor entirely.

**3. The panel reports 468×468; the glass is a 466 px circle.** Take the centre
from `Display.width()/2` (that is what the framebuffer is addressed in) and the
radius from the 466 face, or content drifts off the visible area. Text on a
round screen also has to be fitted to the *chord* at its own height, not to the
full width — `fitScale()` and `layoutPage()` in `p2_blurt` do this.

If the board stops answering esptool, it is almost always just powered off.
Short press = power on/reset, double press = power off; hold ~2 s until the
green LED lights to force download mode.

## This board is half duplex

M5Unified puts the mic on `I2S_NUM_1` and the speaker on `I2S_NUM_0`, but
wires **both to the same BCK (GPIO 17) and WS (GPIO 15) pins**, at different
rates — 16 kHz mono in, 44.1 kHz stereo out. Two I2S peripherals cannot drive
those pins at once, and M5Unified does no coordination, so firmware has to hand
the pins over by hand:

```cpp
M5.Mic.end();  M5.Speaker.begin();   // before playing
M5.Speaker.end();  M5.Mic.begin();   // before listening again
```

The consequence is architectural, not cosmetic: **the device cannot listen
while it speaks**, so voice barge-in is impossible no matter which stack is
used. `p4_agent` makes interruption a screen tap instead, which works well and
is more legible in a noisy room anyway.

## On LiveKit

LiveKit's [ESP32 SDK](https://github.com/livekit/client-sdk-esp32) does target
the ESP32-S3 and is built on Espressif's hardware-accelerated WebRTC, but it is
an **ESP-IDF 5.4+ component** and still a developer preview. Using it means
moving this project off arduino-cli onto ESP-IDF (with arduino-esp32 as a
component, to keep M5Unified) — a toolchain migration, not a library swap.

AssemblyAI's own Voice Agent API is a plain WebSocket, which the Arduino build
already handles: `p4_agent` is that, and it works. And since the shared I2S
pins rule out duplex audio, WebRTC's main advantage over a WebSocket — real
barge-in — cannot be realised on this hardware regardless. LiveKit is worth
revisiting only if an event specifically needs LiveKit rooms.

## Reading WebSocket frames from these APIs

`json.cpp` looks over-careful until you hit both of its reasons, which cost a
debugging session each:

**Frames are not null-terminated and are not small.** `session.ready` echoes
the agent's entire config back, system prompt included — 4.6 KB for Jack.
Copying each frame into a 2 KB buffer first truncated exactly the message that
starts a conversation, so the device sat at "No answer" while Jack was already
streaming his greeting down the same socket.

**The frame contains more than one `"type"`.** That echoed config describes the
audio format with its own nested `type` fields, so a plain substring search for
`"type":"` finds `audio` long before the `session.ready` further down. Keys are
matched at the top level only, stepping over nested objects.

Neither shows up against a hand-written test: a Mac probe using a real JSON
parser handles both without noticing. The device has to be the thing you test.

## Why Blurt stopped using the pre-recorded API

It was the right first move and the wrong thing to keep. Phase 2 recorded to
PSRAM, uploaded a WAV, created a transcript and polled — the smallest thing
that proves mic → network → display, and worth building in that order.

Then someone used it. Measured from a Mac on fast wifi, the round trip is:

| Step | Mac | On the device |
|---|---|---|
| upload | 0.4 s | 300 KB of WAV through TLS off an ESP32 |
| create | 0.2 s | about the same |
| queue + ASR | 2.5 s | the same, then rounded up to the next 2 s poll tick |
| **total** | **3.2 s** | roughly three times that |

Ten seconds of watching a spinner, for a device meant to be handed to a
stranger who has not agreed to wait. Streaming does not shorten that wait, it
removes it: the words arrive while you are still speaking, so the tap that
ends the sentence also ends the job. The 12-second recording cap went with it,
along with the 384 KB PSRAM buffer, because nothing is held to send later.

What the pre-recorded API is actually better at — speaker labels, chapters,
entity detection on a long file — none of it was on screen, so none of it was
being demonstrated by the slow path. Notetaker covers Speech Understanding.

## Making a thumbnail speaker loud

Two separate questions, and the first one has a hardware answer.

**Bluetooth speakers and headphones are not possible on this board.** They
speak A2DP, which needs Bluetooth Classic, and the ESP32-S3 has only BLE. It
is not a missing library — the silicon has no Classic radio, and the installed
toolchain says so plainly:

```
esp32-libs    esp_a2dp_api.h: 1 file    CONFIG_BT_CLASSIC_ENABLED=y
esp32s3-libs  esp_a2dp_api.h: 0 files   CONFIG_BT_BLUEDROID_ENABLED is not set
```

BLE Audio would sidestep Classic, but it needs Bluetooth 5.2 isochronous
channels and the S3 is 5.0. A wired speaker off the Grove port over I2S is the
only external-audio route this board has.

**Volume, then.** Measure before reaching for gain: a real Jack reply peaks at
**-0.2 dBFS** with an RMS of **-19.7**. There is no headroom at all, so
normalising does nothing (x1.02) and plain gain only shears the peaks off.
What the numbers actually describe is a crest factor near 20 dB — a few loud
consonants over quiet speech.

That calls for a limiter, not a volume knob. Lift everything and round the
tops over, and the quiet majority of the signal comes up while the peaks
asymptote toward full scale instead of hitting it. Simulated on a captured
reply:

| gain | loudness | samples at the rail |
|---|---|---|
| x1.5 | +3.2 dB | 0.00% |
| x2.5 | +6.6 dB | 0.00% |
| x5.0 | +9.7 dB | 0.00% |

`kGain` in `mode_jack.cpp` is the knob. With the master volume also moved from
200 to its maximum of 255 (worth +4.2 dB, since the mixer squares it), the
device is about **14 dB louder** than it shipped — a little over twice as loud
to the ear.

## Two traps in playing audio back

**M5Unified mixes, it does not queue.** `playRaw()`'s `channel` argument
defaults to `-1`, which means *any free virtual channel* - it scans from
channel 7 down for one that is not busy. Hand it a sentence in consecutive
100 ms blocks and the second block lands on a **different** channel from the
first and plays **on top of it**. Eight channels is eight overlapping copies
of one voice, which sounds like a bad line rather than like a bug. Streaming
playback has to name a channel:

```cpp
if (M5.Speaker.isPlaying(kSpkCh) >= 2) return false;   // queue full: 2 slots
M5.Speaker.playRaw(pcm, kPlayBlock, kAgentRate, false, 1, kSpkCh, false);
```

The `isPlaying()` check is not optional either: with a named channel and both
slots full, `playRaw()` **blocks** until one frees. In a single-threaded loop
that also pumps the WebSocket, blocking there starves the very stream being
played.

**Two slots is 200 ms of runway.** Starting playback the moment the first
100 ms has arrived leaves nothing for a jittery link to spend, so the agent
path holds the first block until 400 ms is buffered (or the reply ends, for a
reply shorter than that).

## A connecting screen that never changes looks like a hang

`sttStream()` only starts calling `keepGoing()` once the socket is open, which
makes the first call the exact moment the face can stop saying "Connecting".
Captions and Notetaker both used to repaint only when a *turn* arrived - so a
perfectly healthy socket sat on the connecting screen until someone had
spoken a whole sentence, and reported as "still connecting". The socket was
never the problem; only two of the three states were on screen.

Worth remembering for the other modes: every blocking network step wants a
visible state of its own, because the face is the only progress bar.

## The LLM Gateway has reasoning models in it

Notetaker summarises through
`https://llm-gateway.assemblyai.com/v1/chat/completions`, which is
OpenAI-shaped and takes the API key in an `authorization` header with no
Bearer prefix.

**Pick a non-reasoning model, or budget for the thinking.** `claude-haiku-5-5`
with `max_tokens: 220` returned an empty `content` and a usage block reading
`reasoning_tokens: 220` — the entire budget went to thinking and none of it
reached the message. Measured on the same transcript:

| Model | Latency | Reasoning tokens | Notes |
|---|---|---|---|
| `claude-haiku-5-5` | 3 s | 384 | Accurate, needs `max_tokens` ≥ 900 |
| `qwen3.5-4b-32k-fast` | <1 s | 0 | Fastest, but garbled "20 a day" into "100 units daily" |
| `gemini-3.5-flash-lite` | 1 s | 0 | Accurate and fast — what the firmware uses |

## Getting the words into Blurt itself

**`--send` needs both halves.** The device and `tools/relay.py` are done; the
listener they talk to is in an **open, unmerged** PR, so it is only present in
a build made from that branch. Until such a build is the one running,
`~/Library/Application Support/Blurt/insert.sock` never appears and `--send`
exits with `no socket at …`. To check which half you are missing:

```bash
strings -a /Applications/Blurt.app/Contents/MacOS/Blurt | grep -c LoopbackInsert
```

Zero means that build predates the listener: build Blurt from
`claude/loopback-insert-87f12a` (or merge it), then turn on Settings →
Advanced → Local insertion and copy the token. On this Mac that is **Blurt
Dev.app**, whose defaults live under `dev.alex.blurt.dev`; the shipping
`Blurt.app` is older and returns zero. `--type` works against any build and
needs no socket, at the cost of the care described below.

Nothing comes down the cable at all while no reader is attached: the ESP32-S3
only counts its USB CDC as connected while a host asserts DTR, so an unread
device is a silent one, not a buffered one. A `--send` that shows nothing is
usually a relay that was never started.

**Built:** [AssemblyAI/blurt#284](https://github.com/AssemblyAI/blurt/pull/284)
adds a loopback listener on a Unix socket, off by default behind a Settings →
Advanced switch and a token. It routes text through `AppCoordinator`'s own
injector rather than a second one, so a handed-over line serializes against a
Mac-side dictation landing at the same moment instead of racing it on the
global pasteboard.

Known gap in that PR: `priorText` and `windowTitle` are passed as nil, so
`InsertionSeparator` has no basis and adds **no leading space** between
consecutive handed-over transcripts. Closing it means making
`FocusCapture.captureFieldContext()` reachable from the app shell. The wrong
fix is to have the sender guess at a space, which is the duplication this
whole change exists to remove.

### Why it was needed

`tools/relay.py --type` pastes into the front app, which works. What it does
not do is any of the care the Blurt app already takes: `InsertionSeparator`
decides whether a leading space is needed, and the injector pins the focus
captured when the trigger went down, so text follows you if focus moves.

The [Blurt repo](https://github.com/assemblyai/blurt) has **no inbound surface
today** — no macOS URL scheme (the `blurt://` one belongs to the iOS keyboard
extension), no XPC listener, no network listener, no AppleScript, no CLI.
`BlockingIPC.swift` reads like one but is a Dispatch queue for blocking
Accessibility and pasteboard calls.

Adding one is small, because `InjectorProtocol` is already the seam the whole
pipeline delivers through:

```swift
func insert(_ text: String, after priorText: String?, windowTitle: String?) async throws
```

A loopback `NWListener` in `App/Blurt/` that accepts text and calls it is about
one file, and the app is not sandboxed, so no entitlement changes. It must go
in the app shell, **not** `Sources/BlurtEngine/` — the engine is deliberately
dependency-free and `check.sh` guards that.

Do not add a `blurt://insert?text=…` URL scheme. macOS keeps records of URLs,
and dictated speech can be private.

## Before the 100-unit build

- `p2_blurt` calls `WiFiClientSecure::setInsecure()` — no certificate
  validation. Pin AssemblyAI's root CA before these go out on conference wifi.
- Credentials are compiled in. 100 devices on event wifi need provisioning
  (SoftAP setup, or a captive portal) rather than a rebuild per unit.
- `WIFI_SSID_PREFIX` is matched as a prefix and the strongest match wins, which
  is wrong when a venue runs sibling SSIDs: `AssemblyAI` matched
  `AssemblyAI-Guest-SF` (enhanced-open) over `AssemblyAI-Employee-SF` (wpa2/3)
  by 1 dBm. Give it enough of the name to be unambiguous.
- Per the deck: ~20 units flashed a day, ~5 working days for 100.
