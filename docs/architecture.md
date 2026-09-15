# Architecture

How the firmware is put together, and the constraints that shaped it.

## Overview

```
                    ┌──────────────── ESP32-S3 ────────────────┐
  Himax NPU ──SPI──▶│ vision.c    presence + frame retention   │
   (person?)        │    │                                     │
                    │    ├──▶ vapi_app.c   call lifecycle      │
                    │    │         │                           │
                    │    │         ├──▶ vapi_client.c ──TLS──▶ Vapi
                    │    │         │      websocket, PCM both ways
                    │    │         │                           │
                    │    │         └──▶ face.c   LVGL face     │
                    │    │                                     │
                    │    └──▶ vlm.c ──TLS──▶ vision model      │
                    └──────────────────────────────────────────┘
```

Five independent tasks: LVGL rendering, audio capture, audio playback, NPU
inference, and the vision captioner. Nothing blocks anything else.

## The face

Drawn with LVGL primitives rather than bitmaps, so expressions are computed
rather than stored: eye height, pupil offset, mouth curve and a glow ring are
all parameters. Nine states — `BOOT`, `STANDBY`, `DETECTING`, `GREETING`,
`LISTENING`, `THINKING`, `SPEAKING`, `SLEEPING`, `CONFUSED`.

The tick runs at 20 Hz. Blinks are randomised; a face that blinks on a fixed
schedule reads as mechanical.

Mouth movement is driven by `face_set_mouth_level()` from the live output
amplitude, so it tracks what is actually reaching the speaker rather than what
arrived from the network.

Palette: mint `#88F1B8` on near-black `#0E0E12`, with tokens in
[`main/palette.h`](../main/palette.h) chosen to survive RGB565 quantization.

## Audio

Capture and playback go directly through `esp_codec_dev`. `esp_capture` and
`av_render` are not used — they add a pipeline this firmware does not need and
their FIFO level reporting is unreliable on this board.

### Echo gating, not cancellation

