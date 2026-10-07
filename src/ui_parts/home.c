/*
 * ui_parts/home.c - 首页：信号 / 邻区 / Tailscale 卡，和它们用的辅助设备状态.
 * Part of src/ui.c, which #includes it: the parts share ui.c's static state
 * and are one translation unit, not compiled on their own (the Makefile
 * lists only ui.c). Split 2026-09-26 so the 6000-line file stops being
 * the place every change collides; the built binary is byte-identical.
 *
 * SPDX-License-Identifier: MIT
 */
/* ---- Home: 信号 / 邻区 / Tailscale ----
 * Mirrors the old litehtml signal page section for section. The headline is the
 * aggregate line ("5G SA · 3 NR 载波 · 240 MHz"): mode, carrier count and
 * TOTAL aggregated bandwidth. An earlier pass showed only the primary
 * carrier ("SA · n78 · 100") and the user immediately flagged the page as
 * having no focal point — the number people actually look for is how much
 * spectrum is currently aggregated, not which band the PCC happens to be. */
static void home_reflow(void);

/* One 40 px row inside the status card; the box carries its labels so the
 * whole row moves with one lv_obj_set_y. cb = tappable with a chevron. */
static void home_row(home_row_t *r, lv_obj_t *c, const char *key, lv_event_cb_t cb, void *user)
{
    r->box = uk_box(c, 0, 0, UK_CARD_W, UK_ROW_H, T->card, 0);
    lv_obj_set_style_bg_opa(r->box, LV_OPA_TRANSP, 0);
    uk_sep(r->box, 0);
    r->key = uk_label(r->box, UF.cj14, T->t2, UK_PAD, 11, key);
    r->val = uk_label_r(r->box, UF.n15, T->t1, UK_CARD_W - UK_PAD - (cb ? 16 : 0), 10, "");
    if (cb) {
        uk_chevron(r->box, 0);
        uk_tappable(r->box, cb, user);
    }
}

/* 「国家 省 市」只留国家和最后一段（城市）；两段以内原样。英文
 * （agent geo_en）以「, 」分段，地名里本来就有空格（New Taipei） */
static const char *geo_short(const char *geo, char *out, size_t n)
{
    const char *first_c = strstr(geo, ", ");
    if (first_c) {
        const char *last_c = first_c;
        for (const char *p = first_c; (p = strstr(p + 2, ", ")); ) last_c = p;
        if (last_c == first_c) { snprintf(out, n, "%s", geo); return out; }
        snprintf(out, n, "%.*s%s", (int)(first_c - geo), geo, last_c);
        return out;
    }
    const char *first_sp = strchr(geo, ' ');
    const char *last_sp = strrchr(geo, ' ');
    if (!first_sp || first_sp == last_sp) { snprintf(out, n, "%s", geo); return out; }
    snprintf(out, n, "%.*s%s", (int)(first_sp - geo), geo, last_sp);
    return out;
}

static int sim_usable_ui(const char *st) { return !st || !*st || strstr(st, "ready") != NULL; }


/* Load the logo for slug (NULL = hide). Files are optional (trademarks, not in
 * the public repo): a missing file hides the logo rather than draw LVGL's
 * broken-image box. Where it goes is home_logo_place(). */
static void home_logo_set(const char *slug)
{
    if (slug && !slug[0]) slug = NULL;
    if (slug == s_cc_logo_slug || (slug && s_cc_logo_slug && !strcmp(slug, s_cc_logo_slug))) return;
    char path[160];
    if (slug) {
        const char *dir = getenv("U60_DEVUI_LOGO_DIR");
        snprintf(path, sizeof path, "%s/%s%s.png",
                 dir && *dir ? dir : "/data/plugins/u60pro-devui/operator-logos", slug, T->dark ? "-w" : "");
        if (access(path, R_OK) != 0) slug = NULL;
    }
    if (slug) { snprintf(s_cc_logo_buf, sizeof s_cc_logo_buf, "%s", slug); slug = s_cc_logo_buf; }
    s_cc_logo_slug = slug;
    s_cc_logo_w = s_cc_logo_h = 0;
    if (slug) {
        char src[164];
        snprintf(src, sizeof src, "A:%s", path);
        lv_image_header_t hd;
        if (lv_image_decoder_get_info(src, &hd) == LV_RESULT_OK && hd.w > 0 && hd.w <= 96 && hd.h <= 18) {
            lv_image_set_src(s_cc_logo, src);
            lv_obj_set_style_image_opa(s_cc_logo, T->dark ? 217 : LV_OPA_COVER, 0);
            s_cc_logo_w = hd.w; s_cc_logo_h = hd.h;
        } else s_cc_logo_slug = NULL;
    }
}

