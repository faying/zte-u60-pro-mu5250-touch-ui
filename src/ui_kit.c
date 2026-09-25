/*
 * ui_kit.c - components of the touch UI; see ui_kit.h.
 *
 * Ported from the feasibility probe (designs/touch-ui-revamp-20260923/probe),
 * whose renders on the device are the reference for every size here.
 *
 * SPDX-License-Identifier: MIT
 */
#include "ui_kit.h"
#include "ui_logic.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C(x) lv_color_hex(x)



/* ------------------------------------------------------------ primitives */
static int32_t pw(lv_obj_t *p)
{
    int32_t w = lv_obj_get_style_width(p, LV_PART_MAIN);
    if (w > 0 && !LV_COORD_IS_SPEC(w)) return w;
    lv_obj_update_layout(p);
    return lv_obj_get_width(p);
}

lv_obj_t *uk_box(lv_obj_t *p, int x, int y, int w, int h, uint32_t col, int r)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);   /* decoration: taps go to the parent */
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_set_style_bg_color(o, C(col), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

lv_obj_t *uk_label(lv_obj_t *p, const lv_font_t *f, uint32_t col, int x, int y, const char *t)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, C(col), 0);
    lv_label_set_text(l, t ? t : "");
    lv_obj_set_pos(l, x, y);
    return l;
}

lv_obj_t *uk_label_r(lv_obj_t *p, const lv_font_t *f, uint32_t col, int xr, int y, const char *t)
{
    lv_obj_t *l = uk_label(p, f, col, 0, 0, t);
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, xr - pw(p), y);   /* re-applied whenever the text changes size */
    return l;
}

lv_obj_t *uk_label_w(lv_obj_t *p, const lv_font_t *f, uint32_t col, int x, int y, int w, int wrap, const char *t)
{
    lv_obj_t *l = uk_label(p, f, col, x, y, t);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, wrap ? LV_LABEL_LONG_MODE_WRAP : LV_LABEL_LONG_MODE_DOTS);
    if (!wrap) lv_obj_set_height(l, lv_font_get_line_height(f));   /* dots need a fixed height, else it wraps */
    return l;
}

void uk_text_color(lv_obj_t *l, uint32_t col) { lv_obj_set_style_text_color(l, C(col), 0); }
void uk_bg(lv_obj_t *o, uint32_t col) { lv_obj_set_style_bg_color(o, C(col), 0); }

lv_obj_t *uk_dot(lv_obj_t *p, int x, int y, int d, uint32_t col) { return uk_box(p, x, y, d, d, col, d / 2 + 1); }

