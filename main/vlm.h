/* VLM captioning — turn the frame the Himax saw into a sentence.
 *
 * The Himax tells us *that* a person is there. It cannot tell us anything
 * about them. For a booth demo that difference is the whole act: "hello there"
 * is a toy, "nice orange hoodie" is a thing people call their friends over to
 * see.
 *
 * Vapi's add-message carries a plain string (OpenAIMessage.content is typed as
 * a string, not a multimodal array), so the image cannot be handed to the
 * assistant's own model. The caption has to be produced here, in a separate
 * call, and injected as text.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Is an API key compiled in for the selected provider? */
bool vlm_configured(void);

/** Name of the provider/model in effect, for logs. */
const char *vlm_model_name(void);

/**
 * @brief  Describe a base64-encoded JPEG.
 *
 * Blocking: one HTTPS request, no retry. Call it from a worker task, never
 * from the audio or sscma paths.
 *
 * @param b64      base64 JPEG text, NUL-terminated (what vision_take_frame gives).
 * @param out      caption destination.
 * @param out_sz   size of @p out.
 * @return 0 on success and a non-empty caption in @p out, -1 otherwise.
 */
int vlm_describe(const char *b64, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif
