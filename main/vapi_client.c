/* Vapi transport — REST call creation + websocket audio.
 *
 * Two steps, in this order:
 *
 *   1. POST <api_url>/call
 *        Authorization: Bearer <VAPI_API_KEY>
 *        {"assistantId": "...",
 *         "transport": {"provider": "vapi.websocket",
 *                       "audioFormat": {"format":"pcm_s16le",
 *                                       "container":"raw",
 *                                       "sampleRate":16000}},
 *         "assistantOverrides": {...}}
 *      -> {"id": "...", "transport": {"websocketCallUrl": "wss://..."}}
 *
 *   2. Connect that wss:// URL. Binary frames are raw PCM in both directions;
 *      text frames are JSON control/status messages. Sending
 *      {"type":"end-call"} ends the call.
 *
 * Why this and not WebRTC: Vapi's WebRTC transport is Daily, whose client
 * signaling is proprietary and has no C or embedded implementation. The
 * SDP-exchange gateway used by Vapi's 2025 workshop firmware
 * (https://github.com/VapiAI/vapicon-2025-hardware-workshop)
 * — staging-webrtc.vapi.ai — is decommissioned and now answers Cloudflare 530.
 * The websocket transport is the one path an MCU can actually reach, and it is
 * simpler: no ICE, no DTLS-SRTP, no SDP, no jitter-buffer of our own beyond the
 * render FIFO. The cost is uncompressed audio; see settings.h.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "common.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include <cJSON.h>


/* --- FreeRTOS shims -------------------------------------------------------
 * The AtomS3R build used esp-webrtc-solution's media_lib_os wrappers. That
 * component is not part of this board's SDK, and the surface is small enough
 * that wrapping FreeRTOS directly beats adding the dependency. Same semantics,
 * same call shapes, so the transport below is otherwise unmodified.
 */
#define MEDIA_LIB_MAX_LOCK_TIME portMAX_DELAY

typedef SemaphoreHandle_t media_lib_mutex_handle_t;
typedef TaskHandle_t      media_lib_thread_handle_t;

static inline int media_lib_mutex_create(media_lib_mutex_handle_t *m)
{
    *m = xSemaphoreCreateMutex();
    return *m ? 0 : -1;
}

static inline int media_lib_mutex_lock(media_lib_mutex_handle_t m, uint32_t timeout)
{
    return xSemaphoreTake(m, timeout) == pdTRUE ? 0 : -1;
}

static inline int media_lib_mutex_unlock(media_lib_mutex_handle_t m)
{
    return xSemaphoreGive(m) == pdTRUE ? 0 : -1;
}

static inline int media_lib_thread_create(media_lib_thread_handle_t *h, const char *name,
                                          void (*fn)(void *), void *arg,
                                          uint32_t stack, int prio, int core)
{
    (void)core;
    return xTaskCreate((TaskFunction_t)fn, name, stack, arg, prio, h) == pdPASS ? 0 : -1;
}

static inline void media_lib_thread_destroy(media_lib_thread_handle_t h)
{
    vTaskDelete(h);   /* NULL deletes the calling task, matching media_lib */
}

