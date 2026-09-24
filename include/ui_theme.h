/*
 * ui_theme.h - colour tokens and font roles of the touch UI (the new design).
 *
 * One theme is picked per process, before the first object exists
 * (ui_theme_select), and never changes: switching appearance execs a new
 * process (ui_exec.c). So pages read T->x at build time and never need to
 * restyle anything.
 *
 * Every colour is already quantised to RGB565 and contrast-checked against
 * the backgrounds it is used on (design doc §1). Rules that go with them:
 *   - text on a fill*: white; vivid colours (green/orange/red/blue) carry no text;
 *   - dimming = t3, never a lower opacity.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60PRO_UI_THEME_H
#define U60PRO_UI_THEME_H

#include "lvgl.h"

typedef struct {
    uint32_t bg, card, sep;                     /* canvas, card surface, row separator      */
    uint32_t t1, t2, t3;                        /* text: primary / secondary / auxiliary    */
    uint32_t okT, warnT, badT, accT;            /* status + link text                        */
    uint32_t fillBlue, fillOrange, fillGreen, fillRed;   /* fills that carry white text       */
    uint32_t green, orange, red, blue;          /* vivid, no text: switches, dots, charts    */
    uint32_t wash, washW, washB;                /* status block: good / warn / bad           */
    uint32_t glass, track, accS;                /* tab bar & sheets, off/unselected, selected */
    uint32_t pill;                              /* selected segment of a segmented control   */
    uint32_t onFill;                            /* secondary text on a fill* (white-ish)     */
    lv_opa_t hl_opa;                            /* card top highlight (dark only, 0 = none)  */
    lv_opa_t rim_opa;                           /* 1px black rim on glass                    */
    lv_opa_t scrim_opa;                         /* sheet backdrop                            */
    int      dark;
} ui_theme_t;

extern const ui_theme_t *T;

/* dark = 1 → dark table. Call once, before any lv_obj is created. */
void ui_theme_select(int dark);

/* ---- fonts ----
 * n*  = Nunito (rounded numerals; weight in the name) with the device CJK font
 *       as fallback, so "12 条" renders the digits in Nunito and 条 in CJK.
 * cj* = device CJK font (ZTEZhengYuan); *b = synthetic bold (LVGL patch).
 * Missing Nunito → device Roboto at the same sizes (no weights, not rounded);
 * missing Roboto too → the CJK font itself. Never NULL after ui_fonts_load. */
typedef struct {
    const lv_font_t *nbat, *n11, *n12, *n15, *n17, *n20, *n32, *n36;
    const lv_font_t *cj11, *cj12, *cj13, *cj14, *cj15;
    const lv_font_t *cj15b, *cj17b, *cj20b, *cj22b, *cj24b;
    const lv_font_t *cj20;                      /* regular 20: fallback under cj20b */
    const char *numerals;                       /* "nunito" / "roboto" / "cjk" — for the startup log */
} ui_fonts_t;

extern ui_fonts_t UF;

/* Load every font role. Paths: $U60_DEVUI_CJK_FONT (default the device font),
 * $U60_DEVUI_FONT_DIR/Nunito-{600,700,800}.ttf (default the plugin fonts dir),
 * $U60_DEVUI_ROBOTO. Env overrides exist for the offscreen render test.
 * Returns 0 if the CJK font loaded, -1 if everything fell back to Montserrat. */
int ui_fonts_load(void);

#endif /* U60PRO_UI_THEME_H */
