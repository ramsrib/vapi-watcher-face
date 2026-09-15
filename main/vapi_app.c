/* Application wiring — the face reacts to the call.
 *
 * This is the file that turns a dev board into a character. Everything here is
 * a mapping from something the voice client already knows to something the face
 * shows; there is deliberately no new state machine.
 *
 * The mapping that matters most: the echo gate's write-ahead playout clock is a
 * sample-accurate answer to "is the assistant audible right now". It exists to
 * suppress echo, but it is exactly the signal a mouth animation needs — so the
 * lip-sync comes for free from the echo fix.
 */

#include <string.h>
#include <strings.h>   /* strncasecmp */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sensecap-watcher.h"
#include <stdio.h>
#include "common.h"
#include "vlm.h"

#define TAG "VAPI_APP"

static bool s_have_wifi;
static bool s_mic_muted;
static uint32_t s_last_voice_ms;
static uint32_t s_cooldown_until;
static uint32_t s_last_present_ms;
static bool     s_call_was_active;

/* How long after the last activity the face drifts off to sleep. */
#define SLEEP_AFTER_MS (90 * 1000)

#if VLM_ENABLE && VLM_SELFTEST
/* Prove the caption path works, once, at boot.
 *
 * Everything this touches is shared with the real path: the same frame buffer,
 * the same vlm_describe(), the same TLS stack and the same heap. The only thing
 * it does not test is the Vapi injection, which is a websocket send that the
 * call path already exercises constantly.
 *
 * The point is *when* it fails. A bad key or a rejected body otherwise surfaces
 * the first time someone walks up to the device, which at a booth is the worst
 * possible moment and the hardest to read — the conversation still happens, it
 * is just inexplicably less impressive. Here it is a red line on the console
 * ten seconds after power-on. */