static inline void media_lib_thread_sleep(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

#define TAG "VAPI_CLIENT"

#define SAFE_FREE(p) do { if (p) { free(p); (p) = NULL; } } while (0)

/* The websocket rx buffer. Audio chunks are small; transcript messages are the
 * large ones, and anything bigger than this arrives split across events (which
 * handle_text() reassembles). */
#define WS_RX_BUFFER      (4 * 1024)
/* Cap on a reassembled text message. A transcript of a long turn is a few KB;
 * beyond this we drop rather than grow without bound on a misbehaving server. */
#define WS_TEXT_MAX       (16 * 1024)
#define HTTP_TIMEOUT_MS   (15000)
/* How long the websocket gets to finish TLS + the upgrade handshake before
 * vapi_call_query() gives up on it. Generous: an S3 doing a fresh TLS handshake
 * over a weak AP has been seen to take several seconds. */
#define WS_CONNECT_TIMEOUT_MS (20000)
#define WS_SEND_TIMEOUT   pdMS_TO_TICKS(1000)

typedef struct {
    esp_websocket_client_handle_t ws;
    char       *call_id;
    bool        connected;      /* websocket open right now */
    bool        ever_connected; /* it opened at least once this call */
    uint32_t    start_ms;       /* when the websocket was told to start */
    bool        streaming;      /* audio tasks running */
    bool        stopping;       /* teardown in progress; suppresses the reaper */
    media_lib_mutex_handle_t stream_lock; /* serialises start/stop_streaming */
    media_lib_mutex_handle_t call_lock;   /* serialises vapi_call_start/stop */
    /* Reassembly for text frames split across events. */
    char       *text_buf;
    int         text_len;
    /* Opcode of the message currently being reassembled. Continuation frames
     * (op_code 0x0) carry no type of their own, so it has to be remembered from
     * the first fragment — guess wrong and JSON gets played as audio. */
    uint8_t     frag_op;
    /* Odd trailing byte from a binary fragment, carried into the next one so a
     * split never lands mid-sample and swaps the channel phase. */
    uint8_t     carry;
    bool        have_carry;
} vapi_client_t;

static vapi_client_t s_client;

/* Tears the call down from a worker task. Used when the *server* hangs up: the
 * teardown must not run on the websocket task that is delivering the message. */
static void stop_worker(void *arg);
static int  call_start_locked(void);
static int  call_stop_locked(void);

/* --- Step 1: create the call over REST ------------------------------------ */

typedef struct {
    char *buf;
    int   len;
} http_accum_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        http_accum_t *acc = (http_accum_t *)evt->user_data;
        char *p = realloc(acc->buf, acc->len + evt->data_len + 1);
        if (p == NULL) {
            ESP_LOGE(TAG, "OOM accumulating %d bytes", acc->len + evt->data_len);
            return ESP_FAIL;
        }
        acc->buf = p;
        memcpy(acc->buf + acc->len, evt->data, evt->data_len);
        acc->len += evt->data_len;
        acc->buf[acc->len] = 0;
    }
    return ESP_OK;
}

/* Body for POST /call. Caller frees. */
static char *build_create_call_body(const vapi_call_cfg_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "assistantId", cfg->assistant_id);

    cJSON *transport = cJSON_AddObjectToObject(root, "transport");
    cJSON_AddStringToObject(transport, "provider", "vapi.websocket");
    cJSON *fmt = cJSON_AddObjectToObject(transport, "audioFormat");
    cJSON_AddStringToObject(fmt, "format", "pcm_s16le");
    cJSON_AddStringToObject(fmt, "container", "raw");
    cJSON_AddNumberToObject(fmt, "sampleRate", VAPI_SAMPLE_RATE);

    /* Overrides apply to this call only — the saved assistant is never
     * modified, so a device experiment cannot change the dashboard config. */
    cJSON *ov = cJSON_AddObjectToObject(root, "assistantOverrides");
    if (cfg->first_message && cfg->first_message[0]) {
        cJSON_AddStringToObject(ov, "firstMessage", cfg->first_message);
    }
    if (cfg->max_seconds > 0) {
        cJSON_AddNumberToObject(ov, "maxDurationSeconds", cfg->max_seconds);
    }
    /* Ask for transcripts and status updates on the control channel so the
     * serial log shows what the assistant heard — the cheapest way to tell a
     * dead microphone from a dead LLM. */
    cJSON *msgs = cJSON_AddArrayToObject(ov, "clientMessages");
    cJSON_AddItemToArray(msgs, cJSON_CreateString("transcript"));
    cJSON_AddItemToArray(msgs, cJSON_CreateString("status-update"));
    cJSON_AddItemToArray(msgs, cJSON_CreateString("speech-update"));

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

/* Parse {"id":..,"transport":{"websocketCallUrl":".."}}. Both out params are
 * heap strings the caller frees. */
