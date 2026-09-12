/* The face — LVGL drawing and the expression state machine.
 *
 * Geometry note: the panel is 412x412 and *round*, so anything outside the
 * inscribed circle is invisible. The face is laid out from the centre with
 * generous margins rather than from the top-left, and nothing important goes
 * past ~40% of the radius.
 */

#include <stdlib.h>
#include "face.h"
#include "palette.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_random.h"
#include "sensecap-watcher.h"
#include "esp_lvgl_port.h"

#define TAG "FACE"

#define SCREEN       412
#define CX           (SCREEN / 2)
#define CY           (SCREEN / 2)

/* Eye geometry at rest. Width stays fixed; height is the expressive axis —
 * squashing an eye vertically reads as a blink, a squint or a smile depending
 * on how far it goes, which is most of the emotional range for free. */
#define EYE_W        72
#define EYE_H        96
#define EYE_GAP      152          /* centre-to-centre */
#define EYE_Y        (CY - 34)
#define EYE_RADIUS   36           /* fully rounded ends */

#define MOUTH_W      120
#define MOUTH_H      14
#define MOUTH_Y      (CY + 78)

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *eye_l;
    lv_obj_t *eye_r;
    lv_obj_t *shine_l;
    lv_obj_t *shine_r;
    lv_obj_t *mouth;
    face_state_t state;
    uint8_t  mouth_level;
    lv_timer_t *tick;
    uint32_t ticks;
    bool     blinking;
} face_t;

static face_t f;

const char *face_state_name(face_state_t s)
{
    switch (s) {
    case FACE_BOOT:      return "boot";
    case FACE_STANDBY:   return "standby";
    case FACE_DETECTING: return "detecting";
    case FACE_GREETING:  return "greeting";
    case FACE_LISTENING: return "listening";
    case FACE_THINKING:  return "thinking";
    case FACE_SPEAKING:  return "speaking";
    case FACE_SLEEPING:  return "sleeping";
    case FACE_CONFUSED:  return "confused";
    default:             return "?";
    }
}

/* --- drawing helpers ------------------------------------------------------ */

