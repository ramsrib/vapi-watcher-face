/* Put an AI model on the Himax. See model_flash.c for the why. */

#pragma once

#include "sscma_client_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Download a .tflite and write it to the Himax's model region.
 *
 * Blocking, takes a minute or so, and writes another processor's flash — so it
 * runs only when explicitly asked. Returns 0 on success.
 */
int model_flash_from_url(sscma_client_handle_t client, const char *url);

#ifdef __cplusplus
}
#endif
