/* Vision — Himax WiseEye2 via SSCMA. See vision.h. */

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sensecap-watcher.h"
#include "sscma_client_io.h"
#include "sscma_client_ops.h"
#include "settings.h"
#include "vision.h"
#include "model_flash.h"

#define TAG "VISION"

/* A detection has to clear this to count. The default model reports plenty of
 * low-confidence boxes on empty frames; without a floor the device would wake
 * up at shadows. */
/* Detection confidence floor, 0-100.
 *
 * Not yet tuned against real detections — the camera was returning black
 * frames when this was last exercised, so no box has ever cleared it. Treat as
 * a starting point, not a measurement. */
#define MIN_SCORE 40

/* How long a person must be absent before the next sighting counts as an
 * arrival rather than a continuation. Without this, one person shifting in
 * their seat would re-trigger the greeting repeatedly. */
#define ABSENCE_MS 8000

/* Detections are sparse and jittery — a person is routinely missed for a frame
 * or two. Treat them as present until this long after the last sighting, so the
 * presence signal does not flicker. */
#define PRESENCE_HOLD_MS 2500

typedef struct {
    sscma_client_handle_t client;
    bool                  up;
    volatile bool         present;
    volatile bool         seen_ever;
    volatile int          score;
    volatile uint32_t     last_seen_ms;
    volatile uint32_t     last_absent_ms;
    vision_presence_cb_t  on_presence;

    SemaphoreHandle_t     frame_lock;
    uint8_t              *frame;
    int                   frame_size;

    sscma_client_model_t *model;
} vision_t;

static vision_t v;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

bool vision_available(void)      { return v.up; }
int  vision_confidence(void)     { return v.score; }

bool vision_person_present(void)
{
    if (!v.present) {
        return false;
    }
    return (now_ms() - v.last_seen_ms) < PRESENCE_HOLD_MS;
}

const char *vision_class_name(int target)
{
    if (v.model == NULL || target < 0) {
        return NULL;
    }
    for (int i = 0; v.model->classes[i] != NULL; i++) {
        if (i == target) {
            return v.model->classes[i];
        }
    }
    return NULL;
}

uint8_t *vision_take_frame(int *out_size)
{
    uint8_t *copy = NULL;
    xSemaphoreTake(v.frame_lock, portMAX_DELAY);
    if (v.frame && v.frame_size > 0) {
        copy = malloc(v.frame_size);
        if (copy) {
            memcpy(copy, v.frame, v.frame_size);
            *out_size = v.frame_size;
        }
    }
    xSemaphoreGive(v.frame_lock);
    return copy;
}

/* Fires for every inference result the Himax produces. Runs on the sscma
 * client's own task, so it must stay cheap and must not block — anything
 * expensive (a network call, say) belongs on a worker. */
static void on_event(sscma_client_handle_t client,
                     const sscma_client_reply_t *reply, void *ctx)
{
    (void)client; (void)ctx;

#if VISION_KEEP_FRAMES
    /* Retain the JPEG so a VLM can be shown what the device saw. Costs PSRAM
     * per frame, which is why it is behind a switch. */
    char *img = NULL;
    int img_size = 0;
    if (sscma_utils_fetch_image_from_reply(reply, &img, &img_size) == ESP_OK) {
        xSemaphoreTake(v.frame_lock, portMAX_DELAY);
        free(v.frame);
        v.frame = malloc(img_size);
        if (v.frame) {
            memcpy(v.frame, img, img_size);
            v.frame_size = img_size;
        } else {
            v.frame_size = 0;
        }
        xSemaphoreGive(v.frame_lock);
        free(img);
    }
#endif

    sscma_client_box_t *boxes = NULL;
    int count = 0;
    int best = 0;
    if (sscma_utils_fetch_boxes_from_reply(reply, &boxes, &count) == ESP_OK) {
        for (int i = 0; i < count; i++) {
            if (boxes[i].score > best) {
                best = boxes[i].score;
            }
        }
        free(boxes);
    }

    /* Rate-limited heartbeat. Worth keeping: "no detections" is otherwise
     * ambiguous between no events arriving, events with no boxes, and boxes
     * below threshold — three very different faults. At 10 s it is cheap. */
    static uint32_t last_log, events, win_hits;
    static int win_best;
    events++;
    if (count > 0)   win_hits++;
    if (best > win_best) win_best = best;
    uint32_t t = now_ms();
    if ((uint32_t)(t - last_log) > 10000) {
        last_log = t;
        /* Max over the window, not the instantaneous frame. Reporting the frame
         * that happened to coincide with the log makes a working detector look
         * dead whenever the subject blinks out for one frame. */
        ESP_LOGI(TAG, "inference alive: %lu events/10s, frames with a box: %lu, best score: %d (threshold %d)",
                 (unsigned long)events, (unsigned long)win_hits, win_best, MIN_SCORE);
        events = 0; win_hits = 0; win_best = 0;
    }

    v.score = best;

    if (best >= MIN_SCORE) {
        /* The very first sighting is always an arrival.
         *
         * This needs its own flag rather than leaning on the gap. `last_seen_ms`
         * starts at 0, so at boot the gap is simply the uptime — and since
         * detection begins ~2.5 s after power-on, that gap is *smaller* than
         * ABSENCE_MS, not larger. The first person to walk up therefore set
         * `present` without ever being announced, and no further arrival could
         * fire while they stayed in frame. */
        uint32_t gap = t - v.last_seen_ms;
        bool first = !v.seen_ever;
        v.seen_ever = true;
        v.last_seen_ms = t;
        if (!v.present) {
            v.present = true;
            /* Otherwise only count it as an arrival if they were gone long
             * enough to have actually left, rather than being missed for a
             * frame or two. */
            if (first || gap > ABSENCE_MS) {
                ESP_LOGI(TAG, "person arrived (score %d)", best);
                if (v.on_presence) {
                    v.on_presence();
                }
            }
        }
    } else if (v.present && (t - v.last_seen_ms) > PRESENCE_HOLD_MS) {
        v.present = false;
        v.last_absent_ms = t;
        ESP_LOGI(TAG, "person left");
    }
}

