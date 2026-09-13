/* VLM captioning over raw HTTPS.
 *
 * Two providers, one function. They differ only in the URL, the auth header
 * and the shape of the request body; the response parse is one JSON pluck
 * either way. Having both compiled behind a switch is deliberate — at a venue
 * on venue wifi, being able to change one #define and reflash beats debugging
 * whichever one is having a bad afternoon.
 *
 * Two things make this cheap enough to run on a microcontroller:
 *
 *   1. The Himax already emits the frame as base64 text (SSCMA carries it that
 *      way in its JSON reply), so there is no encode step. The string goes
 *      from the SPI reply into the request body untouched.
 *   2. The NPU decides which frames are worth sending. A frame only leaves the
 *      board when a person has just been detected — roughly one request per
 *      visitor, not one per frame.
 *
 * The body is assembled with snprintf rather than cJSON: cJSON would hold a
 * second copy of the ~30 KB base64 payload while printing, and the only part
 * needing escaping is a prompt we wrote ourselves.
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "settings.h"
#include "vlm.h"

#define TAG "VLM"

/* Response bodies are a short caption plus usage metadata. 2 KB is generous;
 * anything larger is an error page we want truncated, not buffered. */
#define RESP_MAX 2048

typedef struct {
    char *buf;
    int   len;
} resp_t;

static esp_err_t on_data(esp_http_client_event_t *evt)
{
    resp_t *r = (resp_t *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0 || r == NULL) {
        return ESP_OK;
    }
    int room = RESP_MAX - 1 - r->len;
    int n = evt->data_len < room ? evt->data_len : room;
    if (n > 0) {
        memcpy(r->buf + r->len, evt->data, n);
        r->len += n;
        r->buf[r->len] = '\0';
    }
    return ESP_OK;
}

bool vlm_configured(void)
{
    return VLM_API_KEY[0] != '\0';
}

const char *vlm_model_name(void)
{
    return VLM_MODEL;
}

/* Pull the caption out of whichever envelope came back.
 *
 * Anthropic:  {"content":[{"type":"text","text":"..."}]}
 * OpenAI:     {"choices":[{"message":{"content":"..."}}]}
 *
 * Both are checked regardless of the compiled provider. It costs two lookups
 * and means a body from the wrong endpoint still parses instead of looking
 * like a network failure. */
static int extract_caption(const char *body, char *out, size_t out_sz)
{
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return -1;
    }
    const char *text = NULL;

    cJSON *content = cJSON_GetObjectItem(root, "content");
    if (cJSON_IsArray(content)) {
        cJSON *blk = NULL;
        cJSON_ArrayForEach(blk, content) {
            cJSON *t = cJSON_GetObjectItem(blk, "text");
            if (cJSON_IsString(t)) {
                text = t->valuestring;
                break;
            }
        }
    }
    if (text == NULL) {
        cJSON *choices = cJSON_GetObjectItem(root, "choices");
        cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
        cJSON *msg = first ? cJSON_GetObjectItem(first, "message") : NULL;
        cJSON *c = msg ? cJSON_GetObjectItem(msg, "content") : NULL;
        if (cJSON_IsString(c)) {
            text = c->valuestring;
        }
    }
    if (text == NULL) {
        /* Surface the provider's own error rather than a bare failure — a
         * bad key and a rate limit look identical from the outside. */
        cJSON *err = cJSON_GetObjectItem(root, "error");
        cJSON *m = err ? cJSON_GetObjectItem(err, "message") : NULL;
        ESP_LOGE(TAG, "no caption in reply%s%s",
                 cJSON_IsString(m) ? ": " : "",
                 cJSON_IsString(m) ? m->valuestring : "");
        cJSON_Delete(root);
        return -1;
    }
    snprintf(out, out_sz, "%s", text);
    cJSON_Delete(root);
    return out[0] ? 0 : -1;
}

