/*
 * ui_parts/speedtest.c - 测速子页.
 * Part of src/ui.c, which #includes it: the parts share ui.c's static state
 * and are one translation unit, not compiled on their own (the Makefile
 * lists only ui.c). Split 2026-09-26 so the 6000-line file stops being
 * the place every change collides; the built binary is byte-identical.
 *
 * SPDX-License-Identifier: MIT
 */
/* ---- 测速 subpage ----
 * 2026-09-22: rebuilt on top of zte-agent's own speed test engine (see
 * speedtest.h/.c) instead of the old, never-installed better-speedtest
 * plugin — this used to be a permanent "not installed" placeholder. */
static void speedtest_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (speedtest_running()) speedtest_stop();
    else                     speedtest_start();
}

static void speedtest_srv_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    speedtest_select_server(idx - 1);   /* row 0 = 自动 = index -1 */
    for (int i = 0; i < ST_SRV_ROWS; i++)   /* 对勾当场挪过去，不等 1 秒的刷新 */
        if (s_st_srv_ok[i]) uk_show(s_st_srv_ok[i], i == idx);
}

static void build_sub_speed(lv_obj_t *t)
{
    t = uk_scroll(t, 0, UI_SUB_VIEW, 4 + 220 + 10 + 20 + ST_SRV_ROWS * UK_ROW_H + 16);

    s_st_card = uk_card(t, UK_MARGIN, 4, UK_CARD_W, 220);
    uk_label(s_st_card, UF.cj12, T->t3, UK_PAD, 10, "网络测速");
    s_st_phase = uk_label_r(s_st_card, UF.cj12, T->t2, UK_CARD_W - UK_PAD, 10, "");
    s_st_live = uk_label(s_st_card, UF.n36, T->t1, UK_PAD, 30, "--");
    s_st_unit = uk_label(s_st_card, UF.n15, T->t3, 100, 50, "Mbps");
    s_st_detail = uk_label_w(s_st_card, UF.cj13, T->t2, UK_PAD, 82, UK_CARD_W - 2 * UK_PAD, 0, "");
    s_st_result = uk_label_w(s_st_card, UF.n15, T->t1, UK_PAD, 104, UK_CARD_W - 2 * UK_PAD, 0, "");
    s_st_server = uk_label_w(s_st_card, UF.cj12, T->t3, UK_PAD, 128, UK_CARD_W - 2 * UK_PAD, 0, "");
    s_st_btn = uk_button(s_st_card, UK_PAD, 160, UK_CARD_W - 2 * UK_PAD, 42, "开始测速", UK_BTN_PRIMARY,
                         speedtest_btn_cb, NULL, &s_st_btn_lbl);
    s_st_offline = uk_label_w(s_st_card, UF.cj13, T->badT, UK_PAD, 190, UK_CARD_W - 2 * UK_PAD, 1, "");
    uk_show(s_st_offline, 0);

    uk_section(t, 4 + 220 + 10, "服务器");
    s_st_srv_card = uk_card(t, UK_MARGIN, 4 + 220 + 10 + 20, UK_CARD_W, ST_SRV_ROWS * UK_ROW_H);
    for (int i = 0; i < ST_SRV_ROWS; i++) {
        lv_obj_t *row = uk_box(s_st_srv_card, 0, i * UK_ROW_H, UK_CARD_W, UK_ROW_H, T->card, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        uk_tappable(row, speedtest_srv_cb, (void *)(intptr_t)i);
        s_st_srv_row[i] = row;
        if (i) uk_sep(row, 0);
        s_st_srv_ok[i] = uk_label(row, &lv_font_montserrat_14, T->accT, UK_PAD, 12, LV_SYMBOL_OK);
        s_st_srv_name[i] = uk_label_w(row, UF.cj14, T->t1, UK_PAD + 22, 11, UK_CARD_W - 2 * UK_PAD - 22, 0, "");
        if (i > 0) uk_show(row, 0);   /* shown once the list arrives */
    }
    lv_label_set_text(s_st_srv_name[0], "自动（最佳服务器）");
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* Every tile now has a real page, so the "not built yet" placeholder body
 * is gone. If a future tile lands before its page does, write the page with
 * an honest description of what it needs (see build_sub_speed) rather than
 * a generic 开发中 box. */

