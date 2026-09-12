/* Palette — see docs/DESIGN-palette.md for the reasoning and contrast figures.
 *
 * Mint green on a near-black ground. The background is deliberately not pure
 * black: the slight blue-violet cast is what keeps the mint from reading as
 * harsh and gives the face something to sit on.
 *
 * Every value here has been checked through RGB565 and back, because the panel
 * is 16-bit and two tokens that differ in hex can render identically on
 * hardware. Green gets 6 bits in RGB565 against 5 for red and blue, so a
 * green-primary design gets the smoothest gradients this display can produce.
 */

#pragma once

#include "lvgl.h"

/* Core */
#define PAL_PRIMARY      lv_color_hex(0x88F1B8)  /* active face, speaking      */
#define PAL_PRIMARY_LIFT lv_color_hex(0xBEF7D8)  /* highlight, eye shine       */
#define PAL_PRIMARY_DIM  lv_color_hex(0x518B6D)  /* idle standby face          */
#define PAL_PRIMARY_DEEP lv_color_hex(0x304E40)  /* outlines; shape only, 2.1:1 */
#define PAL_BG           lv_color_hex(0x0E0E12)  /* screen background          */
#define PAL_BG_RAISE     lv_color_hex(0x18201F)  /* panel behind the face      */

/* Reserved. A face going off-palette should mean something is genuinely wrong;
 * if amber shows up casually it stops carrying any signal. */
#define PAL_ALERT        lv_color_hex(0xFFB46B)
#define PAL_ERROR        lv_color_hex(0xFF6B6B)
