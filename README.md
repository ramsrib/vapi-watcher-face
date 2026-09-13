# Vapi Watcher Face

An animated face on a **SenseCAP Watcher** that sees you and talks back through
[Vapi](https://vapi.ai). Built to live inside a soft toy, with the round display
as its face.

> **Status:** face and voice work on hardware — press the knob, talk, it talks
> back, with no echo. Vision is written but blocked on the Himax having no model
> loaded. A model has since been flashed successfully, but inference still does
> not start. See [Roadmap](#roadmap).

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
  main.c        entry point, expression demo loop
  face.c/.h     the face: LVGL drawing + expression state machine
  palette.h     colour tokens, RGB565-verified
tools/
  flash.py      CH342-safe flasher (see above)
components/
  esp_codec_dev vendored 1.3.6, for the I2C compat knob
```

Device bring-up notes, the flash backup and the capability survey live in
[`../sensecap-watcher/`](../sensecap-watcher/).

## Vision: model flashed, inference still not starting

`vision.c` implements the NPU path — continuous inference, presence detection
with hysteresis, JPEG retrieval, and a callback that wakes a call when someone
arrives. `model_flash.c` provisions the Himax with a model. **The model write
works; `invoke` still does not.** Unfinished, and this is where it stands.

### The model is on the device

The Watcher ships with no AI model: the stock firmware only downloads one once a
SenseCraft task is assigned, which this device never had. The model URL, the
flash address and the procedure were all recovered from the factory firmware
image and Seeed's `app_ota.c`:

| | |
|---|---|
| model | `sensecraft-statics.oss-accelerate.aliyuncs.com/.../epoch_50_int8.tflite` |
| size | 1,258,000 bytes, TFLite (`TFL3` verified before writing) |
| Himax firmware region | `0x0` — **not touched** |
| Himax model region | **`0xA00000`** |
| chunk size | 256 bytes (SPI flasher) |

`model_flash.c` downloads it into PSRAM and streams it over SPI. Measured:
**written in 25 s, all 1,258,000 bytes, no errors.** Set `VISION_FLASH_MODEL` to
run it once, then clear it.

### What still fails

`sscma_client_invoke` returns `ESP_ERR_TIMEOUT`, and the log fills with
`request not found: <cmd>` — a reply arriving *after* the client gave up waiting
for it. Before the model write the stray replies were `NAME?`; after, they are
`AT`. So something changed, but replies are still systematically late.

This looks like protocol timing rather than a missing model. One strong
suspect: **the SPI sync line is on the I2C IO expander**
(`BSP_SSCMA_CLIENT_SPI_SYNC = IO_EXPANDER_PIN_NUM_6`, `sync_use_expander`
true), so every sync check costs an I2C transaction on a bus shared with the
audio codec and the touch panel. That is a lot of latency in a handshake that
appears to be timing sensitive.

Worth trying next, roughly in order:

1. `sscma_client_set_model(4)` — Seeed's own log calls `0xA00000` the *"4th ai
   model"*, while the SDK example (and our code) selects slot 1.
2. Raise `CONFIG_SSCMA_EVENT_QUEUE_SIZE` (currently 2) and the sscma task
   priorities, which are below the display and audio tasks.
3. Bind the device to SenseCraft once with the stock firmware, confirm the
   camera works at all, and watch the Himax console during a working session to
   see what a successful handshake looks like.

### Two corrections worth recording

**`slot_header invalid !!` is not about the model.** The line that follows it is
`slot flash_offset 0x00000000` — the bootloader checking the *firmware* slot at
offset 0. It appears identically before and after a successful model write, so
it is not evidence of a missing model, despite reading like it.

**The Himax was not "already running inference".** An earlier reading of
`rx buffer is full` as a result stream was wrong. Watching its own console shows
it **rebooting in a loop**, and the flood was repeated boot banners.

The Himax's console on the lower of the two USB serial ports (**921600 baud**)
is by far the best diagnostic here — it shows the chip's own side of the
conversation, independent of whatever the ESP32 believes.

## Roadmap

1. ~~Prove the toolchain; face on the display~~ **done**
2. **Port the Vapi voice client** from [`../vapi-atoms3r-voice`](../vapi-atoms3r-voice) —
   the BSP exposes `esp_codec_dev_handle_t` for mic and speaker, the same type
   that firmware builds on, so the transport and echo gate should move nearly
   verbatim. AEC must be re-measured on this board.
3. **Wire the face to call state** — the echo gate's write-ahead playout clock is
   already a sample-accurate "is the assistant audible" signal, which is exactly
   what drives `speaking` and the mouth.
4. **Add vision** — model flashed successfully; `invoke` still times out. See
   above. Once a model is loaded, presence detection already wakes a call, and
   `sscma_utils_fetch_image_from_reply()` gets a JPEG to a VLM for injection via
   Vapi `add-message`.
5. Custom art, then an enclosure — and re-measure the acoustics, because a plush
   changes them completely.
