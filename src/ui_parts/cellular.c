/*
 * ui_parts/cellular.c - 蜂窝页：移动数据 / 数据漫游.
 * Part of src/ui.c, which #includes it: the parts share ui.c's static state
 * and are one translation unit, not compiled on their own (the Makefile
 * lists only ui.c). Split 2026-09-26 so the 6000-line file stops being
 * the place every change collides; the built binary is byte-identical.
 *
 * SPDX-License-Identifier: MIT
 */
/* ---- 移动数据 / 数据漫游（蜂窝页） ----
 * 两个开关都会断网或花钱：按一次只是「待确认」（整行说要做什么），5 秒内再按
 * 同一个才下发，和网络模式一样。下发后 8 秒内不拿读数覆盖开关（固件要重拨）。 */
enum { MD_DATA, MD_ROAM };
static lv_obj_t *s_md_sw[2], *s_md_st[2], *s_md_note;
static uint32_t s_md_arm, s_md_hold;
static int s_md_pending = -1, s_md_want;

static void md_note(const char *t, uint32_t col)
{
    static char c[96];
    snprintf(c, sizeof c, "%s", t);
    lv_label_set_text_static(s_md_note, c);
    uk_text_color(s_md_note, col);
}

static void md_sw_cb(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    int on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    uint32_t now = lv_tick_get();

    if (s_md_pending == id && s_md_want == on && s_md_arm && now - s_md_arm < 5000) {
        int data = id == MD_DATA ? on : s_aux_data != 0;
        int roam = id == MD_ROAM ? on : s_aux_roam == 1;
        char cmd[220];
        snprintf(cmd, sizeof cmd,
                 "ubus call zwrt_data set_wwaniface '{\"cid\":1,\"connect_mode\":1,\"enable\":%d,\"roam_enable\":%d}' >/dev/null 2>&1 &",
                 data, roam);
        {
            /* datad reads the whole get_wwaniface object and overrides only
             * what is named here: just the switch pressed, so the other one
             * keeps its live value even if the web page changed it since our
             * last read (the direct-ubus fallback above has to send both). */
            char params[48];
            if (id == MD_DATA) snprintf(params, sizeof params, "{\"enabled\":%d}", data);
            else               snprintf(params, sizeof params, "{\"roaming\":%d}", roam);
            data_control("cellular.set", params, cmd);
        }
        if (id == MD_DATA) { s_aux_data = on; aux_hold(&s_hold_data); }
        else               { s_aux_roam = on; aux_hold(&s_hold_roam); }
        s_md_arm = 0;
        s_md_pending = -1;
        s_md_hold = now ? now : 1;
        md_note(on ? TR("已下发，正在拨号…") : TR("已下发，正在断开…"), T->t2);
        return;
    }
    /* 第一下：开关先回原位，整行说清按第二下会怎样 */
    sw_apply(sw, !on);
    s_md_arm = now ? now : 1;
    s_md_pending = id;
    s_md_want = on;
    md_note(id == MD_DATA ? (on ? TR("再按一次：打开移动数据") : TR("再按一次：关掉移动数据，所有设备断网"))
                          : (on ? TR("再按一次：打开数据漫游，按漫游计费") : TR("再按一次：关掉数据漫游，漫游时会断网")),
            T->warnT);
}

static void md_refresh(void)
{
    static char c_st[2][16];
    uint32_t now = lv_tick_get();
    if (!s_md_note) return;
    if (s_md_pending >= 0 && now - s_md_arm >= 5000) {   /* 没按第二下：作罢 */
        s_md_pending = -1;
        s_md_arm = 0;
        md_note(TR("会断网或按漫游计费，切换要按两次确认"), T->t3);
    }
    if (s_md_hold && now - s_md_hold >= 8000) {
        s_md_hold = 0;
        md_note(TR("会断网或按漫游计费，切换要按两次确认"), T->t3);
    }
    int v[2] = { s_aux_data, s_aux_roam };
    for (int i = 0; i < 2; i++) {
        if (s_md_pending != i && !s_md_hold) sw_apply(s_md_sw[i], v[i] == 1);
        set_label_fmt(s_md_st[i], c_st[i], sizeof c_st[i], "%s", v[i] < 0 ? "—" : v[i] ? TR("已开启") : TR("已关闭"));
    }
}

