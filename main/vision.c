/* Vision — Himax WiseEye2 via SSCMA. See vision.h. */

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
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

/* How often at most to copy a frame out of the reply stream.
 *
 * Inference runs at ~10 Hz and every reply carries the JPEG, so retaining each
 * one is a ~30 KB copy ten times a second for as long as anyone is standing
 * there. A caption only ever needs one frame, and any frame from the last
 * second is as good as any other. */
#define FRAME_KEEP_INTERVAL_MS 1000

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
    uint8_t              *frame;       /*!< base64 JPEG text, NUL-terminated */
    int                   frame_size;  /*!< length excluding the NUL */
    int                   frame_cap;   /*!< allocation; grown, never shrunk */
    uint32_t              frame_ms;    /*!< when it was captured */

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

char *vision_take_frame(int *out_size, int max_age_ms)
{
    char *copy = NULL;
    xSemaphoreTake(v.frame_lock, portMAX_DELAY);
    /* Refuse a frame older than the caller is willing to act on. A caption of
     * whoever stood here ten minutes ago is worse than no caption: the
     * assistant would describe someone who is not in the room. */
    bool fresh = max_age_ms <= 0 || (now_ms() - v.frame_ms) <= (uint32_t)max_age_ms;
    if (v.frame && v.frame_size > 0 && fresh) {
        copy = heap_caps_malloc(v.frame_size + 1, MALLOC_CAP_SPIRAM);
        if (copy) {
            memcpy(copy, v.frame, v.frame_size);
            copy[v.frame_size] = '\0';
            if (out_size) {
                *out_size = v.frame_size;
            }
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

#if VISION_DUMP_FRAME
    /* One-shot frame dump for aiming/lighting checks. Reassemble on the host. */
    static bool dumped = false;
    if (!dumped && reply->data) {
        char *im = NULL; int im_sz = 0;
        if (sscma_utils_fetch_image_from_reply(reply, &im, &im_sz) == ESP_OK) {
            dumped = true;
            ESP_LOGW(TAG, "FRAME_BEGIN %d", im_sz);
            for (int o = 0; o < im_sz; o += 512) {
                int n = im_sz - o; if (n > 512) n = 512;
                printf("FRAME:%.*s\n", n, im + o);
            }
            ESP_LOGW(TAG, "FRAME_END");
            free(im);
        }
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
    static uint32_t last_log, events, win_hits, win_max_gap, last_hit_ms;
    static int win_best;
    events++;
    uint32_t t = now_ms();
    if (best >= MIN_SCORE) {
        win_hits++;
        if (last_hit_ms) {
            uint32_t g = t - last_hit_ms;
            if (g > win_max_gap) win_max_gap = g;
        }
        last_hit_ms = t;
    }
    if (best > win_best) win_best = best;
    if ((uint32_t)(t - last_log) > 10000) {
        last_log = t;
        /* Max over the window, not the instantaneous frame. Reporting the frame
         * that happened to coincide with the log makes a working detector look
         * dead whenever the subject blinks out for one frame. */
        /* max_gap is the number PRESENCE_HOLD_MS has to clear: the longest
         * stretch, while someone was actually there, that the detector reported
         * nothing. Set the hold from this rather than by guessing. */
        ESP_LOGI(TAG, "inference alive: %lu events/10s, hits: %lu, best: %d, max detect gap: %lu ms (hold %d)",
                 (unsigned long)events, (unsigned long)win_hits, win_best,
                 (unsigned long)win_max_gap, PRESENCE_HOLD_MS);
        events = 0; win_hits = 0; win_best = 0; win_max_gap = 0;
    }

    v.score = best;

#if VISION_KEEP_FRAMES
    /* Retain the frame, but only when there is something in it.
     *
     * Two economies, both of which matter on this chip. Gating on a detection
     * means the buffer holds a picture of a person rather than whichever empty
     * room happened to be last, and it cuts the copy rate from every inference
     * to only those that matter. Reusing one grown buffer avoids a ~20 KB
     * malloc/free pair per retained frame — PSRAM is plentiful here but
     * fragmenting it under a long-running call is not free.
     *
     * What is stored is SSCMA's base64 text verbatim: that is the form both
     * vision APIs want, so decoding it here would only buy a re-encode later.
     * It is NUL-terminated so it can be spliced straight into a request body. */
    if (best >= MIN_SCORE && (uint32_t)(t - v.frame_ms) >= FRAME_KEEP_INTERVAL_MS) {
        char *img = NULL;
        int img_size = 0;
        if (sscma_utils_fetch_image_from_reply(reply, &img, &img_size) == ESP_OK) {
            xSemaphoreTake(v.frame_lock, portMAX_DELAY);
            if (img_size + 1 > v.frame_cap) {
                uint8_t *p = heap_caps_realloc(v.frame, img_size + 1, MALLOC_CAP_SPIRAM);
                if (p) {
                    v.frame = p;
                    v.frame_cap = img_size + 1;
                } else {
                    v.frame_cap = 0;   /* v.frame still valid at its old size */
                }
            }
            if (v.frame && img_size + 1 <= v.frame_cap) {
                memcpy(v.frame, img, img_size);
                v.frame[img_size] = '\0';
                v.frame_size = img_size;
                v.frame_ms = t;
            }
            xSemaphoreGive(v.frame_lock);
            free(img);
        }
    }
#endif

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

#if VISION_QUERY_INFO
    /* What sensor modes does this camera offer? `opt_id` in set_sensor selects
     * one, and if any of them differ in orientation that would be a software
     * fix for the rotated frame. No wrapper exists, so ask directly. */
    {
        sscma_client_reply_t r = { 0 };
        if (sscma_client_request(v.client, "AT+SENSORS?\r\n", &r, true,
                                 pdMS_TO_TICKS(2000)) == ESP_OK && r.data) {
            ESP_LOGW(TAG, "sensors: %.300s", r.data);
            sscma_client_reply_clear(&r);
        }
    }
#endif

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
