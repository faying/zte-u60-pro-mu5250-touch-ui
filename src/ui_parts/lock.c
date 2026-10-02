/*
 * ui_parts/lock.c - 锁频子页.
 * Part of src/ui.c, which #includes it: the parts share ui.c's static state
 * and are one translation unit, not compiled on their own (the Makefile
 * lists only ui.c). Split 2026-09-26 so the 6000-line file stops being
 * the place every change collides; the built binary is byte-identical.
 *
 * SPDX-License-Identifier: MIT
 */
/* ---- 锁频 subpage ----
 * Mirrors the litehtml 锁频 page: network-mode selector, one band picker per
 * technology, reset-to-default. Every action that actually reaches the modem
 * is two-stage (tap to arm, tap again within 5s to send) — a mis-tap here
 * drops the connection, and the same pattern already guards the
 * switch-to-vendor-UI button. */
static void band_group_apply(int gi)
{
    band_group_t *g = &s_bg[gi];
    char csv[256] = "";
    char cmd[400], params[300];
    int o = 0, n = 0;

    for (int i = 0; i < g->n; i++)
        if (g->sel[i]) {
            o += snprintf(csv + o, sizeof csv - (size_t)o, "%s%d", n ? "," : "", g->band_no[i]);
            n++;
        }
    if (!n) return;   /* locking zero bands would strand the modem */
    if (gi == BG_LTE)
        snprintf(cmd, sizeof cmd,
                 "ubus call zte_nwinfo_api nwinfo_set_lte_ext_band '{\"lte_band\":\"%s\"}' >/dev/null 2>&1 &",
                 csv);
    else
        snprintf(cmd, sizeof cmd,
                 "ubus call zte_nwinfo_api nwinfo_set_nrbandlock '{\"nr5g_type\":\"%s\",\"nr5g_band\":\"%s\"}' >/dev/null 2>&1 &",
                 gi == BG_SA ? "0" : "1", csv);   /* vendor web: SA "0", NSA "1" */
    snprintf(params, sizeof params, "{\"bands\":\"%s\"}", csv);
    data_control(gi == BG_LTE ? "band.set_lte" : gi == BG_SA ? "band.set_nr_sa" : "band.set_nr_nsa",
                 params, cmd);
}

static void band_summary_set(int gi)
{
    band_group_t *g = &s_bg[gi];
    char buf[200];
    int o = 0, n = 0, all = 1;

    for (int i = 0; i < g->n; i++) {
        if (!g->sel[i]) { all = 0; continue; }
        n++;
        if (o < (int)sizeof buf - 8)
            o += snprintf(buf + o, sizeof buf - (size_t)o, "%s%c%d",
                          o ? " " : "", g->prefix, g->band_no[i]);
    }
    if (!g->n)      lv_label_set_text(g->summary, "—");
    else if (all)   lv_label_set_text(g->summary, TR("全部频段（未锁定）"));
    else if (!n)    lv_label_set_text(g->summary, TR("未选频段"));
    else            lv_label_set_text(g->summary, buf);
}

static void band_chip_paint(int gi, int i)
{
    band_group_t *g = &s_bg[gi];
    /* Every band selected = not locked, the default: muted (accS), since 21
     * saturated chips would read as 21 alarms. A real lock selection is
     * fillBlue with white text. */
    int all = 1;
    for (int k = 0; k < g->n; k++) if (!g->sel[k]) all = 0;
    if (g->sel[i] && all) {
        uk_bg(g->chip[i], T->accS);
        uk_text_color(g->chip_lbl[i], T->accT);
    } else {
        uk_chip_set(g->chip[i], g->chip_lbl[i], g->sel[i]);
    }
}
static void band_chip_cb(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    int gi = code / 64, i = code % 64;
    if (gi < 0 || gi > 2 || i >= s_bg[gi].n) return;
    s_bg[gi].sel[i] = !s_bg[gi].sel[i];
    for (int k = 0; k < s_bg[gi].n; k++) band_chip_paint(gi, k);
    band_summary_set(gi);
}