static void build_cellular(lv_obj_t *t)
{
    static const int ids1[] = { SUB_ESIM, SUB_APN };
    static const char *const names1[] = { N_("SIM 与 eSIM"), "APN" };
    static const int ids2[] = { SUB_NET, SUB_LOCK, SUB_CELL };
    static const char *const names2[] = { N_("运营商选择"), N_("锁频"), N_("小区信息") };
    static const int ids3[] = { SUB_SMS };
    static const char *const names3[] = { N_("短信") };
    t = s_cell_scroll = uk_scroll(t, 0, UI_VIEW_H, 1000);
    lv_obj_set_parent(s_ca_card, t);      /* 当前连接：每个载波（原首页载波明细） */
    lv_obj_set_pos(s_ca_card, UK_MARGIN, 4);
    t = s_cell_rest = lv_obj_create(t);
    lv_obj_remove_style_all(t);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(t, UK_W);
    int y = nav_card(t, 4, N_("SIM 卡"), ids1, names1, 2);

    uk_section(t, y, TR("移动数据"));
    lv_obj_t *mc = uk_card(t, UK_MARGIN, y + 20, UK_CARD_W, 2 * UK_ROW_H + 30);
    s_md_sw[MD_DATA] = toggle_row(mc, 0, TR("移动数据"), 1, &s_md_st[MD_DATA], md_sw_cb, (void *)(intptr_t)MD_DATA);
    s_md_sw[MD_ROAM] = toggle_row(mc, UK_ROW_H, TR("数据漫游"), 0, &s_md_st[MD_ROAM], md_sw_cb, (void *)(intptr_t)MD_ROAM);
    s_md_note = uk_label_w(mc, UF.cj12, T->t3, UK_PAD, 2 * UK_ROW_H + 6, UK_CARD_W - 2 * UK_PAD, 0,
                           TR("会断网或按漫游计费，切换要按两次确认"));
    y += 20 + 2 * UK_ROW_H + 30 + 10;

    uk_section(t, y, TR("网络模式"));
    /* 高 100：没切成时的说明要两行（再试一次，不行就重启后马上切） */
    lv_obj_t *md = uk_card(t, UK_MARGIN, y + 20, UK_CARD_W, 100);
    uk_seg(&s_lk_seg, md, UK_PAD, 12, UK_CARD_W - 2 * UK_PAD, k_lk_mode_n, LK_MODES, lk_mode_cb);   /* uk_seg TRs the items */
    for (int i = 0; i < LK_MODES; i++) s_lk_mode_btn[i] = s_lk_seg.item[i];
    s_lk_mode_lbl = uk_label_w(md, UF.cj12, T->t3, UK_PAD, 54, UK_CARD_W - 2 * UK_PAD, 1, TR("切换会短暂断网，需要按两次确认"));
    y += 20 + 100 + 10;

    y = nav_card(t, y, N_("网络"), ids2, names2, 3);
    y = nav_card(t, y, N_("消息"), ids3, names3, 1);
    lv_obj_set_height(t, y);
    cell_reflow();
}

static void cell_reflow(void)
{
    if (!s_cell_rest) return;
    int y = 0, h = (int)lv_obj_get_style_height(s_ca_card, 0);
    if (!lv_obj_has_flag(s_ca_card, LV_OBJ_FLAG_HIDDEN)) y = 4 + h + UK_MARGIN - 4;
    lv_obj_set_y(s_cell_rest, y);
    uk_scroll_extent(s_cell_scroll, y + (int)lv_obj_get_style_height(s_cell_rest, 0) - 10 + UK_TAB_PAD);
}

/* 出口：流量从哪出去（Tailscale）、出去有多快。 */
static void build_exit(lv_obj_t *t)
{
    static const int ids[] = { SUB_TS, SUB_SPEED };
    static const char *const names[] = { "Tailscale", N_("测速") };
    t = s_nh_scroll[NH_EXIT] = uk_scroll(t, 0, UI_VIEW_H, 1000);
    s_nh_base[NH_EXIT] = 4;   /* 出口 IP（build_sub_net）在最上面，› 行跟在后面（net_reflow） */
    nav_card(t, 4, N_("连接与测速"), ids, names, 2);
    s_exit_nav_sec = s_nav_sec;
    s_exit_nav_card = s_nav_card;
}

static void bench_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    s_box_x += s_box_dir * 4;
    if (s_box_x >= 16) { s_box_x = 16; s_box_dir = -1; }
    if (s_box_x <= 0)  { s_box_x = 0;  s_box_dir = 1; }
    lv_obj_set_pos(s_t_box, UK_MARGIN + s_box_x, 150);
    lv_obj_invalidate(s_sub_page[SUB_PERF]);  /* force a full-screen redraw */
    lv_refr_now(NULL);                         /* render it now -> counts a frame */
}

