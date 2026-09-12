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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sensecap-watcher.h"
#include "common.h"

#define TAG "VAPI_APP"

static bool s_have_wifi;
static bool s_mic_muted;
static uint32_t s_last_voice_ms;

/* How long after the last activity the face drifts off to sleep. */
#define SLEEP_AFTER_MS (90 * 1000)

void vapi_set_wifi_state(bool connected)
{
    s_have_wifi = connected;
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