static void vlm_selftest_task(void *arg)
{
    (void)arg;
    char *b64 = NULL;
    int   size = 0;

    /* The first frame lands about 2.5 s after boot; wifi is usually up before
     * that, so wait rather than concluding there is no camera. */
    for (int i = 0; i < 60 && b64 == NULL; i++) {
        b64 = vision_take_frame(&size, 0);
        if (b64 == NULL) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    if (b64 == NULL) {
        ESP_LOGW(TAG, "selftest: no frame from the camera — vision path is down");
        vTaskDelete(NULL);
        return;
    }

    /* A call may have started while we were waiting for a frame — presence can
     * fire before wifi settles. Stand down rather than run a second, redundant
     * request against the same image, competing for internal heap exactly
     * during the call's TLS handshake. */
    if (vapi_call_is_active()) {
        ESP_LOGI(TAG, "selftest: call already up, the real path will prove it");
        free(b64);
        vTaskDelete(NULL);
        return;
    }

    char caption[220];
    if (vlm_describe(b64, caption, sizeof(caption)) == 0) {
        ESP_LOGW(TAG, "selftest OK (%s): %s", vlm_model_name(), caption);
    } else {
        ESP_LOGE(TAG, "selftest FAILED — captions will be silently missing");
    }
    free(b64);
    vTaskDelete(NULL);
}
#endif

void vapi_set_wifi_state(bool connected)
{
    s_have_wifi = connected;
#if VLM_ENABLE && VLM_SELFTEST
    /* Once, on the first association — not on every reconnect. */
    static bool tested = false;
    if (connected && !tested && !vapi_call_is_active() &&
        vision_available() && vlm_configured()) {
        tested = true;
        xTaskCreate(vlm_selftest_task, "vlm_test", 8192, NULL, 3, NULL);
    }
#endif
    vapi_refresh_display();
}

/* Collapse everything known into one expression. Ordered by specificity. */
void vapi_refresh_display(void)
{
    face_state_t s;

    if (!vapi_call_is_active()) {
        s = s_have_wifi ? FACE_STANDBY : FACE_BOOT;
    } else if (vapi_media_far_end_active()) {
        s = FACE_SPEAKING;
    } else if (s_mic_muted) {
        s = FACE_CONFUSED;
    } else {
        s = FACE_LISTENING;
    }
    face_set_state(s);
}

/* Runs at the face's own cadence rather than on call events, because two of
 * these — the mouth level and the speaking/listening flip — change many times
 * per second and are not events at all. */
static void expression_task(void *arg)
{
    face_state_t last = FACE_STATE_MAX;
    while (1) {
        if (vapi_call_is_active()) {
            bool far = vapi_media_far_end_active();
            face_state_t want = far ? FACE_SPEAKING
                                    : (s_mic_muted ? FACE_CONFUSED : FACE_LISTENING);
            if (want != last) {
                face_set_state(want);
                last = want;
            }
            /* Drive the mouth from what is actually going to the speaker, so it
             * moves in time with what is heard rather than with what arrived. */
            face_set_mouth_level(far ? vapi_media_get_output_level() : 0);
            s_last_voice_ms = (uint32_t)(esp_timer_get_time() / 1000);
        } else {
            face_state_t want = s_have_wifi ? FACE_STANDBY : FACE_BOOT;
            uint32_t idle = (uint32_t)(esp_timer_get_time() / 1000) - s_last_voice_ms;
            if (s_have_wifi && idle > SLEEP_AFTER_MS) {
                want = FACE_SLEEPING;
            }
            if (want != last) {
                face_set_state(want);
                last = want;
            }
            face_set_mouth_level(0);
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

void vapi_app_init(void)
{
    s_last_voice_ms = (uint32_t)(esp_timer_get_time() / 1000);
    xTaskCreate(expression_task, "face_state", 3072, NULL, 4, NULL);
}

/* Keep telling the assistant what it can see, for as long as the call lasts.
 *
 * This runs on its own task, and that is the design rather than an
 * implementation detail. The alternative — caption first, then start the call —
 * puts a vision model's latency between a person walking up and the device
 * saying anything, which is exactly the silence that makes a demo look broken.
 *
 * It loops rather than firing once because a single frame is a photograph, not
 * sight. The first thing anyone asks is "what am I holding?", and a device with
 * one stale frame can only improvise — which for this character is the worst
 * possible failure, since improvising about what it can see is precisely the
 * thing it must never do.
 *
 * Failure stays invisible by construction: no caption means the assistant never
 * mentions appearance, and the conversation is unaffected. That is why each
 * pass is one attempt with a short timeout rather than a retry loop.
 */
static void caption_task(void *arg)
{
    (void)arg;

    /* Let the websocket's own TLS session and the audio buffers finish landing
     * before adding a second handshake on top of them. See VLM_SETTLE_MS. */
    vTaskDelay(pdMS_TO_TICKS(VLM_SETTLE_MS));

    char last[400] = { 0 };
    bool first = true;

    while (vapi_call_is_active()) {
        char *b64 = NULL;
        int   size = 0;

        /* A frame is normally already waiting, since detection is what started
         * this call. Allow a couple of seconds on the first pass for a call
         * started by hand, before the Himax has seen anyone. */
        for (int i = 0; i < (first ? 20 : 1) && b64 == NULL; i++) {
            b64 = vision_take_frame(&size, VLM_FRAME_MAX_AGE_MS);
            if (b64 == NULL) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        if (b64 == NULL) {
            if (first) {
                ESP_LOGI(TAG, "no recent frame — skipping caption");
            }
            break;
        }

        char caption[400];
        int rc = vlm_describe(b64, caption, sizeof(caption));
        free(b64);

        /* Three reasons to stay quiet. "nobody in view" is a real answer rather
         * than a failure, but not one to narrate at someone who is plainly
         * talking to you; an unchanged description would only teach the model
         * that its eyes report the same thing whatever happens; and a call that
         * ended while the request was in flight has nowhere to put this. */
        if (rc == 0 && vapi_call_is_active() &&
            strncasecmp(caption, "nobody", 6) != 0 &&
            strcmp(caption, last) != 0) {
            snprintf(last, sizeof(last), "%s", caption);
            char msg[640];
            /* Just the description. Instructions about what to do with it
             * live in the system prompt, where they are stated once and can
             * see the whole conversation — repeating them on every refresh
             * would both bloat the context and nag. */
            snprintf(msg, sizeof(msg), "[device context] %s", caption);
            vapi_send_context(msg);
        }
        first = false;

        /* Sleep in short steps so a call ending is noticed promptly rather
         * than one whole refresh interval later. */
        for (int slept = 0; slept < VLM_REFRESH_MS && vapi_call_is_active();
             slept += 200) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    ESP_LOGI(TAG, "vision narration stopped");
    vTaskDelete(NULL);
}

void vapi_on_call_connected(void)
{
    /* Deliberately nothing is injected here.
     *
     * It used to send a vague "your camera can see someone (person, 92%)" the
     * instant the socket came up, and that turned out to cost the greeting.
     * A message arriving before the assistant has spoken makes Vapi plan a
     * reply rather than play firstMessage verbatim: measured 9.8 s of silence
     * where a canned line would have been instant, and the spoken greeting was
     * not the configured one at all.
     *
     * It also cost the point of the feature. The vague message landed first and
     * anchored the model on it, so when asked "what do you see?" it answered
     * "I see you standing right there" while holding an accurate description of
     * a grey printed t-shirt it never mentioned.
     *
     * The system prompt already establishes that it noticed someone and started
     * the conversation itself. Nothing here needs to say so again, and the only
     * message worth sending is the specific one, once it exists. */
#if VLM_ENABLE
    if (vision_available() && vlm_configured()) {
        /* 8 KB: TLS handshake plus a JSON parse. The ~30 KB image never lands
         * on this stack — it is in PSRAM throughout. */
        xTaskCreate(caption_task, "vlm", 8192, NULL, 4, NULL);
    }
#endif
    vapi_refresh_display();
}

/* --- actions, bound to the knob button ------------------------------------ */

/* Call open/close is slow — a blocking HTTPS POST plus a TLS handshake — so it
 * never runs on the caller's task.
 *
 * Which creates a window: for the ~2 s the worker is running, the call is being
 * opened but vapi_call_is_active() is still false. Anything that decides what
 * to do by asking that question will decide wrongly, repeatedly. The presence
 * poll runs every 2 s and did exactly that — a new Vapi call every tick, each
 * one billable, for as long as someone stood in front of the camera.
 *
 * So "a call is in progress" has to mean *including while it is being set up*,
 * which is what s_call_pending adds. The edge-triggered arrival callback never
 * exposed this because it only ever fired once. */
static volatile bool s_call_pending;
static portMUX_TYPE s_call_mux = portMUX_INITIALIZER_UNLOCKED;

static void call_op_worker(void *arg)
{
    bool start = (bool)(intptr_t)arg;
    if (start) {
        ESP_LOGI(TAG, "starting call");
        face_set_state(FACE_GREETING);
        vapi_call_start();
    } else {
        ESP_LOGI(TAG, "ending call");
        vapi_call_stop();
        s_mic_muted = false;
    }
    s_call_pending = false;
    vapi_refresh_display();
    vTaskDelete(NULL);
}

/* Start a call if the situation calls for one. Ticked from the main loop.
 *
 * This exists because the arrival *event* is not trustworthy on its own. It is
 * edge-triggered — it fires once when someone appears and not again while they
 * stay — and its only consumer has to decline it when the network is not up
 * yet. Those two facts combine badly: on a boot where WiFi takes an extra few
 * seconds (a fumbled association, slow DHCP), the arrival lands in the gap, is
 * refused, and never comes back. Measured on this board: detection at 3.3 s,
 * IP address at 9.6 s, and a person standing in front of a device that saw
 * them at 92% confidence and did nothing for ninety seconds.
 *
 * At a booth the worst case is also the common one: someone is already standing
 * there when the device powers on, so the only arrival edge of their visit is
 * the one that happens before the network exists.
 *
 * So the edge stays as a fast path — it is what makes the greeting feel instant
 * — and this is the safety net underneath it. Asking "should a call be running
 * right now?" every couple of seconds cannot lose an event, because it is not
 * looking at events.
 */
void vapi_presence_poll(void)
{
    uint32_t t = (uint32_t)(esp_timer_get_time() / 1000);

    /* Watch for a call ending so the cooldown can run from that moment. */
    bool active = vapi_call_is_active() || s_call_pending;
    if (s_call_was_active && !active) {
        s_cooldown_until = t + WAKE_COOLDOWN_MS;
    }
    s_call_was_active = active;

    if (active) {
        /* Hang up on a visitor who has gone. See HANGUP_AFTER_ABSENT_MS. */
        if (vision_available() && vision_person_present()) {
            s_last_present_ms = t;
        } else if (s_last_present_ms &&
                   (t - s_last_present_ms) > HANGUP_AFTER_ABSENT_MS &&
                   !vapi_media_far_end_active()) {
            ESP_LOGI(TAG, "nobody there any more — ending the call");
            vapi_toggle_call();
        }
        return;
    }
    /* Idle: keep the clock current so an arrival does not inherit a stale
     * absence and hang itself up moments after connecting. */
    s_last_present_ms = t;

    /* Nobody in frame means the previous visitor has genuinely gone, so
     * whoever arrives next is a new person and should not serve their
     * cooldown. */
    if (!vision_person_present()) {
        s_cooldown_until = 0;
    }

    if (!s_have_wifi || !network_is_connected()) {
        return;
    }
    if (!vision_person_present()) {
        return;
    }
    /* Do not immediately re-greet whoever is still standing there when a call
     * ends — that is a loop, not a conversation.
     *
     * But it must only apply to *that* person. A plain timer punishes the next
     * visitor for the last one: measured 26 s between someone walking up and
     * being greeted, because a call they had nothing to do with had just ended.
     * Departure is what distinguishes the two, and it is observable — so the
     * cooldown is cleared the moment the frame is empty, above. */
    if (s_cooldown_until && (int32_t)(s_cooldown_until - t) > 0) {
        return;
    }
    ESP_LOGI(TAG, "someone is here and we are idle — starting a call");
    face_set_state(FACE_DETECTING);
    vapi_toggle_call();
}

void vapi_toggle_call(void)
{
    /* Claim the transition before dispatching. Two tasks reach this — the main
     * loop's presence poll and the button task — so the test and the claim have
     * to be one operation, or both can pass it. */
    bool want_start;
    portENTER_CRITICAL(&s_call_mux);
    bool busy = s_call_pending;
    if (!busy) {
        s_call_pending = true;
        want_start = !vapi_call_is_active();
    }
    portEXIT_CRITICAL(&s_call_mux);

    if (busy) {
        ESP_LOGD(TAG, "call transition already in flight — ignoring");
        return;
    }
    if (xTaskCreate(call_op_worker, "call_op", 8192,
                    (void *)(intptr_t)want_start, 10, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not spawn call worker");
        s_call_pending = false;
    }
}

void vapi_toggle_mute(void)
{
    s_mic_muted = !s_mic_muted;
    ESP_LOGI(TAG, "microphone %s", s_mic_muted ? "muted" : "unmuted");
    vapi_refresh_display();
}

bool vapi_is_mic_muted(void)
{
    return s_mic_muted;
}
