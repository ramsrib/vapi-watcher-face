# Vapi Watcher Face

An animated face on a **SenseCAP Watcher** that sees you and talks back through
[Vapi](https://vapi.ai). Built to live inside a soft toy, with the round display
as its face.

> **Status:** face and voice work on hardware — press the knob, talk, it talks
> back, with no echo. Vision is written but blocked on the Himax having no model
> loaded. See [Roadmap](#roadmap).

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

## Vision: blocked on the Himax having no model

`vision.c` implements the NPU path — continuous inference, presence detection
with hysteresis, JPEG retrieval, and a callback that wakes a call when someone
arrives. It builds and runs. **The Himax will not accept any of it**, and the
evidence says the chip has no AI model loaded.

What happens, in order:

| step | result |
|---|---|
| `sscma_client_init` | ok, link works — the Himax's unsolicited `INIT@STAT?` arrives |
| `get_info` / `get_model` | **~20 s timeout each**, `request name failed` |
| `set_confidence_threshold` | `request set confidence failed` |
| `invoke` | `ESP_ERR_TIMEOUT`, `request invoke failed` |

Two things were learned diagnosing it, both worth keeping:

**The Himax boots already running an inference session** and streams results
continuously over SPI, flooding sscma_client's rx buffer (`rx buffer is full`
every ~550 ms) so replies to our commands never match. Calling
`sscma_client_break()` first quiets the link — warnings dropped from 50 to 5 in
a 30 s window.

**The display starves it.** Bringing LVGL up before the Himax handshake lets the
face's 20 Hz redraw delay the sscma task enough to make its replies arrive after
the client has given up. Initialising vision first cut retries from 197 to 33.

Neither fixed `invoke`, and the likely reason is simpler: **there is no model to
run.** The device's `model` flash partition was completely empty when dumped
(1 MB, zero bytes used), `get_model` reports nothing, and the stock firmware
only downloads a model from Seeed's CDN once a SenseCraft task is assigned —
which this device has never had.

Getting past this means putting a model on the Himax: either bind the device to
SenseCraft once with the stock firmware and let it fetch one, or flash a model
directly (`sscma_client_ota_*`, or the `we2` flashers in the BSP). Until then,
`vision_init()` fails cleanly on a background task and the face and voice are
unaffected.

## Roadmap

1. ~~Prove the toolchain; face on the display~~ **done**
2. **Port the Vapi voice client** from [`../vapi-atoms3r-voice`](../vapi-atoms3r-voice) —
   the BSP exposes `esp_codec_dev_handle_t` for mic and speaker, the same type
   that firmware builds on, so the transport and echo gate should move nearly
   verbatim. AEC must be re-measured on this board.
3. **Wire the face to call state** — the echo gate's write-ahead playout clock is
   already a sample-accurate "is the assistant audible" signal, which is exactly
   what drives `speaking` and the mouth.
4. **Add vision** — code is written; blocked on the Himax having no model. See
   above. Once a model is loaded, presence detection already wakes a call, and
   `sscma_utils_fetch_image_from_reply()` gets a JPEG to a VLM for injection via
   Vapi `add-message`.
5. Custom art, then an enclosure — and re-measure the acoustics, because a plush
   changes them completely.
