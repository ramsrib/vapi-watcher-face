/* The face.
 *
 * A creature on a 412x412 round display: two eyes and a mouth, drawn with LVGL
 * primitives rather than bitmap frames. That choice is deliberate — the stock
 * firmware ships 5 PNG frames per state, which is enough for a status
 * indicator but too coarse to track speech. Drawn shapes can be driven
 * continuously from live signals (audio level, speech state), which is what
 * makes it read as alive rather than as an animation playing.
 *
 * Expressions differ by brightness and geometry, never hue. Staying on one hue
 * is what makes it read as a single creature with moods instead of a status
 * light cycling through colours.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  What the face is doing.
 *
 * These deliberately mirror the stock firmware's EMOJI_* states, because the
 * same set turns out to be exactly what a Vapi call needs — and every one of
 * them maps to a signal the voice client already computes.
 */
typedef enum {
    FACE_BOOT = 0,   /*!< powered, nothing else known yet         */
    FACE_STANDBY,    /*!< nobody around; slow breathing           */
    FACE_DETECTING,  /*!< something moved; waking up              */
    FACE_GREETING,   /*!< a person, and a call opening            */
    FACE_LISTENING,  /*!< the human's turn                        */
    FACE_THINKING,   /*!< waiting on the model                    */
    FACE_SPEAKING,   /*!< its turn; mouth tracks the audio        */
    FACE_SLEEPING,   /*!< idle long enough to close its eyes      */
    FACE_CONFUSED,   /*!< something went wrong, gently            */
    FACE_STATE_MAX,
} face_state_t;

/** Bring up the display and draw the face. Call once, after the BSP is up. */
void face_init(void);

/** Change expression. Cheap and idempotent; transitions are animated. */
void face_set_state(face_state_t state);

/** Current expression. */
face_state_t face_get_state(void);

/**
 * @brief  Drive the mouth from live audio, 0-255.
 *
 * Only meaningful in FACE_SPEAKING. Feeding this from the actual sample
 * amplitude is what separates a talking face from a looping animation.
 */
void face_set_mouth_level(uint8_t level);

/** Make it blink now, on top of whatever it is doing. */
void face_blink(void);

/** Human-readable state name, for logs. */
const char *face_state_name(face_state_t s);

#ifdef __cplusplus
}
#endif
