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

The NPU path works: the Himax is queried, inference runs continuously, and
presence detection wakes a Vapi call. **One thing is unresolved — the camera
returns black frames**, so no detection has ever fired. That looks physical
rather than software (see below).

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

### The remaining problem: black frames

A frame captured off the device decodes to a valid 416x416 JPEG that is almost
entirely black, with a faint shape in the centre. The model is not failing — it
has nothing to see.

This looks physical: a protective film still on the lens, something covering it,
or the device pointing somewhere dark. There is no exposure control in the SSCMA
AT command set (`AT+SENSOR` takes only id/enable/opt_id), so brightness is the
sensor's own business and not something firmware can correct.

To re-test: `VISION_KEEP_FRAMES 1` retains the JPEG, and a frame can be pulled
out with `sscma_utils_fetch_image_from_reply()`. `MIN_SCORE` (40) has never been
validated against a real detection and should be tuned once the camera sees
something.

## Roadmap

1. ~~Prove the toolchain; face on the display~~ **done**
2. **Port the Vapi voice client** from [`../vapi-atoms3r-voice`](../vapi-atoms3r-voice) —
   the BSP exposes `esp_codec_dev_handle_t` for mic and speaker, the same type
   that firmware builds on, so the transport and echo gate should move nearly
   verbatim. AEC must be re-measured on this board.
3. **Wire the face to call state** — the echo gate's write-ahead playout clock is
   already a sample-accurate "is the assistant audible" signal, which is exactly
   what drives `speaking` and the mouth.
4. **Add vision** — inference runs; blocked on the camera returning black
   frames, which looks physical. See above. Once it sees something, presence
   detection already wakes a call and `sscma_utils_fetch_image_from_reply()`
   gets a JPEG to a VLM for injection via Vapi `add-message`. Once a model is loaded, presence detection already wakes a call, and
   `sscma_utils_fetch_image_from_reply()` gets a JPEG to a VLM for injection via
   Vapi `add-message`.
5. Custom art, then an enclosure — and re-measure the acoustics, because a plush
   changes them completely.