#ifdef DEVUI_PERF_BENCH_ON_START
static void perf_bench_jump_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    lv_tileview_set_tile_by_index(s_tv, TAB_SYS, 0, LV_ANIM_OFF);
    sub_open(SUB_PERF);
    lv_timer_resume(s_bench_timer);
}
#endif

/* netinfo 里这台设备的 Wi-Fi 读数（按 MAC，没有 MAC 时按 IP） */
static const ni_client_t *wifi_station_for(const char *mac, const char *ip)
{
    const netinfo_t *n = netinfo_get();
    for (int k = 0; k < n->nclients; k++)
        if (mac[0] && n->clients[k].mac[0] && !strcasecmp(mac, n->clients[k].mac)) return &n->clients[k];
    for (int k = 0; k < n->nclients; k++)
        if (ip[0] && !strcmp(ip, n->clients[k].ip)) return &n->clients[k];
    return NULL;
}

/* 「5 GHz · ↓12K/s ↑3K/s · 信号很好」：频段在前（2.4 还是 5，2026-09-25 用户要的），
 * 信号说成话；Wi-Fi 几代和协商速率放不下，管理网页的已连设备页有 */
static void ni_client_line(char *out, size_t n, const ni_client_t *cl)
{
    char ra[16], rb[16], band[16] = "", sig[48] = "";
    if (cl->down_rate >= 0) fmt_rate_top(ra, sizeof ra, cl->down_rate, s_cf_speed_bits, 0); else snprintf(ra, sizeof ra, "-");
    if (cl->up_rate >= 0)   fmt_rate_top(rb, sizeof rb, cl->up_rate, s_cf_speed_bits, 0);   else snprintf(rb, sizeof rb, "-");
    if (cl->band[0]) snprintf(band, sizeof band, "%s · ", cl->band);
    if (wifi_sig_word(cl->signal_tier))
        snprintf(sig, sizeof sig, " · %s", wifi_sig_word(cl->signal_tier));
    if (cl->down_rate < 0 && cl->up_rate < 0) snprintf(out, n, TR("%s速率稍后显示%s"), band, sig);
    else snprintf(out, n, "%s↓%s/s ↑%s/s%s", band, ra, rb, sig);
}

