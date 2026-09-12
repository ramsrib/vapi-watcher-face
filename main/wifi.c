/* Minimal WiFi station bring-up.
 *
 * The AtomS3R firmware got this from esp-webrtc-solution's solutions/common,
 * which this board's SDK does not ship. It is a small enough surface to own
 * outright rather than drag that dependency in for one function.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "common.h"

#define TAG "WIFI"

static EventGroupHandle_t s_events;
static bool s_connected;
#define GOT_IP BIT0

bool network_is_connected(void)
{
    return s_connected;
}

/* Why the AP dropped us is worth logging verbatim: reason 15 is a 4-way
 * handshake timeout (usually a wrong password), 205 a connection failure. */
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = data;
        ESP_LOGW(TAG, "disconnected: reason=%d rssi=%d", e ? e->reason : -1, e ? e->rssi : 0);
        s_connected = false;
        vapi_set_wifi_state(false);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        xEventGroupSetBits(s_events, GOT_IP);
        vapi_set_wifi_state(true);
    }
}

int wifi_start(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        ESP_LOGE(TAG, "WIFI_SSID is empty — set it in .env");
        return -1;
    }
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    if (pass) {
        strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    return 0;
}
