/*
 * render_test - draw every page of the touch UI offscreen, for one scene and
 * one appearance, and check it:
 *   - each screenful (pages taller than the screen are shot at every scroll
 *     step) is hashed and compared with tests/render/golden/<scene>-<theme>.txt;
 *   - every line of tests/render/expect.txt that applies to this scene must
 *     appear in a visible label on its page (the controls inventory, made
 *     executable: drop a control and this fails);
 *   - a few behaviours: two-step confirms after the first tap, the 外观
 *     switch execs with the current tab.
 *
 * ui.c is included, not linked, so the harness can open subpages and poke
 * the same static objects a finger would. Every device-facing interface is
 * a fixture (fixtures.c). Docker renders byte-identically to the device
 * (lvprobe, 2026-09-23), so the hashes mean the same thing on both.
 *
 *   render_test --scene=good --theme=light [--png=DIR] [--golden=FILE | --write-golden=FILE]
 *               [--expect=FILE]
 * Fonts: U60_DEVUI_CJK_FONT, U60_DEVUI_FONT_DIR, U60_DEVUI_ROBOTO (ui_theme.c).
 *
 * SPDX-License-Identifier: MIT
 */
#include "src/misc/lv_text_private.h"   /* lv_text_encoded_next */
#include "src/misc/lv_area_private.h"   /* lv_area_intersect */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lvgl.h"
#include "fixtures.h"

/* ---- what ui.c must not really do in a test ---- */
static const char *rt_conf_path;
static int rt_system(const char *cmd)
{
    rt_system_calls++;
    snprintf(rt_system_last, sizeof rt_system_last, "%s", cmd ? cmd : "");
    return 0;
}
static FILE *rt_popen(const char *cmd, const char *mode)
{
    (void)mode;
    rt_system(cmd);
    return fopen("/dev/null", "r");
}
static int rt_pclose(FILE *f) { return f ? fclose(f) : -1; }
static time_t rt_time(time_t *t) { if (t) *t = (time_t)rt_now; return (time_t)rt_now; }

#define system(c)        rt_system(c)
#define popen(c, m)      rt_popen(c, m)
#define pclose(f)        rt_pclose(f)
#define time(t)          rt_time(t)
#define DEVUI_CONF_FILE  rt_conf_path

#define UI_LANG_HOLD_MS 0   /* the switching frame is shot, not waited for */
#include "../../src/ui.c"

#undef system
#undef popen
#undef pclose
#undef time

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_ASSERT(x) ((void)0)
#include "stb_image_write.h"

/* ---- offscreen display ---- */
static uint16_t s_fb[UI_W * UI_H];
static uint32_t s_tick;
static uint32_t rt_tick(void) { return s_tick; }
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) { (void)a; (void)px; lv_display_flush_ready(d); }
static lv_display_t *s_disp;

/* A virtual finger, so taps go through LVGL's real hit-testing. */
static int s_fx, s_fy, s_fdown;
static void finger_cb(lv_indev_t *in, lv_indev_data_t *d)
{
    (void)in;
    d->point.x = s_fx;
    d->point.y = s_fy;
    /* the same gate main.c puts in front of the real touchscreen */
    d->state = ui_touch_filter(s_fdown, s_fx, s_fy) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void settle(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 20) {
        s_tick += 20;
        lv_timer_handler();
    }
}

/* ---- results ---- */
static int s_pass, s_fail;
static const char *s_scene_name, *s_theme;
static int s_lang_en;                   /* --lang=en: devui.conf lang=en (L2) */
static void ok(const char *fmt, ...)  { (void)fmt; s_pass++; }
static void bad(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  FAIL [%s/%s%s] ", s_scene_name, s_theme, s_lang_en ? "/en" : "");
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
    s_fail++;
}

/* ---- golden hashes ---- */
#define MAX_SHOTS 256
static struct { char name[64]; uint64_t h; } s_shots[MAX_SHOTS], s_gold[MAX_SHOTS];
static int s_nshots, s_ngold;
static const char *s_png_dir;

