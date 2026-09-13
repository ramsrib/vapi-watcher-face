/* Shared declarations for the Vapi Watcher Face. */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "settings.h"
#include "face.h"
#include "vapi_media.h"
#include "vision.h"
#include "esp_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Vapi call configuration.
 *
 * All fields are borrowed pointers owned by the caller for the session.
 */
typedef struct {
    const char *api_url;       /*!< Base URL, e.g. https://api.vapi.ai */
    const char *api_key;       /*!< Vapi PRIVATE key (required) */
    const char *assistant_id;  /*!< Assistant to call (required) */
    const char *first_message; /*!< Optional greeting override */
    int         max_seconds;   /*!< Optional call cap; <= 0 = assistant default */
} vapi_call_cfg_t;

/* --- call lifecycle (vapi_client.c) --------------------------------------- */

int  vapi_client_init(void);
int  vapi_call_start(void);
int  vapi_call_stop(void);
bool vapi_call_is_active(void);
void vapi_call_query(void);
int  vapi_send_text(const char *text);
int  vapi_send_control(const char *control);

/* --- wifi (wifi.c) --------------------------------------------------------- */

int  wifi_start(const char *ssid, const char *pass);
bool network_is_connected(void);

/* --- app wiring (vapi_app.c) ---------------------------------------------- */

void vapi_app_init(void);

/** Called from the transport once the websocket is up and audio is flowing. */
void vapi_on_call_connected(void);
void vapi_refresh_display(void);
void vapi_set_wifi_state(bool connected);
void vapi_toggle_call(void);
void vapi_toggle_mute(void);
bool vapi_is_mic_muted(void);

#ifdef __cplusplus
}
#endif
