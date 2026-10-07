/*
 * ui_parts/exit.c - 出口页：出口面板、情景卡.
 * Part of src/ui.c, which #includes it: the parts share ui.c's static state
 * and are one translation unit, not compiled on their own (the Makefile
 * lists only ui.c). Split 2026-09-26 so the 6000-line file stops being
 * the place every change collides; the built binary is byte-identical.
 *
 * SPDX-License-Identifier: MIT
 */

/* 情景卡：任何时候都能点，进「情景」页（手动固定情景）。 */
static void sc_card_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    sub_open(SUB_SCENE);
}


/* Tailscale 页（2026-09-25 加详）：本机一块，每台节点三行——名字和怎么连着、
 * IP 和系统、跟本机之间的连接（直连地址或经哪个 DERP、上次握手、收发了多少）。 */
#define TS_PEER_H 70
static void build_sub_ts(lv_obj_t *t)
{
    t = uk_scroll(t, 0, UI_SUB_VIEW, 24 + TS_SELF_ROWS * UK_ROW_H + 10 + 20 + TS_PEER_MAX * TS_PEER_H + 16);
    uk_section(t, 4, TR("本机"));
    lv_obj_t *self = uk_card(t, UK_MARGIN, 24, UK_CARD_W, TS_SELF_ROWS * UK_ROW_H);
    static const char *const k_self_cap[TS_SELF_ROWS] = {
        N_("主机名"), "IP", "IPv6", N_("DERP 中继"), N_("子网路由"), "Tailnet", N_("版本"), N_("密钥到期") };
    for (int i = 0; i < TS_SELF_ROWS; i++) s_tp_self[i] = uk_row(self, i * UK_ROW_H, TR(k_self_cap[i]), i == 0);
    lv_obj_set_style_text_font(s_tp_self[0], UF.cj14, 0);
    lv_obj_set_style_text_font(s_tp_self[5], UF.cj14, 0);
    lv_obj_set_style_text_font(s_tp_self[7], UF.cj14, 0);
    int y = 24 + TS_SELF_ROWS * UK_ROW_H + 10;
    uk_section(t, y, TR("节点（和本机之间）"));
    s_tp_card = uk_card(t, UK_MARGIN, y + 20, UK_CARD_W, TS_PEER_MAX * TS_PEER_H);
    for (int i = 0; i < TS_PEER_MAX; i++) {
        s_tp_row[i] = uk_box(s_tp_card, 0, i * TS_PEER_H, UK_CARD_W, TS_PEER_H, T->card, 0);
        lv_obj_set_style_bg_opa(s_tp_row[i], LV_OPA_TRANSP, 0);
        s_tp_sep[i] = i ? uk_sep(s_tp_row[i], 0) : NULL;
        s_tp_name[i] = uk_label_w(s_tp_row[i], UF.cj14, T->t1, UK_PAD, 8, 170, 0, "");
        lv_label_set_long_mode(s_tp_name[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(s_tp_name[i], lv_font_get_line_height(UF.cj14));
        s_tp_ip[i] = uk_label(s_tp_row[i], UF.n11, T->t3, UK_PAD, 30, "");
        s_tp_tag[i] = uk_label_r(s_tp_row[i], UF.cj13, T->t3, UK_CARD_W - UK_PAD, 9, "");
        s_tp_link[i] = uk_label_w(s_tp_row[i], UF.cj12, T->t2, UK_PAD, 47, UK_CARD_W - 2 * UK_PAD, 0, "");
        lv_label_set_long_mode(s_tp_link[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(s_tp_link[i], lv_font_get_line_height(UF.cj12));
        uk_show(s_tp_row[i], 0);
    }
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* "42 秒前" / "5 分钟前" / "3 小时前" / "2 天前" */
static void fmt_ago(char *out, size_t n, long s)
{
    if (s < 60)         snprintf(out, n, TR("%ld 秒前"), s);
    else if (s < 3600)  snprintf(out, n, TR("%ld 分钟前"), s / 60);
    else if (s < 86400) snprintf(out, n, TR("%ld 小时前"), s / 3600);
    else                snprintf(out, n, TR("%ld 天前"), s / 86400);
}

static void build_sub_cell(lv_obj_t *t)
{
    int y = 4;
    t = uk_scroll(t, 0, UI_SUB_VIEW, 1000);

    s_sg_nr_sec = uk_section(t, y, TR("5G 服务小区")); y += 20;
    lv_obj_t *nr = uk_card(t, UK_MARGIN, y, UK_CARD_W, 6 * UK_ROW_H);
    static const char *const k_nr_cap[6] = { N_("频段"), "ARFCN", "PCI", "Cell ID", "PLMN", "RSRP / RSRQ / SINR" };
    for (int i = 0; i < 6; i++) s_sg_nr[i] = uk_row(nr, i * UK_ROW_H, TR(k_nr_cap[i]), i == 0);
    y += 6 * UK_ROW_H + 10;

    /* LTE 和 5G 一样逐行列（2026-09-25：原来挤成一行看不懂） */
    s_sg_lte_sec = uk_section(t, y, TR("LTE 服务小区")); y += 20;
    lv_obj_t *lte = uk_card(t, UK_MARGIN, y, UK_CARD_W, 6 * UK_ROW_H);
    static const char *const k_lt_cap[6] = { N_("频段"), "EARFCN", "PCI", "Cell ID", "RSRP / RSRQ / SINR", "RSSI" };
    for (int i = 0; i < 6; i++) s_sg_lt[i] = uk_row(lte, i * UK_ROW_H, TR(k_lt_cap[i]), i == 0);
    y += 6 * UK_ROW_H + 10;

    uk_section(t, y, TR("网络")); y += 20;
    lv_obj_t *net = uk_card(t, UK_MARGIN, y, UK_CARD_W, 4 * UK_ROW_H);
    static const char *const k_net_cap[4] = { N_("网络模式"), "WAN", N_("制式"), N_("高铁模式") };
    for (int i = 0; i < 4; i++) s_sg_net[i] = uk_row(net, i * UK_ROW_H, TR(k_net_cap[i]), i == 0);
    lv_obj_set_style_text_font(s_sg_net[3], UF.cj14, 0);
    y += 4 * UK_ROW_H + 10;

    /* 支持频段不在这页列（2026-09-25）：锁频页的频段按钮就是同一份清单 */

    /* 邻小区一块由 build_sub_net 挂在这下面（net_reflow 定位置和滚动范围） */
    s_nh_scroll[NH_CELL] = t;
    s_nh_base[NH_CELL] = y;
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}


