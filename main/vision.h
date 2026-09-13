/* Vision — the Himax WiseEye2 NPU.
 *
 * The ESP32-S3 talks to the Himax over SPI using the SSCMA protocol. The Himax
 * runs the model and streams results back as newline-delimited JSON, which
 * sscma_client parses into boxes, classes, keypoints and (crucially) the JPEG
 * frame itself.
 *
 * The division of labour this enables is the whole point of the device:
 * detection runs locally, continuously, and costs nothing per frame; only
 * frames that matter ever leave the board. That is what makes a cloud VLM
 * affordable here, rather than streaming video somewhere.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Called when a person appears after a period of absence. */
typedef void (*vision_presence_cb_t)(void);

/**
 * @brief  Bring up the Himax and start continuous inference.
 *
 * Safe to fail: everything else keeps working if the NPU does not come up, and
 * vision_available() then reports false.
 */
int vision_init(void);

/** Register the callback fired when someone arrives. */
void vision_on_presence(vision_presence_cb_t cb);

/** Did the Himax come up? */
bool vision_available(void);

/** Is a person in frame right now? */
bool vision_person_present(void);

/** Largest detection confidence in the current frame, 0-100. */
int vision_confidence(void);

/**
 * @brief  Copy the most recent frame that contained a detection.
 *
 * The frame is base64 JPEG *text*, NUL-terminated — the form SSCMA delivers it
 * in and the form both vision APIs want, so it is never decoded on the way
 * through. Frames are only retained while VISION_KEEP_FRAMES is set.
 *
 * @param out_size    receives the length excluding the NUL; may be NULL.
 * @param max_age_ms  reject a frame older than this; <= 0 accepts any age.
 * @return malloc'd buffer the caller must free, or NULL if there is no frame
 *         new enough to be worth acting on.
 */
char *vision_take_frame(int *out_size, int max_age_ms);

/** Human-readable label for a detected class index, or NULL. */
const char *vision_class_name(int target);

#ifdef __cplusplus
}
#endif
