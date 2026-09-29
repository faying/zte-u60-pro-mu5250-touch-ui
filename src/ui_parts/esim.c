/*
 * ui_parts/esim.c - eSIM 子页.
 * Part of src/ui.c, which #includes it: the parts share ui.c's static state
 * and are one translation unit, not compiled on their own (the Makefile
 * lists only ui.c). Split 2026-09-26 so the 6000-line file stops being
 * the place every change collides; the built binary is byte-identical.
 *
 * SPDX-License-Identifier: MIT
 */
/* ---- eSIM subpage ---- */
/* 一句几秒后消失的提示。点击的结果（「已经是…」「正忙」「没执行：…」）
 * 在 eSIM 页写在右上状态行（150 宽，约 12 个字），要短
 * 用它说出来，不静默忽略（2026-09-25）。 */
typedef struct { char txt[140]; uint32_t col, until; } net_flash_t;

static void net_flash(net_flash_t *f, uint32_t col, int ms, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->txt, sizeof f->txt, fmt, ap);
    va_end(ap);
    f->col = col;
    f->until = lv_tick_get() + (uint32_t)ms;
    if (!f->until) f->until = 1;
}
static int net_flash_on(const net_flash_t *f) { return f->until && (int32_t)(f->until - lv_tick_get()) > 0; }

static net_flash_t s_es_flash;
static int         s_es_dirty;     /* 点了一下：下一轮一定重画 eSIM 列表 */
static void esim_paint(void);

static void esim_row_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    int r = esim_select(idx);
    switch (r) {
    case ESIM_SEL_CURRENT:
        net_flash(&s_es_flash, T->accT, 3000, "这张就是正在用的");
        break;
    case ESIM_SEL_BUSY:
        net_flash(&s_es_flash, T->t2, 3000, "正在切换，等它做完");
        break;
    case ESIM_SEL_COOLDOWN:
    case ESIM_SEL_FAIL:
        net_flash(&s_es_flash, T->badT, 8000, "%s", esim_state());   /* 状态行本身就写着原因，标红 */
        break;
    default:   /* ARMED / STARTED：行本身会变色、写「再点一次确认」/「切换中…」 */
        s_es_flash.until = 0;
        break;
    }
    s_es_dirty = 1;
    esim_paint();
}

#define ESIM_ROW_H 50
static void build_sub_esim(lv_obj_t *t)
{
    t = uk_scroll(t, 0, UI_SUB_VIEW, 4 + 20 + UK_HERO_H + 10 + 20 + 3 * UK_ROW_H + 10 + 20 + ESIM_MAX_ROWS * ESIM_ROW_H + 16);
    uk_section(t, 4, "当前配置");
    lv_obj_t *cur = uk_card(t, UK_MARGIN, 24, UK_CARD_W, UK_HERO_H);
    uk_hero(&s_es_hero, cur, UF.cj22b);
    uk_hero_tone(&s_es_hero, 4);
    s_es_cur = s_es_hero.big;
    s_es_state = s_es_hero.r2;
    lv_label_set_text(s_es_hero.st, "使用中");

    /* 卡信息：实体 SIM 和 eSIM 都列（2026-09-25：插普通 SIM 时这页原来只有「-」） */
    int y = 24 + UK_HERO_H + 10;
    uk_section(t, y, "卡信息");
    lv_obj_t *info = uk_card(t, UK_MARGIN, y + 20, UK_CARD_W, 3 * UK_ROW_H);
    static const char *const k_info[3] = { "号码", "ICCID", "IMSI" };
    for (int i = 0; i < 3; i++) s_es_info[i] = uk_row(info, i * UK_ROW_H, k_info[i], i == 0);
    y += 20 + 3 * UK_ROW_H + 10;
    s_es_list_sec = uk_section(t, y, "eSIM 配置");
    s_es_list_card = uk_card(t, UK_MARGIN, y + 20, UK_CARD_W, ESIM_MAX_ROWS * ESIM_ROW_H);
    lv_obj_set_style_clip_corner(s_es_list_card, true, 0);
    s_es_empty = uk_label_w(s_es_list_card, UF.cj14, T->t3, UK_PAD, 11, UK_CARD_W - 2 * UK_PAD, 0, "读取中…");
    for (int i = 0; i < ESIM_MAX_ROWS; i++) {
        lv_obj_t *row = uk_box(s_es_list_card, 0, i * ESIM_ROW_H, UK_CARD_W, ESIM_ROW_H, T->card, 0);
        uk_tappable(row, esim_row_cb, (void *)(intptr_t)i);
        s_es_row[i] = row;
        s_es_row_sep[i] = i ? uk_sep(row, 0) : NULL;
        s_es_row_name[i] = uk_label_w(row, UF.cj14, T->t1, UK_PAD, 8, 190, 0, "");
        s_es_row_sub[i]  = uk_label_w(row, UF.n11, T->t3, UK_PAD, 29, 200, 0, "");
        s_es_row_tag[i]  = uk_label_r(row, UF.cj13, T->accT, UK_CARD_W - UK_PAD, 16, "");
        uk_show(row, 0);
    }
}