static void band_apply_cb(lv_event_t *e)
{
    int gi = (int)(intptr_t)lv_event_get_user_data(e);
    band_group_t *g = &s_bg[gi];
    uint32_t now = lv_tick_get();

    if (g->arm && now - g->arm < 5000) {
        g->arm = 0;
        lv_label_set_text(g->apply_lbl, TR("已下发…"));
        uk_button_kind(g->apply_btn, g->apply_lbl, UK_BTN_PLAIN);
        g->sent = now ? now : 1;
        band_group_apply(gi);
        return;
    }
    g->arm = now ? now : 1;
    lv_label_set_text(g->apply_lbl, TR("再按一次确认"));
    uk_button_kind(g->apply_btn, g->apply_lbl, UK_BTN_ARMED);
}

static void lk_mode_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    uint32_t now = lv_tick_get();
    char cmd[200];

    if (s_lk_mode_want >= 0) {
        /* 上一次还没读回：别叠着发，高亮留在目标上 */
        lv_label_set_text(s_lk_mode_lbl, TR("正在切换，等设备确认…"));
        uk_seg_set(&s_lk_seg, s_lk_mode_want);
        return;
    }
    if (s_lk_mode_pending == idx && s_lk_mode_arm && now - s_lk_mode_arm < 5000) {
        snprintf(cmd, sizeof cmd,
                 "ubus call zte_nwinfo_api nwinfo_set_netselect '{\"net_select\":\"%s\"}' >/dev/null 2>&1 &",
                 k_lk_mode_v[idx]);
        {
            char params[48];
            snprintf(params, sizeof params, "{\"mode\":\"%s\"}", k_lk_mode_v[idx]);
            data_control("network.set_mode", params, cmd);
        }
        s_lk_mode_arm = 0;
        s_lk_mode_pending = -1;
        /* 高亮停在目标上，直到读回是它（「已切到」）或超时（报没切成） */
        s_lk_mode_want = idx;
        s_lk_mode_sent = now ? now : 1;
        uk_seg_set(&s_lk_seg, idx);
        lv_label_set_text(s_lk_mode_lbl, TR("已下发，等设备确认…"));
        return;
    }
    s_lk_mode_arm = now ? now : 1;
    s_lk_mode_pending = idx;
    /* 5G SA only: many countries have no SA and roaming SIMs often can't use
     * it; with no 2G to fall back on that is no signal at all. */
    lv_label_set_text(s_lk_mode_lbl, idx == 2 ? TR("再按一次确认 · 国外和漫游卡常没有 SA")
                                              : TR("再按一次确认切换"));
    uk_seg_set(&s_lk_seg, -1);
    uk_seg_arm(&s_lk_seg, idx);
}

/* Every refresh tick, datad or not (a tap always gets an ending): highlight the
 * 网络模式 the modem is actually on, close the confirm window, and finish a
 * wait for the read-back. sel = the device's mode as an index, known = 0 when
 * datad can't be read (the highlight then stays on the last mode read back).
 * Skipped while a tap is armed so the orange "confirm?" state isn't repainted
 * away by the next tick. */
