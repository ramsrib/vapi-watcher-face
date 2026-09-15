/* WiFi station bring-up, with failover across several networks.
 *
 * The AtomS3R firmware got this from esp-webrtc-solution's solutions/common,
 * which this board's SDK does not ship. It is a small enough surface to own
 * outright rather than drag that dependency in for one function.
 *
 * The failover exists for one reason: this device is meant to sit on a
 * conference booth, and venue WiFi is the least reliable part of a conference.
 * A phone hotspot listed as the second network turns "the demo is down" into a
 * pause of a few seconds that nobody watching even attributes to the network.
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

typedef struct {
    const char *ssid;
    const char *pass;
} wifi_net_t;

/* In order of preference. Blank entries are dropped at startup, so configuring
 * only the first leaves behaviour exactly as it was before failover existed. */
static const wifi_net_t s_all[] = {
    { WIFI_SSID,   WIFI_PASSWORD   },
    { WIFI_SSID_2, WIFI_PASSWORD_2 },
    { WIFI_SSID_3, WIFI_PASSWORD_3 },
};

static wifi_net_t s_nets[sizeof(s_all) / sizeof(s_all[0])];
static int  s_count;
static int  s_idx;        /* network currently being attempted */
static int  s_attempts;   /* consecutive failures on s_idx */
static bool s_connected;

static EventGroupHandle_t s_events;
#define GOT_IP BIT0

bool network_is_connected(void)
{
    return s_connected;
}

/* Point the radio at s_idx and try it. */
static void try_current(void)
{
    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, s_nets[s_idx].ssid, sizeof(wc.sta.ssid) - 1);
    if (s_nets[s_idx].pass) {
        strncpy((char *)wc.sta.password, s_nets[s_idx].pass, sizeof(wc.sta.password) - 1);
    }
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    ESP_LOGI(TAG, "trying \"%s\" (%d of %d)", s_nets[s_idx].ssid, s_idx + 1, s_count);
    esp_wifi_connect();
}

/* Why the AP dropped us is worth logging verbatim: reason 15 is a 4-way
 * handshake timeout (usually a wrong password), 201 no AP found, 205 a
 * connection failure. */
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        try_current();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = data;
        ESP_LOGW(TAG, "\"%s\" disconnected: reason=%d rssi=%d",
                 s_nets[s_idx].ssid, e ? e->reason : -1, e ? e->rssi : 0);
        s_connected = false;
        vapi_set_wifi_state(false);

        /* Stay on this network for one retry. A dropped association and an
         * absent AP are indistinguishable here, and they want opposite
         * responses — the first usually reconnects immediately, the second
         * never will. One retry serves the blip without stranding us on a
         * network that is not there. */
        if (++s_attempts >= WIFI_ATTEMPTS_PER_NET && s_count > 1) {
            s_attempts = 0;
            s_idx = (s_idx + 1) % s_count;
            ESP_LOGW(TAG, "giving up on that one — switching network");
        }
        vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS));
        try_current();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got ip " IPSTR " on \"%s\"", IP2STR(&e->ip_info.ip),
                 s_nets[s_idx].ssid);
        /* Clear the count, not the index: the network that just worked is the
         * one to try first if it blips. */
        s_attempts = 0;
        s_connected = true;
        xEventGroupSetBits(s_events, GOT_IP);
        vapi_set_wifi_state(true);
    }
}

int wifi_start(void)
{
    for (int i = 0; i < (int)(sizeof(s_all) / sizeof(s_all[0])); i++) {
        if (s_all[i].ssid && s_all[i].ssid[0]) {
            s_nets[s_count++] = s_all[i];
        }
    }
    if (s_count == 0) {
        ESP_LOGE(TAG, "no WiFi networks configured — set WIFI_SSID in .env");
        return -1;
    }
    ESP_LOGI(TAG, "%d network%s configured", s_count, s_count == 1 ? "" : "s");

    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());   /* STA_START fires try_current() */
    return 0;
}
