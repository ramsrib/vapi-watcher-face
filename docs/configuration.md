# Configuration

Two layers. `.env` holds credentials and is read at build time; everything else
lives in [`main/settings.h`](../main/settings.h) as a `#define` with its
rationale beside it.

`.env` values override the `menuconfig` equivalents. The header is regenerated
on every build, so editing `.env` and rebuilding is enough.

## `.env`

| key | required | notes |
|---|---|---|
| `WIFI_SSID` | yes | |
| `WIFI_PASSWORD` | yes | |
| `WIFI_SSID_2`, `WIFI_PASSWORD_2` | no | tried if the first does not answer |
| `WIFI_SSID_3`, `WIFI_PASSWORD_3` | no | tried after that |
| `VAPI_API_KEY` | yes | **private** key; the public key only authorizes `/call/web` |
| `VAPI_ASSISTANT_ID` | no | unused while `VAPI_TRANSIENT_ASSISTANT` is 1 |
| `VAPI_API_URL` | no | defaults to `https://api.vapi.ai` |
| `VAPI_FIRST_MESSAGE` | no | overrides `VAPI_GREETING`; leave unset |
| `VAPI_MAX_DURATION_SECONDS` | no | hard cap on one call |
| `ANTHROPIC_API_KEY` | no | camera descriptions; blank disables them |
| `OPENAI_API_KEY` | no | only if `VLM_PROVIDER_ANTHROPIC` is 0 |

Keys are compiled into the image and recoverable from a flash dump. A Vapi
private key is account-wide: it can create calls, read transcripts and spend
money. For anything beyond a demo, front it with a proxy that mints per-device
tokens.

## The assistant

| setting | default | notes |
|---|---|---|
| `VAPI_TRANSIENT_ASSISTANT` | `1` | define the assistant in the call body rather than referencing a saved one |
| `VAPI_LLM_PROVIDER` / `VAPI_LLM_MODEL` | `anthropic` / `claude-sonnet-5` | Vapi validates the model id and rejects unknown ones with a 400 listing the accepted set |
| `VAPI_VOICE_PROVIDER` / `VAPI_VOICE_ID` | `11labs` / `matilda` | presets: `burt marissa andrea sarah phillip steve joseph myra paula ryan drew paul mrb matilda mark`; any 11Labs voice id works if it is in your library |
| `VAPI_TRANSCRIBER_PROVIDER` / `_MODEL` | `deepgram` / `nova-3` | |
| `VAPI_SYSTEM_PROMPT` | — | the character; must be valid inside a JSON string literal |
| `VAPI_GREETING` | `"Oh, hi there!"` | spoken verbatim on connect |

A bad `voiceId` is accepted at call creation and fails at runtime as silence
rather than an error. Validate changes by creating a call against the API before
flashing.

## Vision

| setting | default | notes |
|---|---|---|
| `VLM_ENABLE` | `1` | off costs nothing: no frames retained, no task |
| `VLM_PROVIDER_ANTHROPIC` | `1` | `0` selects OpenAI |
| `VLM_MODEL` | `claude-sonnet-5` | `claude-opus-5` for better descriptions at higher latency |
| `VLM_PROMPT` | — | asks for a plain description; deliberately not task-specific |
| `VLM_MAX_TOKENS` | `120` | two short sentences |
| `VLM_REFRESH_MS` | `8000` | how often to look again during a call |
| `VLM_TIMEOUT_MS` | `8000` | one attempt, no retry |
| `VLM_SETTLE_MS` | `1200` | delay before the first request, to avoid a TLS pile-up |
| `VLM_FRAME_MAX_AGE_MS` | `30000` | refuse to describe a stale frame |
| `VLM_CONTEXT_ROLE` | `"user"` | role for injected descriptions |
| `VLM_SELFTEST` | `1` | caption one frame at boot and log it |

The two providers differ only in URL, auth header and body shape. Anthropic
takes the base64 JPEG verbatim in `source.data`; OpenAI needs it wrapped in a
`data:image/jpeg;base64,` URI and uses `max_completion_tokens` rather than
`max_tokens`.

## Detection

| setting | default | notes |
|---|---|---|
| `VISION_WAKE_ON_PRESENCE` | `1` | start a call when someone appears |
| `VISION_SENSOR_OPT` | `3` | 640×480; see [hardware.md](hardware.md#camera) |
| `VISION_KEEP_FRAMES` | `1` | retain frames for captioning |
| `VISION_DUMP_FRAME` | `1` | print one frame to the console; read it with `make frame` |
| `VISION_DUMP_WAIT_MS` | `25000` | how long to hold out for a frame containing a person |
| `VISION_QUERY_INFO` | `1` | ask the Himax to identify itself at startup |
| `VISION_FLASH_MODEL` | `0` | write a model to the Himax; not needed on a factory device |

`MIN_SCORE` (40), `ABSENCE_MS` (8000) and `PRESENCE_HOLD_MS` (2500) are in
[`main/vision.c`](../main/vision.c). Real detections score 50–87%. The console
prints `max detect gap` every 10 s, which is the figure `PRESENCE_HOLD_MS` has
to clear.

## Call lifecycle

| setting | default | notes |
|---|---|---|
| `HANGUP_AFTER_ABSENT_MS` | `10000` | end the call once nobody is detected |
| `WAKE_COOLDOWN_MS` | `20000` | after a call ends, before presence may start another; cleared on a genuine departure |
| `SLEEP_AFTER_MS` | `90000` | idle time before the face sleeps |
| `VAPI_SELFTEST_CALL` | `0` | place one call after boot with nobody present; bills a real call |

## Audio

| setting | default | notes |
|---|---|---|
| `DEFAULT_PLAYBACK_VOL` | `90` | limited by speaker distortion and current draw, not echo |
| `VAPI_MIC_GAIN_DB` | `24.0` | analog |
| `VAPI_MIC_DIGITAL_GAIN_DB` | `6.0` | post-gate makeup |
| `VAPI_ECHO_GATE` | `1` | |
| `VAPI_ECHO_GATE_ATTEN_DB` | `60.0` | 60 is a hard mute |
| `VAPI_ECHO_GATE_HANGOVER_MS` | `400` | |
| `VAPI_ECHO_GATE_THRESHOLD` | `300` | |
| `VAPI_SAMPLE_RATE` | `16000` | Vapi's websocket transport is 16 kHz mono s16le |
| `VAPI_FRAME_MS` | `20` | |

Raising mic gain does not improve the echo-to-voice ratio; it scales both
equally. See [architecture.md](architecture.md#audio).

## WiFi

| setting | default |
|---|---|
| `WIFI_ATTEMPTS_PER_NET` | `2` |
| `WIFI_RETRY_DELAY_MS` | `2000` |
