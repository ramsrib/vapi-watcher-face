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
static uint32_t s_last_call_end_ms;
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

/* Tell the assistant what the device can see.
 *
 * Vapi's add-message carries a plain string — its OpenAIMessage `content` is
 * typed as a string, not a multimodal array — so an image cannot be passed
 * through to the model even though the assistant's own LLM is multimodal.
 * Anything visual therefore has to arrive as *text*.
 *
 * What the Himax knows is modest but real: whether a person is in frame, which
 * class fired, and how confident it was. Sent as a system message so the model
 * treats it as context rather than as something the user said.
 *
 * This fires the instant the socket is up, so the assistant knows it is looking
 * at someone before it says a word. The richer answer — what the person
 * actually looks like — needs a round trip to a vision model and arrives a
 * second or two later, on its own task; see caption_task below. */
static void inject_vision_context(void)
{
    if (!vision_available()) {
        return;
    }
    char msg[240];
    const char *cls = vision_class_name(0);
    if (vision_person_present()) {
        snprintf(msg, sizeof(msg),
                 "[device context] Your camera can see someone right now "
                 "(%s, %d%% confidence). You noticed them and started this "
                 "conversation yourself — they did not press anything. Greet "
                 "them as if you just spotted them.",
                 cls ? cls : "person", vision_confidence());
    } else {
        snprintf(msg, sizeof(msg),
                 "[device context] You have a camera but cannot see anyone at "
                 "the moment. This conversation was started by a button press.");
    }
    ESP_LOGI(TAG, "vision context -> assistant");
    vapi_send_text(msg);
}

/* Describe what the camera sees, and tell the assistant mid-conversation.
 *
 * This runs on its own task, and that is the whole design rather than an
 * implementation detail. The alternative — caption first, then start the call —
 * puts a vision model's latency between a person walking up and the device
 * saying anything, which is exactly the silence that makes a demo look broken.
 *
 * So the greeting goes out immediately from the assistant's firstMessage, and
 * the description lands a second or two later, in time for the second turn.
 * The assistant opens with "oh, hello!" and follows with "nice hoodie" — which
 * is also just better theatre than leading with the observation.
 *
 * Failure is invisible by construction: no caption simply means the assistant
 * never mentions appearance, and the conversation is unaffected. That is why
 * there is one attempt and a short timeout rather than a retry loop.
 */
static void caption_task(void *arg)
{
    (void)arg;
    char *b64 = NULL;
    int   size = 0;

    /* The frame is captured on detection, and detection is what started this
     * call, so one is normally already waiting. Allow a couple of seconds for
     * the case where the call was started by hand and the Himax has not yet
     * seen anyone. */
    for (int i = 0; i < 20 && b64 == NULL; i++) {
        b64 = vision_take_frame(&size, VLM_FRAME_MAX_AGE_MS);
        if (b64 == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (b64 == NULL) {
        ESP_LOGI(TAG, "no recent frame — skipping caption");
        vTaskDelete(NULL);
        return;
    }

    char caption[220];
    int rc = vlm_describe(b64, caption, sizeof(caption));
    free(b64);

    /* "nobody in view" is a real answer, not a failure — but it is one the
     * assistant should not narrate, since by now the person is talking to it. */
    if (rc == 0 && vapi_call_is_active() &&
        strncasecmp(caption, "nobody", 6) != 0) {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "[device context] Looking through your camera right now you "
                 "see: %s. Work one warm, specific observation about them into "
                 "your next reply, then carry on naturally. Do not describe "
                 "your camera or say you are analysing an image.", caption);
        vapi_send_text(msg);
    }
    vTaskDelete(NULL);
}

void vapi_on_call_connected(void)
{
    inject_vision_context();
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
 * never runs on the caller's task. */
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
    bool active = vapi_call_is_active();
    if (s_call_was_active && !active) {
        s_last_call_end_ms = t;
    }
    s_call_was_active = active;

    if (active || !s_have_wifi || !network_is_connected()) {
        return;
    }
    if (!vision_person_present()) {
        return;
    }
    /* Do not immediately re-greet whoever is still standing there when a call
     * ends — that is a loop, not a conversation. */
    if (s_last_call_end_ms && (t - s_last_call_end_ms) < WAKE_COOLDOWN_MS) {
        return;
    }
    ESP_LOGI(TAG, "someone is here and we are idle — starting a call");
    face_set_state(FACE_DETECTING);
    vapi_toggle_call();
}

void vapi_toggle_call(void)
{
    bool want_start = !vapi_call_is_active();
    xTaskCreate(call_op_worker, "call_op", 8192,
                (void *)(intptr_t)want_start, 10, NULL);
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
