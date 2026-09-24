/*
 * ui_theme.c - colour tables and font loading; see ui_theme.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "ui_theme.h"
#include "src/libs/freetype/lv_freetype.h"

#include <stdio.h>
#include <stdlib.h>

#define CJK_FONT_DEFAULT    "/usr/ui/fonts/ZTEZhengYuan.ttf"
#define ROBOTO_FONT_DEFAULT "/usr/ui/fonts/Roboto.ttf"
#define FONT_DIR_DEFAULT    "/data/plugins/u60pro-devui/fonts"

static const ui_theme_t k_light = {
    .bg = 0xeff3f7, .card = 0xffffff, .sep = 0xdee3e7,
    .t1 = 0x080c10, .t2 = 0x393c42, .t3 = 0x5a616b,
    .okT = 0x107131, .warnT = 0xa54908, .badT = 0xc60008, .accT = 0x005dbd,
    .fillBlue = 0x0065ce, .fillOrange = 0xb55108, .fillGreen = 0x187d39, .fillRed = 0xd60010,
    .green = 0x31c75a, .orange = 0xff8e29, .red = 0xff3839, .blue = 0x008aff,
    .wash = 0xd6efde, .washW = 0xffebd6, .washB = 0xffe3e7,
    .glass = 0xf7fbff, .track = 0xe7e7ef, .accS = 0xe7f3ff, .pill = 0xffffff, .onFill = 0xe7f3ff,
    .hl_opa = 0, .rim_opa = 26, .scrim_opa = 90, .dark = 0,
};

static const ui_theme_t k_dark = {
    .bg = 0x080c10, .card = 0x181c21, .sep = 0x29303a,
    .t1 = 0xf7f7f7, .t2 = 0xc6c7ce, .t3 = 0xa5a6ad,
    .okT = 0x31d35a, .warnT = 0xff9230, .badT = 0xff797b, .accT = 0x5aaaff,
    .fillBlue = 0x0065ce, .fillOrange = 0xb55108, .fillGreen = 0x187d39, .fillRed = 0xd60010,
    .green = 0x31d35a, .orange = 0xff9230, .red = 0xff4245, .blue = 0x0091ff,
    .wash = 0x103018, .washW = 0x392408, .washB = 0x391018,
    .glass = 0x212831, .track = 0x31363a, .accS = 0x10304a, .pill = 0x39414a, .onFill = 0xe7f3ff,
    .hl_opa = 40, .rim_opa = 150, .scrim_opa = 140, .dark = 1,
};

const ui_theme_t *T = &k_light;
ui_fonts_t UF;

void ui_theme_select(int dark) { T = dark ? &k_dark : &k_light; }

static const char *env_or(const char *name, const char *def)
{
    const char *v = getenv(name);
    return v && *v ? v : def;
}

static lv_font_t *ft(const char *path, int size, int bold)
{
    return lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, (uint32_t)size,
                                   bold ? LV_FREETYPE_FONT_STYLE_BOLD : LV_FREETYPE_FONT_STYLE_NORMAL);
}

static const lv_font_t *cjk(const char *path, int size, int bold, const lv_font_t *fallback)
{
    lv_font_t *f = ft(path, size, bold);
    return f ? f : fallback;
}

/* Nunito at `weight`, else Roboto, else the CJK font; CJK as glyph fallback. */
static const lv_font_t *num(const char *dir, const char *roboto, int weight, int size,
                            const lv_font_t *cjk_fb, int *used_roboto, int *used_cjk)
{
    char p[256];
    snprintf(p, sizeof p, "%s/Nunito-%d.ttf", dir, weight);
    lv_font_t *f = ft(p, size, 0);
    if (!f) { f = ft(roboto, size, 0); if (f) *used_roboto = 1; }
    if (!f) { *used_cjk = 1; return cjk_fb; }
    f->fallback = cjk_fb;
    return f;
}

int ui_fonts_load(void)
{
    const char *cj = env_or("U60_DEVUI_CJK_FONT", CJK_FONT_DEFAULT);
    const char *dir = env_or("U60_DEVUI_FONT_DIR", FONT_DIR_DEFAULT);
    const char *rob = env_or("U60_DEVUI_ROBOTO", ROBOTO_FONT_DEFAULT);

    /* lv_init() already ran lv_freetype_init (LV_USE_FREETYPE); a second call
     * only logs a warning, so there is none here. */
    lv_font_t *probe = ft(cj, 13, 0);
    int ok = probe != NULL;
    const lv_font_t *m12 = &lv_font_montserrat_12, *m14 = &lv_font_montserrat_14,
                    *m16 = &lv_font_montserrat_16, *m20 = &lv_font_montserrat_20;

    UF.cj13  = probe ? probe : m14;
    UF.cj11  = cjk(cj, 11, 0, m12);
    UF.cj12  = cjk(cj, 12, 0, m12);
    UF.cj14  = cjk(cj, 14, 0, m14);
    UF.cj15  = cjk(cj, 15, 0, m14);
    UF.cj20  = cjk(cj, 20, 0, m20);
    UF.cj15b = cjk(cj, 15, 1, UF.cj15);
    UF.cj17b = cjk(cj, 17, 1, m16);   /* montserrat 16 only if the CJK font is missing */
    UF.cj20b = cjk(cj, 20, 1, UF.cj20);
    UF.cj22b = cjk(cj, 22, 1, UF.cj20b);
    UF.cj24b = cjk(cj, 24, 1, UF.cj22b);

    int rb = 0, cf = 0;
    UF.nbat = num(dir, rob, 800, 12, UF.cj12, &rb, &cf);   /* battery digits: bold, filling the body */
    UF.n11 = num(dir, rob, 800, 11, UF.cj11, &rb, &cf);
    UF.n12 = num(dir, rob, 600, 12, UF.cj12, &rb, &cf);
    UF.n15 = num(dir, rob, 600, 15, UF.cj15, &rb, &cf);
    UF.n17 = num(dir, rob, 700, 17, UF.cj17b, &rb, &cf);
    UF.n20 = num(dir, rob, 700, 20, UF.cj20b, &rb, &cf);
    UF.n32 = num(dir, rob, 800, 32, UF.cj24b, &rb, &cf);
    UF.n36 = num(dir, rob, 800, 36, UF.cj24b, &rb, &cf);
    UF.numerals = cf ? "cjk" : rb ? "roboto" : "nunito";

    fprintf(stderr, "ui: fonts cjk=%s numerals=%s (dir %s)\n", ok ? "device" : "montserrat", UF.numerals, dir);
    fflush(stderr);
    return ok ? 0 : -1;
}
