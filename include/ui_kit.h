/*
 * ui_kit.h - the touch UI's components (the new design, design doc §3).
 *
 * Every page is built from these and nothing else sets a colour, radius or
 * font on its own: colours come from T (ui_theme.h), fonts from UF. Sizes and
 * behaviour were verified offscreen on the device (lvprobe, 2026-09-23).
 *
 * Positions are relative to the parent. "xr" means "right edge at xr": such
 * labels are aligned to the parent's right side, so they stay right-aligned
 * when refresh_cb changes their text.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60PRO_UI_KIT_H
#define U60PRO_UI_KIT_H

#include "lvgl.h"
#include "ui_theme.h"

#define UK_W        320
#define UK_H        480
#define UK_BAR_H    26      /* status bar                                   */
#define UK_NAV_H    62      /* subpage header, status bar included          */
#define UK_TAB_H    44      /* floating tab capsule                         */
#define UK_TAB_GAP  8       /* capsule to screen bottom                     */
#define UK_TAB_PAD  60      /* scroll content keeps this clear at the end   */
#define UK_MARGIN   10      /* card to screen edge, card to card            */
#define UK_CARD_W   (UK_W - 2 * UK_MARGIN)   /* 300 */
#define UK_PAD      14      /* row inset inside a card                      */
#define UK_ROW_H    40
#define UK_ROW2_H   50
#define UK_HERO_H   72
#define UK_R_CARD   20
#define UK_R_OPT    14
#define UK_R_SHEET  22

#ifndef UI_ANIM
#define UI_ANIM 1           /* 0 = no animation anywhere (design doc §3) */
#endif

/* ---- primitives ---- */
lv_obj_t *uk_box(lv_obj_t *p, int x, int y, int w, int h, uint32_t col, int r);
lv_obj_t *uk_label(lv_obj_t *p, const lv_font_t *f, uint32_t col, int x, int y, const char *t);
lv_obj_t *uk_label_r(lv_obj_t *p, const lv_font_t *f, uint32_t col, int xr, int y, const char *t);
/* Width-limited label: text past `w` ends in "…" (names), or wraps (wrap=1). */
lv_obj_t *uk_label_w(lv_obj_t *p, const lv_font_t *f, uint32_t col, int x, int y, int w, int wrap, const char *t);
void      uk_text_color(lv_obj_t *l, uint32_t col);
void      uk_bg(lv_obj_t *o, uint32_t col);
lv_obj_t *uk_dot(lv_obj_t *p, int x, int y, int d, uint32_t col);
void      uk_show(lv_obj_t *o, int visible);
/* Make `o` tappable with the standard pressed feedback (bg → track; for a
 * filled object: one step darker). Hit area grows to ≥ 40×40. */
void      uk_tappable(lv_obj_t *o, lv_event_cb_t cb, void *user);

/* ---- cards and rows ---- */
lv_obj_t *uk_section(lv_obj_t *p, int y, const char *title);          /* 12 px t3 above a card */
lv_obj_t *uk_card(lv_obj_t *p, int x, int y, int w, int h);
lv_obj_t *uk_sep(lv_obj_t *c, int y);                                  /* from x=14 to the edge */
/* A 40 px key/value row at y inside card c; returns the value label
 * (right-aligned, n15). first = no separator above. */
lv_obj_t *uk_row(lv_obj_t *c, int y, const char *key, int first);
/* A tappable row with "›" at the right; the value label ends before it. */
lv_obj_t *uk_row_nav(lv_obj_t *c, int y, const char *key, int first, lv_event_cb_t cb, void *user);
lv_obj_t *uk_chevron(lv_obj_t *c, int y);

/* ---- controls ---- */
typedef struct {
    lv_obj_t *obj, *item[4], *lbl[4];
    int n, sel;
} uk_seg_t;
/* Segmented control, 30 px high. cb gets user = index. */
void uk_seg(uk_seg_t *s, lv_obj_t *p, int x, int y, int w, const char *const *items, int n, lv_event_cb_t cb);
void uk_seg_set(uk_seg_t *s, int sel);
/* Draw item i as "armed" (first tap of a two-step confirm: fillOrange). */
void uk_seg_arm(uk_seg_t *s, int i);

lv_obj_t *uk_toggle(lv_obj_t *p, int xr, int y, lv_event_cb_t cb, void *user);
void      uk_toggle_set(lv_obj_t *sw, int on);
lv_obj_t *uk_slider(lv_obj_t *p, int x, int y, int w);