static bool parse_create_call_response(const char *json, char **call_id, char **ws_url)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGE(TAG, "call response is not valid JSON");
        return false;
    }
    bool ok = false;
    cJSON *transport = cJSON_GetObjectItemCaseSensitive(root, "transport");
    cJSON *url = transport ? cJSON_GetObjectItemCaseSensitive(transport, "websocketCallUrl") : NULL;
    if (cJSON_IsString(url) && url->valuestring) {
        *ws_url = strdup(url->valuestring);
        cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
        *call_id = (cJSON_IsString(id) && id->valuestring) ? strdup(id->valuestring) : strdup("?");
        ok = (*ws_url != NULL && *call_id != NULL);
    } else {
        /* Vapi returns {"message": [...], "error": ..} on a rejected request;
         * surfacing it verbatim saves a round of guessing at the console. */
        char *dump = cJSON_PrintUnformatted(root);
        ESP_LOGE(TAG, "no transport.websocketCallUrl in response: %s", dump ? dump : "(unprintable)");
        SAFE_FREE(dump);
    }
    cJSON_Delete(root);
    return ok;
}

static int create_call(const vapi_call_cfg_t *cfg, char **call_id, char **ws_url)
{
    char url[256];
    const char *base = (cfg->api_url && cfg->api_url[0]) ? cfg->api_url : "https://api.vapi.ai";
    snprintf(url, sizeof(url), "%s/call", base);

    char *body = build_create_call_body(cfg);
    if (body == NULL) {
        return -1;
    }
    int auth_len = (int)strlen("Bearer ") + (int)strlen(cfg->api_key) + 1;
    char *auth = malloc(auth_len);
    if (auth == NULL) {
        free(body);
        return -1;
    }
    snprintf(auth, auth_len, "Bearer %s", cfg->api_key);

    http_accum_t acc = { .buf = NULL, .len = 0 };
    esp_http_client_config_t http_cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .event_handler     = http_event_cb,
        .user_data         = &acc,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = HTTP_TIMEOUT_MS,
        .buffer_size_tx    = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        free(auth);
        free(body);
        return -1;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, body, strlen(body));

    ESP_LOGI(TAG, "POST %s", url);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(auth);
    free(body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "call creation failed: %s", esp_err_to_name(err));
        SAFE_FREE(acc.buf);
        return -1;
    }
    ESP_LOGI(TAG, "POST /call -> HTTP %d, %d body bytes", status, acc.len);
    if ((status != 200 && status != 201) || acc.buf == NULL) {
        ESP_LOGE(TAG, "unexpected response (status %d)%s%s", status,
                 acc.buf ? ": " : "", acc.buf ? acc.buf : "");
        SAFE_FREE(acc.buf);
        return -1;
    }

    bool ok = parse_create_call_response(acc.buf, call_id, ws_url);
    SAFE_FREE(acc.buf);
    return ok ? 0 : -1;
}

/* --- Step 2: websocket audio ---------------------------------------------- */

/* Inbound JSON control messages.
 *
 * Vapi pushes {type: transcript|status-update|speech-update|...}. A talk/listen
 * device can ignore all of them — audio rides the binary frames — so we log
 * what is useful and leave obvious extension points.
 */
static void handle_control_json(const char *json, int len)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGW(TAG, "control <- non-JSON (%d bytes)", len);
        return;
    }
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *t = (cJSON_IsString(type) && type->valuestring) ? type->valuestring : "?";

    if (strcmp(t, "transcript") == 0) {
        cJSON *role = cJSON_GetObjectItemCaseSensitive(root, "role");
        cJSON *tt   = cJSON_GetObjectItemCaseSensitive(root, "transcriptType");
        cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "transcript");
        /* Partials fire several times a second; only finals are worth a line. */
        if (cJSON_IsString(tt) && tt->valuestring && strcmp(tt->valuestring, "final") == 0) {
            ESP_LOGI(TAG, "%s: %s",
                     (cJSON_IsString(role) && role->valuestring) ? role->valuestring : "?",
                     (cJSON_IsString(text) && text->valuestring) ? text->valuestring : "");
        }
    } else if (strcmp(t, "status-update") == 0) {
        cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
        const char *s = (cJSON_IsString(status) && status->valuestring) ? status->valuestring : "?";
        ESP_LOGI(TAG, "call status: %s", s);
        if (strcmp(s, "ended") == 0) {
            /* The server hung up (end-call phrase, max duration, error). Tear
             * down from a worker: vapi_call_stop() destroys the websocket
             * client, and doing that from inside its own event callback
             * deadlocks. */
            ESP_LOGI(TAG, "server ended the call");
            media_lib_thread_handle_t h = NULL;
            media_lib_thread_create(&h, "vapi_stop", stop_worker, NULL, 6 * 1024, 10, 0);
        }
    } else {
        ESP_LOGD(TAG, "control <- %s", t);
    }
    cJSON_Delete(root);
}