The Watcher has two separate codecs and no loopback of the playback signal into
the capture path, so there is no reference signal and AEC cannot work. See
[hardware.md](hardware.md#audio-two-codecs-no-echo-reference).

The microphone is instead hard-muted whenever the assistant is audible. This
removes echo completely and removes the need to handle interruptions at all.

Gain staging does not help with echo: analog and digital gain scale the echo and
the talker equally, leaving the ratio unchanged. Only speaker volume and
cancellation change that ratio.

### The playout clock

Gating requires knowing when the far end is *audible*, which is later than when
its audio arrived. The FIFO level API returns zero on this board, so timing
comes from a write-ahead clock maintained in `vapi_media_write_frame()`:

```c
uint32_t start = ((int32_t)(m.playout_end_ms - t) > 0) ? m.playout_end_ms : t;
m.playout_end_ms = start + frame_ms;
```

Each frame extends a running estimate of when playback will finish. The gate
holds the microphone shut until that moment plus a hangover period.

The same signal is a sample-accurate "is the assistant speaking right now",
which is exactly what the mouth animation needs — so lip-sync falls out of the
echo fix rather than being built separately.

## Presence and the call lifecycle

The NPU runs continuously at ~10 inferences per second. A detection above
`MIN_SCORE` marks a person present; presence decays after `PRESENCE_HOLD_MS`
without one.

Two mechanisms start a call, deliberately:

- **An arrival callback**, which fires the moment someone appears. This makes
  the greeting feel immediate.
- **`vapi_presence_poll()`**, ticking every 2 s from the main loop, which asks
  whether a call *should* be running.

The callback alone is not sufficient. It is edge-triggered — it fires once when
someone appears and not again while they stay — and it cannot start a call
before the network is up. On a boot where WiFi association takes a few extra
seconds, the arrival lands in that gap and is lost, with no further edge coming
while the person remains in frame. The poll cannot lose an event because it does
not look at events.

A call transition is claimed under a critical section before the worker task is
spawned. Call setup takes ~2 s of HTTPS POST and TLS handshake, during which the
call is being opened but `vapi_call_is_active()` is still false; without the
claim, the 2 s poll starts another call on every tick.

### Ending

`HANGUP_AFTER_ABSENT_MS` ends the call once nobody has been detected for ten
seconds, waiting for the assistant to stop speaking so a goodbye is not cut off.

Nothing else ends a call on departure, and the consequence is specific: the poll
will not start a call while one is active, so without this the next person to
walk up gets no greeting and is dropped into the previous conversation,
mid-context.

`WAKE_COOLDOWN_MS` then prevents re-greeting whoever is still standing there
when a call ends. It is cleared as soon as the frame is empty, so a genuine
departure does not make the next visitor wait out a cooldown that was not theirs.

## Vision

### Why captioning is a separate API call

Vapi's `add-message` carries an `OpenAIMessage`, whose `content` is typed as a
plain string rather than a multimodal array. An image cannot be passed through
to the assistant's own model even when that model is multimodal. Anything visual
must therefore be turned into text first, by a separate request, and injected.

### Captioning never blocks the greeting

Detection starts the call immediately; the greeting plays from `firstMessage`
while `caption_task` runs in parallel and injects a description a second or two
later.

This makes provider latency largely irrelevant — nothing waits on it — and makes
failure invisible: without a description the assistant simply never mentions
appearance.

Measured: ~2.9 s for a caption on-device (~0.9 s of that is the model; the rest
is TLS and upload), against an 8 s timeout. One attempt, no retry. A late
description is worse than none, because the assistant would comment on someone
who has already left.

### Continuous, not one-shot

A single frame per call is a photograph rather than sight, and cannot answer
"what am I holding?". `caption_task` re-captions every `VLM_REFRESH_MS` (8 s)
for the length of the call.

Only changed descriptions are sent. Repeating an identical one fills the context
and implies the camera reports the same thing regardless of what happens.

Cost is bounded by the NPU: frames are retained only when a detection cleared
`MIN_SCORE`, at most once per second, into one grown-and-reused PSRAM buffer.
Roughly seven requests for a minute-long conversation.

There is no encode step. SSCMA delivers the JPEG as base64 text, which is the
form both vision APIs want, so the string goes from the SPI reply into the
request body untouched.

### Injection must be silent

`add-message` carries `triggerResponseEnabled`, which **defaults to `true`**. A
message sent without it does not merely join the conversation history — it
prompts the assistant to speak.

For descriptions arriving every eight seconds that is wrong in a way that looks
like a model problem: the assistant answers the context update rather than the
person. `vapi_send_context()` sets it to `false`; `vapi_send_text()` keeps the
triggering behaviour for anything that should prompt speech.

The message role is `VLM_CONTEXT_ROLE`, defaulting to `user`. `system` is the
honest label, but a mid-conversation system message is the one an LLM most
readily skims, and the failure is subtle — a plausible reply that never uses
what it was given.

### Division of responsibility

The vision prompt asks for a plain description of the frame and nothing else. It
is not told what to look for. The assistant's own system prompt decides what is
worth remarking on.

Task-specific instructions belong in the system prompt, where they can see the
conversation, rather than in the vision prompt, where they cannot. A vision
prompt that asks for "clothing, colours, hair" returns exactly that, however
interesting the object in shot.

## The assistant

Defined inline in the `POST /call` body as a transient assistant — prompt,
model, voice and transcriber — rather than referenced by `assistantId`.

A saved assistant's system prompt outranks anything injected mid-call. Pointed
at an unrelated saved assistant, this device delivers an accurate description of
the person in front of it and the model replies that it cannot see. The persona
has to know it has a camera, which means shipping it with the firmware.

Nothing is injected at connect. A message arriving before the assistant has
spoken makes Vapi plan a reply instead of playing `firstMessage` verbatim,
which costs several seconds of silence where a fixed line would be immediate.

## Memory

Internal RAM is the scarce resource; TLS is what spends it. A call holds three
sessions across setup — `POST /call`, the websocket, and the caption request.

By default mbedTLS keeps the full IN/OUT content buffers for the life of each
session, so an idle websocket holds ~20 KB it is not using.
`CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` allocates them per record instead.

| | without | with |
|---|---|---|
| min free internal heap, one TLS session | 99,406 | 99,398 |
| min free internal heap, during a call | 7,478 | 59,770 |
| SPI transactions failed during call setup | 100 | 0 |

The win is entirely in concurrency, which is why a single-session test shows
nothing.

`VLM_SETTLE_MS` additionally delays the caption handshake 1.2 s so it does not
coincide with the websocket's session and the audio buffers being allocated.

At 7 KB free the failures do not appear in this code. They appear as SPI DMA
descriptors failing to allocate elsewhere — a glitching display and dropped
Himax transactions. On this board, memory pressure is reported by whichever
unrelated subsystem next needs DMA.

## WiFi

`WIFI_SSID`, `WIFI_SSID_2` and `WIFI_SSID_3` are tried in order. Blank entries
are dropped at startup.

Each network gets `WIFI_ATTEMPTS_PER_NET` (3) attempts before the next is tried.
Association on a normal boot routinely fails twice before succeeding, so a lower
budget is consumed by healthy behaviour and switches networks needlessly.
A dropped association and an absent AP are indistinguishable from the device's
side and want opposite responses — the first usually reconnects immediately, the
second never will. One retry serves the transient without stranding the device
on a network that is not there.

On success the index stays put, so a network that just worked is retried first
if it drops.

A full cycle of three networks takes ~12 s, which is longer than a visitor will
wait, so order matters.

## Self-tests

Both are off or cheap by default and exist because the riskiest paths are the
hardest to exercise deliberately.

`VLM_SELFTEST` captions one frame after WiFi comes up and logs the result,
exercising PSRAM allocation, TLS, the request body and the response parse. A bad
key or a rejected body shape fails on the console ten seconds after boot rather
than mid-conversation.

`VAPI_SELFTEST_CALL` places one call after boot with nobody present. The call
path is the tightest moment for memory and normally cannot be reached without a
person in front of the camera. Off by default — it places a real, billable call
on every boot.