static uint64_t fnv64(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

static void save_png(const char *name)
{
    static uint8_t rgb[UI_W * UI_H * 3];
    char path[512];
    for (int i = 0; i < UI_W * UI_H; i++) {
        uint16_t p = s_fb[i];
        uint8_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
        rgb[i * 3]     = (uint8_t)((r << 3) | (r >> 2));
        rgb[i * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
        rgb[i * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
    }
    snprintf(path, sizeof path, "%s/%s-%s%s-%s.png", s_png_dir, s_scene_name, s_theme, s_lang_en ? "-en" : "", name);
    stbi_write_png(path, UI_W, UI_H, 3, rgb, UI_W * 3);
}

/* ---- visible label texts, per page ---- */
#define MAX_TEXTS 1024
static char *s_texts[MAX_TEXTS];
static int s_ntexts;

/* Characters a label shows but its font chain has no glyph for (they would
 * render as nothing). Flags, emoji and joiners are skipped: no font on the
 * device has them, the UI shows them as-is from node names. */
#define MAX_MISSING 32
static uint32_t s_missing[MAX_MISSING];
static int s_nmissing;

static int glyph_exempt(uint32_t c)
{
    return c < 0x20 || c == 0x200D || (c >= 0xFE00 && c <= 0xFE0F) || c >= 0x1F000;
}

static void check_glyphs(lv_obj_t *label, const char *t)
{
    const lv_font_t *f = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    lv_font_glyph_dsc_t dsc;
    for (uint32_t i = 0; t[i];) {
        uint32_t c = lv_text_encoded_next(t, &i);
        if (glyph_exempt(c) || lv_font_get_glyph_dsc(f, &dsc, c, 0)) continue;
        int seen = 0;
        for (int k = 0; k < s_nmissing && !seen; k++) seen = s_missing[k] == c;
        if (!seen && s_nmissing < MAX_MISSING) s_missing[s_nmissing++] = c;
    }
}

static void texts_clear(void)
{
    for (int i = 0; i < s_ntexts; i++) free(s_texts[i]);
    s_ntexts = 0;
    s_nmissing = 0;
}

static void collect(lv_obj_t *o)
{
    if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_check_type(o, &lv_label_class) && s_ntexts < MAX_TEXTS) {
        const char *t = lv_label_get_text(o);
        if (t && *t) { s_texts[s_ntexts++] = strdup(t); check_glyphs(o, t); }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) collect(lv_obj_get_child(o, (int32_t)i));
}

/* ---- geometry (10-01 L2, eng review R11): English is longer, and what goes
 * wrong is text running out of its box or into its neighbour. For every
 * visible label: its box stays inside the screen and every ancestor
 * horizontally (pages scroll vertically, so only x); no two visible siblings
 * that hold text overlap; a DOT/CLIP label whose text is wider than its box is
 * counted as truncated (info: some are meant to shorten, e.g. node names). */
static int s_dump;                      /* --dump: texts (and truncated labels) to stdout */
#define MAX_GEO 32
static char s_geo[MAX_GEO][200];
static int s_ngeo, s_ntrunc;

static void geo_bad(const char *fmt, ...)
{
    if (s_ngeo >= MAX_GEO) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_geo[s_ngeo], sizeof s_geo[s_ngeo], fmt, ap);
    va_end(ap);
    for (int i = 0; i < s_ngeo; i++) if (!strcmp(s_geo[i], s_geo[s_ngeo])) return;
    s_ngeo++;
}

/* Sibling overlap is checked between labels only: panels, cards and the
 * status bar legitimately stack (a subpage slides over a tab). */
static int is_text_label(lv_obj_t *o)
{
    if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) || !lv_obj_check_type(o, &lv_label_class)) return 0;
    if (lv_obj_get_style_opa(o, LV_PART_MAIN) == LV_OPA_TRANSP || lv_obj_get_style_text_opa(o, LV_PART_MAIN) == LV_OPA_TRANSP) return 0;
    const char *t = lv_label_get_text(o);
    return t && *t;
}

/* Where a label's text actually is: boxes are often wider than their text
 * (a full-width row label, right-aligned values), and only text on text is
 * a collision. */
static void ink_box(lv_obj_t *l, lv_area_t *r)
{
    lv_area_t c;
    lv_obj_get_content_coords(l, &c);
    const lv_font_t *f = lv_obj_get_style_text_font(l, LV_PART_MAIN);
    lv_point_t sz;
    int32_t w = lv_area_get_width(&c);
    lv_text_get_size(&sz, lv_label_get_text(l), f, lv_obj_get_style_text_letter_space(l, LV_PART_MAIN),
                     lv_obj_get_style_text_line_space(l, LV_PART_MAIN),
                     lv_label_get_long_mode(l) == LV_LABEL_LONG_MODE_WRAP ? w : LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t tw = sz.x < w ? sz.x : w, th = sz.y < lv_area_get_height(&c) ? sz.y : lv_area_get_height(&c);
    lv_text_align_t a = lv_obj_get_style_text_align(l, LV_PART_MAIN);
    if (a == LV_TEXT_ALIGN_AUTO) a = LV_TEXT_ALIGN_LEFT;
    r->x1 = a == LV_TEXT_ALIGN_RIGHT ? c.x2 - tw + 1 : a == LV_TEXT_ALIGN_CENTER ? c.x1 + (w - tw) / 2 : c.x1;
    r->x2 = r->x1 + tw - 1;
    r->y1 = c.y1;
    r->y2 = c.y1 + th - 1;
}

static int geo_on_screen(const lv_area_t *a) { return a->y2 >= 0 && a->y1 < UI_H && a->x2 >= 0 && a->x1 < UI_W; }

static void geo_label(lv_obj_t *l)
{
    lv_area_t a;
    lv_obj_get_coords(l, &a);
    const char *t = lv_label_get_text(l);
    if (a.x1 < 0 || a.x2 >= UI_W) geo_bad("\"%.60s\" runs off the screen (x %d..%d)", t, (int)a.x1, (int)a.x2);
    for (lv_obj_t *p = lv_obj_get_parent(l); p; p = lv_obj_get_parent(p)) {
        if (lv_obj_get_scroll_dir(p) & LV_DIR_HOR && lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE) &&
            lv_obj_get_scroll_right(p) + lv_obj_get_scroll_left(p) > 0) break;   /* the tileview */
        lv_area_t pa;
        lv_obj_get_coords(p, &pa);
        if (a.x1 < pa.x1 || a.x2 > pa.x2) {
            geo_bad("\"%.60s\" (x %d..%d) sticks out of its container (x %d..%d)", t, (int)a.x1, (int)a.x2,
                    (int)pa.x1, (int)pa.x2);
            break;
        }
    }
    lv_label_long_mode_t m = lv_label_get_long_mode(l);
    if (m == LV_LABEL_LONG_MODE_DOTS || m == LV_LABEL_LONG_MODE_CLIP) {
        const lv_font_t *f = lv_obj_get_style_text_font(l, LV_PART_MAIN);
        lv_point_t sz;
        lv_text_get_size(&sz, t, f, lv_obj_get_style_text_letter_space(l, LV_PART_MAIN),
                         lv_obj_get_style_text_line_space(l, LV_PART_MAIN), LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x > lv_obj_get_content_width(l) + 1) {
            s_ntrunc++;
            if (s_dump) printf("TRUNC\t%s\t%d>%d\n", t, (int)sz.x, (int)lv_obj_get_content_width(l));
        }
    }
}