static void on_log(sscma_client_handle_t client,
                   const sscma_client_reply_t *reply, void *ctx)
{
    (void)client; (void)ctx;
    ESP_LOGD(TAG, "himax: %.*s", reply->len > 120 ? 120 : reply->len, reply->data);
}

void vision_on_presence(vision_presence_cb_t cb)
{
    v.on_presence = cb;
}

int vision_init(void)
{
    v.frame_lock = xSemaphoreCreateMutex();
    if (v.frame_lock == NULL) {
        return -1;
    }
    v.last_seen_ms = 0;
    v.seen_ever = false;

    v.client = bsp_sscma_client_init();
    if (v.client == NULL) {
        ESP_LOGE(TAG, "Himax client init failed — continuing without vision");
        return -1;
    }

    const sscma_client_callback_t cb = {
        .on_event = on_event,
        .on_log   = on_log,
    };
    if (sscma_client_register_callback(v.client, &cb, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "failed to register callbacks");
        return -1;
    }

    sscma_client_init(v.client);

    /* Stop whatever the Himax is already doing before talking to it.
     *
     * It comes up running an inference session of its own and streams results
     * continuously over SPI. That floods sscma_client's rx buffer ("rx buffer
     * is full" every ~550 ms), so replies to our commands never get matched and
     * everything times out — including invoke itself. Breaking first gives us a
     * quiet link to issue commands on. */
    if (sscma_client_break(v.client) != ESP_OK) {
        ESP_LOGW(TAG, "break failed — the Himax may not have been running");
    }
    vTaskDelay(pdMS_TO_TICKS(300));

    /* get_info and get_model are diagnostics, not prerequisites.
     *
     * On this device they fail: the Himax answers its unsolicited INIT@STAT?
     * but never replies to NAME?/VERSION?, so sscma_client retries for ~20 s
     * and gives up ("request name failed"). The stock Himax firmware evidently
     * does not implement the info commands the SDK example assumes. Inference
     * itself is a different command path, so treat these as best-effort and
     * carry on rather than blocking startup for 20 seconds on a nicety. */
#if VISION_QUERY_INFO
    /* Off by default: on this device both of these fail.
     *
     * The Himax answers its unsolicited INIT@STAT? but never replies to
     * NAME?/VERSION?, so sscma_client retries for ~20 SECONDS EACH before
     * giving up ("request name failed"). Two queries meant 40 s of blocked
     * startup for information we only ever logged. Its firmware evidently does
     * not implement the info commands the SDK example assumes. Inference uses a
     * different command path and works regardless. */
    sscma_client_info_t *info = NULL;
    if (sscma_client_get_info(v.client, &info, true) == ESP_OK && info) {
        ESP_LOGI(TAG, "himax %s, fw %s",
                 info->name ? info->name : "?", info->fw_ver ? info->fw_ver : "?");
    }
    if (sscma_client_get_model(v.client, &v.model, true) == ESP_OK && v.model) {
        ESP_LOGI(TAG, "model: %s", v.model->name ? v.model->name : "?");
        for (int i = 0; v.model->classes[i] != NULL; i++) {
            ESP_LOGI(TAG, "  class %d: %s", i, v.model->classes[i]);
        }
    }
#endif

#if VISION_FLASH_MODEL
    /* One-shot: give the Himax a model. Needs the network, so the caller must
     * not run this before WiFi is up. */
    if (model_flash_from_url(v.client, VISION_MODEL_URL) == 0) {
        ESP_LOGW(TAG, "model flashed — power-cycle to load it, then clear VISION_FLASH_MODEL");
    }
#endif

    /* No set_model call, deliberately.
     *
     * The Himax already has one loaded and reports it as "Person Detection"
     * with class 0 = person. Selecting a slot explicitly only breaks things:
     * slot 1 is what the SDK example uses and slot 4 is what the factory
     * firmware uses after downloading its own, and on this device both return
     * ESP_FAIL because neither describes the model actually present.
     *
     * This was hidden for a long time. Until CONFIG_FREERTOS_HZ was corrected,
     * get_model timed out, which read as "there is no model" and sent me off
     * flashing one unnecessarily. Ask the chip before concluding. */

    /* Turn the camera on. Easy to miss — inference starts happily without it
     * and simply never produces a detection, because nothing is feeding it
     * frames. The monitor example calls this; omitting it looks like a model
     * problem rather than a sensor one. */
    esp_err_t ss = sscma_client_set_sensor(v.client, 1, 1, true);
    if (ss != ESP_OK) {
        ESP_LOGW(TAG, "set_sensor failed: %s", esp_err_to_name(ss));
    }

    sscma_client_set_confidence_threshold(v.client, MIN_SCORE);

    /* times = -1 runs continuously. `show` would overlay boxes on the Himax's
     * own preview output, which we do not use — the display is the face. */
    /* Args copied exactly from the monitor example: invoke(-1, false, true).
     * The third argument is inverted inside the wrapper (`show ? 0 : 1` sent as
     * RESULT_ONLY), so passing false here asks for results-only — which on this
     * device yields events with zero boxes. */
    esp_err_t inv = sscma_client_invoke(v.client, -1, false, true);
    if (inv != ESP_OK) {
        ESP_LOGE(TAG, "failed to start inference (%s)", esp_err_to_name(inv));
        return -1;
    }

    v.up = true;
    ESP_LOGI(TAG, "vision up — continuous inference running");
    return 0;
}
