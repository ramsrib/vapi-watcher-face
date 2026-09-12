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
    face_init();
    vapi_media_init();
    vapi_client_init();
    vapi_app_init();
    log_heap();

    wifi_start(WIFI_SSID, WIFI_PASSWORD);
    xTaskCreate(button_task, "button", 3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "ready — press the knob to start a call");
    int tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        vapi_call_query();
        if (++tick % 15 == 0) {
            log_heap();
        }
    }
}
