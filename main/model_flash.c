/* One-shot: put an AI model on the Himax.
 *
 * The Watcher ships with no model — its Himax reports `slot_header invalid !!`
 * at boot and rejects invoke, because the stock firmware only downloads a model
 * once a SenseCraft task is assigned, which never happened on this device.
 *
 * This does what the stock firmware's OTA path does, minus the cloud: fetch a
 * .tflite over HTTPS and stream it to the Himax's flash at 0xA00000.
 *
 * Addresses and chunk size are taken from Seeed's own app_ota.c:
 *   firmware -> 0x0        model -> 0xA00000
 *   SPI flasher chunk size 256 bytes
 *
 * Runs only when VAPI_FLASH_MODEL_URL is set and the Himax has no model. It is
 * deliberately a separate, explicit step rather than something the normal boot
 * path does — writing another processor's flash should not be a side effect.
 */

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "sensecap-watcher.h"
#include "sscma_client_ops.h"
#include "settings.h"
#include "model_flash.h"

#define TAG "MODEL_FLASH"

/* From Seeed's app_ota.c — the model region of the Himax's 16 MB flash. */
#define HIMAX_MODEL_ADDR   0xA00000
/* Also theirs: "this value is copied from the sscma_client_ota example". */
#define SPI_CHUNK          256

typedef struct {
    sscma_client_handle_t client;
    uint8_t *buf;
    int      buf_len;
    int      total;
} dl_t;

static esp_err_t http_cb(esp_http_client_event_t *evt)
{
    dl_t *d = (dl_t *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0) {
        return ESP_OK;
    }
    /* Accumulate in PSRAM. The model is ~1.3 MB and there is 7 MB free, so
     * buffering whole is simpler and far more robust than trying to stream
     * straight through to SPI at HTTP's pace. */
    uint8_t *p = heap_caps_realloc(d->buf, d->buf_len + evt->data_len, MALLOC_CAP_SPIRAM);
    if (p == NULL) {
        ESP_LOGE(TAG, "out of PSRAM at %d bytes", d->buf_len);
        return ESP_FAIL;
    }
    d->buf = p;
    memcpy(d->buf + d->buf_len, evt->data, evt->data_len);
    d->buf_len += evt->data_len;
    return ESP_OK;
}

int model_flash_from_url(sscma_client_handle_t client, const char *url)
{
    if (client == NULL || url == NULL || url[0] == '\0') {
        return -1;
    }

    dl_t d = { .client = client };
    esp_http_client_config_t cfg = {
        .url               = url,
        .event_handler     = http_cb,
        .user_data         = &d,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 60000,
        .buffer_size       = 4096,
    };
    ESP_LOGI(TAG, "downloading model: %s", url);
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (h == NULL) {
        return -1;
    }
    esp_err_t err = esp_http_client_perform(h);
    int status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);

    if (err != ESP_OK || status != 200 || d.buf_len == 0) {
        ESP_LOGE(TAG, "download failed (%s, http %d, %d bytes)",
                 esp_err_to_name(err), status, d.buf_len);
        free(d.buf);
        return -1;
    }
    /* TFLite files start with a 4-byte size then the "TFL3" identifier. Check
     * it before writing another processor's flash. */
    if (d.buf_len < 8 || memcmp(d.buf + 4, "TFL3", 4) != 0) {
        ESP_LOGE(TAG, "not a TFLite model (magic %.4s) — refusing to flash", d.buf + 4);
        free(d.buf);
        return -1;
    }
    ESP_LOGI(TAG, "got %d bytes, TFL3 verified", d.buf_len);

    sscma_client_flasher_handle_t flasher = bsp_sscma_flasher_init();
    if (flasher == NULL) {
        ESP_LOGE(TAG, "flasher init failed");
        free(d.buf);
        return -1;
    }

    ESP_LOGW(TAG, "writing %d bytes to Himax flash @0x%06X — do not power off",
             d.buf_len, HIMAX_MODEL_ADDR);
    if (sscma_client_ota_start(client, flasher, HIMAX_MODEL_ADDR) != ESP_OK) {
        ESP_LOGE(TAG, "ota_start failed");
        free(d.buf);
        return -1;
    }

    int64_t t0 = esp_timer_get_time();
    uint8_t *chunk = calloc(1, SPI_CHUNK);
    int written = 0, last_pct = -1;
    int rc = 0;
    while (written < d.buf_len) {
        int n = d.buf_len - written;
        if (n > SPI_CHUNK) {
            n = SPI_CHUNK;
        }
        /* Always write a full chunk — the flasher expects fixed-size writes, so
         * the tail is zero-padded rather than short. */
        memset(chunk, 0, SPI_CHUNK);
        memcpy(chunk, d.buf + written, n);
        if (sscma_client_ota_write(client, chunk, SPI_CHUNK) != ESP_OK) {
            ESP_LOGE(TAG, "ota_write failed at %d bytes", written);
            rc = -1;
            break;
        }
        written += n;
        int pct = 100 * written / d.buf_len;
        if (pct / 10 != last_pct / 10) {
            last_pct = pct;
            ESP_LOGI(TAG, "  %d%% (%d/%d)", pct, written, d.buf_len);
        }
    }
    free(chunk);

    if (rc == 0) {
        sscma_client_ota_finish(client);
        ESP_LOGI(TAG, "model written in %lld ms — reset the Himax to load it",
                 (esp_timer_get_time() - t0) / 1000);
    } else {
        sscma_client_ota_abort(client);
    }
    free(d.buf);
    return rc;
}
