/* Vapi Watcher Face — entry point.
 *
 * Boots the board, puts the face on the display, joins WiFi, and waits. A call
 * is started by pressing the knob button; the face follows the call state from
 * there.
 */

#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "sensecap-watcher.h"
#include "common.h"

#define TAG "MAIN"

/* Why did we just boot? A silent reboot is otherwise hard to attribute —
 * brownout, a panic and the interrupt watchdog all look identical afterwards. */
static void log_reset_reason(void)
{
    const char *why;
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   why = "power-on";                    break;
    case ESP_RST_SW:        why = "software restart";            break;
    case ESP_RST_PANIC:     why = "PANIC (see backtrace above)"; break;
    case ESP_RST_INT_WDT:   why = "INTERRUPT WATCHDOG";          break;
    case ESP_RST_TASK_WDT:  why = "TASK WATCHDOG";               break;
    case ESP_RST_BROWNOUT:  why = "BROWNOUT — supply sagged";    break;
    case ESP_RST_USB:       why = "USB peripheral reset";        break;
    default:                why = "unknown";                     break;
    }
    ESP_LOGW(TAG, "=== boot: reset reason = %s ===", why);
}

static void log_heap(void)
{
    ESP_LOGI(TAG, "heap: internal free=%u min=%u | psram free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void vision_task(void *arg)
{
#if VISION_FLASH_MODEL
    /* Flashing a model needs the network, and this task starts before WiFi
     * does. Wait rather than reordering boot — the face should still come up
     * instantly whether or not vision is being provisioned. */
    ESP_LOGW(TAG, "VISION_FLASH_MODEL is set — waiting for network");
    for (int i = 0; i < 60 && !network_is_connected(); i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!network_is_connected()) {
        ESP_LOGE(TAG, "no network — cannot flash a model");
        vTaskDelete(NULL);
        return;
    }
#endif
    if (vision_init() == 0) {
        ESP_LOGI(TAG, "vision ready");
    } else {
        ESP_LOGW(TAG, "running without vision — face and voice are unaffected");
    }
    vTaskDelete(NULL);
}

/* Someone walked up. Start a call if we are idle and online — the whole point
 * of the device is that it notices you rather than waiting to be pressed.
 * Runs on the Himax client's task, so it must not block; vapi_toggle_call()
 * already dispatches the slow part to a worker. */
static void on_person_arrived(void)
{
    if (!network_is_connected()) {
        return;
    }
    if (vapi_call_is_active()) {
        return;
    }
    ESP_LOGI(TAG, "someone arrived — starting a call");
    face_set_state(FACE_DETECTING);
    vapi_toggle_call();
}

#if VAPI_SELFTEST_CALL
/* Place one call after boot with nobody present, to exercise the path that
 * normally needs a person in front of the camera. See VAPI_SELFTEST_CALL. */
static void selftest_call_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(VAPI_SELFTEST_CALL_AFTER_MS));
    if (network_is_connected() && !vapi_call_is_active()) {
        ESP_LOGW(TAG, "selftest call — exercising the call path with nobody here");
        vapi_toggle_call();
    } else {
        ESP_LOGW(TAG, "selftest call skipped (offline, or a call is already up)");
    }
    vTaskDelete(NULL);
}
#endif

/* The knob button is the only physical control. A press toggles the call; a
 * long press mutes the microphone. */
static void button_task(void *arg)
{
    bool was_down = false;
    uint32_t down_at = 0;
    bool long_fired = false;
    while (1) {
        bool down = (bsp_exp_io_get_level(BSP_KNOB_BTN) == 0);
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (down && !was_down) {
            down_at = now;
            long_fired = false;
        } else if (down && !long_fired && (now - down_at) > 900) {
            long_fired = true;            /* fires while still held */
            vapi_toggle_mute();
        } else if (!down && was_down && !long_fired) {
            vapi_toggle_call();
        }
        was_down = down;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    log_reset_reason();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    bsp_io_expander_init();

    /* Vision runs on its own task rather than inline.
     *
     * Every sscma command retries for ~20 s before timing out, and on a device
     * with no model loaded they all fail — which would otherwise hold up boot
     * for a minute and delay WiFi with it. Backgrounding it means the face and
     * voice come up immediately and vision simply reports whether it made it.
     *
     * It also still runs before the display: the Himax handshake is timing
     * sensitive, and the face's 20 Hz redraw is enough to starve the sscma
     * task (seen as "request not found", then timeouts). */
    xTaskCreate(vision_task, "vision_init", 5120, NULL, 6, NULL);

    face_init();
    vapi_media_init();
    vapi_client_init();
    vapi_app_init();
    log_heap();

#if VISION_WAKE_ON_PRESENCE
    vision_on_presence(on_person_arrived);
#endif

    wifi_start(WIFI_SSID, WIFI_PASSWORD);
    xTaskCreate(button_task, "button", 3072, NULL, 5, NULL);

#if VAPI_SELFTEST_CALL
    xTaskCreate(selftest_call_task, "call_test", 3072, NULL, 3, NULL);
#endif

    ESP_LOGI(TAG, "ready — press the knob to start a call");
    int tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        vapi_call_query();
        vapi_presence_poll();
        if (++tick % 15 == 0) {
            log_heap();
        }
    }
}