static void lk_mode_sync(int sel, int known)
{
    if (known) s_lk_mode_real = sel;
    else       sel = s_lk_mode_real;
    if (s_lk_mode_pending >= 0) {
        if (lv_tick_get() - s_lk_mode_arm >= 5000) {
            s_lk_mode_pending = -1;          /* confirm window lapsed */
            s_lk_seg.sel = -2;               /* repaint the real selection next tick */
            lv_label_set_text(s_lk_mode_lbl, TR("切换会短暂断网，需要按两次确认"));
        }
        return;
    }
    if (s_lk_mode_want >= 0) {
        /* 等读回：到了说「已切到」，超时说没切成并显示设备真实的模式。
         * 基带因报错重启过几次后，原厂切换程序只记日志不干活（9-27、9-29 都是），
         * 重启设备后它才恢复，所以再试不成就重启、开机后马上切。 */
        if (known && sel == s_lk_mode_want) {
            lv_label_set_text_fmt(s_lk_mode_lbl, TR("已切到「%s」"), TR(k_lk_mode_n[sel]));
            s_lk_mode_want = -1;
        } else if (lv_tick_get() - s_lk_mode_sent >= LK_MODE_WAIT_MS) {
            if (known && sel >= 0)
                lv_label_set_text_fmt(s_lk_mode_lbl, TR("没切成，设备还是「%s」。\n再试一次，不行就重启设备、开机后马上切"),
                                      TR(k_lk_mode_n[sel]));
            else
                lv_label_set_text(s_lk_mode_lbl, TR("没切成，设备没确认。\n再试一次，不行就重启设备、开机后马上切"));
            s_lk_mode_want = -1;
        } else {
            sel = s_lk_mode_want;
        }
    }
    if (sel != s_lk_seg.sel) uk_seg_set(&s_lk_seg, sel);
}

static void lk_reset_cb(lv_event_t *e)
{
    uint32_t now = lv_tick_get();
    LV_UNUSED(e);
    if (s_lk_reset_arm && now - s_lk_reset_arm < 5000) {
        s_lk_reset_arm = 0;
        data_control("band.reset", "{}",
                     "ubus call zte_nwinfo_api nwinfo_reset_band_cell_setting '{}' >/dev/null 2>&1 &");
        lv_label_set_text(s_lk_reset_lbl, TR("已恢复默认"));
        uk_button_kind(s_lk_reset_btn, s_lk_reset_lbl, UK_BTN_DANGER);
        return;
    }
    s_lk_reset_arm = now ? now : 1;
    lv_label_set_text(s_lk_reset_lbl, TR("再按一次确认"));
    uk_button_kind(s_lk_reset_btn, s_lk_reset_lbl, UK_BTN_ARMED);
}

/* Chips flow left to right, as many per line as fit; the card grows with them. */
static int band_group_layout(int gi)
{
    band_group_t *g = &s_bg[gi];
    int x = UK_PAD, y = 36, maxx = UK_CARD_W - UK_PAD;
    for (int i = 0; i < g->n; i++) {
        lv_obj_update_layout(g->chip_lbl[i]);
        int w = lv_obj_get_width(g->chip_lbl[i]) + 22;
        lv_obj_set_width(g->chip[i], w);
        if (x + w > maxx) { x = UK_PAD; y += 36; }
        lv_obj_set_pos(g->chip[i], x, y);
        x += w + 6;
    }
    y += g->n ? 36 : 0;
    lv_obj_set_y(g->apply_btn, y + 4);
    int h = y + 4 + 36 + 12;
    lv_obj_set_height(g->card, h);
    return h;
}

static void lock_reflow(void)
{
    int y = 4;   /* 网络模式 2026-09-25 搬到蜂窝标签，这页只剩频段 */
    for (int gi = 0; gi < 3; gi++) {
        lv_obj_set_y(s_bg[gi].sec, y);
        lv_obj_set_y(s_bg[gi].card, y + 20);
        y += 20 + (int)lv_obj_get_style_height(s_bg[gi].card, 0) + 10;
    }
    lv_obj_set_y(s_lk_reset_sec, y);
    lv_obj_set_y(s_lk_reset_card, y + 20);
    uk_scroll_extent(s_lk_scroll, y + 20 + 88 + 16);
}