/* A visible, coloured non-label sibling smaller than its parent: a control
 * or a pill, not the card or panel behind everything. */
static int is_block(lv_obj_t *d, lv_obj_t *parent)
{
    if (!d || lv_obj_has_flag(d, LV_OBJ_FLAG_HIDDEN) || lv_obj_check_type(d, &lv_label_class)) return 0;
    if (lv_obj_get_style_opa(d, LV_PART_MAIN) == LV_OPA_TRANSP) return 0;
    if (lv_obj_get_style_bg_opa(d, LV_PART_MAIN) < LV_OPA_20 && !lv_obj_check_type(d, &lv_switch_class) &&
        !lv_obj_check_type(d, &lv_slider_class))
        return 0;
    return lv_obj_get_width(d) < lv_obj_get_width(parent) - 8 && lv_obj_get_height(d) < lv_obj_get_height(parent) - 8;
}

static void geo_walk(lv_obj_t *o)
{
    if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_check_type(o, &lv_label_class)) {
        const char *t = lv_label_get_text(o);
        lv_area_t a;
        lv_obj_get_coords(o, &a);
        if (t && *t && geo_on_screen(&a)) geo_label(o);
    }
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
        if (!is_text_label(c)) continue;
        lv_area_t a, b, x;
        ink_box(c, &a);
        if (!geo_on_screen(&a)) continue;
        for (uint32_t j = i + 1; j < n; j++) {
            lv_obj_t *d = lv_obj_get_child(o, (int32_t)j);
            if (!is_text_label(d)) continue;
            ink_box(d, &b);
            /* line boxes of stacked rows touch by a few px of leading; text
             * running into text overlaps for most of a line */
            int32_t hmin = lv_area_get_height(&a) < lv_area_get_height(&b) ? lv_area_get_height(&a) : lv_area_get_height(&b);
            if (lv_area_intersect(&x, &a, &b) && lv_area_get_width(&x) > 1 && lv_area_get_height(&x) * 5 > hmin * 2)
                geo_bad("\"%.40s\" overlaps \"%.40s\"", lv_label_get_text(c), lv_label_get_text(d));
        }
    }
    /* text running into a control or a coloured block beside it (a segment,
     * a switch, a pill): the language note under its segment, 10-02. A label
     * with its own opaque backing sits on top on purpose (chart "collecting"). */
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
        if (!is_text_label(c) || lv_obj_get_style_bg_opa(c, LV_PART_MAIN) >= LV_OPA_50) continue;
        lv_area_t a, b, x;
        ink_box(c, &a);
        if (!geo_on_screen(&a)) continue;
        for (uint32_t j = 0; j < n; j++) {
            lv_obj_t *d = lv_obj_get_child(o, (int32_t)j);
            if (!is_block(d, o)) continue;
            lv_obj_get_coords(d, &b);
            if (lv_area_intersect(&x, &a, &b) && lv_area_get_width(&x) > 1 && lv_area_get_height(&x) > 1)
                geo_bad("\"%.40s\" runs into a %dx%d block", lv_label_get_text(c), (int)lv_area_get_width(&b),
                        (int)lv_area_get_height(&b));
        }
    }
    for (uint32_t i = 0; i < n; i++) geo_walk(lv_obj_get_child(o, (int32_t)i));
}

static void shot(const char *page, int k)
{
    char name[64];
    snprintf(name, sizeof name, "%s.%d", page, k);
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(s_disp);
    geo_walk(lv_screen_active());
    geo_walk(lv_layer_top());
    if (s_nshots < MAX_SHOTS) {
        snprintf(s_shots[s_nshots].name, sizeof s_shots[s_nshots].name, "%s", name);
        s_shots[s_nshots].h = fnv64(s_fb, sizeof s_fb);
        s_nshots++;
    }
    if (s_png_dir) save_png(name);
}

