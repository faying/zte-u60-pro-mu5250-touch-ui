/*
 * ui_parts/refresh.c - 定时刷新：把数据画到每一页.
 * Part of src/ui.c, which #includes it: the parts share ui.c's static state
 * and are one translation unit, not compiled on their own (the Makefile
 * lists only ui.c). Split 2026-09-26 so the 6000-line file stops being
 * the place every change collides; the built binary is byte-identical.
 *
 * SPDX-License-Identifier: MIT
 */
/* ---- refresh ---- */
/* eSIM 列表。定时刷新在数据变了时画，点一下也当场画（esim_row_cb）。 */
static void esim_paint(void)
{
    s_es_dirty = 0;
    int n = esim_profile_count();

    int plain = s_sim.kind == UI_SIM_PLAIN, none = s_sim.kind == UI_SIM_NONE;
    const char *cur = esim_current();
    char tail[32] = "";
    {
        size_t l = strlen(s_sim.iccid);
        while (l && (s_sim.iccid[l - 1] == 'F' || s_sim.iccid[l - 1] == 'f')) l--;
        if (l >= 4) snprintf(tail, sizeof tail, TR("尾号 %.4s"), s_sim.iccid + l - 4);
    }
    /* 顶上一块写的是「现在用的这张卡」：实体 SIM 写运营商，eSIM 写配置名 */
    lv_label_set_text(s_es_hero.st, none ? TR("没有卡") : plain ? TR("使用中 · 实体 SIM 卡") : TR("使用中 · eSIM"));
    lv_label_set_text(s_es_cur, none ? TR("没插卡") : plain ? (s_sim.oper[0] ? s_sim.oper : TR("SIM 卡"))
                                                        : (cur[0] && strcmp(cur, "-") ? cur : "—"));
    if (net_flash_on(&s_es_flash)) {
        lv_label_set_text(s_es_state, s_es_flash.txt);
        uk_text_color(s_es_state, s_es_flash.col);
    } else {
        lv_label_set_text(s_es_state, none ? TR("插上 SIM 卡或 eSIM 卡后这里显示卡信息")
                                     : plain ? (s_sim.msisdn[0] ? s_sim.msisdn : TR("号码没写在卡里")) : esim_state());
        uk_text_color(s_es_state, T->t2);
    }
    lv_label_set_text(s_es_hero.rtop, plain ? tail : "");
    lv_label_set_text(s_es_info[0], s_sim.msisdn[0] ? s_sim.msisdn : "—");
    lv_label_set_text(s_es_info[1], s_sim.iccid[0] ? s_sim.iccid : "—");
    lv_label_set_text(s_es_info[2], s_sim.imsi[0] ? s_sim.imsi : "—");
    lv_label_set_text(s_es_list_sec, plain && n ? TR("eSIM 配置（现在没在用）") : TR("eSIM 配置"));
    uk_show(s_es_empty, n == 0);
    if (n == 0) {
        /* esim_ready() = the state line says 就绪; compare the code, not the
         * (translated) text */
        int ready = esim_ready();
        lv_label_set_text(s_es_empty, plain ? TR("现在插的是普通 SIM 卡，没有 eSIM 配置。换成 eSIM 卡后可以在这里切换。")
                                     : !esim_loaded() && ready ? TR("读取中…")
                                     : !ready ? esim_state() : TR("还没有 eSIM 配置 · 用管理网页添加"));
    }
    for (int i = 0; i < ESIM_MAX_ROWS; i++) {
        if (i >= n) { uk_show(s_es_row[i], 0); continue; }
        esim_profile_t p;
        esim_get_profile(i, &p);
        uk_show(s_es_row[i], 1);
        lv_label_set_text(s_es_row_name[i], p.name);
        lv_label_set_text(s_es_row_sub[i], p.sub);
        if (p.enabled && !plain) lv_label_set_text(s_es_hero.rtop, p.sub);
        lv_label_set_text(s_es_row_tag[i],
            p.going ? TR("切换中…") : p.armed ? TR("再点一次确认切换") : p.enabled ? (plain ? TR("已启用") : TR("使用中")) : "");
        /* 正在用的、切换中的也能点：esim_row_cb 会说一句为什么不动 */
        uk_bg(s_es_row[i], p.armed ? T->fillOrange : p.enabled ? T->accS : T->card);
        uint32_t fg = p.armed ? 0xffffff : T->t1;
        uk_text_color(s_es_row_name[i], fg);
        uk_text_color(s_es_row_sub[i], p.armed ? T->onFill : T->t3);
        uk_text_color(s_es_row_tag[i], p.armed ? 0xffffff : p.enabled ? T->accT : T->t2);
    }
    lv_obj_set_height(s_es_list_card, n ? n * ESIM_ROW_H : UK_ROW_H);
}

