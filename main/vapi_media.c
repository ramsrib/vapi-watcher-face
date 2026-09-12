/* Audio pipeline — direct esp_codec_dev I/O plus the echo gate.
 *
 * See vapi_media.h for why this is so much thinner than the AtomS3R version.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sensecap-watcher.h"
#include "vapi_media.h"
#include "settings.h"

#define TAG "VAPI_MEDIA"

#define BYTES_PER_MS   ((VAPI_SAMPLE_RATE / 1000) * VAPI_CHANNELS * (VAPI_BITS_PER_SAMP / 8))

/* Playback jitter buffer. Vapi does not pace its websocket sends evenly —
 * measured p95 gap between chunks is ~51 ms against a 20 ms frame period — so a
 * shallow buffer underruns audibly on every burst. ~600 ms rides through that
 * without adding so much latency that the conversation feels laggy. */
#define PLAY_BUF_MS    600
#define PLAY_BUF_BYTES (PLAY_BUF_MS * BYTES_PER_MS)

/* How much the playback task moves per write. One frame keeps the playout
 * clock fine-grained, which the gate depends on. */
#define PLAY_CHUNK     (VAPI_FRAME_MS * BYTES_PER_MS)

typedef struct {
    esp_codec_dev_handle_t mic;
    esp_codec_dev_handle_t spk;

    StreamBufferHandle_t   play_buf;
    TaskHandle_t           play_task;
    volatile bool          playing;
    volatile bool          capturing;

    /* Write-ahead playout clock: wall-clock time at which everything handed to
     * the speaker so far will have finished being heard. */
    volatile uint32_t      playout_end_ms;
    volatile uint32_t      far_until_ms;

    volatile uint8_t       out_level;     /* 0-255, drives the mouth */

    int32_t                dgain_q8;      /* post-gate makeup, Q8 */
    int32_t                gate_q8;       /* -1 = off, 0 = hard mute */
    int                    volume;
} media_t;

static media_t m = {
    .dgain_q8 = 256,
    .gate_q8  = 0,
    .volume   = DEFAULT_PLAYBACK_VOL,
};

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t mean_abs(const int16_t *s, int count)
{
    if (count <= 0) {
        return 0;
    }
    uint64_t acc = 0;
    for (int i = 0; i < count; i++) {
        acc += (uint32_t)(s[i] < 0 ? -s[i] : s[i]);
    }
    return (uint32_t)(acc / count);
}

/* --- init ----------------------------------------------------------------- */

int vapi_media_init(void)
{
    if (bsp_codec_init() != ESP_OK) {
        ESP_LOGE(TAG, "bsp_codec_init failed");
        return -1;
    }
    m.mic = bsp_codec_microphone_get();
    m.spk = bsp_codec_speaker_get();
    if (m.mic == NULL || m.spk == NULL) {
        ESP_LOGE(TAG, "codec handles missing (mic=%p spk=%p)", m.mic, m.spk);
        return -1;
    }
    /* The BSP already defaults to 16 kHz / 16-bit / mono, which is exactly
     * Vapi's wire format — set it explicitly anyway so a BSP change cannot
     * silently alter the format underneath us. */
    bsp_codec_set_fs(VAPI_SAMPLE_RATE, VAPI_BITS_PER_SAMP, VAPI_CHANNELS);
    bsp_codec_volume_set(m.volume, NULL);
    vapi_media_set_mic_gain(VAPI_MIC_GAIN_DB);
    vapi_media_set_digital_gain(VAPI_MIC_DIGITAL_GAIN_DB);
    vapi_media_set_echo_gate(VAPI_ECHO_GATE_ATTEN_DB);
    ESP_LOGI(TAG, "codecs up: %d Hz / %d ch / %d-bit",
             VAPI_SAMPLE_RATE, VAPI_CHANNELS, VAPI_BITS_PER_SAMP);
    return 0;
}