int vlm_describe(const char *b64, char *out, size_t out_sz)
{
    if (b64 == NULL || b64[0] == '\0' || out == NULL || out_sz == 0) {
        return -1;
    }
    if (!vlm_configured()) {
        ESP_LOGW(TAG, "no API key — skipping caption");
        return -1;
    }
    out[0] = '\0';

    size_t b64_len = strlen(b64);
    /* Envelope plus the payload. The slack covers the JSON scaffolding, the
     * prompt and the data: URI prefix on the OpenAI path. */
    size_t body_sz = b64_len + strlen(VLM_PROMPT) + 1024;
    char *body = heap_caps_malloc(body_sz, MALLOC_CAP_SPIRAM);
    char *resp = heap_caps_malloc(RESP_MAX, MALLOC_CAP_SPIRAM);
    if (body == NULL || resp == NULL) {
        ESP_LOGE(TAG, "out of PSRAM (%u bytes wanted)", (unsigned)body_sz);
        free(body);
        free(resp);
        return -1;
    }
    resp[0] = '\0';

#if VLM_PROVIDER_ANTHROPIC
    /* The image block comes first: a model reads better when the picture is
     * already in context by the time the instruction arrives. */
    int n = snprintf(body, body_sz,
        "{\"model\":\"%s\",\"max_tokens\":%d,\"messages\":[{\"role\":\"user\","
        "\"content\":[{\"type\":\"image\",\"source\":{\"type\":\"base64\","
        "\"media_type\":\"image/jpeg\",\"data\":\"%s\"}},"
        "{\"type\":\"text\",\"text\":\"%s\"}]}]}",
        VLM_MODEL, VLM_MAX_TOKENS, b64, VLM_PROMPT);
#else
    /* OpenAI wants the JPEG as a data: URI inside image_url. Note
     * max_completion_tokens — the newer models reject the old max_tokens. */
    int n = snprintf(body, body_sz,
        "{\"model\":\"%s\",\"max_completion_tokens\":%d,\"messages\":[{\"role\":\"user\","
        "\"content\":[{\"type\":\"image_url\",\"image_url\":"
        "{\"url\":\"data:image/jpeg;base64,%s\"}},"
        "{\"type\":\"text\",\"text\":\"%s\"}]}]}",
        VLM_MODEL, VLM_MAX_TOKENS, b64, VLM_PROMPT);
#endif
    if (n < 0 || (size_t)n >= body_sz) {
        ESP_LOGE(TAG, "body truncated (%d of %u)", n, (unsigned)body_sz);
        free(body);
        free(resp);
        return -1;
    }

    resp_t r = { .buf = resp, .len = 0 };
    esp_http_client_config_t cfg = {
        .url               = VLM_API_URL,
        .method            = HTTP_METHOD_POST,
        .event_handler     = on_data,
        .user_data         = &r,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = VLM_TIMEOUT_MS,
        .buffer_size       = 2048,
        /* The request is one large POST body; the TX buffer bounds each write,
         * not the total, so the default is fine — but a bigger one halves the
         * number of TLS records on a ~30 KB upload. */
        .buffer_size_tx    = 4096,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (h == NULL) {
        free(body);
        free(resp);
        return -1;
    }
    esp_http_client_set_header(h, "Content-Type", "application/json");
#if VLM_PROVIDER_ANTHROPIC
    esp_http_client_set_header(h, "x-api-key", VLM_API_KEY);
    esp_http_client_set_header(h, "anthropic-version", "2023-06-01");
#else
    {
        char auth[128];
        snprintf(auth, sizeof(auth), "Bearer %s", VLM_API_KEY);
        esp_http_client_set_header(h, "Authorization", auth);
    }
#endif
    esp_http_client_set_post_field(h, body, n);

    /* Internal heap around the request, because this is where it is scarcest:
     * a TLS handshake on top of a live call is the tightest moment the board
     * ever sees, and the symptom when it goes wrong is not an error here but
     * SPI DMA failing somewhere else entirely. */
    size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(h);
    size_t heap_low = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int status = esp_http_client_get_status_code(h);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    esp_http_client_cleanup(h);
    free(body);

    int rc = -1;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "request failed after %d ms: %s", ms, esp_err_to_name(err));
    } else if (status != 200) {
        ESP_LOGE(TAG, "http %d after %d ms: %.200s", status, ms, resp);
    } else {
        rc = extract_caption(resp, out, out_sz);
        if (rc == 0) {
            ESP_LOGI(TAG, "%s in %d ms (%u KB image, internal heap %u -> %u): %s",
                     VLM_MODEL, ms, (unsigned)(b64_len / 1024),
                     (unsigned)heap_before, (unsigned)heap_low, out);
        }
    }
    free(resp);
    return rc;
}