typedef enum { UK_BTN_PRIMARY, UK_BTN_PLAIN, UK_BTN_DANGER, UK_BTN_ARMED } uk_btn_kind_t;
/* Capsule button; returns it and its label via *lbl. w = 0: fit the text. */
lv_obj_t *uk_button(lv_obj_t *p, int x, int y, int w, int h, const char *text, uk_btn_kind_t k,
                    lv_event_cb_t cb, void *user, lv_obj_t **lbl);
void      uk_button_kind(lv_obj_t *b, lv_obj_t *lbl, uk_btn_kind_t k);

/* Band chip (28 high, fits the text); selected = fillBlue. */
lv_obj_t *uk_chip(lv_obj_t *p, int x, int y, const char *text, lv_obj_t **lbl);
void      uk_chip_set(lv_obj_t *chip, lv_obj_t *lbl, int on);
int       uk_chip_w(lv_obj_t *chip);

/* 2×2 option (134×46): title + subtitle; selected = fillBlue. */
typedef struct { lv_obj_t *obj, *t, *s; } uk_opt_t;
void uk_opt(uk_opt_t *o, lv_obj_t *p, int x, int y, const char *title, const char *sub, lv_event_cb_t cb, void *user);
void uk_opt_set(uk_opt_t *o, int on, int armed);

/* ---- tiles (功能) ---- */
typedef struct { lv_obj_t *obj, *icon, *name, *sub, *badge, *badge_l, *dot; } uk_tile_t;
void uk_tile(uk_tile_t *t, lv_obj_t *p, int x, int y, const char *icon, const char *name,
             lv_event_cb_t cb, void *user);
void uk_tile_set(uk_tile_t *t, int on, int dim, int badge);

/* ---- status block at the top of a card ---- */
typedef struct { lv_obj_t *wash, *wash2, *dot, *st, *rtop, *big, *unit, *r1, *r2; } uk_hero_t;
/* big_font: UF.n32 for numbers, UF.cj22b/cj24b for words. */
void uk_hero(uk_hero_t *h, lv_obj_t *card, const lv_font_t *big_font);
/* tone: 0 good, 1 warn, 2 bad, 3 neutral (track, t3 text), 4 selected (accS) */
void uk_hero_tone(uk_hero_t *h, int tone);
/* Place the unit right after the big number (call after changing big). */
void uk_hero_layout(uk_hero_t *h);

/* ---- status-bar battery ---- */
typedef struct { lv_obj_t *body, *fill, *digits, *nub; int xr; } uk_battery_t;
void uk_battery(uk_battery_t *b, lv_obj_t *p, int xr, int y);
void uk_battery_set(uk_battery_t *b, int pct, int charging);
int  uk_battery_w(int pct, int charging);   /* body + nub + gap: what the rate must keep clear of */

/* ---- charts ---- */
/* Line chart without its own background; second series (c2 != 0) is dashed.
 * The first series gets a low-opacity area fill. */
lv_obj_t *uk_chart(lv_obj_t *p, int x, int y, int w, int h, int points, uint32_t c1, uint32_t c2,
                   lv_chart_series_t **s1, lv_chart_series_t **s2);

/* ---- scrolling ---- */
/* Scroll body of exactly the visible area; content_h fixes the scroll range
 * (a 1 px spacer), so reflowing cards never moves it under the finger. */
lv_obj_t *uk_scroll(lv_obj_t *p, int y, int view_h, int content_h);
void      uk_scroll_extent(lv_obj_t *sc, int content_h);

/* ---- bottom sheet ---- */
typedef struct { lv_obj_t *scrim, *panel, *cancel, *cancel_l; int panel_h; } uk_sheet_t;
/* On lv_layer_top(); hidden until uk_sheet_show. cancel_cb closes it. */
void uk_sheet(uk_sheet_t *s, int panel_h, lv_event_cb_t cancel_cb);
void uk_sheet_show(uk_sheet_t *s, int show);
int  uk_sheet_visible(const uk_sheet_t *s);
/* A full-width option line in a sheet at y; returns its label. */
lv_obj_t *uk_sheet_item(uk_sheet_t *s, int y, const char *text, uint32_t col, lv_event_cb_t cb, void *user, lv_obj_t **row);

/* Slide o in from the right (150 ms) / from below (180 ms), ease-out. */
void uk_anim_push(lv_obj_t *o);
void uk_anim_rise(lv_obj_t *o, int from_dy);

#endif