static lv_obj_t *make_blob(lv_obj_t *parent, int w, int h, lv_color_t c, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void set_eye(lv_obj_t *eye, lv_obj_t *shine, int h, lv_color_t c, bool show_shine)
{
    if (h < 4) {
        h = 4;    /* never fully zero — a 4px line still reads as a closed eye */
    }
    lv_obj_set_height(eye, h);
    lv_obj_set_style_bg_color(eye, c, 0);
    /* Keep the corner radius at half the height so the eye stays a capsule
     * rather than turning into a rectangle as it squashes. */
    lv_obj_set_style_radius(eye, h / 2, 0);
    lv_obj_set_style_opa(shine, show_shine && h > 40 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

static void set_mouth(int w, int h, lv_color_t c)
{
    if (h < 6) {
        h = 6;
    }
    lv_obj_set_size(f.mouth, w, h);
    lv_obj_set_style_bg_color(f.mouth, c, 0);
    lv_obj_set_style_radius(f.mouth, h / 2, 0);
}

/* --- the expression itself ------------------------------------------------ */

/* Redraw for the current state. Called every tick so states that breathe or
 * track audio can update; cheap enough at 20 Hz that not diffing is fine. */
static void render(void)
{
    /* A slow sine used by the resting states. Integer-only: this runs on the
     * LVGL timer and there is no reason to pull in floating point. */
    int phase = (int)(f.ticks % 100);
    int breath = phase < 50 ? phase : 100 - phase;   /* 0..50 triangle */

    lv_color_t c   = PAL_PRIMARY;
    int eye_h      = EYE_H;
    int mouth_w    = MOUTH_W;
    int mouth_h    = MOUTH_H;
    bool shine     = true;

    switch (f.state) {
    case FACE_BOOT:
        c = PAL_PRIMARY_DEEP;
        eye_h = 8; mouth_w = 40; shine = false;
        break;

    case FACE_STANDBY:
        /* Dim and slowly breathing: asleep, but alive. */
        c = PAL_PRIMARY_DIM;
        eye_h = EYE_H - 30 + breath / 3;
        mouth_w = 70;
        break;

    case FACE_SLEEPING:
        c = PAL_PRIMARY_DEEP;
        eye_h = 6; mouth_w = 50; shine = false;
        break;

    case FACE_DETECTING:
        /* Waking: eyes opening, brightening. */
        c = PAL_PRIMARY_DIM;
        eye_h = EYE_H + breath / 5;
        mouth_w = 80;
        break;

    case FACE_GREETING:
        /* Wide eyes, big smile. */
        c = PAL_PRIMARY;
        eye_h = EYE_H + 14;
        mouth_w = MOUTH_W + 40;
        mouth_h = MOUTH_H + 18;
        break;

    case FACE_LISTENING:
        /* Attentive and still. Stillness is the point — it should feel like it
         * is holding its breath while you talk, in contrast to speaking. */
        c = PAL_PRIMARY;
        eye_h = EYE_H + 6;
        mouth_w = 60;
        mouth_h = MOUTH_H;
        break;

    case FACE_THINKING:
        /* Squint, and a small mouth drifting side to side. */
        c = PAL_PRIMARY_DIM;
        eye_h = EYE_H - 40;
        mouth_w = 46;
        lv_obj_set_x(f.mouth, -(MOUTH_W / 2) + 20 - 14 + (breath / 2));
        break;

    case FACE_SPEAKING: {
        /* The mouth tracks the audio. This is the one state driven by a live
         * signal rather than a timer, and it is what separates a talking face
         * from an animation that happens to be playing. */
        c = PAL_PRIMARY;
        eye_h = EYE_H;
        int open = (f.mouth_level * 54) / 255;
        mouth_h = MOUTH_H + open;
        mouth_w = MOUTH_W - open / 3;      /* opens taller and slightly narrower */
        break;
    }

    case FACE_CONFUSED:
        c = PAL_ALERT;
        eye_h = EYE_H - 20;
        mouth_w = 56; mouth_h = MOUTH_H;
        break;

    default:
        break;
    }

    if (f.blinking) {
        eye_h = 6;
        shine = false;
    }

    if (f.state != FACE_THINKING) {
        lv_obj_set_x(f.mouth, 0);
    }

    set_eye(f.eye_l, f.shine_l, eye_h, c, shine);
    set_eye(f.eye_r, f.shine_r, eye_h, c, shine);
    set_mouth(mouth_w, mouth_h, c);
}

/* Blinks are randomised rather than periodic. A face that blinks on a fixed
 * schedule looks mechanical; irregular gaps read as alive. */
static void tick_cb(lv_timer_t *t)
{
    (void)t;
    f.ticks++;

    if (f.blinking) {
        f.blinking = false;                    /* one tick long, ~50 ms */
    } else if (f.state != FACE_SLEEPING && f.state != FACE_BOOT) {
        if ((esp_random() % 100) < 2) {        /* ~2% per tick */
            f.blinking = true;
        }
    }
    render();
}

/* --- public --------------------------------------------------------------- */

void face_init(void)
{
    bsp_lvgl_init();
    bsp_lcd_brightness_set(100);

    lvgl_port_lock(0);

    f.screen = lv_scr_act();
    lv_obj_set_style_bg_color(f.screen, PAL_BG, 0);
    lv_obj_set_style_bg_opa(f.screen, LV_OPA_COVER, 0);

    f.eye_l = make_blob(f.screen, EYE_W, EYE_H, PAL_PRIMARY, EYE_RADIUS);
    f.eye_r = make_blob(f.screen, EYE_W, EYE_H, PAL_PRIMARY, EYE_RADIUS);
    lv_obj_align(f.eye_l, LV_ALIGN_CENTER, -EYE_GAP / 2, EYE_Y - CY);
    lv_obj_align(f.eye_r, LV_ALIGN_CENTER,  EYE_GAP / 2, EYE_Y - CY);

    /* A small off-centre highlight in each eye. This is the cheapest trick
     * available for making a shape look like it is looking at you. */
    f.shine_l = make_blob(f.eye_l, 18, 18, PAL_PRIMARY_LIFT, 9);
    f.shine_r = make_blob(f.eye_r, 18, 18, PAL_PRIMARY_LIFT, 9);
    lv_obj_align(f.shine_l, LV_ALIGN_TOP_MID, 12, 16);
    lv_obj_align(f.shine_r, LV_ALIGN_TOP_MID, 12, 16);

    f.mouth = make_blob(f.screen, MOUTH_W, MOUTH_H, PAL_PRIMARY, MOUTH_H / 2);
    lv_obj_align(f.mouth, LV_ALIGN_CENTER, 0, MOUTH_Y - CY);

    f.state = FACE_BOOT;
    f.mouth_level = 0;
    render();

    /* 20 Hz. Fast enough for the mouth to track speech, slow enough to leave
     * the CPU to the audio path, which has much harder real-time constraints. */
    f.tick = lv_timer_create(tick_cb, 50, NULL);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "face up: %dx%d round", SCREEN, SCREEN);
}

void face_set_state(face_state_t state)
{
    if (state == f.state || state >= FACE_STATE_MAX) {
        return;
    }
    ESP_LOGI(TAG, "%s -> %s", face_state_name(f.state), face_state_name(state));
    lvgl_port_lock(0);
    f.state = state;
    if (state != FACE_SPEAKING) {
        f.mouth_level = 0;
    }
    render();
    lvgl_port_unlock();
}

face_state_t face_get_state(void)
{
    return f.state;
}

void face_set_mouth_level(uint8_t level)
{
    f.mouth_level = level;   /* picked up by the next tick; no lock needed */
}

void face_blink(void)
{
    f.blinking = true;
}