static void refresh_wifi(const devui_data_t *d)
{
    static char c_ssid[72] = "", c_pass[80] = "", c_enc[40] = "", c_state[24] = "";
    /* "Open" means the *encryption mode* says so. An empty wifi_key does
     * NOT mean open: zwrt-datad's `wlan` section reports ssid/enc/enabled
     * and deliberately never includes the passphrase, so keying off the
     * empty password labelled this sae-mixed network as 开放 and, worse,
     * built a `T:nopass` QR that cannot actually join it. */
    int open = d->wifi_enc[0] == 0 || strstr(d->wifi_enc, "none") != NULL;

    set_label_fmt(s_w_ssid, c_ssid, sizeof c_ssid, "%s", d->wifi_ssid[0] ? d->wifi_ssid : "—");
    set_label_fmt(s_w_pass, c_pass, sizeof c_pass, "%s",
                  d->wifi_key[0] ? d->wifi_key
                  : open          ? TR("无密码")
                                  : TR("密码未公开"));
    /* The encryption mode as the backend reports it, not a hand-rolled
     * "WPA"/"open" guess — psk2/sae/sae-mixed are meaningfully different
     * and this is the only place in the UI that can tell you which one. */
    set_label_fmt(s_w_enc, c_enc, sizeof c_enc, "%s", open ? TR("开放")
                                                          : d->wifi_enc);
    set_label_fmt(s_w_state, c_state, sizeof c_state, "%s",
                  d->wifi_enabled ? TR("已开启")
                                  : TR("已关闭"));
    uk_text_color(s_w_state, d->wifi_enabled ? T->okT : T->t3);

    /* Client list — fixed slots, hidden/shown, never created per tick. */
    static char c_cli_n[24] = "";
    static char c_cli_name[WIFI_MAX_CLI][48], c_cli_ip[WIFI_MAX_CLI][28],
                c_cli_mac[WIFI_MAX_CLI][24];
    int n = d->client_n > WIFI_MAX_CLI ? WIFI_MAX_CLI : d->client_n;
    if (d->client_n > WIFI_MAX_CLI)
        set_label_fmt(s_w_cli_n, c_cli_n, sizeof c_cli_n, TR("%d/%d 台"), n, d->client_n);
    else
        set_label_fmt(s_w_cli_n, c_cli_n, sizeof c_cli_n, TRN("%d 台", d->client_n), d->client_n);
    int cli_y = 0;
    for (int i = 0; i < WIFI_MAX_CLI; i++) {
        if (i >= n) { uk_show(s_w_cli[i], 0); continue; }
        lv_obj_remove_flag(s_w_cli[i], LV_OBJ_FLAG_HIDDEN);
        set_label_fmt(s_w_cli_name[i], c_cli_name[i], sizeof c_cli_name[i], "%s",
                      d->client[i].name[0] ? d->client[i].name : "?");
        set_label_fmt(s_w_cli_ip[i], c_cli_ip[i], sizeof c_cli_ip[i], "%s", d->client[i].ip);
        /* 这台是 Wi-Fi 设备：第二行换成「5 GHz · ↓… ↑… · 信号很好」，右下是总流量
         * （原情景·网络页的设备流量，2026-09-25 并到这里）；网线 / USB 设备照旧写 MAC */
        const ni_client_t *w = wifi_station_for(d->client[i].mac, d->client[i].ip);
        static char c_tot[WIFI_MAX_CLI][64];
        if (w) {
            char line[96], a[16], b[16];
            ni_client_line(line, sizeof line, w);
            set_label_fmt(s_w_cli_mac[i], c_cli_mac[i], sizeof c_cli_mac[i], "%s", line);
            fmt_bytes_total(a, sizeof a, (long)w->down);
            fmt_bytes_total(b, sizeof b, (long)w->up);
            set_label_fmt(s_w_cli_tot[i], c_tot[i], sizeof c_tot[i], TR("连上以来 ↓%s ↑%s"), a, b);
        } else {
            set_label_fmt(s_w_cli_mac[i], c_cli_mac[i], sizeof c_cli_mac[i], "%s", d->client[i].mac);
            set_label_fmt(s_w_cli_tot[i], c_tot[i], sizeof c_tot[i], "%s", "");
        }
        /* Wi-Fi 设备多一行总流量：行高跟着变 */
        int rh = w ? WIFI_CLI_H + 18 : WIFI_CLI_H;
        lv_obj_set_y(s_w_cli[i], cli_y);
        lv_obj_set_height(s_w_cli[i], rh);
        cli_y += rh;
    }
    /* Reflow: the card shrinks to the rows shown, DHCP follows. */
    uk_show(s_w_cli_empty, n == 0);
    int cli_h = n ? cli_y : UK_ROW_H;
    lv_obj_set_height(s_w_cli_card, cli_h);
    int dy = lv_obj_get_style_y(s_w_cli_card, 0) + cli_h + 10;
    lv_obj_set_y(s_w_dhcp_sec, dy);
    lv_obj_set_y(s_w_dhcp_card, dy + 20);
    int base = dy + 20 + 3 * UK_ROW_H + 10;
    if (base != s_nh_base[NH_WIFI]) { s_nh_base[NH_WIFI] = base; net_relayout(); }

    /* DHCP — mirrors htmlmain.c's own summary formatting. */
    static char c_gw[28] = "", c_pool[48] = "", c_lease[24] = "";
    set_label_fmt(s_w_gw, c_gw, sizeof c_gw, "%s", d->dhcp_ip[0] ? d->dhcp_ip : "-");
    set_label_fmt(s_w_pool, c_pool, sizeof c_pool, TR("%s · 共 %s"),
                  d->dhcp_start[0] ? d->dhcp_start : "-", d->dhcp_limit[0] ? d->dhcp_limit : "-");
    long lt = atol(d->dhcp_leasetime);
    if (lt >= 3600)     set_label_fmt(s_w_lease, c_lease, sizeof c_lease, TR("%ld 小时"), lt / 3600);
    else if (lt >= 60)  set_label_fmt(s_w_lease, c_lease, sizeof c_lease, TR("%ld 分钟"), lt / 60);
    else if (lt > 0)    set_label_fmt(s_w_lease, c_lease, sizeof c_lease, TR("%ld 秒"), lt);
    else                set_label_fmt(s_w_lease, c_lease, sizeof c_lease, "%s", "-");
}

