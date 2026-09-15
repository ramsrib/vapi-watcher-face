# Debugging notes

Symptoms observed on this board and what causes them. Most of these present as
something other than their cause, which is the reason for the list.

## Build and flash

**`idf.py flash` fails on the first block, reads work fine.**
The CH342 bridge drops writes over 256 bytes. The ROM reports the missing bytes
as a CRC error, which reads as corruption. Baud rate is irrelevant — identical
at 115200 and 921600. Use `make flash`.
See [hardware.md](hardware.md#flashing-256-byte-writes-only).

**`E i2c: CONFLICT! driver_ng is not allowed to be used with this old driver`, before any of your code runs.**
Both I2C driver implementations are linked into the image. The check runs in a
global constructor, before `app_main`, so not calling the codec does not help.
Requires `esp_codec_dev` 1.3.x with `CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE=y`.

**"SDK not found" when the SDK is plainly there.**
A relative `WATCHER_SDK` is resolved against the build directory, not the
project. Pass an absolute path.

**`ninja` fails immediately after editing `partitions.csv`.**
Offsets come from `build/flash_args`, which is regenerated at configure time. A
stale build flashes the *old* table — which is how a writable `nvs` can land on
`0x9000` and destroy the device identity. Check `build/flash_args` before
flashing a partition change.

## NPU and camera

**Every SSCMA command times out; the log fills with `request not found: <cmd>` and `rx buffer is full`.**
`CONFIG_FREERTOS_HZ` is 100 (IDF's default) instead of 1000. Replies are
arriving after the 2000 ms request timeout has already removed the request.
See [hardware.md](hardware.md#required-sdkconfig).

**`sscma_client_set_model()` returns `ESP_FAIL`.**
The loaded model is in neither slot 1 (SDK examples) nor slot 4 (factory
firmware). Do not call it at all.

**Inference runs but never produces a detection.**
`sscma_client_set_sensor()` was not called, so nothing is feeding the model
frames.

**`slot_header invalid !!` at boot.**
Not an indication of a missing model. The factory device has Person Detection
loaded.

**A timeout on `get_model` looks like an absent model.**
It is the tick-rate problem above. Confirm against the Himax console at 921600
baud before concluding anything about the model.

**A flood of `rx buffer is full` looks like the NPU streaming inference.**
It can equally be the Himax rebooting in a loop and repeating its boot banner.
The Himax console distinguishes them.

**Frames are black.**
There is no exposure control in the AT command set, so this is physical: a
covered lens, a dark room, or the camera pointing somewhere dark. `make frame`
settles it in one step.

**Detections never fire even though inference is alive.**
A covered lens, a dark room, and a camera aimed above head height all produce
`hits: 0` identically. `make frame` distinguishes them; log lines do not.

**Presence never fires on the first person seen.**
If arrival is gated on a gap since the last sighting, `last_seen_ms` starting at
zero makes the first gap equal to uptime — which is *smaller* than the absence
threshold, not larger. The first sighting needs its own flag.

**The detection heartbeat reports zero while detection is working.**
Reporting the single frame that coincides with the log tick makes a working
detector look dead whenever the subject blinks out for one frame. Report the
maximum over the window.

## Calls

**Someone is detected but no call starts.**
The arrival callback is edge-triggered and cannot start a call before the
network is up. If WiFi association is slow, the arrival lands in that gap and is
lost, with no further edge while the person stays in frame. This is why
`vapi_presence_poll()` exists.

**A new call is created every couple of seconds.**
`vapi_call_is_active()` is false for the ~2 s that call setup is running, so
anything polling that question starts another call on every tick. A transition
has to be claimed before the worker is spawned.

**The second visitor gets no greeting and lands mid-conversation.**
Nothing ended the previous call when its visitor left. The presence poll will
not start a call while one is active.

**A visitor waits ~26 s to be greeted.**
A plain post-call cooldown makes the next arrival serve the previous visitor's
cooldown. Clear it when the frame goes empty.

**The spoken greeting is not the configured one, and arrives seconds late.**
A message injected before the assistant has spoken makes Vapi plan a reply
rather than play `firstMessage` verbatim. Inject nothing at connect.

**`VAPI_FIRST_MESSAGE` in `.env` silently overrides `VAPI_GREETING`.**
Leave it unset unless you mean it.

## Assistant behaviour

**The assistant insists it cannot see, while holding an accurate description.**
A saved assistant's system prompt outranks injected context. Use a transient
assistant whose prompt establishes that it has a camera.

**The assistant answers something nobody asked, or stalls with "hold it closer".**
`add-message` defaults to `triggerResponseEnabled: true`, so every injected
description prompts a reply. The assistant is answering the context update
rather than the person. Use `vapi_send_context()`.

**Descriptions arrive but are never used in speech.**
Two known causes. A mid-conversation `system` message is readily skimmed — try
`VLM_CONTEXT_ROLE` as `user`. And a system prompt that forbids saying "I can
see" can be read as forbidding visual commentary altogether; forbid announcing
the *capability*, not describing the scene.

**Descriptions only ever mention clothing.**
The vision prompt asks for clothing. Ask for a plain description of the frame
and let the system prompt decide what matters.

**Held objects are described vaguely or invented.**
At 416×416 a held object is a few dozen pixels. Use `VISION_SENSOR_OPT 3`.

## Audio

**The assistant interrupts itself / hears its own voice.**
There is no echo reference on this board, so AEC cannot work. Gate the
microphone instead.

**Raising or lowering mic gain does not change the echo.**
Gain scales echo and voice equally. Only speaker volume and cancellation change
the ratio.

**Cutting mic gain kills the echo and the talker together.**
The fix is a hard gate plus post-gate digital makeup gain, not lower gain.

**The gate opens mid-sentence and closes over silence.**
`av_render_get_audio_fifo_level()` returns 0 on this board, so it cannot be used
for delay compensation. Use the write-ahead playout clock in `vapi_media.c`.

## Memory

**`spi transmit (queue) failed` from `sscma_client` or the LCD, during call setup.**
Free internal heap is near zero and SPI DMA descriptors cannot be allocated.
The messages name whichever subsystem happened to need DMA next, not the cause.
Enable `CONFIG_MBEDTLS_DYNAMIC_BUFFER`.
See [architecture.md](architecture.md#memory).

**A single-TLS-session test shows no memory problem.**
The cost is concurrent sessions. Exercise a real call — `VAPI_SELFTEST_CALL`
does this without a person present.

## Serial

**The monitor shows nothing, or mojibake.**
Two ports enumerate. The ESP32 console is 115200; the Himax console is 921600.
Pass `PORT=` explicitly.

**A partially-written app partition bricks the boot.**
`esp_image: Checksum failed` / `OTA app partition slot 0 is not bootable` after
an interrupted flash. The bootloader falls back to the other OTA slot. Reflash.
