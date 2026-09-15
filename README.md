# Vapi Watcher Face

An animated face on a **SenseCAP Watcher** that notices people, sees what they
look like, and talks to them through [Vapi](https://vapi.ai). Designed to sit
inside a soft toy with the round display as its face.

Nobody presses anything. The on-board NPU detects a person, the device places a
call by itself, and a vision model describes the camera frame — so it can say
"nice tennis ball" instead of "hello, user".

```
Himax NPU ──person?──▶ ESP32-S3 ──POST /call──▶ Vapi ──websocket──▶ PCM audio
    │                      │                      ▲
    └──JPEG──▶ vision model ──description─────────┘   (add-message, silent)
```

Runs on ESP-IDF 5.5.1. Around 3,000 lines of C.

## Hardware

- **SenseCAP Watcher W1-A** (Seeed Studio) — ESP32-S3 + Himax WiseEye2 NPU,
  round 412×412 touch display, camera, microphone, speaker
- A USB-C cable and a powered port or hub

## Accounts and keys

| | required | what for |
|---|---|---|
| [Vapi](https://vapi.ai) private API key | yes | the conversation |
| [Anthropic](https://console.anthropic.com/settings/keys) API key | no | describing what the camera sees |

Without a vision key the device still detects people and holds a conversation;
it just never mentions what they look like. OpenAI can be used instead — see
[docs/configuration.md](docs/configuration.md).

## Setup

Install [ESP-IDF 5.5.1 or newer](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/get-started/),
then:

```bash
git clone https://github.com/ramsrib/vapi-watcher-face.git
cd vapi-watcher-face

make deps        # clone Seeed's SDK into deps/ (one-time, large)
make setup       # create .env from the template
$EDITOR .env     # fill in WiFi and keys
make run         # build, flash, and open the monitor
```

`make help` lists every target.

### Filling in `.env`

```bash
WIFI_SSID="..."
WIFI_PASSWORD="..."

# Vapi PRIVATE key — the public key only authorizes /call/web, which returns a
# Daily room this device cannot join.
VAPI_API_KEY="..."

# Optional: camera descriptions.
ANTHROPIC_API_KEY="sk-ant-..."
```

Backup WiFi networks (`WIFI_SSID_2`, `WIFI_SSID_3`) are tried in order when the
first does not answer. Everything else has a working default; the full list is
in [`.env.example`](.env.example) and
[docs/configuration.md](docs/configuration.md).

> Keys are compiled into the image and recoverable from a flash dump. Use keys
> you are willing to rotate, and put a proxy in front for anything real.

## Two things that will bite you

**Do not run `idf.py flash`.** The Watcher's CH342 USB-serial bridge drops bytes
on writes over 256 bytes. `make flash` uses `tools/flash.py`, which caps the
block size. Details in [docs/hardware.md](docs/hardware.md).

**Do not erase the flash.** `idf.py erase-flash` destroys the `nvsfactory`
partition at `0x9000`, which holds the device's provisioned identity. It is
written at the factory and cannot be regenerated. There is deliberately no
`make erase`.

## Using it

Walk into the camera's view. The device greets you, describes what it sees, and
hangs up about ten seconds after you leave. The knob starts and stops a call by
hand; holding it mutes the microphone.

Useful commands:

```bash
make monitor     # serial console
make frame       # save what the camera sees as frame.jpg
```

`make frame` is the fastest way to check aim and lighting. A covered lens, a
dark room and a camera pointed at the ceiling all look identical in the logs.

## Layout

```
main/
  main.c            boot sequence, knob button
  face.c/.h         the face: LVGL drawing + expression states
  palette.h         colour tokens, RGB565-verified
  vapi_client.c     call creation and the websocket transport
  vapi_media.c/.h   audio: codec I/O, gain staging, echo gate
  vapi_app.c        call state → expression, presence → call lifecycle
  vision.c/.h       Himax NPU: inference, presence, frame capture
  vlm.c/.h          frame → description, via Anthropic or OpenAI
  wifi.c            station bring-up with failover
  settings.h        every tuning knob, with its rationale
tools/
  flash.py          CH342-safe flasher
  grab_frame.py     pull the camera frame off the console
  gen_env_header.py .env → compile-time defines
```

## Documentation

| | |
|---|---|
| [docs/architecture.md](docs/architecture.md) | how the pieces fit: audio, vision, the call lifecycle |
| [docs/hardware.md](docs/hardware.md) | SenseCAP Watcher specifics: flashing, required config, partitions |
| [docs/configuration.md](docs/configuration.md) | every setting and what it does |
| [docs/debugging-notes.md](docs/debugging-notes.md) | symptoms seen on this board and what causes them |

## Status

Working on hardware: it notices people, greets them, describes clothing and
held objects, and hangs up when they leave.

Known limitations:

- Mounted flat the camera looks past people; it needs angling towards face
  height.
- `PRESENCE_HOLD_MS` (2500 ms) is close to the measured worst-case detection
  gap of ~2000 ms.
- `ledc: GPIO 8 is not usable` at boot — backlight brightness control is inert.

## Related

[vapi-atoms3r-voice](https://github.com/ramsrib/vapi-atoms3r-voice) — the same
Vapi transport and echo-gating approach on a $20 M5Stack AtomS3R, without the
camera or display.

## Licence

MIT — see [LICENSE](LICENSE).

`components/esp_codec_dev/` is Espressif's, vendored unmodified at 1.3.6 under
its own Apache-2.0 licence. It is vendored rather than pulled from the registry
because of the I2C driver conflict described in
[docs/hardware.md](docs/hardware.md).
