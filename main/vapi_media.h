/* Audio pipeline for the Vapi Watcher Face.
 *
 * Deliberately much thinner than the AtomS3R sibling's version. That board ran
 * mic audio through esp_capture's AEC source and played through av_render,
 * because a single ES8311 served both directions and could loop the DAC back
 * into the ADC as an echo reference.
 *
 * This board has *two* codecs — ES8311 for the speaker, ES7243/ES7243E for the
 * microphone — so no such loopback exists and the AEC has nothing to cancel
 * against. That is fine: on the AtomS3R the AEC turned out to contribute almost
 * nothing anyway, and echo was actually solved by hard-muting the microphone
 * while the assistant is audible. That technique needs no reference signal, so
 * the whole esp_capture / av_render stack drops out and we talk to
 * esp_codec_dev directly.
 *
 * It also makes the gate *more* accurate here. On the AtomS3R the playout clock
 * had to estimate av_render's buffering (via an API that turned out to return
 * 0). Here we own every write to the codec, so we know exactly how much audio
 * is queued and therefore exactly when it will be heard.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Bring up both codecs at Vapi's format. Call once, after the BSP is up. */
int vapi_media_init(void);

/** Start capturing from the microphone. */
int vapi_media_capture_start(void);

/** Stop capturing. */
int vapi_media_capture_stop(void);

/**
 * @brief  Pull one PCM frame from the microphone.
 *
 * Returns bytes written into @p buf, 0 if nothing is ready, negative on error.
 * Gain and the echo gate are applied here, in that order.
 */
int vapi_media_read_frame(uint8_t *buf, int buf_size);

/** Open the speaker path and start the playback task. */
int vapi_media_play_start(void);

/** Stop playback and drop anything still queued. */
int vapi_media_play_stop(void);

/** Queue inbound PCM (16 kHz mono s16le) for playback. Non-blocking. */
int vapi_media_write_frame(const uint8_t *data, int size);

/* --- tuning, all live-adjustable from the console ------------------------- */

/** Microphone gain in dB. Lower values reduce how much echo reaches the mic. */
int vapi_media_set_mic_gain(float db);

/** Digital makeup gain applied after the gate, in dB (0..40). */
int vapi_media_set_digital_gain(float db);

/**
 * @brief  Mic attenuation while the assistant is audible, in dB (0 = off).
 *
 * 60 or above is a hard mute, which is what actually works. Costs barge-in.
 */
int vapi_media_set_echo_gate(float atten_db);

/** Speaker volume, 0-100. */
int vapi_media_set_volume(int vol);

/**
 * @brief  Current playback level, 0-255, for driving the mouth.
 *
 * Derived from the audio actually being written to the speaker, so it is in
 * sync with what is heard rather than with what has merely arrived.
 */
uint8_t vapi_media_get_output_level(void);

/** True while the assistant's audio is still coming out of the speaker. */
bool vapi_media_far_end_active(void);

#ifdef __cplusplus
}
#endif