/* 2026-09-26 真机反馈两轮后：logo 放顶行最左，代替状态点和运营商名（颜色整块底色已经
 * 在说），后面接「5G SA · 漫游」；没有 logo 时照旧是点 + 名字。 */
static void home_logo_place(void)
{
    /* 量过像素（9-26）：左边从 UK_PAD 起，和右边「信号强」离卡边一样远；logo 竖直中心
     * 对齐顶行汉字的字面中心（卡内 y≈16，不是标签框中心 19，否则 logo 偏低 2～3 像素）。 */
    int w = s_cc_logo_w, h = s_cc_logo_h;
    int x = w ? UK_PAD + w + 6 : 26;
    if (w) lv_obj_set_pos(s_cc_logo, UK_PAD, 16 - h / 2);
    uk_show(s_cc_logo, w > 0);
    uk_show(s_cc_hero.dot, w == 0);
    lv_obj_set_x(s_cc_hero.st, x);
    lv_obj_set_width(s_cc_hero.st, UK_CARD_W - x - UK_PAD);
}

/* The status block's hint row: 查原因 › (网络诊断), or 详情 › while it shows a
 * write transaction (E4, the 事务页). */
/* DD18 (write-op-layer.md 打开自动退回): the one-time 「自动退回已打开」 line at
 * the top of Home, in the 事务行's dress (full width, card ground, a dot,
 * a line under it). Shown and acknowledged by op.c (op_notice_paint). */
static lv_obj_t *s_hn_box, *s_hn_lbl, *s_hn_ack;
static int s_hn_h = UK_ROW_H;
static void op_notice_cb(lv_event_t *e);

static int s_cc_hint_sub = SUB_DIAG;
static void cc_hint_cb(lv_event_t *e) { LV_UNUSED(e); sub_open(s_cc_hint_sub); }