static void band_group_build(lv_obj_t *t, int gi, const char *title, char prefix)
{
    band_group_t *g = &s_bg[gi];
    g->prefix = prefix;
    g->sec = uk_section(t, 0, TR(title));   /* title: an N_() literal */
    g->card = uk_card(t, UK_MARGIN, 0, UK_CARD_W, 100);
    g->summary = uk_label_w(g->card, UF.cj13, T->t2, UK_PAD, 11, UK_CARD_W - 2 * UK_PAD, 0, "—");
    for (int i = 0; i < BAND_MAX; i++) {
        g->chip[i] = uk_chip(g->card, UK_PAD, 36, "", &g->chip_lbl[i]);
        uk_tappable(g->chip[i], band_chip_cb, (void *)(intptr_t)(gi * 64 + i));
        uk_show(g->chip[i], 0);
    }
    g->apply_btn = uk_button(g->card, UK_PAD, 40, UK_CARD_W - 2 * UK_PAD, 36, TR("应用锁频"), UK_BTN_PLAIN,
                             band_apply_cb, (void *)(intptr_t)gi, &g->apply_lbl);
    band_group_layout(gi);
}

static void build_sub_lock(lv_obj_t *t)
{
    t = s_lk_scroll = uk_scroll(t, 0, UI_SUB_VIEW, 1400);
    band_group_build(t, BG_SA,  N_("5G SA 频段"), 'n');
    band_group_build(t, BG_NSA, N_("5G NSA 频段"), 'n');
    band_group_build(t, BG_LTE, N_("4G 频段"), 'B');

    s_lk_reset_sec = uk_section(t, 0, TR("恢复"));
    s_lk_reset_card = uk_card(t, UK_MARGIN, 0, UK_CARD_W, 88);
    s_lk_reset_btn = uk_button(s_lk_reset_card, UK_PAD, 12, UK_CARD_W - 2 * UK_PAD, 36, TR("恢复默认配置"), UK_BTN_DANGER,
                               lk_reset_cb, NULL, &s_lk_reset_lbl);
    uk_label_w(s_lk_reset_card, UF.cj12, T->t3, UK_PAD, 58, UK_CARD_W - 2 * UK_PAD, 0, TR("清掉全部锁频和锁小区设置，回到自动选网"));
    lock_reflow();
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* Chips = every band the modem supports; selected = the current lock set
 * (9-26: chips used to come from the lock set itself, so after locking to
 * n78 only n78 was left, fully selected, and read "全部频段（未锁定）").
 * sup is empty on a datad without the vendor defaults: fall back to lock. */
static int band_in_csv(const char *csv, int b)
{
    for (const char *p = csv; *p; ) {
        if (atoi(p) == b) return 1;
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
    return 0;
}

static void band_group_sync(int gi, const char *sup, const char *lock)
{
    band_group_t *g = &s_bg[gi];
    char key[sizeof g->last_csv];
    char buf[256];
    char *save = NULL;

    if (!sup[0]) sup = lock;
    snprintf(key, sizeof key, "%s|%s", sup, lock);
    if (!strcmp(g->last_csv, key)) return;
    snprintf(g->last_csv, sizeof g->last_csv, "%s", key);
    snprintf(buf, sizeof buf, "%s", sup);
    g->n = 0;
    for (char *tk = strtok_r(buf, ",", &save); tk && g->n < BAND_MAX; tk = strtok_r(NULL, ",", &save)) {
        int b = atoi(tk);
        if (b <= 0) continue;
        g->band_no[g->n] = b;
        g->sel[g->n] = !lock[0] || band_in_csv(lock, b);   /* all selected == not locked */
        lv_label_set_text_fmt(g->chip_lbl[g->n], "%c%d", g->prefix, b);
        lv_obj_remove_flag(g->chip[g->n], LV_OBJ_FLAG_HIDDEN);
        g->n++;
    }
    for (int i = 0; i < g->n; i++) band_chip_paint(gi, i);
    for (int i = g->n; i < BAND_MAX; i++) lv_obj_add_flag(g->chip[i], LV_OBJ_FLAG_HIDDEN);
    /* The lock read back changed: whatever 已下发… was waiting for is here. */
    if (g->sent) {
        g->sent = 0;
        lv_label_set_text(g->apply_lbl, TR("应用锁频"));
    }
    band_summary_set(gi);
    band_group_layout(gi);
    lock_reflow();
}