/* Scroll body of a page: the first scrollable descendant that has overflow. */
static lv_obj_t *find_scroller(lv_obj_t *o)
{
    if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) && lv_obj_get_scroll_dir(o) & LV_DIR_VER) {
        lv_obj_update_layout(o);
        if (lv_obj_get_scroll_bottom(o) > 0 || lv_obj_get_scroll_y(o) > 0) return o;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *s = find_scroller(lv_obj_get_child(o, (int32_t)i));
        if (s) return s;
    }
    return NULL;
}

/* Shoot a page top to bottom; collect its texts (all scroll positions). */
static void shoot_page(const char *page, lv_obj_t *root)
{
    texts_clear();
    collect(root);
    collect(lv_layer_top());
    lv_obj_t *sc = find_scroller(root);
    if (!sc) { shot(page, 0); return; }
    lv_obj_scroll_to_y(sc, 0, LV_ANIM_OFF);
    int step = lv_obj_get_height(sc) - 60;
    if (step < 60) step = 60;
    for (int k = 0, y = 0; k < 12; k++, y += step) {
        lv_obj_scroll_to_y(sc, y, LV_ANIM_OFF);
        lv_obj_update_layout(sc);
        shot(page, k);
        if (lv_obj_get_scroll_bottom(sc) <= 0) break;
    }
    lv_obj_scroll_to_y(sc, 0, LV_ANIM_OFF);
}

/* ---- expect.txt: <scenes>\t<page>\t<text> ---- */
typedef struct { char page[32]; char text[160]; } expect_t;
#define MAX_EXPECT 1024
static expect_t s_exp[MAX_EXPECT];
static int s_nexp;

static int scene_matches(const char *spec)
{
    int neg = spec[0] == '!';
    if (!neg && !strcmp(spec, "*")) return 1;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", spec + neg);
    int hit = 0;
    for (char *save = NULL, *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save))
        if (!strcmp(t, s_scene_name)) hit = 1;
    return neg ? !hit : hit;
}

static void load_expect(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];
    if (!f) { bad("cannot read %s", path); return; }
    while (fgets(line, sizeof line, f) && s_nexp < MAX_EXPECT) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        char *a = strtok(line, "\t"), *b = strtok(NULL, "\t"), *c = strtok(NULL, "");
        if (!a || !b || !c) continue;
        if (!scene_matches(a)) continue;
        snprintf(s_exp[s_nexp].page, sizeof s_exp[s_nexp].page, "%s", b);
        snprintf(s_exp[s_nexp].text, sizeof s_exp[s_nexp].text, "%s", c);
        s_nexp++;
    }
    fclose(f);
}

static void check_expect(const char *page)
{
    for (int i = 0; i < s_nexp; i++) {
        if (strcmp(s_exp[i].page, page)) continue;
        int found = 0;
        for (int j = 0; j < s_ntexts && !found; j++) found = strstr(s_texts[j], s_exp[i].text) != NULL;
        if (found) ok("");
        else bad("%s: no visible text \"%s\"", page, s_exp[i].text);
    }
}

/* Dump every text on a page (--dump), to write expect.txt from. */
static void dump_texts(const char *page)
{
    if (!s_dump) return;
    for (int j = 0; j < s_ntexts; j++) {
        char one[200];
        snprintf(one, sizeof one, "%s", s_texts[j]);
        for (char *p = one; *p; p++) if (*p == '\n') *p = '|';
        printf("TEXT\t%s\t%s\n", page, one);
    }
}