/* Reassemble a text frame that the client split across events, then dispatch. */
static void handle_text(esp_websocket_event_data_t *d)
{
    /* Fits in one event and is not a continuation: parse in place. */
    if (d->payload_offset == 0 && d->data_len == d->payload_len) {
        char *tmp = malloc(d->data_len + 1);
        if (tmp == NULL) {
            return;
        }
        memcpy(tmp, d->data_ptr, d->data_len);
        tmp[d->data_len] = 0;
        handle_control_json(tmp, d->data_len);
        free(tmp);
        return;
    }
    if (d->payload_len > WS_TEXT_MAX) {
        ESP_LOGW(TAG, "control message %d bytes > %d cap, dropped", d->payload_len, WS_TEXT_MAX);
        return;
    }
    if (d->payload_offset == 0) {
        SAFE_FREE(s_client.text_buf);
        s_client.text_len = 0;
        s_client.text_buf = malloc(d->payload_len + 1);
        if (s_client.text_buf == NULL) {
            return;
        }
    }
    if (s_client.text_buf == NULL) {
        return;   /* missed the first fragment; wait for the next whole message */
    }
    memcpy(s_client.text_buf + s_client.text_len, d->data_ptr, d->data_len);
    s_client.text_len += d->data_len;
    if (s_client.text_len >= d->payload_len) {
        s_client.text_buf[s_client.text_len] = 0;
        handle_control_json(s_client.text_buf, s_client.text_len);
        SAFE_FREE(s_client.text_buf);
        s_client.text_len = 0;
    }
}

/* Inbound assistant audio. PCM is a byte stream, so fragments can be written
 * straight through — but only on even boundaries, or every later sample is
 * byte-swapped into noise. The carry byte keeps that from happening. */
static void handle_binary(esp_websocket_event_data_t *d)
{
    const uint8_t *p = (const uint8_t *)d->data_ptr;
    int n = d->data_len;
    if (n <= 0) {
        return;
    }
    if (s_client.have_carry) {
        uint8_t pair[2] = { s_client.carry, p[0] };
        vapi_media_write_frame(pair, 2);
        s_client.have_carry = false;
        p++;
        n--;
    }
    if (n & 1) {
        s_client.carry = p[n - 1];
        s_client.have_carry = true;
        n--;
    }
    if (n > 0) {
        vapi_media_write_frame(p, n);
    }
}

/* Mic pump: capture → websocket, on its own task so neither the websocket task
 * nor the button task ever waits on audio. */
static void send_audio_task(void *arg)
{
    uint8_t *buf = malloc(VAPI_FRAME_BYTES * 4);
    if (buf == NULL) {
        ESP_LOGE(TAG, "OOM allocating send buffer");
        media_lib_thread_destroy(NULL);
        return;
    }
    ESP_LOGI(TAG, "audio send task started");
    while (s_client.streaming) {
        int n;
        bool sent_any = false;
        while (s_client.streaming &&
               (n = vapi_media_read_frame(buf, VAPI_FRAME_BYTES * 4)) > 0) {
            if (esp_websocket_client_send_bin(s_client.ws, (const char *)buf, n,
                                              WS_SEND_TIMEOUT) < 0) {
                ESP_LOGW(TAG, "websocket send failed (%d bytes)", n);
            }
            sent_any = true;
        }
        /* Poll a little faster than the frame period when idle so a frame never
         * waits a full period after it becomes ready. */
        media_lib_thread_sleep(sent_any ? VAPI_FRAME_MS / 2 : VAPI_FRAME_MS);
    }
    free(buf);
    ESP_LOGI(TAG, "audio send task stopped");
    media_lib_thread_destroy(NULL);
}