void uk_show(lv_obj_t *o, int visible)
{
    if (!o) return;
    if (visible) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* Pressed = one shade towards the page: darker on light, lighter on dark.
 * A filter rather than a fixed colour, so it also works on objects whose
 * background refresh_cb recolours (tiles, options, armed buttons).
 * A filter only tints what is drawn, so list rows with a transparent
 * background also get a real pressed background (uk_tappable). Without it a
 * press on a row showed nothing at all (2026-09-25). */
static lv_color_t press_filter_cb(const lv_color_filter_dsc_t *d, lv_color_t c, lv_opa_t opa)
{
    (void)d;
    return T->dark ? lv_color_lighten(c, opa) : lv_color_darken(c, opa);
}
static lv_color_filter_dsc_t s_press_filter;

void uk_tappable(lv_obj_t *o, lv_event_cb_t cb, void *user)
{
    static int init;
    if (!init) { lv_color_filter_dsc_init(&s_press_filter, press_filter_cb); init = 1; }
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_color_filter_dsc(o, &s_press_filter, LV_STATE_PRESSED);
    lv_obj_set_style_color_filter_opa(o, 40, LV_STATE_PRESSED);
    if (lv_obj_get_style_bg_opa(o, LV_PART_MAIN) < LV_OPA_50) {
        lv_obj_set_style_bg_color(o, C(T->track), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
    }
    if (cb) lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, user);
    int32_t h = lv_obj_get_style_height(o, 0), w = lv_obj_get_style_width(o, 0);
    int32_t small = LV_MIN(h > 0 && !LV_COORD_IS_SPEC(h) ? h : 40, w > 0 && !LV_COORD_IS_SPEC(w) ? w : 40);
    if (small < 40) lv_obj_set_ext_click_area(o, (40 - small + 1) / 2);
}

/* ------------------------------------------------------------ cards, rows */
lv_obj_t *uk_section(lv_obj_t *p, int y, const char *title)
{
    return uk_label(p, UF.cj12, T->t3, UK_MARGIN + 6, y, title);
}

lv_obj_t *uk_card(lv_obj_t *p, int x, int y, int w, int h)
{
    lv_obj_t *c = uk_box(p, x, y, w, h, T->card, UK_R_CARD);
    if (T->dark) {   /* dark cards: 1 px top highlight instead of a shadow */
        lv_obj_set_style_border_side(c, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_color(c, lv_color_white(), 0);
        lv_obj_set_style_border_opa(c, T->hl_opa, 0);
    }
    return c;
}

lv_obj_t *uk_sep(lv_obj_t *c, int y) { return uk_box(c, UK_PAD, y, pw(c) - UK_PAD, 1, T->sep, 0); }

lv_obj_t *uk_row(lv_obj_t *c, int y, const char *key, int first)
{
    if (!first) uk_sep(c, y);
    uk_label(c, UF.cj14, T->t2, UK_PAD, y + 11, key);
    return uk_label_r(c, UF.n15, T->t1, pw(c) - UK_PAD, y + 10, "");
}

lv_obj_t *uk_chevron(lv_obj_t *c, int y) { return uk_label_r(c, UF.cj15, T->t3, pw(c) - UK_PAD, y + 10, "›"); }

lv_obj_t *uk_row_nav(lv_obj_t *c, int y, const char *key, int first, lv_event_cb_t cb, void *user)
{
    lv_obj_t *hit = uk_box(c, 0, y, pw(c), UK_ROW_H, T->card, 0);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(hit, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(hit, C(T->track), LV_STATE_PRESSED);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(hit, cb, LV_EVENT_CLICKED, user);
    if (!first) uk_sep(c, y);
    uk_label(c, UF.cj14, T->t1, UK_PAD, y + 11, key);
    uk_chevron(c, y);
    return uk_label_r(c, UF.cj13, T->t2, pw(c) - UK_PAD - 16, y + 12, "");
}

/* ------------------------------------------------------------ controls */
void uk_seg(uk_seg_t *s, lv_obj_t *p, int x, int y, int w, const char *const *items, int n, lv_event_cb_t cb)
{
    memset(s, 0, sizeof *s);
    s->n = n;
    s->sel = -1;
    s->obj = uk_box(p, x, y, w, 30, T->track, 15);
    int iw = (w - 4) / n;
    for (int i = 0; i < n; i++) {
        lv_obj_t *it = uk_box(s->obj, 2 + i * iw, 2, iw, 26, T->pill, 13);
        lv_obj_set_style_bg_opa(it, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_color(it, lv_color_black(), 0);
        lv_obj_set_style_shadow_offset_y(it, 1, 0);
        uk_tappable(it, cb, (void *)(intptr_t)i);
        lv_obj_set_ext_click_area(it, 7);
        s->item[i] = it;
        s->lbl[i] = uk_label(it, UF.cj13, T->t2, 0, 0, items[i]);
        lv_obj_center(s->lbl[i]);
    }
}

void uk_seg_set(uk_seg_t *s, int sel)
{
    s->sel = sel;
    for (int i = 0; i < s->n; i++) {
        int on = i == sel;
        lv_obj_set_style_bg_color(s->item[i], C(T->pill), 0);
        lv_obj_set_style_bg_opa(s->item[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(s->item[i], on ? 4 : 0, 0);
        lv_obj_set_style_shadow_opa(s->item[i], on ? 40 : 0, 0);
        uk_text_color(s->lbl[i], on ? T->t1 : T->t2);
    }
}

void uk_seg_arm(uk_seg_t *s, int i)
{
    if (i < 0 || i >= s->n) return;
    lv_obj_set_style_bg_color(s->item[i], C(T->fillOrange), 0);
    lv_obj_set_style_bg_opa(s->item[i], LV_OPA_COVER, 0);
    uk_text_color(s->lbl[i], 0xffffff);
}

lv_obj_t *uk_toggle(lv_obj_t *p, int xr, int y, lv_event_cb_t cb, void *user)
{
    lv_obj_t *sw = lv_switch_create(p);
    lv_obj_set_size(sw, 44, 26);
    lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, xr - pw(p), y);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_style_bg_color(sw, C(T->track), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, C(T->green), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    /* oval thumb: the knob widened with negative padding */
    lv_obj_set_style_pad_left(sw, -5, LV_PART_KNOB);
    lv_obj_set_style_pad_right(sw, -5, LV_PART_KNOB);
    lv_obj_set_style_pad_top(sw, -2, LV_PART_KNOB);
    lv_obj_set_style_pad_bottom(sw, -2, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sw, 4, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(sw, 60, LV_PART_KNOB);
    lv_obj_set_style_shadow_offset_y(sw, 1, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, 5, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(sw, 0, 0);
    lv_obj_set_ext_click_area(sw, 7);
    if (cb) lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, user);
    return sw;
}

void uk_toggle_set(lv_obj_t *sw, int on)
{
    if (!sw || on == lv_obj_has_state(sw, LV_STATE_CHECKED)) return;
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else    lv_obj_remove_state(sw, LV_STATE_CHECKED);
}

lv_obj_t *uk_slider(lv_obj_t *p, int x, int y, int w)
{
    lv_obj_t *sl = lv_slider_create(p);
    lv_obj_set_pos(sl, x, y);
    lv_obj_set_size(sl, w, 6);
    lv_obj_remove_flag(sl, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_style_bg_color(sl, C(T->track), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, C(T->blue), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_hor(sl, 11, LV_PART_KNOB);   /* 28×22 capsule knob */
    lv_obj_set_style_pad_ver(sl, 8, LV_PART_KNOB);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sl, 6, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(sl, 70, LV_PART_KNOB);
    lv_obj_set_ext_click_area(sl, 17);
    return sl;
}

void uk_button_kind(lv_obj_t *b, lv_obj_t *lbl, uk_btn_kind_t k)
{
    uint32_t bg = k == UK_BTN_PRIMARY ? T->fillBlue : k == UK_BTN_ARMED ? T->fillOrange : T->track;
    uint32_t fg = k == UK_BTN_PLAIN ? T->accT : k == UK_BTN_DANGER ? T->badT : 0xffffff;
    uk_bg(b, bg);
    if (lbl) uk_text_color(lbl, fg);
}

lv_obj_t *uk_button(lv_obj_t *p, int x, int y, int w, int h, const char *text, uk_btn_kind_t k,
                    lv_event_cb_t cb, void *user, lv_obj_t **lbl)
{
    lv_obj_t *b = uk_box(p, x, y, w > 0 ? w : 80, h, T->track, h / 2);
    lv_obj_t *l = uk_label(b, UF.cj15b, T->accT, 0, 0, text);
    if (w <= 0) {
        lv_obj_set_width(b, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(b, 16, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    }
    lv_obj_center(l);
    uk_button_kind(b, l, k);
    uk_tappable(b, cb, user);
    if (lbl) *lbl = l;
    return b;
}

lv_obj_t *uk_chip(lv_obj_t *p, int x, int y, const char *text, lv_obj_t **lbl)
{
    lv_obj_t *ch = uk_box(p, x, y, 10, 28, T->track, 14);
    lv_obj_t *l = uk_label(ch, UF.n15, T->t2, 11, 4, text);
    lv_obj_update_layout(l);
    lv_obj_set_width(ch, lv_obj_get_width(l) + 22);
    if (lbl) *lbl = l;
    return ch;
}

void uk_chip_set(lv_obj_t *chip, lv_obj_t *lbl, int on)
{
    uk_bg(chip, on ? T->fillBlue : T->track);
    uk_text_color(lbl, on ? 0xffffff : T->t2);
}

int uk_chip_w(lv_obj_t *chip) { return (int)lv_obj_get_style_width(chip, 0); }

void uk_opt(uk_opt_t *o, lv_obj_t *p, int x, int y, const char *title, const char *sub, lv_event_cb_t cb, void *user)
{
    o->obj = uk_box(p, x, y, 134, 46, T->track, UK_R_OPT);
    o->t = uk_label(o->obj, UF.cj14, T->t1, 0, 0, title);
    lv_obj_align(o->t, LV_ALIGN_TOP_MID, 0, 5);
    o->s = uk_label(o->obj, UF.cj12, T->t3, 0, 0, sub);
    lv_obj_align(o->s, LV_ALIGN_TOP_MID, 0, 26);
    uk_tappable(o->obj, cb, user);
}

void uk_opt_set(uk_opt_t *o, int on, int armed)
{
    uk_bg(o->obj, armed ? T->fillOrange : on ? T->fillBlue : T->track);
    uk_text_color(o->t, on || armed ? 0xffffff : T->t1);
    uk_text_color(o->s, on || armed ? T->onFill : T->t3);
}

/* ------------------------------------------------------------ tiles */
void uk_tile(uk_tile_t *t, lv_obj_t *p, int x, int y, const char *icon, const char *name,
             lv_event_cb_t cb, void *user)
{
    t->obj = uk_card(p, x, y, 145, 78);
    t->icon = uk_label(t->obj, &lv_font_montserrat_16, T->accT, 12, 12, icon);
    t->name = uk_label(t->obj, UF.cj15, T->t1, 36, 12, name);
    t->sub = uk_label_w(t->obj, UF.cj12, T->t2, 12, 50, 121, 0, "");
    t->dot = uk_dot(t->obj, 12, 55, 7, T->green);
    uk_show(t->dot, 0);
    t->badge = uk_box(t->obj, 145 - 12 - 22, 12, 22, 18, T->fillRed, 9);
    t->badge_l = uk_label(t->badge, UF.n12, 0xffffff, 0, 0, "");
    lv_obj_center(t->badge_l);
    uk_show(t->badge, 0);
    uk_tappable(t->obj, cb, user);
}

/* 「开着」= a green dot before the subtitle and the subtitle in okT; the tile
 * itself stays white. (A filled tile read as "stuck selected", 2026-09-24.) */
void uk_tile_set(uk_tile_t *t, int on, int dim, int badge)
{
    uk_show(t->dot, on);
    lv_obj_set_x(t->sub, on ? 24 : 12);
    uk_text_color(t->name, dim ? T->t3 : T->t1);
    uk_text_color(t->sub, on ? T->okT : T->t2);
    if (badge > 0) {
        char b[8];
        snprintf(b, sizeof b, "%d", badge > 99 ? 99 : badge);
        lv_label_set_text(t->badge_l, b);
    }
    uk_show(t->badge, badge > 0);
}

/* ------------------------------------------------------------ status block */
void uk_hero(uk_hero_t *h, lv_obj_t *card, const lv_font_t *big_font)
{
    int cw = pw(card);
    h->wash = uk_box(card, 0, 0, cw, UK_HERO_H, T->wash, UK_R_CARD);
    /* LVGL has no per-corner radius: a square filler covers the wash's bottom corners */
    h->wash2 = uk_box(card, 0, UK_HERO_H - 20, cw, 20, T->wash, 0);
    h->dot = uk_dot(card, 14, 15, 7, T->green);
    h->st = uk_label(card, UF.cj13, T->okT, 26, 10, "");
    h->rtop = uk_label_w(card, UF.cj13, T->t2, cw - UK_PAD - 170, 10, 170, 0, "");
    lv_obj_set_style_text_align(h->rtop, LV_TEXT_ALIGN_RIGHT, 0);
    h->big = uk_label(card, big_font, T->t1, 12, big_font == UF.n32 ? 30 : 34, "");
    h->unit = uk_label(card, UF.n15, T->t2, 60, 44, "");
    h->r1 = uk_label_r(card, UF.n17, T->t1, cw - UK_PAD, 30, "");
    h->r2 = uk_label_w(card, UF.cj12, T->t2, cw - UK_PAD - 150, 52, 150, 0, "");
    lv_obj_set_style_text_align(h->r2, LV_TEXT_ALIGN_RIGHT, 0);
}

void uk_hero_tone(uk_hero_t *h, int tone)
{
    static const int n = 5;
    uint32_t wash[5] = { T->wash, T->washW, T->washB, T->track, T->accS };
    uint32_t text[5] = { T->okT, T->warnT, T->badT, T->t3, T->accT };
    uint32_t dot[5]  = { T->green, T->orange, T->red, T->t3, T->blue };
    if (tone < 0 || tone >= n) tone = 3;
    uk_bg(h->wash, wash[tone]);
    uk_bg(h->wash2, wash[tone]);
    uk_bg(h->dot, dot[tone]);
    uk_text_color(h->st, text[tone]);
    uk_text_color(h->big, tone == 3 ? T->t3 : T->t1);
}

void uk_hero_layout(uk_hero_t *h)
{
    lv_obj_update_layout(h->big);
    lv_obj_set_x(h->unit, lv_obj_get_x(h->big) + lv_obj_get_width(h->big) + 3);
}

/* ------------------------------------------------------------ battery */
void uk_battery(uk_battery_t *b, lv_obj_t *p, int xr, int y)
{
    b->xr = xr;
    b->body = uk_box(p, xr - 30, y, 27, 14, T->t1, 5);
    lv_obj_set_style_clip_corner(b->body, true, 0);
    b->fill = uk_box(b->body, 0, 0, 3, 14, T->red, 0);
    b->digits = uk_label(b->body, UF.nbat, T->bg, 0, 0, "");
    b->nub = uk_box(p, xr - 2, y + 4, 2, 6, T->t1, 1);
    lv_obj_set_style_bg_opa(b->nub, 140, 0);
}

int uk_battery_w(int pct, int charging) { return ui_bat_body_w(pct, charging) + 3; }

/* Centre the digits on their ink, not on the label box: the box carries the
 * font's descender and side bearings, so box-centred digits sat high and to
 * the right on the device fonts (reported 2026-09-25). */
static void bat_digits_place(lv_obj_t *l, const char *s, int x0, int w, int h)
{
    const lv_font_t *f = lv_obj_get_style_text_font(l, 0);
    int pen = 0, il = 0, ir = 0, top = 1000, bot = -1000, any = 0;
    for (const char *c = s; *c; c++) {
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(f, &g, (uint32_t)(unsigned char)c[0], (uint32_t)(unsigned char)c[1])) continue;
        if (!any) il = pen + g.ofs_x;
        ir = pen + g.ofs_x + g.box_w;
        int gt = f->line_height - f->base_line - (g.ofs_y + g.box_h);
        int gb = f->line_height - f->base_line - g.ofs_y;
        if (gt < top) top = gt;
        if (gb > bot) bot = gb;
        pen += g.adv_w;
        any = 1;
    }
    if (!any) { lv_obj_align(l, LV_ALIGN_CENTER, 0, 0); return; }
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x0 + (w - (ir - il)) / 2 - il, (h - (bot - top)) / 2 - top);
}

/* Normal: solid body, digits knocked out in the page colour. Charging: the
 * whole body vivid green (as the approved mock; no bolt, width unchanged).
 * ≤ 20 %: grey body with a red sliver, digits stay clear of it. */
void uk_battery_set(uk_battery_t *b, int pct, int charging)
{
    char s[8];
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    snprintf(s, sizeof s, "%d", pct);
    ui_bat_state_t st = ui_bat_state(pct, charging);
    int w = ui_bat_body_w(pct, charging);
    uint32_t body = st == UI_BAT_CHARGING ? T->green : st == UI_BAT_LOW ? T->track : T->t1;
    uint32_t fg = st == UI_BAT_CHARGING ? (T->dark ? T->bg : 0xffffff) : st == UI_BAT_LOW ? T->t1 : T->bg;
    lv_obj_set_width(b->body, w);
    lv_obj_set_x(b->body, b->xr - w - 3);
    uk_bg(b->body, body);
    lv_label_set_text(b->digits, s);
    uk_text_color(b->digits, fg);
    uk_show(b->fill, st == UI_BAT_LOW);
    if (st == UI_BAT_LOW) {
        lv_obj_set_width(b->fill, ui_bat_red_w(pct, w));
        bat_digits_place(b->digits, s, 7, w - 7, 14);
    } else {
        bat_digits_place(b->digits, s, 0, w, 14);
    }
    uk_bg(b->nub, st == UI_BAT_CHARGING ? T->green : T->t1);
}

/* ------------------------------------------------------------ charts */
/* Area under the first series: flat low-opacity triangles + rects (LVGL has
 * no area series). */
static void chart_fill_cb(lv_event_t *e)
{
    lv_draw_task_t *dt = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(dt);
    if (base->part != LV_PART_ITEMS || lv_draw_task_get_type(dt) != LV_DRAW_TASK_TYPE_LINE) return;
    lv_obj_t *obj = lv_event_get_target_obj(e);
    if (base->id1 == 1) {   /* second series: dashed */
        lv_draw_line_dsc_t *ld = lv_draw_task_get_line_dsc(dt);
        ld->dash_width = 4;
        ld->dash_gap = 3;
        return;
    }
    if (base->id1 != 0) return;
    lv_area_t co;
    lv_obj_get_coords(obj, &co);
    lv_color_t col = lv_chart_get_series_color(obj, lv_chart_get_series_next(obj, NULL));
    lv_draw_line_dsc_t *ld = lv_draw_task_get_line_dsc(dt);
    for (int i = 0; i < (int)ld->point_cnt - 1; i++) {
        lv_point_precise_t p1 = ld->points[i], p2 = ld->points[i + 1];
        if (p1.x == LV_DRAW_LINE_POINT_NONE || p2.x == LV_DRAW_LINE_POINT_NONE) continue;
        lv_draw_triangle_dsc_t td;
        lv_draw_triangle_dsc_init(&td);
        td.p[0] = p1;
        td.p[1] = p2;
        td.p[2].x = p1.y < p2.y ? p1.x : p2.x;
        td.p[2].y = LV_MAX(p1.y, p2.y);
        td.color = col;
        td.opa = 36;
        lv_draw_triangle(base->layer, &td);
        lv_draw_rect_dsc_t rd;
        lv_draw_rect_dsc_init(&rd);
        rd.bg_color = col;
        rd.bg_opa = 36;
        lv_area_t a = { (int32_t)p1.x, (int32_t)LV_MAX(p1.y, p2.y), (int32_t)p2.x - 1, co.y2 };
        if (a.x2 >= a.x1 && a.y2 >= a.y1) lv_draw_rect(base->layer, &rd, &a);
    }
}

lv_obj_t *uk_chart(lv_obj_t *p, int x, int y, int w, int h, int points, uint32_t c1, uint32_t c2,
                   lv_chart_series_t **s1, lv_chart_series_t **s2)
{
    lv_obj_t *ch = lv_chart_create(p);
    lv_obj_set_pos(ch, x, y);
    lv_obj_set_size(ch, w, h);
    lv_obj_remove_flag(ch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ch, 0, 0);
    lv_obj_set_style_pad_all(ch, 0, 0);
    lv_obj_set_style_radius(ch, 0, 0);
    lv_obj_set_style_line_color(ch, C(T->sep), LV_PART_MAIN);
    lv_obj_set_style_line_width(ch, 1, LV_PART_MAIN);
    lv_chart_set_div_line_count(ch, 3, 0);
    lv_obj_set_style_size(ch, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(ch, 2, LV_PART_ITEMS);
    lv_chart_set_type(ch, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ch, (uint32_t)points);
    lv_chart_set_update_mode(ch, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_axis_range(ch, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_obj_add_event_cb(ch, chart_fill_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_add_flag(ch, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    *s1 = lv_chart_add_series(ch, C(c1), LV_CHART_AXIS_PRIMARY_Y);
    if (s2) *s2 = c2 ? lv_chart_add_series(ch, C(c2), LV_CHART_AXIS_PRIMARY_Y) : NULL;
    lv_chart_set_all_values(ch, *s1, LV_CHART_POINT_NONE);
    if (s2 && *s2) lv_chart_set_all_values(ch, *s2, LV_CHART_POINT_NONE);
    return ch;
}

/* ------------------------------------------------------------ scrolling */
lv_obj_t *uk_scroll(lv_obj_t *p, int y, int view_h, int content_h)
{
    lv_obj_t *sc = lv_obj_create(p);
    lv_obj_remove_style_all(sc);
    lv_obj_set_pos(sc, 0, y);
    lv_obj_set_size(sc, UK_W, view_h);
    lv_obj_set_scroll_dir(sc, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sc, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(sc, C(T->t3), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(sc, 110, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(sc, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(sc, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(sc, 2, LV_PART_SCROLLBAR);
    lv_obj_t *sp = lv_obj_create(sc);   /* child 0: fixes the scroll range */
    lv_obj_remove_style_all(sp);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(sp, 1, 1);
    lv_obj_set_pos(sp, 0, content_h - 1);
    return sc;
}

void uk_scroll_extent(lv_obj_t *sc, int content_h)
{
    lv_obj_t *sp = lv_obj_get_child(sc, 0);
    if (sp) lv_obj_set_y(sp, content_h - 1);
}

/* ------------------------------------------------------------ animation */
static void anim_tx(void *o, int32_t v) { lv_obj_set_style_translate_x((lv_obj_t *)o, v, 0); }
static void anim_ty(void *o, int32_t v) { lv_obj_set_style_translate_y((lv_obj_t *)o, v, 0); }

static void anim_run(lv_obj_t *o, lv_anim_exec_xcb_t cb, int from, int ms)
{
#if UI_ANIM
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, 0);
    lv_anim_set_duration(&a, (uint32_t)ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
#else
    (void)from; (void)ms;
    cb(o, 0);
#endif
}

void uk_anim_push(lv_obj_t *o) { anim_run(o, anim_tx, UK_W, 150); }
void uk_anim_rise(lv_obj_t *o, int from_dy) { anim_run(o, anim_ty, from_dy, 180); }

/* ------------------------------------------------------------ sheet */
static lv_obj_t *glass(lv_obj_t *p, int x, int y, int w, int h, int r)
{
    lv_obj_t *g = uk_box(p, x, y, w, h, T->glass, r);
    lv_obj_set_style_border_width(g, 1, 0);
    lv_obj_set_style_border_color(g, lv_color_black(), 0);
    lv_obj_set_style_border_opa(g, T->rim_opa, 0);
    return g;
}

void uk_sheet(uk_sheet_t *s, int panel_h, lv_event_cb_t cancel_cb)
{
    lv_obj_t *top = lv_layer_top();
    s->panel_h = panel_h;
    s->scrim = uk_box(top, 0, 0, UK_W, UK_H, 0x000000, 0);
    lv_obj_set_style_bg_opa(s->scrim, T->scrim_opa, 0);
    lv_obj_add_flag(s->scrim, LV_OBJ_FLAG_CLICKABLE);   /* swallows taps; tapping it cancels */
    if (cancel_cb) lv_obj_add_event_cb(s->scrim, cancel_cb, LV_EVENT_CLICKED, NULL);
    s->panel = glass(s->scrim, 8, UK_H - 8 - 46 - 8 - panel_h, UK_W - 16, panel_h, UK_R_SHEET);
    lv_obj_add_flag(s->panel, LV_OBJ_FLAG_CLICKABLE);   /* taps between items don't reach the scrim */
    s->cancel = glass(s->scrim, 8, UK_H - 8 - 46, UK_W - 16, 46, UK_R_SHEET);
    s->cancel_l = uk_label(s->cancel, UF.cj17b, T->accT, 0, 0, "取消");
    lv_obj_center(s->cancel_l);
    uk_tappable(s->cancel, cancel_cb, NULL);
    uk_show(s->scrim, 0);
}

void uk_sheet_show(uk_sheet_t *s, int show)
{
    int was = uk_sheet_visible(s);
    uk_show(s->scrim, show);
    if (show && !was) {
        lv_obj_move_foreground(s->scrim);
        uk_anim_rise(s->panel, s->panel_h + 62);
        uk_anim_rise(s->cancel, s->panel_h + 62);
    }
}

int uk_sheet_visible(const uk_sheet_t *s) { return s->scrim && !lv_obj_has_flag(s->scrim, LV_OBJ_FLAG_HIDDEN); }

lv_obj_t *uk_sheet_item(uk_sheet_t *s, int y, const char *text, uint32_t col, lv_event_cb_t cb, void *user, lv_obj_t **row)
{
    int w = UK_W - 16;
    lv_obj_t *r = uk_box(s->panel, 0, y, w, 40, T->glass, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(r, C(T->track), LV_STATE_PRESSED);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, user);
    uk_box(s->panel, 12, y, w - 24, 1, T->sep, 0);
    lv_obj_t *l = uk_label(r, UF.cj15, col, 0, 0, text);
    lv_obj_center(l);
    if (row) *row = r;
    return l;
}