static void build_home(lv_obj_t *t)
{
    /* Worst case (5 active carriers, 5 Tailscale rows, …) is
     * ~1010; the spacer is moved by home_reflow to the real height. */
    t = s_home_scroll = uk_scroll(t, 0, UI_VIEW_H, 1100);

    /* 自动退回已打开 · 知道了: text left (wraps), 知道了 a ≥40 px target right */
    {
        enum { DOT_X = UK_PAD + UK_MARGIN, LBL_X = UK_PAD + UK_MARGIN + 16, ACK_W = 72 };
        s_hn_box = uk_box(t, 0, 0, UK_W, UK_ROW_H, T->card, 0);
        lv_obj_set_style_border_side(s_hn_box, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(s_hn_box, 1, 0);
        lv_obj_set_style_border_color(s_hn_box, lv_color_hex(T->sep), 0);
        lv_obj_t *dot = uk_dot(s_hn_box, DOT_X, 0, 8, T->accT);
        s_hn_lbl = uk_label_w(s_hn_box, UF.cj13, T->t1, LBL_X, 0, UK_W - LBL_X - ACK_W, 1,
                              TR("自动退回已开：切模式没通会退回"));
        lv_obj_update_layout(s_hn_lbl);
        int lh = (int)lv_obj_get_height(s_hn_lbl);
        s_hn_h = lh + 18 > UK_ROW_H ? lh + 18 : UK_ROW_H;
        lv_obj_set_height(s_hn_box, s_hn_h);
        lv_obj_set_y(s_hn_lbl, (s_hn_h - lh) / 2);
        lv_obj_set_y(dot, (s_hn_h - 8) / 2);
        lv_obj_t *ack = s_hn_ack = uk_box(s_hn_box, UK_W - ACK_W, 0, ACK_W, s_hn_h - 1, T->card, 0);
        lv_obj_set_style_bg_opa(ack, LV_OPA_TRANSP, 0);
        lv_obj_t *al = uk_label(ack, UF.cj14, T->accT, 0, 0, TR("知道了"));
        lv_obj_align(al, LV_ALIGN_CENTER, 0, 0);
        uk_tappable(ack, op_notice_cb, NULL);
        uk_show(s_hn_box, 0);
    }

    /* status card: the conclusion, then Wi-Fi · traffic · carrier summary */
    s_cell_card = uk_card(t, UK_MARGIN, 4, UK_CARD_W, UK_HERO_H + 4 * UK_ROW_H);
    lv_obj_t *c = s_cell_card;
    uk_hero(&s_cc_hero, c, UF.cj24b);
    uk_show(s_cc_hero.unit, 0);
    lv_obj_set_x(s_cc_hero.big, UK_PAD);   /* 左右边距一致（右边是 UK_PAD） */
    /* 提示行：结论异常时整行可点，右端「查原因 ›」开网络诊断（大字本身不可点） */
    s_cc_hint_box = uk_box(c, 0, UK_HERO_H, UK_CARD_W, UK_ROW_H, T->card, 0);
    lv_obj_set_style_bg_opa(s_cc_hint_box, LV_OPA_TRANSP, 0);
    s_cc_hint = uk_label_w(s_cc_hint_box, UF.cj13, T->t2, UK_PAD, 8, UK_CARD_W - 2 * UK_PAD, 1, "");
    s_cc_diag = uk_label(s_cc_hint_box, UF.cj13, T->accT, 0, 8, TR("查原因 ›"));
    uk_tappable(s_cc_hint_box, cc_hint_cb, NULL);
    uk_show(s_cc_diag, 0);
    uk_show(s_cc_hint_box, 0);
    /* 顶行（运营商 · 制式 · 本地/漫游）名字可能很长：限宽，末尾「…」 */
    lv_obj_set_size(s_cc_hero.st, UK_CARD_W - 26 - UK_PAD, 18);
    lv_label_set_long_mode(s_cc_hero.st, LV_LABEL_LONG_MODE_DOTS);
    /* 运营商 logo（2026-09-26）：顶行右边本来空着，透明底 PNG，高 16、宽 ≤96 */
    s_cc_logo = lv_image_create(c);
    lv_obj_remove_flag(s_cc_logo, LV_OBJ_FLAG_CLICKABLE);
    uk_show(s_cc_logo, 0);
    /* 摘要行（2026-09-25 按任务分标签）：蜂窝 / Wi-Fi / 出口 各一行，点了跳到
     * 那个标签；流量只读，没有 ›、没有按下态。 */
    home_row(&s_hr_ca, c, TR("载波"), tab_go_cb, (void *)(intptr_t)TAB_CELL);
    home_row(&s_hr_wifi, c, "Wi-Fi", tab_go_cb, (void *)(intptr_t)TAB_WIFI);
    home_row(&s_hr_exit, c, TR("出口"), tab_go_cb, (void *)(intptr_t)TAB_EXIT);
    home_row(&s_hr_traf, c, TR("流量"), NULL, NULL);
    s_hr_ca_sub = uk_label_w(s_hr_ca.box, UF.cj12, T->t3, UK_PAD, 34, UK_CARD_W - 2 * UK_PAD - 16, 0, "");
    s_hr_exit_sub = uk_label_w(s_hr_exit.box, UF.cj12, T->t3, UK_PAD, 34, UK_CARD_W - 2 * UK_PAD - 16, 0, "");
    lv_obj_t *subs[2] = { s_hr_ca_sub, s_hr_exit_sub };
    for (int i = 0; i < 2; i++) {
        lv_obj_set_height(subs[i], 18);
        lv_label_set_long_mode(subs[i], LV_LABEL_LONG_MODE_DOTS);
    }
    lv_obj_set_y(s_hr_ca.box, UK_HERO_H);
    lv_obj_set_y(s_hr_wifi.box, UK_HERO_H + UK_ROW_H);
    lv_obj_set_y(s_hr_exit.box, UK_HERO_H + 2 * UK_ROW_H);
    lv_obj_set_y(s_hr_traf.box, UK_HERO_H + 3 * UK_ROW_H);
    lv_label_set_text(s_hr_wifi.val, "—");
    for (int i = 0; i < 2; i++) {
        lv_obj_t *v = i ? s_hr_ca.val : s_hr_exit.val;
        lv_label_set_text(v, "—");
        lv_obj_set_width(v, 210);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(v, UF.cj14, 0);
        lv_obj_set_height(v, 20);    /* 一行：放不下末尾「…」，不折行 */
        lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -(UK_PAD + 16), 11);
    }
    lv_label_set_text(s_hr_traf.val, "—");

    /* 情景: what the device decided about where it is (opens the 情景 page) */
    /* 和上面「出口」一行同一写法：左键名、右边情景名 ›，小字一行铺满卡宽（9-26 真机：
     * 原来按半块磁贴排，大字 + 两行小字，字多了就折行、右边空一大片） */
    s_sc_card = uk_card(t, UK_MARGIN, 0, UK_CARD_W, SC_CARD_H);
    uk_label(s_sc_card, UF.cj14, T->t2, UK_PAD, 11, TR("情景"));
    s_sc_state = uk_label_r(s_sc_card, UF.cj14, T->t1, UK_CARD_W - UK_PAD - 16, 11, "");
    s_sc_note = uk_label_w(s_sc_card, UF.cj12, T->t2, UK_PAD, 34, UK_CARD_W - 2 * UK_PAD, 0, "");
    uk_chevron(s_sc_card, 0);   /* 开页：有 ›（DESIGN.md §4 导航） */
    uk_tappable(s_sc_card, sc_card_cb, NULL);
    uk_show(s_sc_card, 0);

    /* 载波明细: every carrier row (moved to the top of 蜂窝 by build_cellular) */
    s_ca_card = uk_card(t, UK_MARGIN, 0, UK_CARD_W, CA_CARD_TOP + 40);
    uk_show(s_ca_card, 0);
    uk_label(s_ca_card, UF.cj12, T->t3, UK_PAD, 10, TR("载波"));
    s_ca_qos = uk_label_r(s_ca_card, UF.n12, T->t2, UK_CARD_W - UK_PAD, 9, "");
    c = s_ca_card;
    for (int i = 0; i < CA_SLOTS; i++) {
        home_ca_t *k = &s_ca[i];
        k->box = uk_box(c, 0, CA_CARD_TOP + i * 40, UK_CARD_W, 40, T->card, 0);
        lv_obj_set_style_bg_opa(k->box, LV_OPA_TRANSP, 0);
        k->sep = uk_sep(k->box, 0);
        k->band   = uk_label(k->box, UF.n17, T->t1, UK_PAD, 10, "");
        k->bw     = uk_label(k->box, UF.n12, T->t3, 60, 15, "");
        k->rsrp   = uk_label_r(k->box, UF.n15, T->t1, 144, 4, "");
        k->rsrp_c = uk_label_r(k->box, UF.n11, T->t3, 144, 23, "RSRP");
        k->sinr   = uk_label_r(k->box, UF.n17, T->okT, 190, 3, "");
        k->sinr_c = uk_label_r(k->box, UF.n11, T->t3, 190, 23, "SINR");
        k->pci    = uk_label_r(k->box, UF.n11, T->t3, UK_CARD_W - UK_PAD, 5, "");
        k->arfcn  = uk_label_r(k->box, UF.n11, T->t3, UK_CARD_W - UK_PAD, 21, "");
        k->ina      = uk_label(k->box, UF.n15, T->t3, UK_PAD, 7, "");
        k->ina_tag  = uk_label(k->box, UF.cj12, T->t3, 100, 9, TR("未激活"));
        k->ina_info = uk_label_r(k->box, UF.n11, T->t3, UK_CARD_W - UK_PAD, 9, "");
        uk_show(k->box, 0);
    }



    /* Tailscale: hidden entirely when the device has no tailscaled. The card
     * is the entry to the peer list (no 功能 tile for it). */
    s_ts_card = uk_card(t, UK_MARGIN, 500, UK_CARD_W, UK_ROW_H);
    /* 「节点」「出口」here are Tailscale peers / exit node: own TRC keys, other pages say them differently */
    const char *const ts_keys[TS_HOME_ROWS] = { "Tailscale", TRC("tailscale", "本机"), TRC("tailscale", "节点"),
                                                TRC("tailscale", "子网"), TRC("tailscale", "出口") };
    for (int i = 0; i < TS_HOME_ROWS; i++) {
        if (i) s_ts_sep[i] = uk_sep(s_ts_card, i * UK_ROW_H);
        s_ts_key[i] = uk_label(s_ts_card, UF.cj14, i ? T->t2 : T->t1, UK_PAD, i * UK_ROW_H + 11, ts_keys[i]);
        s_ts_val[i] = uk_label_r(s_ts_card, i ? UF.n15 : UF.cj14, T->t1, UK_CARD_W - UK_PAD - (i ? 0 : 16), i * UK_ROW_H + (i ? 10 : 11), "");
    }
    s_ts_dot = uk_dot(s_ts_card, 0, 17, 7, T->green);
    uk_chevron(s_ts_card, 0);   /* 整张卡开 Tailscale 页 */
    uk_tappable(s_ts_card, tile_click_cb, (void *)(intptr_t)SUB_TS);
    uk_show(s_ts_card, 0);
    home_reflow();
}