static void refresh_cb(lv_timer_t *t)
{
    appearance_tick();
    LV_UNUSED(t);
    static devui_data_t d;

    /* perf counters (per-second deltas) — these two legitimately change
     * almost every tick, so the dirty-check below is just a cheap no-op
     * guard here, not a real win — kept for consistency (every refresh_cb
     * label update goes through set_label_fmt, no exceptions to remember). */
    static unsigned long last_f, last_r;
    static char c_fps[16] = "", c_touch[16] = "";
    unsigned long f = g_frame_count, r = touch_input_report_count();
    set_label_fmt(s_t_fps, c_fps, sizeof c_fps, "%lu FPS", f - last_f);
    set_label_fmt(s_t_touch, c_touch, sizeof c_touch, "%lu Hz", r - last_r);
    last_f = f; last_r = r;

    /* Wall clock, independent of the backend — matches htmlmain.c's own
     * status-bar clock (time(NULL) + localtime_r), so it keeps ticking even
     * when zwrt-datad is down. */
    {
        static char c_time[8] = "";
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        set_label_fmt(s_top_time, c_time, sizeof c_time, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    }

    /* zte-agent health: read from the scenario poller (which now polls on
     * every page). The Wi-Fi watchdog keeps Wi-Fi up without the agent; this
     * is only so the owner knows. Runs before the datad check below, so the
     * dot stays right while the data service is down too. */
    /* Polled here, before the datad early-return below: agent_health() is
     * "time since the last successful read", so skipping the read whenever
     * datad is down would turn a datad outage into a false "agent lost". */
    data_set_pace(backlight_panel_lit());
    int sc_changed = scenario_poll(tab_visible(TAB_HOME));
    if (s_vendor_arm && lv_tick_get() - s_vendor_arm >= 5000) {
        s_vendor_arm = 0;
        uk_button_kind(s_vendor_btn, s_vendor_lbl, UK_BTN_PLAIN);
        lv_label_set_text(s_vendor_lbl, TR("切换到原厂界面"));
    }
    if (s_sc_force) { s_sc_force = 0; sc_changed = 1; }
    agent_health_t ah;
    agent_health(&ah);
    {
        s_alert_st = ah.lost_secs ? 2 : ah.unread > 0 ? 1 : 0;
    }

    {
        static char c_h[40] = "";
        static uint32_t c_hcol;
        uint32_t col = T->t3;
        if (!ah.checked) {
            set_label_fmt(s_set_health, c_h, sizeof c_h, "%s", "—");
        } else if (ah.bad) {
            col = T->badT;
            set_label_fmt(s_set_health, c_h, sizeof c_h, TRN("■ %d 项异常", ah.bad), ah.bad);
        } else if (ah.warn) {
            col = T->warnT;
            set_label_fmt(s_set_health, c_h, sizeof c_h, TRN("▲ %d 项注意", ah.warn), ah.warn);
        } else {
            col = T->okT;
            set_label_fmt(s_set_health, c_h, sizeof c_h, "%s", TR("● 一切正常"));
        }
        if (col != c_hcol) {
            c_hcol = col;
            uk_text_color(s_set_health, col);
        }
    }

    /* 网络页只靠 zte-agent，不靠 datad：放在 datad 的提前返回之前。首页的
     * 网络卡也读它（30 秒一次、精简读取），画在后面首页那段。 */
    {
        /* 只有运营商选择页做完整读取（agent 会读 AT+COPS?）；别的页只要它显示的那部分 */
        int ni_mode = sub_visible(SUB_NET) ? NI_PAGE :
                      (sub_visible(SUB_APN) || tab_visible(TAB_CELL)) ? NI_APN :
                      tab_visible(TAB_WIFI) ? NI_CLIENTS :
                      (sub_visible(SUB_SCENE) || sub_visible(SUB_CELL) || tab_visible(TAB_EXIT)) ? NI_LITE :
                      tab_visible(TAB_HOME) ? NI_HOME : NI_OFF;
        int paint = ni_mode != NI_OFF && ni_mode != NI_HOME;
        static int ni_last;
        int ni_changed = netinfo_poll(ni_mode) || (paint && ni_last != ni_mode);
        ni_last = ni_mode;
        if (paint) net_paint(ni_changed);
    }

    if (!data_refresh(&d)) {
        /* Backend down is a device-wide condition, so it is reported once in
         * the shared banner instead of overwriting home-page content. */
        banner_set(TR("后台数据服务不可用"));
        home_signal_down();
        lk_mode_sync(-1, 0);    /* 点过的网络模式也要有下文：确认窗口到时、等读回超时 */
        op_refresh();
        if (sub_visible(SUB_PLACE)) place_paint(&d, 0);
        diag_refresh();
        return;
    }
    /* Answered before, silent now: keep the last numbers but dimmed and say
     * since when; the parts fed by zte-agent (eSIM, alerts, …) go on. */
    int datad_silent = data_backend_silent();
    /* 首页信号卡和状态栏的结论由 zwrt-datad 算（/v2/screen），每份新快照读一次 */
    static net_view_t nv;
    /* dark screen: nothing shows it, so don't make datad compute it; the first
     * lit refresh fetches (the snapshot version has moved on) */
    if (backlight_is_on() && !datad_silent) screen_feed_poll(data_backend_version());
    {
        const net_view_t *sv = screen_feed_net();
        if (sv) nv = *sv;
        else if (screen_feed_status() == SF_OLD_DATAD)
            net_view_placeholder(&nv, "—", TR("数据服务版本太旧：要更新 zwrt-datad"));   /* on the top line */
        else
            net_view_placeholder(&nv, TR("读取中…"), "");
    }
    s_net_roam = nv.roam;
    op_refresh();           /* E4: 事务行、事务页（/v2/screen 的 op） */
    if (sub_visible(SUB_PLACE)) place_paint(&d, !datad_silent);
    diag_refresh();
    data_notice_t note;
    if (datad_silent) {
        banner_set(TR("后台数据服务不可用"));
        home_signal_down();
    } else if (screen_feed_stuck()) {
        /* E4: answers, but its executor has not moved for 20 s (V2-40) */
        banner_set(TR("数据服务没响应 · 暂时不能改设置"));
    } else if (data_control_notice(&note, 4000)) {
        /* a write datad turned down: say so for a few seconds */
        if (note.kind == UI_CTL_BUSY && note.say_zh[0])
            banner_set(pick(note.say_zh, note.say_en));
        else if (note.kind == UI_CTL_BUSY)
            banner_set(TR("正在改别的设置，稍等"));
        else if (note.kind == UI_CTL_FULL)
            banner_set(TR("数据服务忙 · 没改，稍后再试"));
        else
            banner_set(TR("没改成 · 数据服务回了错误"));
    } else if (ah.lost_secs) {
        /* Same banner, lower priority than "data service down" above. */
        char msg[96];
        snprintf(msg, sizeof msg, TR("管理后台失联 %ld 分钟"), ah.lost_secs / 60);
        banner_set(msg);
    } else if (d.bat_temp >= 50) {
        banner_set(TR("电池过热，拔掉充电器放到阴凉处"));
    } else if (d.bat_percent <= 10 && !(d.charger_connect && d.chg_uv > 1000000)) {
        banner_set(TR("电量低，尽快充电"));
    } else {
        banner_set(NULL);
    }

    /* Top status bar — same signal-strength color tiers as the Home card's
     * dots, reused here for the always-visible summary. */
    {
        static char c_net[16] = "";
        /* 和首页右边的「信号强/中/弱」同一套：4–5 格绿、3 格橙、1–2 格红 */
        int tier = nv.bars_tier;
        /* tier -1 with bars > 0 = datad has not graded it (no view yet): grey, not red */
        uint32_t sig_col = tier == 2 ? T->green : tier == 1 ? T->orange : tier == 0 ? T->red : T->track;
        static uint32_t c_sig[5];
        for (int i = 0; i < 5; i++) {
            uint32_t col = i < d.bars ? sig_col : T->track;
            if (col != c_sig[i]) { c_sig[i] = col; uk_bg(s_top_sig[i], col); }
        }
        /* the phone-style label (5G-A / 5G+ / 4G+ / 3G …) needs the carrier
         * counts, so the signal card below writes it (s_top_label) */
        set_label_fmt(s_top_net, c_net, sizeof c_net, "%s", s_top_label);
        /* 「无服务」这类中文要中文字体，数字字体里没有 */
        lv_obj_set_style_text_font(s_top_net, (unsigned char)c_net[0] >= 0x80 ? UF.cj12 : UF.n12, 0);
        char dn[16], up[16], dn2[16], up2[16], full[48], shrt[48], down[24];
        fmt_rate_top(dn, sizeof dn, d.rx_speed, s_cf_speed_bits, 0);
        fmt_rate_top(up, sizeof up, d.tx_speed, s_cf_speed_bits, 0);
        fmt_rate_top(dn2, sizeof dn2, d.rx_speed, s_cf_speed_bits, 1);
        fmt_rate_top(up2, sizeof up2, d.tx_speed, s_cf_speed_bits, 1);
        snprintf(full, sizeof full, "↓%s ↑%s", dn, up);
        snprintf(shrt, sizeof shrt, "↓%s ↑%s", dn2, up2);
        snprintf(down, sizeof down, "↓%s", dn2);
        /* Charging = charger_connect only: d.charging is a status code
         * ("2" = discharging on this hardware), truthy almost always. */
        int chg = d.charger_connect && d.chg_uv > 1000000;
        static int c_pct = -1, c_chg = -1;
        if (d.bat_percent != c_pct || chg != c_chg) {
            c_pct = d.bat_percent; c_chg = chg;
            uk_battery_set(&s_top_batt, d.bat_percent, chg);
            uk_show(s_top_batt.body, 1);
            uk_show(s_top_batt.nub, 1);
        }
        statusbar_layout(full, shrt, down, d.bat_percent, chg, s_alert_st != 0, s_alert_st == 2 ? T->badT : T->warnT);
    }

    /* ---- Home: signal card ----
     * Status block: state words (not colour alone), QCI · AMBR, total MHz,
     * RAT, operator · carrier count. Then one row per carrier: the serving
     * cell first from nr_* (its live signal is only there), then every
     * nrca / lteca entry as-is — the serving band may appear again as a
     * -140-floor "inactive" entry, which is what the modem reports. The
     * total counts every carrier reported, active or not (htmlmain.c's
     * total_show_bw). */
    if (!datad_silent) {
        s_ever_valid = 1;
        s_last_valid_wall = time(NULL);
    }
    if (!datad_silent) {   /* home_signal_down() above already says since when */
        static char c_rtop[48], c_big[16], c_r1[20], c_r2[96], c_st[128];
        static char c_ca_band[CA_SLOTS][16], c_ca_bw[CA_SLOTS][12], c_ca_rsrp[CA_SLOTS][12],
                    c_ca_sinr[CA_SLOTS][12], c_ca_pci[CA_SLOTS][16], c_ca_arfcn[CA_SLOTS][24],
                    c_ca_ina[CA_SLOTS][24], c_ca_info[CA_SLOTS][40];
        /* 载波、结论、顶行、载波汇总都在 net_view()（net_view.c，不碰 LVGL）；
         * 这里只画。 */
        const nv_carrier_t *ca = nv.ca;
        int ca_n = nv.ca_n;
        /* 大字换结论要先稳 15 秒（阈值附近别来回闪）；没服务、没卡这类马上显示。
         * 下面五行一直是实时的。 */
        static ui_net_story_t shown;
        static nv_state_t shown_state;
        static ui_net_hold_t hold;
        {
            /* keyed on datad's verdict code: in English every 慢 reads "Slow",
             * so the headline text would not see a change of cause; a datad
             * from before 10-01 sends no code → the headline, as before */
            unsigned key = 5381;
            if (nv.state != NV_STATE_UNKNOWN) key = 0x80000000u | (unsigned)nv.state;
            else for (const char *p = nv.story.headline; *p; p++) key = key * 33u + (unsigned char)*p;
            if (nv.story.tone >= UI_NET_BAD || shown.tone >= UI_NET_BAD) hold.have = 0;
            /* a write transaction (net.home) shows at once and goes at once */
            if (nv.state == NV_STATE_CHANGING || nv.state == NV_STATE_REVERT_FAIL ||
                shown_state == NV_STATE_CHANGING || shown_state == NV_STATE_REVERT_FAIL) hold.have = 0;
            if (ui_net_hold(&hold, key, lv_tick_get())) {
                shown.tone = nv.story.tone; shown.cause = nv.story.cause;
                shown_state = nv.state;
                memcpy(shown.headline, nv.story.headline, sizeof shown.headline);
                memcpy(shown.hint, nv.story.hint, sizeof shown.hint);
            }
        }
        snprintf(s_top_label, sizeof s_top_label, "%s", nv.story.rat);
        static uint32_t nosig_since;
        int nosvc = nv.nosvc;
        if (nosvc) { if (!nosig_since) nosig_since = lv_tick_get() ? lv_tick_get() : 1; }
        else nosig_since = 0;
        int tone = shown.tone == UI_NET_OK ? 0 : shown.tone == UI_NET_WARN ? 1 : shown.tone == UI_NET_BAD ? 2 : 3;
        if (tone != s_cc_tone) { s_cc_tone = tone; uk_hero_tone(&s_cc_hero, tone); }
        const char *hint = shown.hint;
        /* 顶行：谁的网 · 什么网 · 本地/漫游 */
        home_logo_set(nv.logo[0] ? nv.logo : NULL);
        if (!nv.sim_usable)
            set_label_fmt(s_cc_hero.st, c_st, sizeof c_st, "%s", TR("没有 SIM 卡"));
        else
        {
            /* 顶栏已经是简写（5G-A / 4G+ …），这里写更细的：怎么组网、哪种技术 */
            char fine[32];
            const char *name = nv.name, *where = nv.where;
            int other = nv.other, roam = nv.roam;
            snprintf(fine, sizeof fine, "%s", nv.fine);
            /* 放不下时先丢制式的补充说明（「5G NSA · 4G 锚点」→「5G NSA」），
             * 漫游到哪家比锚点重要；还放不下才由标签末尾「…」截断（9-26 真机反馈）。 */
            char line[128];
            for (int pass = 0; pass < 2; pass++) {
                if (pass) { char *dot = strstr(fine, " · "); if (!dot) break; *dot = 0; }
                if (s_cc_logo_w)   /* logo 就是卡的运营商，文字从制式写起 */
                    snprintf(line, sizeof line, "%s%s%s", fine, fine[0] && where[0] ? " · " : "", where);
                else               /* 没 logo：照旧 名字 · 制式 · 本地/漫游 */
                    snprintf(line, sizeof line, "%s%s%s%s%s", name, fine[0] ? " · " : "", fine,
                             where[0] ? " · " : "", other ? (roam ? TR("漫游") : "") : where);
                int avail = s_cc_logo_w ? UK_CARD_W - 2 * UK_PAD - s_cc_logo_w - 6 : UK_CARD_W - 26 - UK_PAD;
                lv_point_t sz;
                lv_text_get_size(&sz, line, lv_obj_get_style_text_font(s_cc_hero.st, 0), 0, 0,
                                 LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                if (sz.x <= avail) break;
            }
            set_label_fmt(s_cc_hero.st, c_st, sizeof c_st, "%s", line);
        }
        set_label_fmt(s_cc_hero.big, c_big, sizeof c_big, "%s", shown.headline);
        if (nosvc) {
            uint32_t mins = (lv_tick_get() - nosig_since) / 60000;
            if (mins) set_label_fmt(s_cc_hero.rtop, c_rtop, sizeof c_rtop, TR("已 %u 分钟"), (unsigned)mins);
            else      set_label_fmt(s_cc_hero.rtop, c_rtop, sizeof c_rtop, "%s", TR("刚刚"));
        } else
            set_label_fmt(s_cc_hero.rtop, c_rtop, sizeof c_rtop, "%s", "");
        /* 右边：三件互不决定的事，位置固定。
         *   上：信号强/中/弱（就是状态栏的格数，颜色也一样）
         *   下：干扰小/中/大 · 负载正常/高（没在下载时不写负载）
         * 数字只在大字下面那行提示里，跟着「为什么慢」走。 */
        {
            char a[40] = "", b[64] = "";
            ui_net_tone_t at = nv.story.sig_tone;
            if (nosvc) snprintf(a, sizeof a, "%s", TR("正在搜网"));
            else if (nv.story.sig[0]) {
                snprintf(a, sizeof a, TR("信号%s"), nv.story.sig);
                /* whole sentences, so the English can be worded on its own */
                if (nv.story.noise[0] && nv.story.load[0])
                    snprintf(b, sizeof b, TR("干扰%s · 负载%s"), nv.story.noise, nv.story.load);
                else if (nv.story.noise[0]) snprintf(b, sizeof b, TR("干扰%s"), nv.story.noise);
                else if (nv.story.load[0])  snprintf(b, sizeof b, TR("负载%s"), nv.story.load);
                else if (d.rssi) snprintf(b, sizeof b, "RSSI %d dBm", d.rssi);
            }
            set_label_fmt(s_cc_hero.r1, c_r1, sizeof c_r1, "%s", a);
            set_label_fmt(s_cc_hero.r2, c_r2, sizeof c_r2, "%s", b);
            uk_text_color(s_cc_hero.r1, nosvc ? T->t1 : at == UI_NET_OK ? T->okT : at == UI_NET_WARN ? T->warnT : T->badT);
        }
        uk_hero_layout(&s_cc_hero);
        home_logo_place();

        int y = UK_HERO_H;
        {
            /* 结论异常（慢、没连上、连上了但不通）：提示行可点，右端「查原因 ›」。
             * 提示在缩窄后两行放得下就并排，放不下「查原因 ›」另起一行（提示行不许第 3 行） */
            int op = shown_state == NV_STATE_CHANGING || shown_state == NV_STATE_REVERT_FAIL;
            int diag = op || nv_abnormal(shown_state, shown.cause);
            static char c_hint[128];
            static int c_diag = -1;
            /* E4: the status block shows a change in progress; its › opens the 事务页 */
            if (op != (s_cc_hint_sub == SUB_OP)) {
                s_cc_hint_sub = op ? SUB_OP : SUB_DIAG;
                lv_label_set_text(s_cc_diag, op ? TR("详情 ›") : TR("查原因 ›"));
                c_diag = -1;
            }
            uk_show(s_cc_hint_box, hint[0] || diag);
            uk_show(s_cc_diag, diag);
            if (diag != c_diag) {
                c_diag = diag;
                if (diag) lv_obj_add_flag(s_cc_hint_box, LV_OBJ_FLAG_CLICKABLE);
                else      lv_obj_remove_flag(s_cc_hint_box, LV_OBJ_FLAG_CLICKABLE);   /* no › = not tappable */
            }
            if (hint[0] || diag) {
                const int full = UK_CARD_W - 2 * UK_PAD, lh = lv_font_get_line_height(UF.cj13);
                int dw = 0, w = full, bh, hy = 8;
                if (diag) { lv_obj_update_layout(s_cc_diag); dw = (int)lv_obj_get_width(s_cc_diag); }
                if (diag && hint[0]) {
                    /* beside the hint only when that costs it no extra line (no word
                     * pushed alone onto a new line, never a 3rd line) */
                    lv_point_t sn, sf;
                    lv_text_get_size(&sn, hint, UF.cj13, 0, 0, full - dw - 10, LV_TEXT_FLAG_NONE);
                    lv_text_get_size(&sf, hint, UF.cj13, 0, 0, full, LV_TEXT_FLAG_NONE);
                    w = sn.y <= sf.y ? full - dw - 10 : full;
                }
                lv_obj_set_width(s_cc_hint, w);
                set_label_fmt(s_cc_hint, c_hint, sizeof c_hint, "%s", hint);
                uk_show(s_cc_hint, hint[0] != 0);
                lv_obj_update_layout(s_cc_hint);
                int th = hint[0] ? (int)lv_obj_get_height(s_cc_hint) : 0;
                bh = 8 + (th ? th : lh) + 8;
                if (diag) {
                    if (!hint[0] || w < full) hy = 8 + (th > lh ? th - lh : 0);   /* beside the last line */
                    else { hy = 8 + th + 2; bh = hy + lh + 8; }                  /* its own line under it */
                    lv_obj_set_pos(s_cc_diag, UK_CARD_W - UK_PAD - dw, hy);
                }
                lv_obj_set_y(s_cc_hint_box, y);
                lv_obj_set_height(s_cc_hint_box, bh);
                y += bh;
            }
        }
        /* 载波：基站配了几条、在用几条；小字是下行用哪几条、上行用哪条。
         * 配了没激活的 = RSRP 在 -140 底值的那几条；服务小区在 nrca 里会以
         * 未激活的样子再出现一次，按 PCI + 频段去重。上行只写主载波：
         * 上行聚合的数据还没对上（home-net-card.md「待确认」）。 */
        {
            static char c_cas[48], c_sub[160];
            const char *val = nv.ca_val, *sub = nv.ca_sub;
            int act_n = nv.act_n;
            set_label_fmt(s_hr_ca.val, c_cas, sizeof c_cas, "%s", val);
            uk_text_color(s_hr_ca.val, shown.cause == UI_CAUSE_NARROW ? T->warnT : act_n ? T->t1 : T->t3);
            set_label_fmt(s_hr_ca_sub, c_sub, sizeof c_sub, "%s", sub);
            uk_show(s_hr_ca_sub, sub[0] != 0);
            int ch = sub[0] ? 60 : UK_ROW_H;
            lv_obj_set_height(s_hr_ca.box, ch);
            lv_obj_set_y(s_hr_ca.box, y);
            y += ch;
            static char c_qos[48];
            set_label_fmt(s_ca_qos, c_qos, sizeof c_qos, "QCI %d · AMBR %d/%d", d.qci, (int)d.ambr_dl, (int)d.ambr_ul);
        }
        /* Wi-Fi · 设备数（点进 Wi-Fi 页） */
        {
            static char c_w[64];
            if (!d.wifi_enabled)
                set_label_fmt(s_hr_wifi.val, c_w, sizeof c_w, "%s", TR("已关闭"));
            else
                set_label_fmt(s_hr_wifi.val, c_w, sizeof c_w, TRN("%.16s · %d 台", d.clients_total), d.wifi_ssid[0] ? d.wifi_ssid : "-",
                              d.clients_total);
            uk_text_color(s_hr_wifi.val, d.wifi_enabled ? T->t1 : T->t3);
            lv_obj_set_y(s_hr_wifi.box, y);
            y += UK_ROW_H;
        }
        /* 出口：一行；有另一条路时两行（小字写另一条路），和下面写字的条件一致 */
        {
            int eh = UK_ROW_H;
            lv_obj_set_height(s_hr_exit.box, eh);
            lv_obj_set_y(s_hr_exit.box, y);
            y += eh;
        }
        /* 今日/本月：固件（zwrt_data）按日历日/月累计的计数器，不是本次开机的 rx/tx */
        {
            char c_day[32], c_month[32];
            static char c_traf[80] = "";
            fmt_bytes_total(c_day, sizeof c_day, d.day_rx_bytes + d.day_tx_bytes);
            fmt_bytes_total(c_month, sizeof c_month, d.month_rx_bytes + d.month_tx_bytes);
            set_label_fmt(s_hr_traf.val, c_traf, sizeof c_traf, TR("今日 %s · 本月 %s"), c_day, c_month);
            uk_text_color(s_hr_traf.val, T->t1);
            lv_obj_set_y(s_hr_traf.box, y);
            y += UK_ROW_H;
        }
        lv_obj_set_height(s_cell_card, y);
        y = CA_CARD_TOP;
        for (int i = 0; i < CA_SLOTS; i++) {
            home_ca_t *k = &s_ca[i];
            if (i >= ca_n) { uk_show(k->box, 0); continue; }
            uk_show(k->box, 1);
            uk_show(k->sep, 1);
            int act = ca[i].active;
            const char *band_s = ca[i].label;
            const char *fl = ca[i].kind == 'n' ? "ARFCN" : "EARFCN";
            lv_obj_t *on[8] = { k->band, k->bw, k->rsrp, k->rsrp_c, k->sinr, k->sinr_c, k->pci, k->arfcn };
            for (int j2 = 0; j2 < 8; j2++) uk_show(on[j2], act);
            uk_show(k->ina, !act); uk_show(k->ina_tag, !act); uk_show(k->ina_info, !act);
            if (act) {
                /* datad sends the numbers already written (%.0f / %.1f) */
                const char *rsrp_s = ca[i].rsrp, *sinr_s = ca[i].sinr;
                set_label_fmt(k->band, c_ca_band[i], sizeof c_ca_band[i], "%s", band_s);
                set_label_fmt(k->bw, c_ca_bw[i], sizeof c_ca_bw[i], "%dM", ca[i].bw);
                lv_obj_update_layout(k->band);
                lv_obj_set_x(k->bw, UK_PAD + 3 + lv_obj_get_width(k->band));
                set_label_fmt(k->rsrp, c_ca_rsrp[i], sizeof c_ca_rsrp[i], "%s", rsrp_s);
                set_label_fmt(k->sinr, c_ca_sinr[i], sizeof c_ca_sinr[i], "%s", sinr_s);
                set_label_fmt(k->pci, c_ca_pci[i], sizeof c_ca_pci[i], "PCI %d", ca[i].pci);
                set_label_fmt(k->arfcn, c_ca_arfcn[i], sizeof c_ca_arfcn[i], "%s %ld", fl, ca[i].arfcn);
                uk_text_color(k->band, T->t1);
                uk_text_color(k->rsrp, T->t1);
                uk_text_color(k->sinr, ca[i].sinr_tone == UI_NET_OK ? T->okT : ca[i].sinr_tone == UI_NET_WARN ? T->warnT : T->badT);
                lv_obj_set_height(k->box, 40);
            } else {
                set_label_fmt(k->ina, c_ca_ina[i], sizeof c_ca_ina[i], "%s %dM", band_s, ca[i].bw);
                set_label_fmt(k->ina_info, c_ca_info[i], sizeof c_ca_info[i], "%s %ld · PCI %d", fl, ca[i].arfcn, ca[i].pci);
                lv_obj_set_height(k->box, 32);
            }
            lv_obj_set_y(k->box, y);
            y += act ? 40 : 32;
        }
        lv_obj_set_height(s_ca_card, y + (ca_n ? 2 : 0));
        uk_show(s_ca_card, ca_n > 0);
        cell_reflow();
    }

    /* ---- 出口 (Home) ----
     * IP 和归属地来自 zte-agent 的缓存（netinfo_poll 在首页 30 秒读一次）；
     * 运营商和漫游已经在状态卡顶行。 */
    if (tab_visible(TAB_HOME)) {
        static char c_nip[48], c_ngeo[320];
        const netinfo_t *n = netinfo_get();
        int dead = s_cc_tone >= 2;      /* 没信号 / 没卡：出口是旧的 */
        char g[320];
        set_label_fmt(s_nh_ip, c_nip, sizeof c_nip, "%s", n->direct.ip[0] ? n->direct.ip : "—");
        uk_text_color(s_nh_ip, dead ? T->t3 : T->t1);
        if (n->err[0]) snprintf(g, sizeof g, "%s", n->err);
        else if (!n->direct.present) snprintf(g, sizeof g, "%s", TR("归属地查询中…"));
        else if (!n->direct.ip[0]) snprintf(g, sizeof g, "%s", TR("查不到归属地"));
        else snprintf(g, sizeof g, "%s%s%s", n->direct.geo[0] ? n->direct.geo : n->direct.ip,
                      n->direct.isp[0] ? " · " : "", n->direct.isp);
        set_label_fmt(s_nh_geo, c_ngeo, sizeof c_ngeo, "%s", g);
        /* 首页出口：直连 · 国家 城市；完整归属地在出口标签。 */
        static char c_hx[96], c_hx2[256];
        char sg[96], sub[256] = "";
        if (n->direct.ip[0])
            snprintf(g, sizeof g, TR("直连 · %s"), n->direct.geo[0] ? geo_short(n->direct.geo, sg, sizeof sg) : n->direct.ip);
        else
            snprintf(g, sizeof g, "%s", n->err[0] ? "—" : TR("查询中…"));
        set_label_fmt(s_hr_exit.val, c_hx, sizeof c_hx, "%s", g);
        uk_text_color(s_hr_exit.val, dead ? T->t3 : T->t1);
        set_label_fmt(s_hr_exit_sub, c_hx2, sizeof c_hx2, "%s", sub);
        uk_show(s_hr_exit_sub, sub[0] != 0);
    }

    /* ---- 情景 (Home) ---- */
    {
        if (sc_changed) {
            static char c_ss[64] = "", c_sn[96] = "";
            scenario_status_t sc;
            scenario_get_status(&sc);
            if (!sc.available) {
                lv_obj_add_flag(s_sc_card, LV_OBJ_FLAG_HIDDEN);
            } else {
                char when[24] = "";
                lv_obj_remove_flag(s_sc_card, LV_OBJ_FLAG_HIDDEN);
                if (!sc.enabled)
                    set_label_fmt(s_sc_state, c_ss, sizeof c_ss, "%s", TR("已停用"));
                else
                    set_label_fmt(s_sc_state, c_ss, sizeof c_ss, "%s", sc.name[0] ? sc.name : TR("判定中"));
                uk_text_color(s_sc_state, sc.enabled ? T->t1 : T->t3);
                if (sc.last_switch > 0) {
                    time_t tt = (time_t)sc.last_switch;
                    struct tm tm;
                    /* 当天只写时间，更早的只写日期 */
                    time_t nw = time(NULL);
                    struct tm tn;
                    localtime_r(&tt, &tm);
                    localtime_r(&nw, &tn);
                    int today = tm.tm_year == tn.tm_year && tm.tm_yday == tn.tm_yday;
                    strftime(when, sizeof when, today ? "%H:%M" : lang_is_en() ? "%d %b" : "%m-%d", &tm);
                }
                /* 在家：解释 Wi-Fi 为什么没了；判定中：为什么还没结论；
                 * 其他：上次什么时候切过来的。手动固定的，先说「已固定」 */
                const char *pinned = sc.pin[0] ? TR("已固定 · ") : "";
                uint32_t note_col = T->t2;
                if (sc.enabled && !sc.name[0])
                    set_label_fmt(s_sc_note, c_sn, sizeof c_sn, "%s", TR("开机后要连续两次扫描确认位置"));
                else
                    set_label_fmt(s_sc_note, c_sn, sizeof c_sn, "%s%s%s%s", pinned,
                                  sc.wifi_off ? TR("Wi-Fi 已关 · ") : (when[0] ? TR("切换于 ") : ""),
                                  when, sc.wifi_off && !when[0] ? TR("手机走家里网络") : "");
                uk_text_color(s_sc_note, note_col);
            }
        }
    }

    /* ---- Tailscale (Home) ---- */
    {
        if (tailscale_poll(tab_visible(TAB_HOME) || tab_visible(TAB_EXIT) || sub_visible(SUB_TS))) {
            tailscale_status_t ts;
            tailscale_get_status(&ts);
            if (!ts.available) {
                lv_obj_add_flag(s_ts_card, LV_OBJ_FLAG_HIDDEN);
                uk_show(s_nh_tsrow, 0);
                lv_obj_set_height(s_nh_card, NET_EXIT_ROW_H);
            } else {
                const char *state_txt, *note = "";
                uint32_t state_col, dot_col;
                int running = ts.ok && !strcmp(ts.state, "Running");
                lv_obj_remove_flag(s_ts_card, LV_OBJ_FLAG_HIDDEN);
                if (!ts.ok)                                    { state_txt = TR("未运行");   state_col = T->t3; }
                else if (running)                              { state_txt = ts.self_online ? TR("已连接") : TR("离线");
                                                                   state_col = ts.self_online ? T->t1 : T->badT; }
                else if (!strcmp(ts.state, "Starting"))         { state_txt = TR("连接中");   state_col = T->warnT; }
                else if (!strcmp(ts.state, "NeedsLogin"))       { state_txt = TR("需要登录"); state_col = T->warnT;
                                                                   note = TR("在管理网页打开登录链接"); }
                else if (!strcmp(ts.state, "NeedsMachineAuth")) { state_txt = TR("等待批准"); state_col = T->warnT;
                                                                   note = TR("在 Tailscale 后台批准这台设备"); }
                else if (!strcmp(ts.state, "Stopped"))          { state_txt = TR("已停止");   state_col = T->t3; }
                else                                            { state_txt = ts.state[0] ? ts.state : "-"; state_col = T->t3; }
                dot_col = running && ts.self_online ? T->green : state_col == T->warnT ? T->orange : state_col == T->badT ? T->red : T->t3;
                {
                    /* 出口卡里的一行摘要；完整的节点、子网在下面的 Tailscale 卡 */
                    static char c_nts[80];
                    if (running && ts.ip[0])
                        set_label_fmt(s_nh_tsval, c_nts, sizeof c_nts, "%s · %s", state_txt, ts.ip);
                    else
                        set_label_fmt(s_nh_tsval, c_nts, sizeof c_nts, "%s", state_txt);
                    uk_text_color(s_nh_tsval, running && ts.self_online ? T->okT : state_col);
                    if (lv_obj_has_flag(s_nh_tsrow, LV_OBJ_FLAG_HIDDEN)) {
                        uk_show(s_nh_tsrow, 1);
                        lv_obj_set_height(s_nh_card, NET_EXIT_ROW_H + UK_ROW_H);
                    }
                }
                lv_label_set_text(s_ts_val[0], state_txt);
                uk_text_color(s_ts_val[0], state_col);
                uk_bg(s_ts_dot, dot_col);
                lv_obj_update_layout(s_ts_val[0]);
                lv_obj_set_x(s_ts_dot, lv_obj_get_x(s_ts_val[0]) - 12);
                int rows = 1;
                if (running) {
                    lv_label_set_text_fmt(s_ts_val[1], "%s%s%s%s", ts.ip,
                        ts.ip[0] && ts.relay[0] ? " · " : "", ts.relay[0] ? "DERP " : "", ts.relay);
                    if (ts.peers_online == 0)
                        lv_label_set_text(s_ts_val[2], TR("只有本机在线"));
                    else
                        lv_label_set_text_fmt(s_ts_val[2], TR("在线 %d/%d · 直连 %d · 中继 %d"),
                                              ts.peers_online, ts.peers, ts.direct, ts.active - ts.direct);
                    lv_label_set_text(s_ts_val[3], ts.routes[0] ? ts.routes : "-");
                    rows = 4;
                    /* health beats the exit node: it is the actionable one */
                    if (ts.health[0]) {
                        lv_label_set_text(s_ts_key[4], TR("提示"));
                        lv_label_set_text(s_ts_val[4], ts.health);
                        uk_text_color(s_ts_val[4], T->warnT);
                        rows = 5;
                    } else if (ts.exit_node[0]) {
                        lv_label_set_text(s_ts_key[4], TR("出口节点"));
                        lv_label_set_text_fmt(s_ts_val[4], "%s%s", ts.exit_node, ts.exit_online ? "" : TR(" · 离线"));
                        uk_text_color(s_ts_val[4], T->t1);
                        rows = 5;
                    }
                } else if (note[0]) {
                    lv_label_set_text(s_ts_key[1], TR("下一步"));
                    lv_label_set_text(s_ts_val[1], note);
                    rows = 2;
                }
                if (!running) lv_label_set_text(s_ts_key[1], TR("下一步"));
                else          lv_label_set_text(s_ts_key[1], TRC("tailscale", "本机"));
                for (int i = 1; i < TS_HOME_ROWS; i++) {
                    uk_show(s_ts_key[i], i < rows); uk_show(s_ts_val[i], i < rows); uk_show(s_ts_sep[i], i < rows);
                }
                lv_obj_set_width(s_ts_val[4], LV_SIZE_CONTENT);
                s_ts_rows = rows;
                lv_obj_set_height(s_ts_card, rows * UK_ROW_H);
                {
                    /* 出口标签「Tailscale ›」行的小字 */
                    static char c_tsn[64];
                    if (running && ts.peers)
                        set_label_fmt(s_tile_sub[SUB_TS], c_tsn, sizeof c_tsn, TR("%s · 在线 %d/%d"), state_txt, ts.peers_online, ts.peers);
                    else
                        set_label_fmt(s_tile_sub[SUB_TS], c_tsn, sizeof c_tsn, "%s", state_txt);
                    uk_text_color(s_tile_sub[SUB_TS], running && ts.self_online ? T->okT : T->t2);
                }
                /* Subpage: own identity + the peer list. */
                lv_label_set_text(s_tp_self[0], ts.name[0] ? ts.name : "-");
                lv_label_set_text(s_tp_self[1], ts.ip[0] ? ts.ip : "-");
                lv_label_set_text(s_tp_self[2], ts.ip6[0] ? ts.ip6 : "-");
                lv_label_set_text(s_tp_self[3], ts.relay[0] ? ts.relay : "-");
                lv_label_set_text(s_tp_self[4], ts.routes[0] ? ts.routes : "-");
                lv_label_set_text(s_tp_self[5], ts.tailnet[0] ? ts.tailnet : "-");
                lv_label_set_text_fmt(s_tp_self[6], "%s%s%s", ts.version[0] ? ts.version : "-",
                                      ts.os[0] ? " · " : "", ts.os);
                lv_label_set_text(s_tp_self[7], ts.key_expiry[0] ? ts.key_expiry : TR("不过期"));
                int pn = tailscale_peer_count();
                if (pn > TS_PEER_MAX) pn = TS_PEER_MAX;
                for (int i = 0; i < TS_PEER_MAX; i++) {
                    if (i >= pn) { uk_show(s_tp_row[i], 0); continue; }
                    tailscale_peer_t pe;
                    tailscale_get_peer(i, &pe);
                    lv_obj_remove_flag(s_tp_row[i], LV_OBJ_FLAG_HIDDEN);
                    lv_label_set_text(s_tp_name[i], pe.name[0] ? pe.name : "-");
                    {
                        char rx[16], tx[16], tr[48] = "";
                        fmt_bytes_total(rx, sizeof rx, (long)pe.rx);
                        fmt_bytes_total(tx, sizeof tx, (long)pe.tx);
                        if (pe.rx || pe.tx) snprintf(tr, sizeof tr, " · ↓%s ↑%s", rx, tx);
                        lv_label_set_text_fmt(s_tp_ip[i], "%s%s%s%s%s", pe.ip, pe.os[0] ? " · " : "", pe.os,
                                              pe.exit_node ? TR(" · 出口节点") : "", tr);
                    }
                    {
                        /* 第三行：跟本机之间怎么连、多久前握手、收发了多少 */
                        char how[80], ago[24] = "", line[160];
                        if (pe.active && pe.direct) snprintf(how, sizeof how, TR("直连 %s"), pe.cur_addr);
                        else if (pe.active)         snprintf(how, sizeof how, TR("经 DERP %s 中继"), pe.relay[0] ? pe.relay : "?");
                        else if (pe.online)         snprintf(how, sizeof how, "%s", TR("在线，现在没在传数据"));
                        /* 不写「最后在线多久」：LastSeen 是控制服务器给的真 UTC，设备时钟是
                         * 当地时间标成 UTC，一减就差一个时区 */
                        else                        snprintf(how, sizeof how, "%s", TR("离线"));
                        if (pe.hs_ago >= 0 && (pe.active || pe.online)) fmt_ago(ago, sizeof ago, pe.hs_ago);
                        snprintf(line, sizeof line, "%s%s%s", how, ago[0] ? TR(" · 握手 ") : "", ago);
                        lv_label_set_text(s_tp_link[i], line);
                        uk_text_color(s_tp_link[i], pe.online ? T->t2 : T->t3);
                    }
                    /* One tag, most specific first: how it is connected beats
                     * plain online/offline. */
                    const char *tag = pe.active
                        ? (pe.direct ? TR("直连")
                                     : TR("中继"))
                        : (pe.online ? TR("在线")
                                     : TR("离线"));
                    lv_label_set_text(s_tp_tag[i], tag);
                    uk_text_color(s_tp_tag[i], pe.active ? T->okT : pe.online ? T->t2 : T->t3);
                    uk_text_color(s_tp_name[i], pe.online ? T->t1 : T->t3);
                }
                lv_obj_set_height(s_tp_card, (pn ? pn : 1) * TS_PEER_H);
            }
        }
    }


    home_reflow();

    /* ---- eSIM subpage ---- */
    {
        static int es_flash_shown;
        int es_changed = esim_poll(sub_visible(SUB_ESIM));
        /* 蜂窝标签开着、手没在动时读一次 eSIM 列表（每张卡一次），才分得清
         * 插的是普通 SIM 还是 eSIM 卡；读列表要跑 lpac，会卡一下，所以挑空闲时 */
        if (tab_visible(TAB_CELL) && !sub_visible(SUB_ESIM) && lv_display_get_inactive_time(NULL) > 1500
            && esim_prefetch(d.sim_iccid))
            es_changed = 1;
        {
            const netinfo_t *ni = netinfo_get();
            ui_sim_kind_t k = ui_sim_kind(d.sim_state, d.sim_iccid, esim_enabled_iccid());
            const char *op = ni->home.name[0] ? ni->home.name : d.operator_name;
            if (k != s_sim.kind || strcmp(op, s_sim.oper) || strcmp(d.sim_iccid, s_sim.iccid)
                || strcmp(d.sim_imsi, s_sim.imsi) || strcmp(d.sim_msisdn, s_sim.msisdn)) {
                s_sim.kind = k;
                snprintf(s_sim.oper, sizeof s_sim.oper, "%s", op);
                snprintf(s_sim.iccid, sizeof s_sim.iccid, "%s", d.sim_iccid);
                snprintf(s_sim.imsi, sizeof s_sim.imsi, "%s", d.sim_imsi);
                snprintf(s_sim.msisdn, sizeof s_sim.msisdn, "%s", d.sim_msisdn);
                es_changed = 1;
            }
        }
        if (es_flash_shown && !net_flash_on(&s_es_flash)) s_es_dirty = 1;   /* 提示到时间了：换回状态行 */
        if (es_changed || s_es_dirty) {
            es_flash_shown = net_flash_on(&s_es_flash);
            esim_paint();
        }
    }

    /* ---- Speedtest subpage ---- */
    {
        /* Also polled while just the 功能 tile wall is up (not only the
         * subpage itself) — the tile's own subtitle (功能 tile subtitles,
         * below) needs live data to replace the old hardcoded "插件未安装"
         * text, same as WiFi/SMS/eSIM/锁频 already do. */
        if (speedtest_poll(sub_visible(SUB_SPEED) || tab_visible(TAB_EXIT))) {
            int online = speedtest_agent_reachable();
            if (!online) {
                lv_obj_add_flag(s_st_live, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_st_detail, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_st_result, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_st_server, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_st_btn, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_st_offline, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(s_st_offline, UK_PAD, 32);
                uk_show(s_st_unit, 0);
                lv_label_set_text(s_st_phase, "");
                lv_label_set_text(s_st_offline,
                    TR("连不上测速服务（zte-agent）。\n"
                       "检查 zte-agent 有没有在跑，以及 /data/zte-agent.env\n"
                       "（或 start_zte_agent.sh）里有没有 ZTE_AGENT_PASSWORD。"));
            } else {
                lv_obj_remove_flag(s_st_live, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_st_detail, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_st_result, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_st_server, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_st_btn, LV_OBJ_FLAG_HIDDEN);
                uk_show(s_st_unit, 1);

                static char c_ph[24] = "", c_live[32] = "", c_detail[64] = "",
                            c_result[64] = "", c_srv[96] = "";
                speedtest_phase_t ph = speedtest_phase();
                double dl = speedtest_download_mbps(), ul = speedtest_upload_mbps();
                double ping = speedtest_ping_ms(), jitter = speedtest_jitter_ms();

                set_label_fmt(s_st_phase, c_ph, sizeof c_ph, "%s", speedtest_phase_label());

                if (ph == ST_DOWNLOAD || ph == ST_UPLOAD)
                    set_label_fmt(s_st_live, c_live, sizeof c_live, "%.1f", speedtest_live_mbps());
                else if (ph == ST_COMPLETE)
                    set_label_fmt(s_st_live, c_live, sizeof c_live, "%.1f", dl >= 0 ? dl : 0.0);
                else
                    set_label_fmt(s_st_live, c_live, sizeof c_live, "%s", "--");

                if (ping >= 0)
                    set_label_fmt(s_st_detail, c_detail, sizeof c_detail,
                                  TR("延迟 %.0fms · 抖动 %.0fms"), ping, jitter >= 0 ? jitter : 0.0);
                else
                    set_label_fmt(s_st_detail, c_detail, sizeof c_detail, "%s",
                                  ph == ST_IDLE && dl < 0 ? TR("还没测过") : "");

                if (dl >= 0 || ul >= 0)
                    set_label_fmt(s_st_result, c_result, sizeof c_result,
                                  "↓ %.1f Mbps  ↑ %.1f Mbps",
                                  dl >= 0 ? dl : 0.0, ul >= 0 ? ul : 0.0);
                else
                    set_label_fmt(s_st_result, c_result, sizeof c_result, "%s", "");

                set_label_fmt(s_st_server, c_srv, sizeof c_srv, "%s", speedtest_server());

                int running = speedtest_running();
                lv_label_set_text(s_st_btn_lbl,
                    running ? TR("停止") : ph == ST_COMPLETE ? TR("重新测速") : TR("开始测速"));
                uk_bg(s_st_btn, running ? T->fillRed : T->fillBlue);
                lv_obj_update_layout(s_st_live);
                lv_obj_set_x(s_st_unit, UK_PAD + lv_obj_get_width(s_st_live) + 4);

                if (s_st_refused_at && lv_tick_get() - s_st_refused_at < 6000 && !running) {
                    /* refused at the start (speedtest_btn_cb wrote why): leave it up a while */
                } else if (ph == ST_ERROR && speedtest_error()[0]) {
                    s_st_refused_at = 0;
                    lv_obj_remove_flag(s_st_offline, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_pos(s_st_offline, UK_PAD, 206);
                    uk_text_color(s_st_offline, T->badT);
                    lv_label_set_text(s_st_offline, speedtest_error());
                } else {
                    s_st_refused_at = 0;
                    lv_obj_add_flag(s_st_offline, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }

        /* 服务器列表：只在二级页真正打开时拉（不像 progress 那样功能磁贴墙
         * 打开也拉——列表要不了那么勤，磁贴副标题不需要它）。 */
        if (speedtest_servers_poll(sub_visible(SUB_SPEED))) {
            int n = speedtest_servers_count();
            static char c_sn[ST_SRV_ROWS][112];
            for (int i = 1; i < ST_SRV_ROWS; i++) {
                if (i > n) { lv_obj_add_flag(s_st_srv_row[i], LV_OBJ_FLAG_HIDDEN); continue; }
                speedtest_server_t s;
                speedtest_get_server(i - 1, &s);
                lv_obj_remove_flag(s_st_srv_row[i], LV_OBJ_FLAG_HIDDEN);
                set_label_fmt(s_st_srv_name[i], c_sn[i], sizeof c_sn[i], "%s · %s, %s",
                              s.sponsor, s.name, s.country);
            }
            lv_obj_set_height(s_st_srv_card, ((n > 0 ? n : 0) + 1) * UK_ROW_H);
        }
        {
            int sel = speedtest_selected_index();
            for (int i = 0; i < ST_SRV_ROWS; i++) {
                if (!s_st_srv_row[i]) continue;
                int is_sel = (sel < 0 && i == 0) || (sel >= 0 && i == sel + 1);
                uk_show(s_st_srv_ok[i], is_sel);
            }
        }
    }

    /* 预估由 agent 算，只在系统页亮着时去取 */
    battery_est_poll(tab_visible(TAB_SYS));

    /* ---- System page ---- */
    {
        static char c_sest[96] = "";
        set_label_fmt(s_sy_est, c_sest, sizeof c_sest, "%s", battery_est_text());
    }
    {
        static char c_sbat[40] = "", c_schg[48] = "", c_scpu[40] = "", c_smem[40] = "", c_sup[32] = "";
        set_label_fmt(s_sy_bat, c_sbat, sizeof c_sbat, "%d%% · %d°""C %d.%02ldV %ldmA",
                      d.bat_percent, d.bat_temp, (int)(d.bat_uv / 1000000),
                      (d.bat_uv / 10000) % 100, d.bat_ua / 1000);
        /* charger_connect alone still reads 1 with nothing plugged in on
         * this hardware (shows 0.03V/0mA), so gate on a real voltage. */
        if (d.chg_uv > 1000000)
            set_label_fmt(s_sy_chg, c_schg, sizeof c_schg, "%d.%02ldV %ldmA",
                          (int)(d.chg_uv / 1000000), (d.chg_uv / 10000) % 100, d.chg_ua / 1000);
        else
            set_label_fmt(s_sy_chg, c_schg, sizeof c_schg, "%s", TR("未接入"));
        set_label_fmt(s_sy_cpu, c_scpu, sizeof c_scpu,
                      TR("占用 %ld%% · %ld°""C"), d.cpu_usage, d.cpu_temp);
        set_label_fmt(s_sy_mem, c_smem, sizeof c_smem, "%ld%% · %ldM / %ldM",
                      d.mem_used_pct, (d.mem_total - d.mem_avail) / 1048576, d.mem_total / 1048576);
        long up = d.uptime;
        set_label_fmt(s_sy_up, c_sup, sizeof c_sup, lang_is_en() ? "%ldh %02ldm" : "%ldh%02ldm", up / 3600, (up / 60) % 60);

        static char c_ver[40] = "", c_imei[32] = "", c_usb[24] = "", c_fw[96] = "";
        set_label_fmt(s_set_ver, c_ver, sizeof c_ver, "%s", d.sw_version[0] ? d.sw_version : "-");
        if (strlen(d.imei) >= 8) {
            char masked[32];
            size_t len = strlen(d.imei);
            snprintf(masked, sizeof masked, "%.4s********%s", d.imei, d.imei + len - 3);
            set_label_fmt(s_set_imei, c_imei, sizeof c_imei, "%s", masked);
        } else {
            set_label_fmt(s_set_imei, c_imei, sizeof c_imei, "%s", "-");
        }
        set_label_fmt(s_set_usb, c_usb, sizeof c_usb, "%s",
                      !strcmp(d.usb_mode, "debug") ? TR("调试已开")
                      : d.usb_mode[0]              ? TR("调试关闭")
                                                   : "-");
        set_label_fmt(s_set_fw, c_fw, sizeof c_fw, "%s", d.fw[0] ? d.fw : "-");
    }

    /* ---- charts ----
     * Fed every tick whatever page is showing, so 图表 opens on real history.
     * One point per CHART_STEP ticks: the average of those readings. */
    {
        static long acc_cpu, acc_mem, acc_rx, acc_tx, acc_bat;
        static int acc_n, pts;
        if (datad_silent) {
            /* no fake flat history while datad is gone, and don't let one point
             * average readings from before and after the gap */
            acc_cpu = acc_mem = acc_rx = acc_tx = acc_bat = 0;
            acc_n = 0;
            goto charts_done;
        }
        acc_cpu += d.cpu_usage < 0 ? 0 : d.cpu_usage; acc_mem += d.mem_used_pct;
        acc_rx += d.rx_speed; acc_tx += d.tx_speed; acc_bat += d.bat_percent;
        if (++acc_n >= CHART_STEP) {
            lv_chart_set_next_value(s_ch_cpu, s_cs_cpu, (int32_t)(acc_cpu / acc_n));
            lv_chart_set_next_value(s_ch_mem, s_cs_mem, (int32_t)(acc_mem / acc_n));
            lv_chart_set_next_value(s_ch_net, s_cs_rx, rate_scale(acc_rx / acc_n));
            lv_chart_set_next_value(s_ch_net, s_cs_tx, rate_scale(acc_tx / acc_n));
            lv_chart_set_next_value(s_ch_bat, s_cs_bat, (int32_t)(acc_bat / acc_n));
            acc_cpu = acc_mem = acc_rx = acc_tx = acc_bat = 0;
            acc_n = 0;
            if (pts < CHART_READY) {
                pts++;
                for (int i = 0; i < 4; i++) uk_show(s_ch_wait[i], pts < CHART_READY);
            }
        }
        static char c_cpu[16], c_cput[16], c_mem[16], c_mems[32], c_dn[32], c_up[32], c_bat[16], c_bats[40];
        set_label_fmt(s_ch_cpu_v, c_cpu, sizeof c_cpu, "%ld%%", d.cpu_usage < 0 ? 0 : d.cpu_usage);
        set_label_fmt(s_ch_cpu_t, c_cput, sizeof c_cput, "%ld°C", d.cpu_temp);
        set_label_fmt(s_ch_mem_v, c_mem, sizeof c_mem, "%ld%%", d.mem_used_pct);
        set_label_fmt(s_ch_mem_s, c_mems, sizeof c_mems, "%ldM / %ldM",
                      (d.mem_total - d.mem_avail) / 1048576, d.mem_total / 1048576);
        char dn2[32], up2[32];
        fmt_rate(dn2, sizeof dn2, d.rx_speed);
        fmt_rate(up2, sizeof up2, d.tx_speed);
        set_label_fmt(s_ch_net_dn, c_dn, sizeof c_dn, "↓ %s", dn2);
        set_label_fmt(s_ch_net_up, c_up, sizeof c_up, "↑ %s", up2);
        lv_obj_update_layout(s_ch_net_up);
        lv_obj_align(s_ch_net_dn, LV_ALIGN_TOP_RIGHT, -(12 + lv_obj_get_width(s_ch_net_up) + 8), 8);
        int chg = d.charger_connect && d.chg_uv > 1000000;
        set_label_fmt(s_ch_bat_v, c_bat, sizeof c_bat, "%d%%", d.bat_percent);
        set_label_fmt(s_ch_bat_s, c_bats, sizeof c_bats, "%s · %d°C", chg ? TR("充电中") : TR("放电中"), d.bat_temp);
    charts_done:;
    }

    /* ---- SMS subpage ---- */
    {
        static char c_num[SMS_MAX_ROWS][48], c_date[SMS_MAX_ROWS][24], c_body[SMS_MAX_ROWS][160];
        static char c_cnt[40];
        int n = d.sms_n > SMS_MAX_ROWS ? SMS_MAX_ROWS : d.sms_n, unread = 0;
        for (int i = 0; i < d.sms_n; i++) unread += d.sms[i].unread ? 1 : 0;
        if (d.sms_n == 0) set_label_fmt(s_sms_count, c_cnt, sizeof c_cnt, "%s", "");
        else if (unread) set_label_fmt(s_sms_count, c_cnt, sizeof c_cnt, TR("%d 条未读 · 共 %d 条"), unread, d.sms_n);
        else set_label_fmt(s_sms_count, c_cnt, sizeof c_cnt, TR("共 %d 条，都已读"), d.sms_n);
        if (unread) lv_obj_remove_flag(s_sms_allread_btn, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(s_sms_allread_btn, LV_OBJ_FLAG_HIDDEN);
        uk_show(s_sms_empty, d.sms_n == 0);
        uk_show(s_sms_list, n > 0);
        if (n) lv_obj_set_height(s_sms_list, n * SMS_ROW_H);
        for (int i = 0; i < SMS_MAX_ROWS; i++) {
            if (i >= n) {
                s_sms_row_id[i] = -1;
                uk_show(s_sms_row[i], 0);
                continue;
            }
            char pv[150];
            s_sms_row_id[i] = d.sms[i].id;
            lv_obj_remove_flag(s_sms_row[i], LV_OBJ_FLAG_HIDDEN);
            set_label_fmt(s_sms_num[i], c_num[i], sizeof c_num[i], "%s", d.sms[i].num);
            set_label_fmt(s_sms_date[i], c_date[i], sizeof c_date[i], "%s", d.sms[i].date);
            /* Only two lines show; cut on a UTF-8 boundary so a Chinese
             * character is never split into a broken glyph (snprintf alone
             * cuts bytes). The label adds the "…". */
            utf8_prefix(pv, sizeof pv, d.sms[i].text);
            set_label_fmt(s_sms_body[i], c_body[i], sizeof c_body[i], "%s", pv);
            if (d.sms[i].unread) lv_obj_remove_flag(s_sms_dot[i], LV_OBJ_FLAG_HIDDEN);
            else                 lv_obj_add_flag(s_sms_dot[i], LV_OBJ_FLAG_HIDDEN);
            uk_bg(s_sms_row[i], sms_delete_armed(i) ? T->washB : T->card);
        }
    }

    /* ---- SMS detail subpage ---- */
    if (s_sub_cur == SUB_SMS_DETAIL) {
        static char c_dn[48], c_dd[24], c_del[32];
        static char c_dbody[DEVUI_SMS_TEXT_MAX];
        int k = -1;
        for (int i = 0; i < d.sms_n; i++)
            if (d.sms[i].id == s_smsd_id) { k = i; break; }
        if (k < 0) {
            /* Deleted (here or elsewhere) while open: nothing left to show. */
            s_smsd_id = -1;
            sub_back();
        } else {
            set_label_fmt(s_smsd_num, c_dn, sizeof c_dn, "%s", d.sms[k].num);
            set_label_fmt(s_smsd_date, c_dd, sizeof c_dd, "%s", d.sms[k].date);
            if (strcmp(c_dbody, d.sms[k].text)) {   /* full text: too big for set_label_fmt */
                snprintf(c_dbody, sizeof c_dbody, "%s", d.sms[k].text);
                lv_label_set_text(s_smsd_body, c_dbody);
            }
            int armed = s_smsd_del_arm && lv_tick_get() - s_smsd_del_arm < 4000;
            if (!armed) s_smsd_del_arm = 0;
            set_label_fmt(s_smsd_del_lbl, c_del, sizeof c_del, "%s", armed ? TR("再点一次确认删除") : TR("删除这条"));
            uk_button_kind(s_smsd_del_btn, s_smsd_del_lbl, armed ? UK_BTN_ARMED : UK_BTN_DANGER);
        }
    }

    /* ---- Alerts subpage ---- */
    if (alerts_poll(sub_visible(SUB_ALERTS))) {
        static char c_ac[48], c_al[ALERTS_MAX][128], c_at[ALERTS_MAX][24], c_ax[ALERTS_MAX][160];
        int n = alerts_count(), un = alerts_unread();
        const char *err = alerts_error();
        {
            static char c_hl[HEALTH_MAX][48], c_hd[HEALTH_MAX][320], c_hn[96];
            int hn = health_count();
            for (int i = 0; i < HEALTH_MAX; i++) {
                health_item_t h;
                if (i >= hn) { uk_show(s_hc_row[i], 0); continue; }
                health_get(i, &h);
                uk_show(s_hc_row[i], 1);
                lv_label_set_text(s_hc_mark[i], h.bad ? "■" : "▲");
                uk_text_color(s_hc_mark[i], h.bad ? T->badT : T->warnT);
                set_label_fmt(s_hc_label[i], c_hl[i], sizeof c_hl[i], "%s", h.label);
                set_label_fmt(s_hc_detail[i], c_hd[i], sizeof c_hd[i], "%s", h.detail);
            }
            if (hn == -2) set_label_fmt(s_hc_none, c_hn, sizeof c_hn, "%s", TR("读不到体检结果（管理后台没响应）"));
            else if (hn < 0) set_label_fmt(s_hc_none, c_hn, sizeof c_hn, "%s", TR("体检结果读取中…"));
            else set_label_fmt(s_hc_none, c_hn, sizeof c_hn, TR("● %d 项检查都正常"), health_checked());
            uk_show(s_hc_none, hn <= 0);
            int hh = hn > 0 ? hn * HC_ROW_H : UK_ROW_H;
            lv_obj_set_height(s_hc_card, hh);
            lv_obj_set_y(s_al_body, HC_TOP + hh + 10);
        }
        if (err[0]) set_label_fmt(s_al_count, c_ac, sizeof c_ac, "%s", err);
        else if (!n) set_label_fmt(s_al_count, c_ac, sizeof c_ac, "%s", TR("告警记录"));
        else if (un) set_label_fmt(s_al_count, c_ac, sizeof c_ac, TR("告警记录 · %d 条未读"), un);
        else set_label_fmt(s_al_count, c_ac, sizeof c_ac, "%s", TR("告警记录 · 都已读"));
        if (un && !err[0]) lv_obj_remove_flag(s_al_allread_btn, LV_OBJ_FLAG_HIDDEN);
        else               lv_obj_add_flag(s_al_allread_btn, LV_OBJ_FLAG_HIDDEN);
        if (!n && !err[0]) {
            lv_label_set_text(s_al_empty, TR("没有告警记录。程序崩溃、Wi-Fi 被看门狗打开这类事会记在这里。"));
            lv_obj_remove_flag(s_al_empty, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_al_empty, LV_OBJ_FLAG_HIDDEN);
        }
        uk_show(s_al_list, n > 0);
        if (n) lv_obj_set_height(s_al_list, (n > ALERTS_MAX ? ALERTS_MAX : n) * AL_ROW_H);
        for (int i = 0; i < ALERTS_MAX; i++) {
            alert_item_t a;
            char when[40];
            if (i >= n) { uk_show(s_al_row[i], 0); continue; }
            alerts_get(i, &a);
            lv_obj_remove_flag(s_al_row[i], LV_OBJ_FLAG_HIDDEN);
            if (a.time > 0) {
                /* Device clock = local wall time under TZ=UTC: localtime gives the right digits. */
                time_t tt = (time_t)a.time;
                struct tm tm;
                localtime_r(&tt, &tm);
                strftime(when, sizeof when, lang_is_en() ? "%d %b %H:%M" : "%m-%d %H:%M", &tm);
            } else {
                snprintf(when, sizeof when, TR("开机后%ld分"), a.uptime / 60);
            }
            set_label_fmt(s_al_label[i], c_al[i], sizeof c_al[i], "%s", a.label);
            set_label_fmt(s_al_time[i], c_at[i], sizeof c_at[i], "%s", when);
            set_label_fmt(s_al_text[i], c_ax[i], sizeof c_ax[i], "%s", a.text);
            if (a.unread) lv_obj_remove_flag(s_al_mark[i], LV_OBJ_FLAG_HIDDEN);
            else          lv_obj_add_flag(s_al_mark[i], LV_OBJ_FLAG_HIDDEN);
            uk_text_color(s_al_label[i], a.unread ? T->t1 : T->t2);
        }
    }

    /* ---- 信令读取 subpage ---- */
    {
        static char c_sg[6][64], c_sgn[4][48], c_nrb[160], c_lteb[200];
        set_label_fmt(s_sg_nr[0], c_sg[0], sizeof c_sg[0], "%s  %s MHz",
                      d.nr_band[0] ? d.nr_band : "-", d.nr_bw[0] ? d.nr_bw : "-");
        set_label_fmt(s_sg_nr[1], c_sg[1], sizeof c_sg[1], "%ld", d.nr_channel);
        set_label_fmt(s_sg_nr[2], c_sg[2], sizeof c_sg[2], "%d", d.nr_pci);
        set_label_fmt(s_sg_nr[3], c_sg[3], sizeof c_sg[3], "%ld", d.nr_cell_id);
        set_label_fmt(s_sg_nr[4], c_sg[4], sizeof c_sg[4], "%d-%02d %s",
                      d.mcc, d.mnc, d.operator_name);
        set_label_fmt(s_sg_nr[5], c_sg[5], sizeof c_sg[5], "%d / %d / %s",
                      d.nr_rsrp, d.nr_rsrq, d.nr_snr[0] ? d.nr_snr : "-");
        {
            /* 服务小区：有 lteca 就取第一条（主载波），否则取 net.* 的 lte_* */
            static char c_lt[6][48];
            nv_carrier_t pc[1];
            int have = nv_parse_ca(d.lteca, pc, 1, 'B') > 0;
            char bs[16] = "";
            int bw = have ? pc[0].bw : atoi(d.bandwidth);
            long earfcn = have ? pc[0].arfcn : d.channel;
            int pci = have ? pc[0].pci : d.lte_pci;
            if (have) snprintf(bs, sizeof bs, "B%d", pc[0].band);
            else if (d.band[0] && strstr(d.band, "LTE")) ui_band_short(d.band, 0, bs, sizeof bs);
            ui_rat_t rat = ui_rat(d.net_type);
            int in_use = rat == UI_RAT_4G || rat == UI_RAT_5G_NSA;
            lv_label_set_text(s_sg_lte_sec, in_use ? TR("LTE 服务小区") : TR("LTE（现在不用，下面是测量值）"));
            lv_label_set_text(s_sg_nr_sec, rat == UI_RAT_4G ? TR("5G（现在不用，下面是测量值）") : TR("5G 服务小区"));
            if (bs[0] && bw > 0) set_label_fmt(s_sg_lt[0], c_lt[0], sizeof c_lt[0], "%s  %d MHz", bs, bw);
            else set_label_fmt(s_sg_lt[0], c_lt[0], sizeof c_lt[0], "%s", bs[0] ? bs : "-");
            if (earfcn > 0) set_label_fmt(s_sg_lt[1], c_lt[1], sizeof c_lt[1], "%ld", earfcn);
            else set_label_fmt(s_sg_lt[1], c_lt[1], sizeof c_lt[1], "%s", "-");
            if (pci > 0) set_label_fmt(s_sg_lt[2], c_lt[2], sizeof c_lt[2], "%d", pci);
            else set_label_fmt(s_sg_lt[2], c_lt[2], sizeof c_lt[2], "%s", "-");
            if (in_use && d.lte_cell_id > 0) set_label_fmt(s_sg_lt[3], c_lt[3], sizeof c_lt[3], "%ld", d.lte_cell_id);
            else set_label_fmt(s_sg_lt[3], c_lt[3], sizeof c_lt[3], "%s", "-");
            if (d.lte_rsrp != 0)
                set_label_fmt(s_sg_lt[4], c_lt[4], sizeof c_lt[4], "%d / %d / %s",
                              d.lte_rsrp, d.lte_rsrq, d.lte_snr[0] ? d.lte_snr : "-");
            else set_label_fmt(s_sg_lt[4], c_lt[4], sizeof c_lt[4], "%s", "-");
            if (d.lte_rssi != 0) set_label_fmt(s_sg_lt[5], c_lt[5], sizeof c_lt[5], "%d", d.lte_rssi);
            else set_label_fmt(s_sg_lt[5], c_lt[5], sizeof c_lt[5], "%s", "-");
        }
        set_label_fmt(s_sg_net[0], c_sgn[0], sizeof c_sgn[0], "%s", nv.mode_word[0] ? nv.mode_word : "-");
        set_label_fmt(s_sg_net[1], c_sgn[1], sizeof c_sgn[1], "%s",
                      d.wan_status[0] ? d.wan_status : "-");
        set_label_fmt(s_sg_net[2], c_sgn[2], sizeof c_sgn[2], "%s", d.net_type);
        set_label_fmt(s_sg_net[3], c_sgn[3], sizeof c_sgn[3], "%s",
                      d.hsr ? TR("开启") : TR("关闭"));
        char nrf[160], ltef[200];
        fmt_band_list(nrf, sizeof nrf, d.sa_bands, 'n');
        fmt_band_list(ltef, sizeof ltef, d.lte_bands, 'B');
        set_label_fmt(s_sg_nrb, c_nrb, sizeof c_nrb, "%s", nrf[0] ? nrf : "-");
        set_label_fmt(s_sg_lteb, c_lteb, sizeof c_lteb, "%s", ltef[0] ? ltef : "-");
    }

    /* ---- 锁频 subpage ---- */
    for (int gi = 0; gi < 3; gi++) {   /* confirm window over, or the lock never read back */
        band_group_t *g = &s_bg[gi];
        uint32_t now = lv_tick_get();
        if (g->arm && now - g->arm >= 5000) {
            g->arm = 0;
            lv_label_set_text(g->apply_lbl, TR("应用锁频"));
            uk_button_kind(g->apply_btn, g->apply_lbl, UK_BTN_PLAIN);
        }
        if (g->sent && now - g->sent >= 30000) {
            g->sent = 0;
            lv_label_set_text(g->apply_lbl, TR("没生效，可再试一次"));
        }
    }
    band_group_sync(BG_SA,  d.sa_sup,  d.sa_bands);
    band_group_sync(BG_NSA, d.nsa_sup, d.nsa_bands);
    band_group_sync(BG_LTE, d.lte_sup, d.lte_bands);
    /* 网络模式那一排：设备真实的模式 */
    {
        int sel = -1;
        for (int i = 0; i < LK_MODES; i++) if (!strcmp(d.net_select, k_lk_mode_v[i])) sel = i;
        /* TCHGWL_5G and NETWORK_auto (after 恢复默认) are automatic too (datad says which are) */
        if (nv.mode_auto) sel = 0;
        lk_mode_sync(sel, 1);
    }

    /* ---- › 行右边的状态字（蜂窝 / 出口标签） ---- */
    {
        static char c_t1[40] = "", c_t6[112] = "";
        /* unread SMS = accent (the old tile's badge) */
        {
            static int c_on[2] = { -1, -1 };
            int unread = d.sms_unread > 0;
            if (unread != c_on[1]) { c_on[1] = unread; uk_text_color(s_tile_sub[SUB_SMS], unread ? T->accT : T->t2); }
        }
        if (d.sms_unread)
            set_label_fmt(s_tile_sub[SUB_SMS], c_t1, sizeof c_t1,
                          TR("%d 条 · %d 未读"), d.sms_n, d.sms_unread);
        else
            set_label_fmt(s_tile_sub[SUB_SMS], c_t1, sizeof c_t1, TRC("短信", "%d 条"), d.sms_n);
        /* 实体 SIM 写「SIM 卡 · 运营商」，eSIM 写「eSIM · 配置名」 */
        if (s_sim.kind == UI_SIM_ESIM)
            set_label_fmt(s_tile_sub[SUB_ESIM], c_t6, sizeof c_t6, "eSIM · %s", esim_current());
        else if (s_sim.kind == UI_SIM_PLAIN)
            set_label_fmt(s_tile_sub[SUB_ESIM], c_t6, sizeof c_t6, TR("SIM 卡 · %s"), s_sim.oper[0] ? s_sim.oper : TR("已插入"));
        else
            set_label_fmt(s_tile_sub[SUB_ESIM], c_t6, sizeof c_t6, "%s", TR("没插卡"));
        static char c_t3[40] = "";
        int locked = 0, known = 0;
        for (int gi = 0; gi < 3; gi++)
            for (int i = 0; i < s_bg[gi].n; i++) { known = 1; if (!s_bg[gi].sel[i]) locked = 1; }
        set_label_fmt(s_tile_sub[SUB_LOCK], c_t3, sizeof c_t3, "%s", !known ? "" : locked ? TR("已锁定") : TR("未锁定"));
        static char c_t4[40] = "";
        if (!speedtest_agent_reachable())
            set_label_fmt(s_tile_sub[SUB_SPEED], c_t4, sizeof c_t4, "%s",
                          TR("服务不可用"));
        else if (speedtest_running())
            set_label_fmt(s_tile_sub[SUB_SPEED], c_t4, sizeof c_t4, "%s",
                          speedtest_phase_label());
        else if (speedtest_phase() == ST_COMPLETE)
            set_label_fmt(s_tile_sub[SUB_SPEED], c_t4, sizeof c_t4,
                          "↓ %.0f · ↑ %.0f Mbps",
                          speedtest_download_mbps() >= 0 ? speedtest_download_mbps() : 0.0,
                          speedtest_upload_mbps() >= 0 ? speedtest_upload_mbps() : 0.0);
        else
            set_label_fmt(s_tile_sub[SUB_SPEED], c_t4, sizeof c_t4, "%s",
                          TR("点击测速"));
        if (s_cell_speed_sub) lbl_set(s_cell_speed_sub, c_t4);   /* 蜂窝标签上同一行 */
    }

    /* ---- WiFi subpage ---- */
    aux_refresh(tab_visible(TAB_WIFI) || tab_visible(TAB_SYS) || tab_visible(TAB_CELL), &d);
    md_refresh();
    refresh_wifi(&d);
    {
        static char c_wsw[5][32];
        int st[5] = { (s_aux_w24 == 1 || s_aux_w5 == 1), s_aux_w24 == 1, s_aux_w5 == 1,
                      s_aux_psm == 1, d.nfc_switch };
        for (int i = 0; i < 5; i++) {
            sw_apply(s_w_sw[i], st[i]);
            set_label_fmt(s_w_sw_st[i], c_wsw[i], sizeof c_wsw[i], "%s",
                          st[i] ? TR("已开启") : TR("已关闭"));
        }
        if (s_aux_pool[0]) {
            static char c_pool2[48] = "";
            set_label_fmt(s_w_pool, c_pool2, sizeof c_pool2, "%s", s_aux_pool);
        }
        static char c_dps[40] = "";
        sw_apply(s_sy_dps_sw, s_aux_dps == 1);
        set_label_fmt(s_sy_dps_st, c_dps, sizeof c_dps, "%s",
                      s_aux_dps < 0 ? "—"
                      : s_aux_dps   ? TR("已开启")
                                    : TR("已关闭"));
        /* Reflects whichever of {this switch, the topbar tap} was touched
         * last — both just write s_cf_speed_bits, this only redraws it. */
        sw_apply(s_sy_speedunit_sw, s_cf_speed_bits);
        lv_label_set_text(s_sy_speedunit_st, s_cf_speed_bits ? "Mbps" : "MB/s");
    }
}

