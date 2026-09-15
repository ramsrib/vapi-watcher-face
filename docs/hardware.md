# SenseCAP Watcher hardware notes

Board-specific constraints that affect any firmware written for the Watcher,
independent of this project. Everything here was measured on a W1-A.

## Processors

| | |
|---|---|
| ESP32-S3 | application, 8 MB PSRAM, 32 MB flash |
| Himax WiseEye2 HX6538 | AI NPU, own 16 MB flash, own firmware (build 2024.08.16) |

They communicate over SPI2 using Seeed's `sscma_client` component, which speaks
the SSCMA protocol: `AT+...` commands out, newline-delimited JSON replies back,
in 256-byte packets.

## Two USB serial endpoints

Plugging the Watcher in enumerates two ports:

| port | speed | carries |
|---|---|---|
| ESP32 console | 115200 | application log |
| Himax console | 921600 | the NPU's own boot and runtime output |

Which name maps to which is not predictable. Picking the wrong one produces
silence or mojibake rather than an error, so pass `PORT=` explicitly when
auto-detection guesses wrong.

The Himax console is the most useful diagnostic on the board: it shows the NPU's
side of any exchange, independent of what the ESP32 believes is happening.

## Flashing: 256-byte writes only

The CH342 USB-serial bridge silently drops bytes when the host writes more than
256 at a time. esptool's defaults are far larger — `0x1800` for RAM, `0x4000`
for stub flash writes — so every write fails on its first block while reads,
which are small device-to-host replies, work perfectly. The ROM reports the
missing bytes as a CRC error, which reads as corruption rather than overflow.

| write block | result |
|---|---|
| 256 bytes | works |
| 512 bytes and above | fails on the first block |

`tools/flash.py` caps the block size on every loader class and passes
`--no-compress`. Capping only the base class is not enough:
`ESP32S3StubLoader.FLASH_WRITE_SIZE` overrides it.

Baud rate is not the variable. The failure is identical at 115200 and 921600.

## Never erase the flash

`nvsfactory` at `0x9000` holds the device's provisioned identity — its EUI and
SenseCraft binding credentials, written at the factory and not regenerable.

`idf.py erase-flash` destroys it. So does any partition table that puts a
writable `nvs` at `0x9000` — which the SDK example partition tables do.

`partitions.csv` here mirrors the stock layout exactly:

```
nvsfactory,   data, nvs,     0x9000,    0x32000,
nvs,          data, nvs,     0x3b000,   0xd2000,
otadata,      data, ota,     0x10d000,  0x2000,
phy_init,     data, phy,     0x10f000,  0x1000,
ota_0,        app,  ota_0,   0x110000,  0xc00000,
ota_1,        app,  ota_1,   0xd10000,  0xc00000,
model,        data, spiffs,  0x1910000, 0x100000,
storage,      data, spiffs,  0x1a10000, 0x5f0000,
```

Take a full backup before flashing anything. Verify it by reading twice and
comparing, not by trusting esptool's exit code. Flash encryption and secure boot
are both disabled, so images are plain and restorable.

## Required sdkconfig

Three settings are not optional on this board.

```
CONFIG_FREERTOS_HZ=1000
CONFIG_SSCMA_PROCESS_TASK_STACK_SIZE=10240
CONFIG_SSCMA_PROCESS_TASK_AFFINITY_CPU1=y
CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE=y
```

**`CONFIG_FREERTOS_HZ` must be 1000.** IDF defaults to 100. At 100 Hz every tick
is 10 ms instead of 1 ms, so every delay in the SSCMA read path inflates
tenfold. Replies arrive in 256-byte packets each preceded by a sync check, so
the cost multiplies per packet and pushes replies past sscma's 2000 ms request
timeout. Every NPU command then fails with `request not found: <cmd>` — replies
arriving after the client gave up.

| | 100 Hz | 1000 Hz |
|---|---|---|
| `request not found` per 30 s | 112+ | 1 |
| `rx buffer is full` per 30 s | 50 | 0 |
| `sscma_client_invoke` | `ESP_ERR_TIMEOUT` | succeeds |

**`CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE` must be set,** and `esp_codec_dev` must
be 1.3.x. The BSP, `sscma_client` and the IO expander all use the legacy
`driver/i2c.h` API. `esp_codec_dev` 1.2.0 hard-selects the new
`driver/i2c_master.h` on IDF ≥ 5.3 with no opt-out, and IDF aborts if both
implementations are merely *linked* into the image:

```
E i2c: CONFLICT! driver_ng is not allowed to be used with this old driver
```

That check runs in a global constructor, before `app_main`, so avoiding the
calls does not help. 1.3.6 is vendored in `components/` via `override_path`
because the BSP manifest pins 1.2.0 exactly and the version solver would reject
the newer one.

## Audio: two codecs, no echo reference

Capture and playback are separate chips — an ES7243 ADC and an ES8311 DAC — on
separate I2S paths. There is no shared clock domain and no loopback of the
playback signal into the capture path, so there is no reference signal for
acoustic echo cancellation. On-device AEC cannot work here regardless of the
algorithm.

Echo has to be gated rather than cancelled. See
[architecture.md](architecture.md#audio).

## Camera

The sensor offers four modes, selected by `opt_id` in `sscma_client_set_sensor`:

| opt_id | resolution |
|---|---|
| 0 | 240×240 |
| 1 | 416×416 |
| 2 | 480×480 |
| 3 | 640×480 |

416×416 matches the detection model's input. Identifying small objects needs
more: at ~8 KB of JPEG a held object is a few dozen pixels, and a vision model
asked to name one will hedge or invent. This firmware uses 640×480.

Frames arrive inside the SSCMA JSON reply as base64 text, already encoded.

There is no exposure control in the AT command set — `AT+SENSOR` takes only
id/enable/opt_id — so brightness is the sensor's own business.

## NPU model

The device ships with **Person Detection** loaded from the factory (class 0:
person). It reports `slot_header invalid !!` at boot, which does not indicate a
missing model.

**Do not call `sscma_client_set_model()`.** The SDK example selects slot 1 and
the factory firmware selects slot 4; both return `ESP_FAIL` here, because the
loaded model is in neither. Omitting the call is what allows `invoke` to
succeed.

The correct bring-up order is:

```c
sscma_client_init()
sscma_client_break()            /* + ~300 ms */
sscma_client_set_sensor(client, 1, opt_id, true)
sscma_client_set_confidence_threshold(client, MIN_SCORE)
sscma_client_invoke(client, -1, false, true)
```

`sscma_client_set_sensor` is required. Without it inference runs and never
produces a detection, because nothing is feeding it frames.

`model_flash.c` writes a `.tflite` to the Himax's flash at `0xA00000` using the
same addresses and 256-byte chunks the stock firmware's OTA path uses. It is not
needed on a factory device and is off by default.