/* --- playback ------------------------------------------------------------- */

/* Drains the jitter buffer into the codec. esp_codec_dev_write() blocks until
 * I2S has taken the data, so this task is paced by the hardware at exactly
 * realtime — which is what makes the playout clock trustworthy. */
static void play_task(void *arg)
{
    uint8_t *chunk = malloc(PLAY_CHUNK);
    if (chunk == NULL) {
        ESP_LOGE(TAG, "OOM in playback task");
        m.play_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "playback task started");
    while (m.playing) {
        size_t n = xStreamBufferReceive(m.play_buf, chunk, PLAY_CHUNK, pdMS_TO_TICKS(50));
        if (n == 0) {
            /* Underrun or idle. Report silence so the mouth closes rather than
             * freezing mid-syllable. */
            m.out_level = 0;
            continue;
        }
        m.out_level = (uint8_t)(mean_abs((const int16_t *)chunk, n / 2) >> 5);
        esp_codec_dev_write(m.spk, chunk, n);
    }
    free(chunk);
    m.out_level = 0;
    ESP_LOGI(TAG, "playback task stopped");
    m.play_task = NULL;
    vTaskDelete(NULL);
}

int vapi_media_play_start(void)
{
    if (m.playing) {
        return 0;
    }
    if (m.play_buf == NULL) {
        m.play_buf = xStreamBufferCreate(PLAY_BUF_BYTES, PLAY_CHUNK);
        if (m.play_buf == NULL) {
            ESP_LOGE(TAG, "failed to allocate %d byte play buffer", PLAY_BUF_BYTES);
            return -1;
        }
    }
    xStreamBufferReset(m.play_buf);
    m.playout_end_ms = 0;
    m.far_until_ms = 0;
    m.playing = true;
    if (xTaskCreate(play_task, "vapi_play", 4096, NULL, 12, &m.play_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to start playback task");
        m.playing = false;
        return -1;
    }
    return 0;
}

int vapi_media_play_stop(void)
{
    if (!m.playing) {
        return 0;
    }
    m.playing = false;
    /* Let the task observe the flag and exit before anything else touches the
     * codec. It polls on a 50 ms timeout, so this is comfortably enough. */
    vTaskDelay(pdMS_TO_TICKS(120));
    if (m.play_buf) {
        xStreamBufferReset(m.play_buf);
    }
    m.playout_end_ms = 0;
    m.far_until_ms = 0;
    m.out_level = 0;
    return 0;
}

int vapi_media_write_frame(const uint8_t *data, int size)
{
    if (!m.playing || size <= 0) {
        return 0;
    }

    /* Advance the playout clock. Everything already queued must drain before
     * this frame is heard, so it becomes audible at `start` and ends one frame
     * later. If the clock has fallen behind wall time the buffer had emptied,
     * so restart it from now. */
    uint32_t t = now_ms();
    uint32_t frame_ms = (uint32_t)(size / BYTES_PER_MS);
    uint32_t start = ((int32_t)(m.playout_end_ms - t) > 0) ? m.playout_end_ms : t;
    m.playout_end_ms = start + frame_ms;

    if (m.gate_q8 >= 0 &&
        mean_abs((const int16_t *)data, size / 2) > VAPI_ECHO_GATE_THRESHOLD) {
        /* Hold the mic shut until this frame has finished playing, plus a tail
         * for the I2S DMA depth and the room. */
        uint32_t until = m.playout_end_ms + VAPI_ECHO_GATE_HANGOVER_MS;
        if ((int32_t)(until - m.far_until_ms) > 0) {
            m.far_until_ms = until;
        }
    }

    size_t sent = xStreamBufferSend(m.play_buf, data, size, 0);
    if (sent < (size_t)size) {
        /* Buffer full: the speaker is not keeping up, which should not happen
         * since Vapi streams at 1x. Dropping beats blocking the websocket task,
         * which would also stall control messages and the hangup path. */
        ESP_LOGW(TAG, "play buffer full, dropped %d of %d bytes", (int)(size - sent), size);
    }
    return (int)sent;
}

/* --- capture -------------------------------------------------------------- */

int vapi_media_capture_start(void)
{
    m.capturing = true;
    ESP_LOGI(TAG, "capture started");
    return 0;
}

int vapi_media_capture_stop(void)
{
    m.capturing = false;
    ESP_LOGI(TAG, "capture stopped");
    return 0;
}

static void apply_digital_gain(int16_t *s, int count)
{
    if (m.dgain_q8 == 256) {
        return;
    }
    for (int i = 0; i < count; i++) {
        int32_t v = ((int32_t)s[i] * m.dgain_q8) >> 8;
        if (v > 32767)  v = 32767;
        if (v < -32768) v = -32768;
        s[i] = (int16_t)v;
    }
}

int vapi_media_read_frame(uint8_t *buf, int buf_size)
{
    if (!m.capturing || m.mic == NULL) {
        return 0;
    }
    int want = buf_size < PLAY_CHUNK ? buf_size : PLAY_CHUNK;
    /* Blocking, paced by I2S at realtime — this is what sets the send task's
     * cadence, so it needs no timer of its own. */
    if (esp_codec_dev_read(m.mic, buf, want) != ESP_CODEC_DEV_OK) {
        return 0;
    }

    int16_t *s = (int16_t *)buf;
    int count = want / 2;

    apply_digital_gain(s, count);

    /* Gate last, so the makeup gain cannot undo it. */
    if (m.gate_q8 >= 0 && (int32_t)(m.far_until_ms - now_ms()) > 0) {
        if (m.gate_q8 == 0) {
            memset(buf, 0, want);
        } else {
            for (int i = 0; i < count; i++) {
                s[i] = (int16_t)(((int32_t)s[i] * m.gate_q8) >> 8);
            }
        }
    }
    return want;
}

/* --- tuning --------------------------------------------------------------- */

int vapi_media_set_mic_gain(float db)
{
    if (m.mic == NULL) {
        return -1;
    }
    int ret = esp_codec_dev_set_in_gain(m.mic, db);
    ESP_LOGI(TAG, "mic gain: %.1f dB%s", db, ret == 0 ? "" : " (FAILED)");
    return ret == 0 ? 0 : -1;
}

int vapi_media_set_digital_gain(float db)
{
    if (db < 0.0f || db > 40.0f) {
        return -1;
    }
    m.dgain_q8 = (int32_t)(powf(10.0f, db / 20.0f) * 256.0f + 0.5f);
    ESP_LOGI(TAG, "digital makeup gain: %.1f dB", db);
    return 0;
}

int vapi_media_set_echo_gate(float atten_db)
{
    if (atten_db <= 0.0f) {
        m.gate_q8 = -1;
        ESP_LOGI(TAG, "echo gate: off (barge-in possible, echo likely)");
    } else if (atten_db >= 60.0f) {
        m.gate_q8 = 0;
        ESP_LOGI(TAG, "echo gate: hard mute while the assistant is audible");
    } else {
        m.gate_q8 = (int32_t)(powf(10.0f, -atten_db / 20.0f) * 256.0f + 0.5f);
        if (m.gate_q8 < 1) {
            m.gate_q8 = 1;
        }
        ESP_LOGI(TAG, "echo gate: -%.0f dB", atten_db);
    }
    return 0;
}

int vapi_media_set_volume(int vol)
{
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    m.volume = vol;
    bsp_codec_volume_set(vol, NULL);
    ESP_LOGI(TAG, "volume: %d", vol);
    return 0;
}

uint8_t vapi_media_get_output_level(void)
{
    return m.out_level;
}

bool vapi_media_far_end_active(void)
{
    return m.gate_q8 >= 0 && (int32_t)(m.far_until_ms - now_ms()) > 0;
}