static int home_visible_h(lv_obj_t *o) { return lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) ? -1 : (int)lv_obj_get_style_height(o, 0); }

/* Stack the home cards under whatever the signal card is showing. Property
 * changes on existing objects only; nothing is allocated. */
static void home_reflow(void)
{
    int y = 4;
    if (home_visible_h(s_hn_box) >= 0) y += s_hn_h;   /* 自动退回已打开 (DD18) */
    lv_obj_set_y(s_cell_card, y);
    y += home_visible_h(s_cell_card) + UK_MARGIN;
    /* 情景：独立的一块，整行 */
    if (home_visible_h(s_sc_card) >= 0) {
        lv_obj_set_y(s_sc_card, y);
        y += SC_CARD_H + UK_MARGIN;
    }
    /* Tailscale：2026-09-25 用户要回首页（离家时靠它连回来，一眼要看到） */
    if (!lv_obj_has_flag(s_ts_card, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_y(s_ts_card, y);
        y += (int)lv_obj_get_style_height(s_ts_card, 0) + UK_MARGIN;
    }
    if (s_ch_net_card) {
        lv_obj_set_y(s_ch_net_card, y);
        y += (int)lv_obj_get_style_height(s_ch_net_card, 0) + UK_MARGIN;
    }
    uk_scroll_extent(s_home_scroll, y + UK_TAB_PAD);
    /* 载波等卡片在别的标签上，显隐变了那边也要重排 */
    cell_reflow();
    net_relayout();
}

/* No snapshot from the data service. Before the first one: 「正在读取…」.
 * After: the numbers stay where they were, dimmed, and the block says when
 * they stopped — a data-service outage is not "no signal". */
static int  s_ever_valid;
static long s_last_valid_wall;
static int  s_cc_tone = -1;
static void home_signal_down(void)
{
    static char c_st[64];
    uk_hero_tone(&s_cc_hero, 3);
    s_cc_tone = 3;
    home_logo_set(NULL);   /* 数据停了：不知道现在在谁的网上 */
    home_logo_place();
    if (!s_ever_valid) {
        set_label_fmt(s_cc_hero.st, c_st, sizeof c_st, "%s", TR("正在读取…"));
        lv_label_set_text(s_cc_hero.big, "--");
        lv_label_set_text(s_cc_hero.rtop, "");
        lv_label_set_text(s_cc_hero.r1, "");
        lv_label_set_text(s_cc_hero.r2, "");
        uk_hero_layout(&s_cc_hero);
    } else {
        char hm[8] = "--:--";
        long alive = data_backend_alive_wall();
        time_t tt = (time_t)(alive ? alive : s_last_valid_wall);
        struct tm tm;
        if (localtime_r(&tt, &tm)) strftime(hm, sizeof hm, "%H:%M", &tm);
        set_label_fmt(s_cc_hero.st, c_st, sizeof c_st, TR("数据服务掉线 · 数字停在 %s"), hm);
        lv_label_set_text(s_cc_hero.rtop, "");
        lv_label_set_text(s_cc_hero.big, TR("读不到数据"));
    }
    for (int i = 0; i < CA_SLOTS; i++) {
        uk_text_color(s_ca[i].band, T->t3);
        uk_text_color(s_ca[i].rsrp, T->t3);
        uk_text_color(s_ca[i].sinr, T->t3);
    }
    /* 数字停住了：下面凡是跟着网络走的都调淡，别让人当成实时 */
    uk_text_color(s_hr_wifi.val, T->t3);
    uk_text_color(s_hr_traf.val, T->t3);
    uk_text_color(s_hr_exit.val, T->t3);
    uk_text_color(s_hr_ca.val, T->t3);
    uk_text_color(s_cc_hero.r1, T->t3);
    home_reflow();
}

/* ---- auxiliary device state ----
 * Things the main /state fields above don't cover. DHCP pool, direct power
 * supply and the cellular data/roaming switches come from zwrt-datad's
 * /state (T13: datad is the only program polling ubus/uci; this used to be a
 * 5 s popen running uci and ubus on the UI thread). Radio up/down is read
 * from /sys, Wi-Fi power save still needs `iw` (nl80211, not ubus) — both
 * only while a page that shows them is visible, at most every 5 s.
 *
 * A switch the user just flipped keeps its new value for AUX_HOLD_MS or
 * until datad reports the same value, so it does not snap back while datad's
 * next read is still on its way. */
static int  s_aux_w24 = -1, s_aux_w5 = -1, s_aux_psm = -1, s_aux_dps = -1;
/* 蜂窝页的移动数据 / 数据漫游：zwrt_data get_wwaniface 的 enable / roam_enable */
static int  s_aux_data = -1, s_aux_roam = -1;
static char s_aux_pool[48];

#define AUX_HOLD_MS 30000
static uint32_t s_hold_dps, s_hold_data, s_hold_roam;   /* lv_tick of the local flip, 0 = none */
/* Wi-Fi APs: datad reloads Wi-Fi after the write, the interfaces come and go for a few s */
static uint32_t s_hold_w24, s_hold_w5;

/* nonzero without ever being ahead of now (tick | 1 could be, and then
 * now - hold wraps and the hold is dropped at once) */
static void aux_hold(uint32_t *hold) { uint32_t t = lv_tick_get(); *hold = t ? t : 1; }

/* Take datad's value unless a local flip is still being held. */
static void aux_take(int *cur, uint32_t *hold, int from_datad, uint32_t now)
{
    if (from_datad < 0) return;                     /* not in /state: keep what we have */
    if (*hold && from_datad != *cur && now - *hold < AUX_HOLD_MS) return;
    *hold = 0;
    *cur = from_datad;
}

static int sys_oper_up(const char *ifname)
{
    char path[64], buf[16] = "";
    FILE *f;

    snprintf(path, sizeof path, "/sys/class/net/%s/operstate", ifname);
    f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
    fclose(f);
    return !strncmp(buf, "up", 2);
}

static void aux_refresh(int active, const devui_data_t *d)
{
    static uint32_t last;
    uint32_t now = lv_tick_get();
    char line[64];
    FILE *fp;

    if (d->valid) {
        aux_take(&s_aux_dps,  &s_hold_dps,  d->dps_mode,  now);
        aux_take(&s_aux_data, &s_hold_data, d->cell_data, now);
        aux_take(&s_aux_roam, &s_hold_roam, d->cell_roam, now);
        s_aux_pool[0] = 0;
        if (d->dhcp_ip[0] && d->dhcp_start[0])
            ui_dhcp_pool_text(d->dhcp_ip, d->dhcp_start, d->dhcp_limit, s_aux_pool, sizeof s_aux_pool);
    }

    if (!active) return;
    if (last && now - last < 5000) return;
    last = now;

    aux_take(&s_aux_w24, &s_hold_w24, sys_oper_up("wlan0"), now);
    aux_take(&s_aux_w5,  &s_hold_w5,  sys_oper_up("wlan2"), now);
    fp = popen(
        "ps=$(iw dev wlan0 get power_save 2>/dev/null | grep -o 'o[nf]*' | tail -1);"
        "[ -z \"$ps\" ] && ps=$(iw dev wlan2 get power_save 2>/dev/null | grep -o 'o[nf]*' | tail -1);"
        "echo PSM=$([ \"$ps\" = on ] && echo 1 || echo 0)",
        "r");
    if (!fp) return;
    while (fgets(line, sizeof line, fp))
        if (!strncmp(line, "PSM=", 4)) s_aux_psm = atoi(line + 4);
    pclose(fp);
}

/* A switch row: name on the left, state text right-aligned next to the
 * switch, lv_switch on the right — all on one line. The first version
 * stacked the state under the name, which at 34px row pitch read as if the
 * state belonged to the row below. Returns the switch so the caller can
 * bind a callback; `state_out` receives the state label. */

static void sw_apply(lv_obj_t *sw, int on)
{
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else    lv_obj_remove_state(sw, LV_STATE_CHECKED);
}

/* A Wi-Fi client's signal in words; zte-agent grades it (netinfo.rs
 * signal_tier), the admin web words the same grade in its own language. */
static const char *wifi_sig_word(const char *tier)
{
    if (!strcmp(tier, "great")) return TR("信号很好");
    if (!strcmp(tier, "good"))  return TR("信号好");
    if (!strcmp(tier, "fair"))  return TR("信号一般");
    if (!strcmp(tier, "weak"))  return TR("信号弱");
    return NULL;
}