static void start_streaming(void)
{
    /* Reached from the websocket task; stop_streaming() is reached from both
     * that task (on disconnect) and whichever task called vapi_call_stop().
     * Without this lock a hangup racing a disconnect can double-close the
     * codec, and a check-then-act on `streaming` is not enough on its own. */
    media_lib_mutex_lock(s_client.stream_lock, MEDIA_LIB_MAX_LOCK_TIME);
    if (s_client.streaming) {
        media_lib_mutex_unlock(s_client.stream_lock);
        return;
    }
    if (vapi_media_play_start() != 0) {
        ESP_LOGE(TAG, "failed to start playback");
        media_lib_mutex_unlock(s_client.stream_lock);
        return;
    }
    if (vapi_media_capture_start() != 0) {
        ESP_LOGE(TAG, "failed to start capture");
        vapi_media_play_stop();
        media_lib_mutex_unlock(s_client.stream_lock);
        return;
    }
    s_client.streaming = true;
    media_lib_thread_handle_t h = NULL;
    if (media_lib_thread_create(&h, "vapi_tx", send_audio_task, NULL, 8 * 1024, 12, 0) != 0) {
        ESP_LOGE(TAG, "failed to start audio send task");
        s_client.streaming = false;
        vapi_media_capture_stop();
        vapi_media_play_stop();
        media_lib_mutex_unlock(s_client.stream_lock);
        return;
    }
    media_lib_mutex_unlock(s_client.stream_lock);
    vapi_refresh_display();
}

static void stop_streaming(void)
{
    media_lib_mutex_lock(s_client.stream_lock, MEDIA_LIB_MAX_LOCK_TIME);
    if (!s_client.streaming) {
        media_lib_mutex_unlock(s_client.stream_lock);
        return;
    }
    s_client.streaming = false;
    /* Let the send task observe the flag and exit before the codec goes away.
     * It polls on a VAPI_FRAME_MS cadence, so three periods is comfortably more
     * than one loop iteration. */
    media_lib_thread_sleep(VAPI_FRAME_MS * 3);
    vapi_media_capture_stop();
    vapi_media_play_stop();
    s_client.have_carry = false;
    media_lib_mutex_unlock(s_client.stream_lock);
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "websocket connected — start talking.");
        s_client.connected = true;
        s_client.ever_connected = true;
        start_streaming();
        /* The app layer injects context here. Deliberately after the socket is
         * up rather than at call creation: add-message only has somewhere to go
         * once the session exists. */
        vapi_on_call_connected();
        break;

    case WEBSOCKET_EVENT_DATA: {
        /* 0x1 text, 0x2 binary, 0x0 a continuation of whichever came before.
         * 0x8/0x9/0xA are close/ping/pong and the client handles those. */
        uint8_t op = d->op_code;
        if (op == 0x0) {
            op = s_client.frag_op;          /* continuation: inherit the type */
        } else if (d->payload_offset == 0) {
            s_client.frag_op = op;          /* first fragment: remember it */
        }
        if (op == 0x2) {
            handle_binary(d);
        } else if (op == 0x1) {
            handle_text(d);
        } else if (d->op_code == 0x8) {
            ESP_LOGI(TAG, "websocket close frame from server");
        }
        break;
    }

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "websocket error (handshake status %d)",
                 d ? d->error_handle.esp_ws_handshake_status_code : -1);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGI(TAG, "websocket %s",
                 event_id == WEBSOCKET_EVENT_CLOSED ? "closed" : "disconnected");
        s_client.connected = false;
        /* Only tear down the audio path here; the handle itself is destroyed by
         * vapi_call_stop(), which never runs on this task. */
        stop_streaming();
        vapi_refresh_display();
        break;

    default:
        break;
    }
}

/* --- Public API ----------------------------------------------------------- */

/* Create the locks before any task can reach start/stop. Doing this lazily
 * inside vapi_call_start() would just move the race onto the creation itself. */
