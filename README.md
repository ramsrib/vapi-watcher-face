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