static void check_missing(const char *page)
{
    for (int k = 0; k < s_nmissing; k++) {
        char u[8] = "";
        uint32_t c = s_missing[k];
        if (c < 0x80) { u[0] = (char)c; }
        else if (c < 0x800) { u[0] = (char)(0xC0 | c >> 6); u[1] = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { u[0] = (char)(0xE0 | c >> 12); u[1] = (char)(0x80 | (c >> 6 & 0x3F)); u[2] = (char)(0x80 | (c & 0x3F)); }
        bad("%s: no glyph for U+%04X \"%s\" in the label's fonts", page, (unsigned)c, u);
    }
    if (!s_nmissing) ok("");
    s_nmissing = 0;
}

static void check_geo(const char *page)
{
    for (int i = 0; i < s_ngeo; i++) bad("%s: %s", page, s_geo[i]);
    if (!s_ngeo) ok("");
    s_ngeo = 0;
}

static void page_done(const char *page) { check_expect(page); check_missing(page); check_geo(page); dump_texts(page); }

/* ---- navigation ---- */
static void to_tab(int i)
{
    sub_close();
    power_menu_set(0);
    lv_tileview_set_tile_by_index(s_tv, i, 0, LV_ANIM_OFF);
    update_tabs();
    bench_gate();
    settle(1100);
}
static void click(lv_obj_t *o);
static void to_sub(int id, int parent)
{
    to_tab(TAB_HOME);
    if (id == SUB_ALERT_DETAIL) {
        /* 详情的内容是点行时拷的：真的点一行（有体检项点体检，否则点第一条告警） */
        sub_open(parent); settle(200);
        click(health_count() > 0 ? s_hc_row[0] : s_al_row[0]);
    }
    else if (parent >= 0) { sub_open(parent); settle(200); sub_open_child(id, parent); }
    else sub_open(id);
    settle(1100);
}

static const struct { int id, parent; const char *name; } k_subs[] = {
    { SUB_SMS, -1, "sms" }, { SUB_SMS_DETAIL, SUB_SMS, "sms-detail" },
    { SUB_CELL, -1, "cell" }, { SUB_LOCK, -1, "lock" }, { SUB_SPEED, -1, "speed" },
    { SUB_ESIM, -1, "esim" },
    { SUB_TS, -1, "tailscale" }, { SUB_ALERTS, -1, "alerts" },
    { SUB_ALERT_DETAIL, SUB_ALERTS, "alert-detail" }, { SUB_PERF, -1, "perf" },
    { SUB_NET, -1, "net" }, { SUB_SCENE, -1, "scene" }, { SUB_APN, -1, "apn" },
};
static const char *const k_tabs[UI_TABS] = { "home", "cellular", "wifi", "exit", "system" };

static void click(lv_obj_t *o) { if (o) lv_obj_send_event(o, LV_EVENT_CLICKED, NULL); }

static void tap_at(int x, int y)
{
    s_fx = x; s_fy = y; s_fdown = 1;
    settle(100);
    s_fdown = 0;
    settle(200);
}

/* A finger drag from (x0, y) to (x1, y), in 5 moves. */
static void swipe_at(int x0, int y, int x1)
{
    s_fx = x0; s_fy = y; s_fdown = 1;
    settle(60);
    for (int k = 1; k <= 5; k++) { s_fx = x0 + (x1 - x0) * k / 5; settle(40); }
    s_fdown = 0;
    settle(200);
}

/* Screen centre of an object (for tap_at). */
static void centre(lv_obj_t *o, int *x, int *y)
{
    lv_area_t a;
    lv_obj_update_layout(o);
    lv_obj_get_coords(o, &a);
    *x = (a.x1 + a.x2) / 2;
    *y = (a.y1 + a.y2) / 2;
}

int main(int argc, char **argv)
{
    const char *golden = NULL, *write_golden = NULL, *expect = NULL;
    s_scene_name = "good";
    s_theme = "light";
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--scene=", 8)) s_scene_name = argv[i] + 8;
        else if (!strncmp(argv[i], "--theme=", 8)) s_theme = argv[i] + 8;
        else if (!strncmp(argv[i], "--png=", 6)) s_png_dir = argv[i] + 6;
        else if (!strncmp(argv[i], "--golden=", 9)) golden = argv[i] + 9;
        else if (!strncmp(argv[i], "--write-golden=", 15)) write_golden = argv[i] + 15;
        else if (!strncmp(argv[i], "--expect=", 9)) expect = argv[i] + 9;
        else if (!strcmp(argv[i], "--dump")) s_dump = 1;
        else if (!strcmp(argv[i], "--lang=en")) s_lang_en = 1;
    }
    rt_scene = -1;
    for (int s = 0; s < RT_SCENES; s++) if (!strcmp(rt_scene_name(s), s_scene_name)) rt_scene = s;
    if (rt_scene < 0) { fprintf(stderr, "unknown scene %s\n", s_scene_name); return 2; }

    static char conf[64];
    snprintf(conf, sizeof conf, "/tmp/rt-devui-%d.conf", (int)getpid());
    rt_conf_path = conf;
    FILE *cf = fopen(conf, "w");
    if (cf) { fprintf(cf, "appearance=%s\n%s", s_theme, s_lang_en ? "lang=en\n" : ""); fclose(cf); }
    if (expect) load_expect(expect);

    lv_init();
    lv_tick_set_cb(rt_tick);
    s_disp = lv_display_create(UI_W, UI_H);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp, s_fb, NULL, sizeof s_fb, LV_DISPLAY_RENDER_MODE_DIRECT);

    /* --launched-tab=N: start as a theme-switch exec would (--tab=N) */
    int launched_tab = -1;
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], "--launched-tab=", 15)) launched_tab = atoi(argv[i] + 15);
    lv_indev_t *finger = lv_indev_create();
    lv_indev_set_type(finger, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(finger, finger_cb);
    char tabarg[16];
    snprintf(tabarg, sizeof tabarg, "--tab=%d", launched_tab);
    char *av[] = { "u60pro-devui", tabarg, NULL };
    ui_set_launch(launched_tab >= 0 ? 2 : 1, av);
    ui_create();
    if (s_dark != !strcmp(s_theme, "dark")) bad("appearance=%s but dark=%d", s_theme, s_dark);
    settle(4000);

    /* Right after a theme exec the 系统 page is up (--tab=4): a real tap on
     * the other appearance must reach it (device bug 2026-09-24: taps on the
     * page went nowhere until a tab was tapped). */
    if (launched_tab == TAB_SYS) {
        int x, y, other = s_dark ? 0 : 1;
        centre(s_ap_btn[other], &x, &y);
        tap_at(x, y);
        if (rt_exec_calls != 1) bad("after --tab=4 start, a tap on %s did not switch (exec calls %d)",
                                    other ? "深色" : "浅色", rt_exec_calls);
        else ok("");
        printf("[%s/%s] launched-tab: exec calls %d\n", s_scene_name, s_theme, rt_exec_calls);
        unlink(conf);
        return s_fail ? 1 : 0;
    }

    /* 系统 (with the charts) first as it is at start (collecting), then with 5 minutes of history */
    to_tab(TAB_SYS);
    shoot_page("charts-collecting", s_tiles[TAB_SYS]);
    page_done("charts-collecting");
    settle(5 * 60 * 1000);
    for (int i = 0; i < UI_TABS; i++) {
        to_tab(i);
        shoot_page(k_tabs[i], s_tiles[i]);
        page_done(k_tabs[i]);
    }
    for (size_t i = 0; i < sizeof k_subs / sizeof *k_subs; i++) {
        if (k_subs[i].id == SUB_SMS_DETAIL) {
            if (!s_sms_row_id[0] || s_sms_row_id[0] < 0) continue;   /* no message to open */
            s_smsd_id = s_sms_row_id[0];
        }
        to_sub(k_subs[i].id, k_subs[i].parent);
        if (k_subs[i].id == SUB_SMS_DETAIL && s_sub_cur != SUB_SMS_DETAIL) { bad("sms-detail did not open"); continue; }
        shoot_page(k_subs[i].name, s_sub_page[k_subs[i].id]);
        page_done(k_subs[i].name);
    }

    /* overlays */
    to_tab(TAB_HOME);
    power_menu_set(1);
    settle(200);
    texts_clear(); collect(s_power_menu); shot("power-menu", 0); page_done("power-menu");
    power_menu_set(0);

    /* two-step confirms, first tap only (never the second) */
    to_tab(TAB_SYS);
    click(s_vendor_btn);
    settle(100);
    texts_clear(); collect(s_tiles[TAB_SYS]);
    shoot_page("system-vendor-armed", s_tiles[TAB_SYS]);
    page_done("system-vendor-armed");
    if (rt_system_calls && strstr(rt_system_last, "zte_topsw_devui")) bad("vendor switch ran on the first tap");

    to_tab(TAB_CELL);                            /* 网络模式在蜂窝标签上 */
    click(s_lk_mode_btn[2]);                     /* 5G SA：带国外提醒 */
    settle(100);
    shoot_page("lock-mode-armed", s_tiles[TAB_CELL]);
    page_done("lock-mode-armed");
    /* 下发了、读回超时（不按第二下：直接当作 4G+3G 已下发，场景读回的是别的模式）：
     * 两行说明「再试一次，不行就重启设备、开机后马上切」要放得下（2026-09-29） */
    s_lk_mode_pending = -1;
    s_lk_mode_want = 3;
    s_lk_mode_sent = lv_tick_get();
    settle(LK_MODE_WAIT_MS + 1100);
    shoot_page("lock-mode-failed", s_tiles[TAB_CELL]);
    page_done("lock-mode-failed");

    /* 情景：每一次点击都要当场看得见（2026-09-25） */
    to_sub(SUB_SCENE, -1);
    click(s_net_sc_row[0]);                      /* 已经生效的那个：说一句，不静默 */
    settle(100);
    shoot_page("net-sc-already", s_sub_page[SUB_SCENE]);
    page_done("net-sc-already");
    s_sc_flash.until = 0;
    click(s_net_sc_row[2]);                      /* 第一下：整行变色 + 后果 */
    settle(100);
    shoot_page("net-sc-armed", s_sub_page[SUB_SCENE]);
    page_done("net-sc-armed");
    settle(500);
    click(s_net_sc_row[2]);                      /* 第二下：切换中，直到真的变过去 */
    settle(100);
    shoot_page("net-sc-pending", s_sub_page[SUB_SCENE]);
    page_done("net-sc-pending");
    s_sc_pend = -1;
    s_sc_flash.until = 0;
    net_paint(1);
    lv_obj_add_state(s_net_sc_row[3], LV_STATE_PRESSED);   /* 按下：行有底色 */
    settle(40);
    shoot_page("net-sc-pressed", s_sub_page[SUB_SCENE]);
    page_done("net-sc-pressed");
    lv_obj_remove_state(s_net_sc_row[3], LV_STATE_PRESSED);
    net_clear_arms();
    to_sub(SUB_NET, -1);
    click(s_net_auto_btn);                       /* 已经是自动选网：说一句，不静默 */
    settle(100);
    shoot_page("net-auto-already", s_sub_page[SUB_NET]);
    page_done("net-auto-already");
    s_ms_flash.until = 0;

    /* APN：点正在生效的说一句；点别的两下确认，切换中 */
    if (rt_scene != RT_LOADING && rt_scene != RT_DATAD_DOWN && rt_scene != RT_EMPTY) {
        to_sub(SUB_APN, -1);
        click(s_apn_row[0]);
        settle(100);
        shoot_page("apn-already", s_sub_page[SUB_APN]);
        page_done("apn-already");
        s_apn_flash.until = 0;
        int calls0 = rt_apn_calls, other = rt_scene == RT_ABROAD ? 0 : 1;
        click(s_apn_row[other]);
        settle(100);
        if (rt_apn_calls != calls0) bad("APN switched on the first tap");
        shoot_page("apn-armed", s_sub_page[SUB_APN]);
        page_done("apn-armed");
        settle(500);
        click(s_apn_row[other]);
        settle(100);
        if (rt_apn_calls != calls0 + 1 || strcmp(rt_apn_last, other ? "manu1" : "auto"))
            bad("APN second tap: calls %d id %s", rt_apn_calls - calls0, rt_apn_last);
        else ok("");
        shoot_page("apn-pending", s_sub_page[SUB_APN]);
        page_done("apn-pending");
        s_apn_pend = -1;
        s_apn_arm_idx = -1;
        s_apn_flash.until = 0;
    }

    /* 点正在用的那张：说一句，不静默（以前这一行点了连按下效果都没有） */
    if (esim_profile_count() > 0) {
        to_sub(SUB_ESIM, -1);
        for (int k = 0; k < esim_profile_count(); k++) {
            esim_profile_t ep;
            esim_get_profile(k, &ep);
            if (ep.enabled) { click(s_es_row[k]); break; }
        }
        settle(100);
        shoot_page("esim-current", s_sub_page[SUB_ESIM]);
        page_done("esim-current");
        s_es_flash.until = 0;
        s_es_dirty = 1;
    }

    if (esim_profile_count() > 1) {
        to_sub(SUB_ESIM, -1);
        click(s_es_row[1]);
        settle(1100);
        shoot_page("esim-armed", s_sub_page[SUB_ESIM]);
        page_done("esim-armed");
    }

    /* 外观: a manual switch execs with the current tab; the harness's exec
     * "fails", so the UI must stay as it is and put the setting back. */
    if (!strcmp(s_theme, "light") && rt_scene == RT_GOOD) {
        to_tab(TAB_SYS);
        click(s_ap_btn[1]);   /* 深色 */
        if (rt_exec_calls != 1) bad("深色: exec calls %d, want 1", rt_exec_calls);
        else if (rt_exec_last.tab != TAB_SYS) bad("深色: exec tab %d, want %d", rt_exec_last.tab, TAB_SYS);
        else if (rt_exec_last.hist.count != 0) bad("manual switch counted as automatic");
        else ok("");
        if (s_cf_appear != UI_APPEAR_LIGHT) bad("failed exec left appearance=%s", ui_appear_name(s_cf_appear));
        click(s_ap_btn[0]);   /* 浅色 = current: no exec */
        if (rt_exec_calls != 1) bad("浅色 (already light) exec'd");

        /* A dark screen takes no taps (ui_logic.h ui_tgate_step). */
        int x, y, n0;
        to_tab(TAB_SYS);
        centre(s_ap_btn[1], &x, &y);
        s_autooff_ms = 30000;
        /* turned off with the power key: nothing wakes it but the key */
        backlight_off(); s_auto_slept = 0; n0 = rt_exec_calls;
        tap_at(x, y); tap_at(x, y);
        if (rt_exec_calls != n0) bad("dark (power key): a double tap reached 深色");
        else if (backlight_is_on()) bad("dark (power key): a double tap lit the screen");
        else ok("");
        /* went off by itself: one tap does nothing */
        settle(1000);
        s_auto_slept = 1; n0 = rt_exec_calls;
        tap_at(x, y);
        if (rt_exec_calls != n0) bad("dark (auto): a single tap reached 深色");
        else if (backlight_is_on()) bad("dark (auto): a single tap lit the screen");
        else ok("");
        /* a double tap wakes it, and is not a click */
        settle(1000);
        tap_at(x, y); tap_at(x, y);
        if (!backlight_is_on()) bad("dark (auto): a double tap did not wake the screen");
        else if (rt_exec_calls != n0) bad("dark (auto): the waking double tap also tapped 深色");
        else ok("");
        /* once awake (after the quiet time) taps work again */
        settle(400);
        tap_at(x, y);
        if (rt_exec_calls != n0 + 1) bad("after waking, a tap on 深色 did not switch");
        else ok("");
        s_autooff_ms = 0;
    }

    /* 语言 (L2): busy → no exec for 语言 nor 外观, the row says why; a switch
     * execs back to 系统; the harness's exec "fails", so the setting, the
     * file and the segment go back, the frame goes and the row says so. */
    if (!strcmp(s_theme, "light") && rt_scene == RT_GOOD) {
        int other = !lang_is_en(), n0 = rt_exec_calls;
        sub_close();
        to_tab(TAB_SYS);
        rt_busy = 1;
        click(s_lang_btn[other]);
        if (rt_exec_calls != n0) bad("busy: a language tap exec'd");
        else if (s_cf_lang_en != lang_is_en()) bad("busy: the language setting changed");
        else if (!strstr(lv_label_get_text(s_lang_note), "Busy")) bad("busy: the 语言 row did not say why");
        else ok("");
        click(s_ap_btn[1]);   /* 深色 */
        if (rt_exec_calls != n0) bad("busy: an appearance tap exec'd");
        else if (s_cf_appear != UI_APPEAR_LIGHT) bad("busy: appearance changed to %s", ui_appear_name(s_cf_appear));
        else ok("");
        rt_busy = 0;
        click(s_lang_btn[other]);
        FILE *cf = fopen(rt_conf_path, "r");
        char conf[512] = "";
        if (cf) { conf[fread(conf, 1, sizeof conf - 1, cf)] = 0; fclose(cf); }
        if (rt_exec_calls != n0 + 1) bad("language: exec calls %d, want 1", rt_exec_calls - n0);
        else if (rt_exec_last.tab != TAB_SYS) bad("language: exec tab %d, want %d", rt_exec_last.tab, TAB_SYS);
        else if (s_cf_lang_en != lang_is_en()) bad("failed exec left lang=%d", s_cf_lang_en);
        else if (!strstr(conf, lang_is_en() ? "lang=en\n" : "lang=zh\n")) bad("failed exec left devui.conf: %s", conf);
        else if (s_lang_seg.sel != lang_is_en()) bad("failed exec left the segment on %d", s_lang_seg.sel);
        else if (!s_lang_frame || !lv_obj_has_flag(s_lang_frame, LV_OBJ_FLAG_HIDDEN)) bad("failed exec left the switching frame up");
        else if (!strstr(lv_label_get_text(s_lang_note), "Couldn't switch")) bad("failed exec: the 语言 row did not say so");
        else ok("");
        lv_obj_scroll_to_y(tab_scroller(TAB_SYS), 0, LV_ANIM_OFF);
        settle(50);
        shoot_page("system-lang-failed", s_tiles[TAB_SYS]);
        page_done("system-lang-failed");
        lang_frame_show(other);
        settle(50);
        shoot_page("lang-switching", s_lang_frame);
        page_done("lang-switching");
        lang_frame_hide();
        lang_note_reset_cb(NULL);
    }

    /* 左边缘右滑 = 返回（放在息屏测试后面：真按下会刷新「最近有操作」）；起点压在一行会断网的选项上，这一行不能被点中 */
    {
        int x, y;
        net_clear_arms();
        s_sc_flash.until = 0;
        to_sub(SUB_SCENE, -1);
        centre(s_net_sc_row[2], &x, &y);
        swipe_at(12, y, 130);
        if (s_sub_cur != -1) bad("edge swipe on 情景 did not go back (sub %d)", s_sub_cur);
        else if (s_net_arm_sc || s_sc_pend >= 0) bad("edge swipe also tapped the row under the finger");
        else ok("");
        to_sub(SUB_SCENE, -1);
        if (lv_obj_is_visible(s_net_sc_row[2])) {   /* 没有情景的场景里这行不在 */
            tap_at(12, y);                           /* 同一个点，普通点一下：这行确实在那里 */
            if (!s_net_arm_sc) bad("tap at the swipe start point did not reach the row");
            else ok("");
        }
        net_clear_arms();
        s_smsd_id = s_sms_row_id[0];
        to_sub(SUB_SMS_DETAIL, SUB_SMS);
        if (s_sub_cur == SUB_SMS_DETAIL) {   /* 没有短信的场景里详情页打不开 */
            swipe_at(8, 300, 120);
            if (s_sub_cur != SUB_SMS) bad("edge swipe on a child page: sub %d, want 短信", s_sub_cur);
            else ok("");
        }
        to_sub(SUB_SCENE, -1);
        swipe_at(150, y, 290);                       /* 不是从边缘起：不返回 */
        if (s_sub_cur != SUB_SCENE) bad("a swipe from mid-screen went back");
        else ok("");
        net_clear_arms();
        s_sc_flash.until = 0;
        sub_close();
    }

    /* ---- hashes ---- */
    if (write_golden) {
        FILE *g = fopen(write_golden, "w");
        if (!g) { bad("cannot write %s", write_golden); }
        else {
            for (int i = 0; i < s_nshots; i++) fprintf(g, "%s %016llx\n", s_shots[i].name, (unsigned long long)s_shots[i].h);
            fclose(g);
        }
    } else if (golden) {
        FILE *g = fopen(golden, "r");
        char n[64];
        unsigned long long h;
        if (!g) bad("no golden file %s (make render-golden)", golden);
        else {
            while (s_ngold < MAX_SHOTS && fscanf(g, "%63s %llx", n, &h) == 2) {
                snprintf(s_gold[s_ngold].name, sizeof s_gold[s_ngold].name, "%s", n);
                s_gold[s_ngold++].h = h;
            }
            fclose(g);
            for (int i = 0; i < s_nshots; i++) {
                int j = 0;
                while (j < s_ngold && strcmp(s_gold[j].name, s_shots[i].name)) j++;
                if (j == s_ngold) bad("%s: not in the golden file", s_shots[i].name);
                else if (s_gold[j].h != s_shots[i].h) bad("%s: pixels changed", s_shots[i].name);
                else ok("");
            }
            for (int j = 0; j < s_ngold; j++) {
                int i = 0;
                while (i < s_nshots && strcmp(s_gold[j].name, s_shots[i].name)) i++;
                if (i == s_nshots) bad("%s: in the golden file but no longer drawn", s_gold[j].name);
            }
        }
    }

    unlink(conf);
    printf("[%s/%s%s] %d shots, passed %d, failed %d, truncated labels %d\n", s_scene_name, s_theme,
           s_lang_en ? "/en" : "", s_nshots, s_pass, s_fail, s_ntrunc);
    return s_fail ? 1 : 0;
}
