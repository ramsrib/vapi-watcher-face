# Vapi Watcher Face

An animated face on a **SenseCAP Watcher** that sees you and talks back through
[Vapi](https://vapi.ai). Built to live inside a soft toy, with the round display
as its face.

> **Status:** face and voice work on hardware — press the knob, talk, it talks
> back, with no echo. Vision is written but blocked on the Himax having no model
> Vision now runs — the Himax reports its model and inference streams at ~11/s —
> but the camera returns black frames, which appears to be physical.
> See [Roadmap](#roadmap).

## The face

Nine expressions drawn with LVGL primitives, not bitmap frames:

```
boot  standby  detecting  greeting  listening  thinking  speaking  sleeping  confused
```

Drawn shapes rather than PNG sequences is a deliberate choice. The stock
firmware ships five PNG frames per state, which is fine for a status indicator
but far too coarse to track speech. Shapes can be driven continuously from live
signals — `face_set_mouth_level()` will be fed real audio amplitude — which is
what makes it read as alive rather than as an animation playing.

Expressions differ by **brightness and geometry, never hue**. Staying on one hue
is what makes it read as a single creature with moods instead of a status light
cycling through colours. Blinks are randomised, because a face that blinks on a
fixed schedule looks mechanical.

Palette: mint `#88F1B8` on near-black `#0E0E12`. Full reasoning, contrast
figures and RGB565 notes in [`../sensecap-watcher/DESIGN-palette.md`](../sensecap-watcher/DESIGN-palette.md).

## Build and flash

Needs ESP-IDF **5.5.1+** and Seeed's SDK checked out at `../_sdk/SenseCAP-Watcher-Firmware`
**with submodules initialised**:

```bash
git submodule update --init --recursive        # in the SDK checkout
idf.py build
python3 tools/flash.py                         # NOT idf.py flash — see below
```

### Why `tools/flash.py` instead of `idf.py flash`

**The Watcher's CH342 USB-serial bridge drops bytes when the host sends more
than 256 at a time.** esptool's defaults are far larger (0x1800 for RAM, 0x4000
for stub flash writes), so every write fails on its first block while reads —
small device→host replies — work perfectly. The ROM reports the missing bytes as
a CRC error, which looks like corruption rather than an overflow.

Measured on this board:

| write block | result |
|---|---|
| 6144 (esptool default) | fail |
| 1024 | fail |
| 512 | fail |
| 384 | fail |
| **256** | **works** |

`tools/flash.py` caps the block size and adds `--no-compress` (the stub's
compressed path batches independently of the cap). It reads offsets from
`build/flash_args` rather than hardcoding them.

This is a host-side limit, not a board fault — and it is why Seeed's documented
`esptool.py -b 2000000 write_flash ...` cannot work here. **No Rosetta, no WCH
driver and no Linux machine are needed**, contrary to where the symptoms first
point.

## Audio: why there is no AEC here

This board has **two codecs** — ES8311 for the speaker, ES7243/ES7243E for the
microphone. Boards where one chip serves both directions can loop the DAC back
into the ADC as an echo reference; with two chips that reference does not exist,
so on-device acoustic echo cancellation has nothing to cancel against.

That turned out not to matter. On the AtomS3R, where an AEC *was* available, it
contributed almost nothing — echo was actually solved by **hard-muting the
microphone whenever the assistant is audible**, timed off a write-ahead playout
clock. That technique needs no reference signal, so it ports here unchanged.

Dropping the AEC also let `esp_capture`, `av_render` and the whole
esp-webrtc-solution dependency fall away in favour of direct `esp_codec_dev`
reads and writes. The gate is *more* accurate as a result: we own every write to
the codec, so the playout clock is exact rather than inferred from a render
FIFO — which on the AtomS3R reported 0 and silently broke the first attempt.

Verified on hardware: across a full conversation, every transcribed `user:` turn
was the actual speaker and none was an echo of the preceding `assistant:` line.

The cost is **no barge-in** — talking over the assistant does nothing.

## Gotchas, all of which cost real time

**The I2C driver conflict aborts before `app_main`.** IDF runs
`check_i2c_driver_conflict` in a *global constructor*, so if both the legacy
`driver/i2c.h` and the new `driver/i2c_master.h` implementations are merely
**linked**, the board panics at boot:

```
E i2c: CONFLICT! driver_ng is not allowed to be used with this old driver
```

Avoiding codec calls does not help — presence alone is fatal. The BSP is
all-legacy, but `esp_codec_dev` 1.2.0 (which the BSP pins exactly) hard-selects
the new API on IDF ≥ 5.3 with no opt-out. Fix: **vendor 1.3.6 under
`components/` and reference it with `override_path`**, which bypasses the
version solver, then set `CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE=y`. Verify with
`nm` that no `i2c_new_master_bus` symbols survive.

**rlottie breaks CMake 4.** The SDK's vendored rlottie declares
`cmake_minimum_required(VERSION <3.5)`, which modern CMake refuses. It arrives
unavoidably (BSP → `esp_lvgl_port` → SDK `lvgl` → rlottie), so it cannot be
scoped out. `set(CMAKE_POLICY_VERSION_MINIMUM 3.5)` is CMake's supported escape
hatch and leaves the SDK unpatched.

**`esp_lcd_touch_chsc6x` does not compile** against this IDF and is for a
different panel revision — the BSP uses `esp_lcd_touch_spd2010`. Do not include
it.

**The partition table must preserve `nvsfactory`.** The SDK examples' table puts
a writable `nvs` at `0x9000`, exactly where the stock device keeps its
provisioned identity (EUI, SenseCraft credentials). `partitions.csv` here
mirrors the stock layout instead, verified byte-identical against a dump of the
real device. Seeed's docs warn about this specifically.

**Known warning, not yet resolved:** `ledc: GPIO 8 is not usable, maybe conflict
with others` at boot — the LCD backlight PWM. Brightness control via
`bsp_lcd_brightness_set()` may not work.

## Layout

```
main/
  main.c          entry point, boot sequence, knob button
  face.c/.h       the face: LVGL drawing + expression state machine
  palette.h       colour tokens, RGB565-verified
  vapi_client.c   REST call creation + websocket transport (ported verbatim)
  vapi_media.c/.h audio: direct codec I/O, gain staging, echo gate
  vapi_app.c      call state -> expression mapping
  vision.c/.h     Himax NPU: inference, presence detection, frame capture
  vlm.c/.h        frame -> sentence, via Anthropic or OpenAI
  model_flash.c/.h  one-shot: put an AI model on the Himax
  wifi.c          station bring-up
  settings.h      all tuning knobs, each with its measured justification
tools/
  flash.py        CH342-safe flasher — use this, not `idf.py flash`
  grab_frame.py   pull the frame the camera saw, as a JPEG
  gen_env_header.py  .env -> compile-time defines
components/
  esp_codec_dev   vendored 1.3.6, for the I2C compat knob
```

Device-level knowledge — hardware inventory, the dual USB ports, the verified
stock backup, the Himax console, audio architecture — lives in
[`../sensecap-watcher/`](../sensecap-watcher/). Start at its
[README](../sensecap-watcher/README.md).

The voice transport and echo-gate technique came from
[`../vapi-atoms3r-voice`](../vapi-atoms3r-voice), which is the same idea on an
M5Stack AtomS3R and is published at
<https://github.com/ramsrib/vapi-atoms3r-voice>. Its
[`docs/AEC-TUNING.md`](../vapi-atoms3r-voice/docs/AEC-TUNING.md) is the fuller
treatment of why gating beats cancelling.

## Vision

The NPU path works end to end: the Himax is queried, inference runs
continuously, people are detected, and walking up to the device starts a Vapi
call on its own. Detections score 50-87% in ordinary room light.

What comes up now, in 2.4 s from power-on:

```
VISION: himax SenseCAP Watcher, fw 2024.08.16
VISION: model: Person Detection
VISION:   class 0: person
VISION: vision up — continuous inference running
```

and inference reports `perf: [7, 76, 0]` — 7 ms preprocessing, 76 ms inference,
around 11 events per second, with a real 416x416 JPEG in every reply.

### What was actually wrong: `CONFIG_FREERTOS_HZ`

Every SSCMA command used to time out, and the log filled with
`request not found: <cmd>` — replies arriving *after* the client had given up.

The cause was **not** the Himax, the SPI link, the IO-expander sync line, or a
missing model. It was this project inheriting **IDF's default tick rate of
100 Hz**, where both of Seeed's working examples set **1000 Hz**:

```
CONFIG_FREERTOS_HZ=1000                      # ours had the IDF default, 100
CONFIG_SSCMA_PROCESS_TASK_STACK_SIZE=10240   # ours had 4096
CONFIG_SSCMA_PROCESS_TASK_AFFINITY_CPU1=y    # ours had no affinity
```

At 100 Hz every tick is 10 ms rather than 1 ms, so every delay in the SSCMA read
path inflates tenfold. Replies arrive in 256-byte packets, each preceded by a
sync check, so the cost multiplies per packet and pushes the total past
sscma's 2000 ms request timeout.

Measured, same hardware, only the tick rate changed:

| | before | after |
|---|---|---|
| `request not found` per 30 s | 112+ | **1** |
| `rx buffer is full` per 30 s | 50 | **0** |
| `sscma_client_invoke` | `ESP_ERR_TIMEOUT` | **succeeds** |

**If you scaffold a project for this board yourself, diff your `sdkconfig` against
the SDK examples before debugging anything.** This cost hours and was entirely
self-inflicted.

### Two conclusions that were wrong

Recorded because both were confidently held and both sent me somewhere useless.

**"The device has no AI model."** It has always had *Person Detection* loaded.
`get_model` was timing out for the tick-rate reason above, and a timeout reads
exactly like an absence. This led to writing a model to the Himax's flash at
`0xA00000` that was never needed. No harm done — separate region, firmware
untouched — but it was a real write to another processor's memory on a false
premise. `model_flash.c` is kept because the mechanism is correct and useful if
a device genuinely lacks a model.

**"The Himax was already running inference and flooding the bus."** It was
rebooting in a loop; the flood was repeated boot banners.

Also worth knowing: **do not call `sscma_client_set_model()` at all** on this
device. The SDK example selects slot 1 and the factory firmware selects slot 4;
both return `ESP_FAIL` here, because the loaded model is in neither. Removing
the call is what let `invoke` succeed.

### The black frames were a lens cap

An early frame pulled off the device decoded to a valid 416x416 JPEG that was
almost entirely black. The model was not failing — it had nothing to see. There
is no exposure control in the SSCMA AT command set (`AT+SENSOR` takes only
id/enable/opt_id), so brightness is the sensor's business and not something
firmware can correct. Uncovering the lens fixed it.

Worth keeping as a habit: `VISION_DUMP_FRAME` prints one base64 JPEG over the
console at startup. Reassembling it on the host answers "is the camera seeing
anything" in one step, which no amount of reading detection scores does.

`MIN_SCORE` is 40 and real detections land at 50-87%, so it is clearing but has
never been pushed against a false positive.

## Seeing, not just noticing

Detection answers *that* someone is there. It cannot say anything about them,
and for a device whose whole trick is reacting to people, that gap is most of
the act: "hello there" is a toy, "nice orange hoodie" is a thing people call
their friends over to see.

So a frame goes to a vision model and comes back as a sentence.

### Why it has to be a separate call

Vapi's `add-message` carries a plain string — `OpenAIMessage.content` is typed
as a string, not a multimodal content array — so the image cannot be handed to
the assistant's own model even though that model can see. The caption has to be
produced by a second request and injected as text.

### The greeting never waits for it

This is the part that matters for a demo. The obvious pipeline — caption, then
start the call — puts a vision model's round trip between a person walking up
and the device saying anything, which is exactly the silence that makes a booth
demo look broken.

Instead:

```
person detected ──> call starts immediately ──> "oh, hello!"   (firstMessage)
                └──> caption_task (parallel) ──> add-message ──> "nice hoodie"
```

The description lands a second or two later, in time for the second turn. Three
things follow from that, all of them good:

- **provider latency stops mattering.** Nothing is waiting on it.
- **failure is invisible.** No caption means the assistant simply never mentions
  appearance. That is why there is one attempt and an 8 s timeout, not a retry
  loop — a late caption is worse than none, because the assistant would remark
  on someone who has already walked off.
- **it is better theatre anyway.** Greeting first, observation second, is how a
  person does it.

### What it costs to run

Almost nothing, because the NPU decides what leaves the board. A frame is sent
once per arrival — roughly one request per visitor, not one per frame — and the
image never leaves PSRAM on the way out. There is also no encode step: SSCMA
already delivers the JPEG as base64 text inside its JSON reply, which is the
exact form both vision APIs want, so the string goes from the SPI reply into the
request body untouched.

Frames are retained only when a detection cleared `MIN_SCORE`, at most one per
second, into a single buffer that is grown and reused. Retaining every frame
meant a ~30 KB copy ten times a second in exchange for pictures of an empty
room.

### Anthropic or OpenAI

Both are compiled in; `VLM_PROVIDER_ANTHROPIC` in `settings.h` picks one. The
default is Anthropic, and the reasons are operational rather than about model
quality — one sentence about one person in one small frame is not where frontier
models separate:

- Anthropic takes the base64 JPEG verbatim in its own `source.data` field;
  OpenAI wants it wrapped in a `data:image/jpeg;base64,` URI.
- Anthropic's `max_tokens` has been stable; OpenAI renamed theirs to
  `max_completion_tokens` on the newer models, and the old name is rejected.

Fewer things to be wrong about on venue wifi. Having the second path one
`#define` away is the actual point: at a booth, a provider you can switch to in
a reflash is worth more than whichever one benchmarks better today.

The model is `claude-sonnet-5`, which is a deliberate exception to reaching for
the strongest model available. The task is a 20-word description of a person
standing two feet from a camera — Sonnet does that as well as Opus — and the
result is spoken aloud in a live conversation, where a second of latency is
audible and a point of caption quality is not. Swap `VLM_MODEL` to
`"claude-opus-5"` if a caption ever disappoints; nothing else changes.

### Proving it works before the demo

`VLM_SELFTEST` captions one frame at boot and logs the result:

```
VLM: claude-sonnet-5 in 2851 ms (16 KB image): nobody in view
VAPI_APP: selftest OK (claude-sonnet-5): nobody in view
```

That one line exercises the entire device-side path — PSRAM allocation, TLS to
the provider, the request body, the response parse. The three things most likely
to be wrong (a bad key, a rejected body shape, too little heap for the
handshake) all fail here, on the console, ten seconds after power-on.

The alternative is finding out the first time someone walks up to the device,
which at a booth is both the worst moment and the hardest to read: the
conversation still happens, it is just inexplicably less impressive.

Measured on this board: **2851 ms** for a 16 KB frame, against **870 ms** for
the same request from a Mac. The difference is the TLS handshake and a slower
upload. Neither number matters much, since nothing waits on it — but both are
comfortably inside the 8 s timeout.

### Looking through the camera

`tools/grab_frame.py --reset` pulls the frame the Himax actually saw and writes
it as a JPEG. It prefers a frame with a person in it and falls back to whatever
is in front of the lens after 25 s.

Worth doing whenever the device moves. Every other signal about the camera is
indirect — detection scores tell you the model is running, not what it is
looking at, and a covered lens, a dark room and a sideways mount all read
identically as "no detections". One picture settles all three, and it is the
same picture the vision model will be asked to describe.

### Setting it up

Put a key in `.env`:

```
ANTHROPIC_API_KEY="sk-ant-..."
```

Leave it blank and everything still works — the device greets people it sees, it
just never comments on how they look. Same DEV ONLY caveat as the Vapi key: it
is compiled into the image and recoverable from a flash dump.

The prompt lives in `VLM_PROMPT` and is written for a caption that will be
*spoken*: no hedging, no inventory of the room, no mention of the camera. It
also tells the model to ignore orientation, because the Watcher is easy to mount
rotated and without that line the model spends its one sentence observing that
the person is lying down.

## What a real conversation taught us

Three findings, none of which showed up in any amount of unattended testing.

**The greeting was not the configured one.** `VAPI_GREETING` is "Oh, hello
there!"; the device said "Oh, hi there. Uh, you just wandered right into my
little world." A message arriving before the assistant has spoken makes Vapi
plan a reply rather than play `firstMessage` verbatim — so injecting camera
context the instant the socket came up cost **9.8 s of silence** where a canned
line would have been immediate. The entire point of captioning off the critical
path was to avoid that, and the connect-time injection put it back.

**It held an accurate description and never used it.** Asked "what do you see?",
it answered "I see you standing right there" while sitting on "a grey Bulldogs
t-shirt". The vague `person, 92%` message landed first and anchored it. Nothing
now goes out at connect; the only message worth sending is the specific one,
once it exists.

**One frame is a photograph, not sight.** Asked "what's in my hand?", it
improvised: "hold it closer so I can get a good look." That sounds alive and
is not, and for this character improvising about what it can see is the worst
available failure. `VLM_REFRESH_MS` looks again every 8 s for the length of the
call, sending only changed descriptions — repeating an identical one teaches the
model that its eyes report the same thing whatever happens.

## add-message triggers a reply unless you say otherwise

Vapi's `add-message` carries `triggerResponseEnabled`, and it **defaults to
`true`**. A message sent without it does not merely join the conversation
history — it prods the assistant to speak.

For camera descriptions arriving every eight seconds that is badly wrong, and
wrong in a way that reads as a model problem rather than a protocol one. Asked
"what am I holding?", the device replied "hold it up a little closer so I can
get a good look" while holding a description that read *a yellow tennis ball
with "Wilson 4" printed on it*. It was not dodging the question. It was
answering the **frame refresh**, which had just prodded it, and the question was
never addressed at all.

`vapi_send_context()` inserts silently; `vapi_send_text()` keeps the triggering
behaviour. The person's own speech is what should make it talk.

Worth noting the role was never the problem — `system` is valid, alongside
`assistant`, `user`, `function` and `tool`. The bug was an absent default.

## Seeing an object needs more pixels than seeing a person

416x416 is what the detection model wants and it is plenty for "is that a
person". It is not plenty for "what is that in their hand": at ~8 KB of JPEG a
held object is a few dozen pixels, and a vision model asked to identify one will
hedge or invent. `VISION_SENSOR_OPT` selects 640x480 from the sensor's own list
(0 = 240x240, 1 = 416x416, 2 = 480x480, 3 = 640x480) — 1.8x the pixels, which is
affordable because frames are only retained once a second.

With that and a *generic* prompt, the descriptions became specific enough to be
worth having:

```
VLM: ...holding a yellow-green tennis ball up near their face.
VLM: ...holds up a yellow tennis ball with "Wilson 4" printed on it
```

The prompt change mattered as much as the pixels. It used to ask for "clothing,
colours, hair, glasses" — so that is what came back, however interesting the
thing in shot. It now asks for a plain description of the frame and nothing
else. The vision layer reports what is in front of the camera; the assistant's
system prompt decides what is worth saying about it. Task-specific instructions
belong there, where they can see the conversation, not here, where they cannot.

## WiFi failover

`WIFI_SSID_2` / `WIFI_SSID_3` in `.env` are backup networks, tried in order when
the one above them does not answer. Leave them blank and nothing changes —
blank entries are dropped at startup.

For a booth the second one wants to be a phone hotspot. Venue WiFi is the least
reliable part of a conference, and the difference it makes is between "the demo
is down" and a pause of a few seconds nobody attributes to the network.

Each network gets two attempts before the next is tried. Two, because a dropped
association and an absent AP are indistinguishable from the device's side and
want opposite responses: the first usually reconnects immediately, the second
never will. One retry serves the blip without stranding the device on a network
that is not there.

At roughly 4 s per network a full cycle of three is ~12 s, which is longer than
a booth visitor will wait — so the order matters. Put the network you expect to
work first. On success the index stays put rather than resetting, so a network
that just worked is the one retried first if it blips.

## A booth is a queue, not one conversation

Two bugs that only matter once the second visitor walks up, which is to say:
every real use of this device.

**The call outlived the visitor.** Nothing ended a call when someone left —
`vision.c` logged `person left` and did nothing with it, and the only
`vapi_call_stop()` was the knob. That is worse than wasted minutes, because the
presence poll declines to start a call while one is active: visitor two got no
greeting and was dropped into visitor one's conversation, mid-context, carrying
visitor one's description. `HANGUP_AFTER_ABSENT_MS` ends the call ten seconds
after the last detection, waiting for the assistant to stop talking first so a
goodbye is not truncated.

The cooldown that stops a finished visitor being re-greeted in a loop then has
to not punish the *next* one: measured 26 s between someone walking up and being
greeted, because a call they had nothing to do with had just ended. Departure is
what distinguishes the two cases and it is observable, so the cooldown is
cleared the moment the frame is empty.

**The poll started a new call every tick.** `vapi_toggle_call()` dispatches to a
worker, so for the ~2 s of HTTPS POST and TLS handshake the call is being opened
while `vapi_call_is_active()` is still false. The presence poll asked that
question every 2 s and answered it wrongly each time: a fresh billable Vapi call
per tick, for as long as anyone stood there. Measured four calls in six seconds
before the guard.

The fix is that "a call is in progress" has to include *while it is being set
up*. `s_call_pending` is claimed inside a critical section, because two tasks
reach `vapi_toggle_call()` — the presence poll and the button — and the test and
the claim have to be one operation or both pass it.

Worth noting the edge-triggered arrival callback hid this completely: it fired
once, so it could not race itself. Moving to a level-triggered poll is what made
the window reachable, and the same change is what fixed the lost-arrival bug
above. Level-triggered logic is more robust about *what* it decides and much
less forgiving about *when*.

## Internal RAM is the constraint, and TLS is what spends it

Measured low-water mark of free internal heap, same firmware, same boot:

| scenario | before | after |
|---|---|---|
| boot + one TLS session (caption selftest) | 99,406 | 99,398 |
| boot + a call (POST, websocket, caption) | **7,478** | **59,770** |
| SPI transactions failed during call setup | 100 | **0** |

At 7 KB the failures did not appear in this code at all. They appeared as SPI
DMA descriptors failing to allocate elsewhere on the board:

```
E sscma_client.io.spi: client_io_spi_read(353): spi transmit (queue) failed   (x100)
E lcd_panel.io.spi: panel_io_spi_tx_color(395): spi transmit (queue) color failed
```

— a glitching display and a hundred dropped Himax transactions every time a call
started, with nothing pointing at the cause. That indirection is the thing worth
remembering: on this board, memory pressure is reported by whichever unrelated
subsystem happens to need DMA next.

Two changes, and the first is most of it:

**`CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`.** By default mbedTLS holds the full IN/OUT
content buffers for the life of each session, so an idle websocket sits on
~20 KB it is not using — which is exactly the 20 KB the caption request's
handshake needs. With dynamic buffers they are allocated per record and freed in
between. Note it does nothing for the single-session case (99,406 → 99,398);
the win is entirely in concurrency.

**`VLM_SETTLE_MS`.** The caption task waits 1.2 s before its handshake, so it is
not competing with the websocket's own TLS session and the audio buffers landing
at the same instant. Free, because the greeting is still playing.

`VAPI_SELFTEST_CALL` places one call automatically after boot with nobody
present. The call path is both the tightest moment the board sees and the one
that normally cannot be tested without a person in front of the camera — which
makes the riskiest code the hardest to exercise. It is off by default because it
places a real, billable call on every boot.

## Roadmap

1. ~~Prove the toolchain; face on the display~~ **done**
2. **Port the Vapi voice client** from [`../vapi-atoms3r-voice`](../vapi-atoms3r-voice) —
   the BSP exposes `esp_codec_dev_handle_t` for mic and speaker, the same type
   that firmware builds on, so the transport and echo gate should move nearly
   verbatim. AEC must be re-measured on this board.
3. **Wire the face to call state** — the echo gate's write-ahead playout clock is
   already a sample-accurate "is the assistant audible" signal, which is exactly
   what drives `speaking` and the mouth.
4. ~~**Add vision**~~ **done** — inference runs, presence detection wakes a
   call on its own, and the frame that triggered it is captioned by a vision
   model and injected via Vapi `add-message` while the greeting is already
   playing.
5. Custom art, then an enclosure — and re-measure the acoustics, because a plush
   changes them completely.