int vapi_client_init(void)
{
    if (media_lib_mutex_create(&s_client.stream_lock) != 0 ||
        media_lib_mutex_create(&s_client.call_lock) != 0) {
        ESP_LOGE(TAG, "failed to create client locks");
        return -1;
    }
    return 0;
}

bool vapi_call_is_active(void)
{
    return s_client.ws != NULL;
}

int vapi_call_start(void)
{
    if (network_is_connected() == false) {
        ESP_LOGE(TAG, "WiFi not connected yet");
        return -1;
    }
    media_lib_mutex_lock(s_client.call_lock, MEDIA_LIB_MAX_LOCK_TIME);
    int ret = call_start_locked();
    media_lib_mutex_unlock(s_client.call_lock);
    return ret;
}

/* Caller must hold call_lock. */
static int call_start_locked(void)
{
    if (s_client.ws != NULL) {
        call_stop_locked();   /* already holding the lock; don't re-enter it */
    }
    if (VAPI_API_KEY[0] == '\0') {
        ESP_LOGE(TAG, "VAPI_API_KEY is empty. Set it in .env or via menuconfig.");
        return -1;
    }
    if (VAPI_ASSISTANT_ID[0] == '\0') {
        ESP_LOGE(TAG, "VAPI_ASSISTANT_ID is empty. Set it in .env or via menuconfig.");
        return -1;
    }

    vapi_call_cfg_t cfg = {
        .api_url       = VAPI_API_URL,
        .api_key       = VAPI_API_KEY,
        .assistant_id  = VAPI_ASSISTANT_ID,
        .first_message = VAPI_FIRST_MESSAGE,
        .max_seconds   = atoi(VAPI_MAX_DURATION_SECONDS),
    };

    char *call_id = NULL, *ws_url = NULL;
    if (create_call(&cfg, &call_id, &ws_url) != 0) {
        SAFE_FREE(call_id);
        SAFE_FREE(ws_url);
        return -1;
    }
    ESP_LOGI(TAG, "call %s created", call_id);

    s_client.stopping       = false;
    s_client.ever_connected = false;
    s_client.start_ms       = (uint32_t)(esp_timer_get_time() / 1000);
    s_client.call_id        = call_id;

    esp_websocket_client_config_t ws_cfg = {
        .uri                    = ws_url,
        .crt_bundle_attach      = esp_crt_bundle_attach,
        .buffer_size            = WS_RX_BUFFER,
        .task_stack             = 8 * 1024,
        .task_prio              = 14,
        .network_timeout_ms     = 10000,
        /* The call URL is single-use: it belongs to the call we just created,
         * and reconnecting to it after Vapi ends the call reaches nothing. A
         * new call means a new POST /call, which vapi_call_start() does. */
        .disable_auto_reconnect = true,
        /* Vapi's own audio is the liveness signal we care about, and a silent
         * pause in conversation is normal — so don't let a missed pong drop a
         * healthy call. */
        .disable_pingpong_discon = true,
    };
    s_client.ws = esp_websocket_client_init(&ws_cfg);
    SAFE_FREE(ws_url);
    if (s_client.ws == NULL) {
        ESP_LOGE(TAG, "failed to init websocket client");
        SAFE_FREE(s_client.call_id);
        return -1;
    }
    esp_websocket_register_events(s_client.ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    if (esp_websocket_client_start(s_client.ws) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start websocket client");
        esp_websocket_client_destroy(s_client.ws);
        s_client.ws = NULL;
        SAFE_FREE(s_client.call_id);
        return -1;
    }
    return 0;
}

/* Caller must hold call_lock. */
static int call_stop_locked(void)
{
    if (s_client.ws == NULL) {
        return 0;
    }
    s_client.stopping = true;

    /* Tell Vapi first so the call ends cleanly rather than being dropped.
     * `end-call` is the documented ClientInboundMessage type (checked against
     * https://api.vapi.ai/api-json); closing the socket alone also ends the
     * call, which is why an undocumented spelling would look like it worked.
     * Best effort — if the socket is already gone, the close below is enough. */
    if (s_client.connected) {
        static const char end_call[] = "{\"type\":\"end-call\"}";
        esp_websocket_client_send_text(s_client.ws, end_call, sizeof(end_call) - 1, WS_SEND_TIMEOUT);
    }
    stop_streaming();

    esp_websocket_client_handle_t ws = s_client.ws;
    s_client.ws = NULL;
    s_client.connected = false;
    esp_websocket_client_close(ws, pdMS_TO_TICKS(1000));
    esp_websocket_client_destroy(ws);

    ESP_LOGI(TAG, "call %s ended", s_client.call_id ? s_client.call_id : "?");
    SAFE_FREE(s_client.call_id);
    SAFE_FREE(s_client.text_buf);
    s_client.text_len = 0;
    s_client.stopping = false;
    return 0;
}

/* Safe to call from any task, and from two at once — a button hangup racing a
 * server-initiated one would otherwise both see a non-NULL handle and both
 * destroy it. */
int vapi_call_stop(void)
{
    media_lib_mutex_lock(s_client.call_lock, MEDIA_LIB_MAX_LOCK_TIME);
    int ret = call_stop_locked();
    media_lib_mutex_unlock(s_client.call_lock);
    return ret;
}

static void stop_worker(void *arg)
{
    vapi_call_stop();
    vapi_refresh_display();
    media_lib_thread_destroy(NULL);
}

/* Auto-reconnect is off, so once the socket goes down the call is over — but
 * the handle is still allocated and vapi_call_is_active() still reports true,
 * which would swallow the next button tap. This reaps it.
 *
 * The two cases have to be told apart. A call that connected and then dropped
 * is finished. A call that has *not yet* connected is mid-handshake: TLS plus
 * the websocket upgrade takes a few seconds on this chip, and reaping on
 * `!connected` alone tore down every call about two seconds after it started.
 */
void vapi_call_query(void)
{
    if (s_client.ws == NULL || s_client.stopping || s_client.connected) {
        return;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    bool dropped = s_client.ever_connected;
    bool timed_out = !s_client.ever_connected &&
                     (now - s_client.start_ms) > WS_CONNECT_TIMEOUT_MS;
    if (dropped || timed_out) {
        ESP_LOGI(TAG, "reaping call (%s)", dropped ? "socket dropped" : "connect timed out");
        vapi_call_stop();
        vapi_refresh_display();
    }
}

int vapi_send_text(const char *text)
{
    if (s_client.ws == NULL || !s_client.connected) {
        ESP_LOGE(TAG, "not connected");
        return -1;
    }
    /* Vapi's control channel accepts an "add-message" envelope, which injects a
     * message into the conversation as if the model produced or heard it. */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "add-message");
    cJSON *message = cJSON_AddObjectToObject(root, "message");
    cJSON_AddStringToObject(message, "role", "system");
    cJSON_AddStringToObject(message, "content", text ? text : "");
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s == NULL) {
        return -1;
    }
    int ret = esp_websocket_client_send_text(s_client.ws, s, strlen(s), WS_SEND_TIMEOUT);
    ESP_LOGI(TAG, "add-message -> %s", text ? text : "");
    free(s);
    return ret < 0 ? -1 : 0;
}

/* Send one ClientInboundMessageControl. `control` must be one of the values the
 * spec enumerates: mute-assistant, unmute-assistant, mute-customer,
 * unmute-customer, say-first-message. Anything else is rejected server-side. */
int vapi_send_control(const char *control)
{
    if (s_client.ws == NULL || !s_client.connected) {
        ESP_LOGW(TAG, "control '%s' ignored — not connected", control ? control : "?");
        return -1;
    }
    char msg[96];
    int n = snprintf(msg, sizeof(msg), "{\"type\":\"control\",\"control\":\"%s\"}", control);
    if (n <= 0 || n >= (int)sizeof(msg)) {
        return -1;
    }
    ESP_LOGI(TAG, "control -> %s", control);
    return esp_websocket_client_send_text(s_client.ws, msg, n, WS_SEND_TIMEOUT) < 0 ? -1 : 0;
}
