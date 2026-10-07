/*
 * ui_parts/system.c - 系统页、图表页、性能测试子页.
 * Part of src/ui.c, which #includes it: the parts share ui.c's static state
 * and are one translation unit, not compiled on their own (the Makefile
 * lists only ui.c). Split 2026-09-26 so the 6000-line file stops being
 * the place every change collides; the built binary is byte-identical.
 *
 * SPDX-License-Identifier: MIT
 */
/* ---- System page ---- */
static const uint32_t k_off_ms[3] = { 0, 30000, 120000 };  /* Never / 30s / 2m */

static void bright_label(void)
{
    char b[8];
    int max = backlight_max() > 0 ? backlight_max() : 255;
    snprintf(b, sizeof b, "%d%%", lv_slider_get_value(s_set_bright) * 100 / max);
    lv_label_set_text(s_set_bright_v, b);
}

static void bright_cb(lv_event_t *e)
{
    backlight_set(lv_slider_get_value((lv_obj_t *)lv_event_get_target(e)));
    bright_label();
}

/* Saved on release, not per step: the slider fires dozens of changes per drag. */
static void bright_save_cb(lv_event_t *e)
{
    s_cf_bright = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    save_devui_conf();
}

static void highlight_off_btns(int sel) { uk_seg_set(&s_off_seg, sel); }

static void offsel_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_autooff_ms = k_off_ms[idx];
    s_auto_slept = 0;
    highlight_off_btns(idx);
    if (s_cf_autooff_ms != (int)k_off_ms[idx]) {   /* kept across restarts */
        s_cf_autooff_ms = (int)k_off_ms[idx];
        save_devui_conf();
    }
}

static void dps_cb(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    int on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    data_control("power.direct_supply.set", on ? "{\"enabled\":true}" : "{\"enabled\":false}",
                 on ? "enabled=1" : "enabled=0");
    s_aux_dps = on;
    aux_hold(&s_hold_dps);
}

/* Same setting the status-bar tap (topbar_speed_unit_cb) flips — this is
 * just a discoverable, labelled home for it (2026-09-22: tap-to-toggle on
 * the topbar has no visible affordance beyond the accent color, easy to
 * never find). Both write the same s_cf_speed_bits + devui.conf, so
 * whichever one the user touches, the other stays in sync via refresh_cb's
 * sw_apply() below. */
static void speedunit_cb(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    s_cf_speed_bits = lv_obj_has_state(sw, LV_STATE_CHECKED);
    save_devui_conf();
}

/* Ask u60-uid (the screen-owner daemon) to switch. It stops us, waits for
 * /dev/dri/card0 to be free and starts the vendor UI — and because it asked
 * us to stop, it does not count our exit as a crash. Opening a FIFO for write
 * with O_NONBLOCK fails (ENXIO) when nobody is reading it, which is exactly
 * "u60-uid is not running". */
static int request_vendor_via_uid(void)
{
    int fd = open("/tmp/u60-uid.ctl", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return 0;
    int ok = write(fd, "vendor\n", 7) == 7;
    close(fd);
    return ok;
}

/* Switch to the vendor UI. Two-stage confirm (same pattern v1 used for
 * act:exitstock in htmlmain.c): a misfire here is expensive — the vendor UI
 * has no button back to us, so the only way home is the corner long-press
 * (u60-uid) or SSH. Without u60-uid (older installs) we do it ourselves: the
 * panel is bare DRM with no compositor, so this process must exit and close
 * /dev/dri/card0 before the vendor UI can open it — the init.d start is
 * scheduled for +2s and we exit via SIGTERM. */
static void act_switch_vendor(lv_event_t *e)
{
    static uint32_t arm;
    uint32_t now = lv_tick_get();

    LV_UNUSED(e);
    arm = s_vendor_arm;
    if (arm && now - arm < 5000) {
        lv_label_set_text(s_vendor_lbl, TR("切换中…"));
        if (request_vendor_via_uid()) return;
        system("( sleep 2; /etc/init.d/zte_topsw_devui start ) >/dev/null 2>&1 &");
        raise(SIGTERM);
        return;
    }
    s_vendor_arm = now ? now : 1;
    uk_button_kind(s_vendor_btn, s_vendor_lbl, UK_BTN_ARMED);
    lv_label_set_text(s_vendor_lbl, TR("再按一次确认切换"));
}

static lv_obj_t *s_ap_btn[3];   /* = s_ap_seg.item[] (the render test taps them) */
static const ui_appear_t k_ap_val[3] = { UI_APPEAR_LIGHT, UI_APPEAR_DARK, UI_APPEAR_AUTO };

static void appearance_ui_sync(void)
{
    if (!s_ap_seg.obj) return;
    for (int i = 0; i < 3; i++)
        if (k_ap_val[i] == s_cf_appear) uk_seg_set(&s_ap_seg, i);
}

static void appearance_btn_cb(lv_event_t *e)
{
    s_vendor_arm = 0;   /* a pending 切换到原厂界面 confirm does not survive a theme switch */
    appearance_set(k_ap_val[(int)(intptr_t)lv_event_get_user_data(e)]);
}

/* 语言 / Language: the first row of 屏幕, in both languages in either mode
 * (whoever switched by mistake can still find the way back). The small line
 * says the alert SMS follow it, or for a few seconds why a switch did not
 * happen. */
static lv_obj_t *s_lang_btn[2];   /* = s_lang_seg.item[] (the render test taps them) */
static lv_obj_t *s_lang_note;
static lv_timer_t *s_lang_note_t;
#define LANG_NOTE "告警短信也用这个语言 · Alert SMS use this too"

static void lang_note_reset_cb(lv_timer_t *tm)
{
    LV_UNUSED(tm);
    s_lang_note_t = NULL;
    if (!s_lang_note) return;
    lv_label_set_text(s_lang_note, LANG_NOTE);
    uk_text_color(s_lang_note, T->t3);
}

static void lang_ui_note(const char *msg, int warn)
{
    if (!s_lang_note) return;
    lv_label_set_text(s_lang_note, msg);
    uk_text_color(s_lang_note, warn ? T->warnT : T->t3);
    if (s_lang_note_t) lv_timer_reset(s_lang_note_t);
    else {
        s_lang_note_t = lv_timer_create(lang_note_reset_cb, 5000, NULL);
        lv_timer_set_repeat_count(s_lang_note_t, 1);
    }
}

static void lang_btn_cb(lv_event_t *e)
{
    lang_set((int)(intptr_t)lv_event_get_user_data(e));
}

static int build_charts(lv_obj_t *t, int y0);

static void build_system(lv_obj_t *t)
{
    const char *const off_lbl[3] = { TR("常亮"), TR("30秒"), TR("2分钟") };
    const char *const ap_lbl[3] = { TR("浅色"), TR("深色"), TR("自动") };
    int y = 4;
    t = uk_scroll(t, 0, UI_VIEW_H, 1000);

    uk_section(t, y, TR("屏幕")); y += 20;
    lv_obj_t *c = uk_card(t, UK_MARGIN, y, UK_CARD_W, 60 + 120);
    {
        static const char *const lang_lbl[2] = { "中文", "English" };   /* not translated: each in its own language */
        uk_label(c, UF.cj14, T->t2, UK_PAD, 11, "语言 / Language");
        uk_seg(&s_lang_seg, c, 166, 6, 120, lang_lbl, 2, lang_btn_cb);
        for (int i = 0; i < 2; i++) s_lang_btn[i] = s_lang_seg.item[i];
        uk_seg_set(&s_lang_seg, lang_is_en());
        /* own line under the segment (the segment ends at y 36; on 10-02 the
         * note at 31 ran under it on the device) */
        s_lang_note = uk_label_w(c, UF.cj12, T->t3, UK_PAD, 39, UK_CARD_W - 2 * UK_PAD, 0, LANG_NOTE);
        uk_sep(c, 60);
        c = uk_box(c, 0, 60, UK_CARD_W, 120, T->card, 0);   /* the rows below keep their offsets */
        lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    }
    uk_label(c, UF.cj14, T->t2, UK_PAD, 11, TR("亮度"));
    /* "Brightness" is wider than 亮度: in English the slider starts later, same right end */
    s_set_bright = lang_is_en() ? uk_slider(c, 96, 17, 134) : uk_slider(c, 58, 17, 172);
    lv_slider_set_range(s_set_bright, 10, backlight_max());
    lv_slider_set_value(s_set_bright, backlight_get() > 0 ? backlight_get() : backlight_max(), LV_ANIM_OFF);
    lv_obj_add_event_cb(s_set_bright, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_set_bright, bright_save_cb, LV_EVENT_RELEASED, NULL);
    s_set_bright_v = uk_label_r(c, UF.n15, T->t2, UK_CARD_W - UK_PAD, 10, "");
    bright_label();
    uk_sep(c, 40);
    uk_label(c, UF.cj14, T->t2, UK_PAD, 51, TR("自动息屏"));
    uk_seg(&s_off_seg, c, 116, 45, 170, off_lbl, 3, offsel_cb);
    uk_sep(c, 80);
    uk_label(c, UF.cj14, T->t2, UK_PAD, 91, TR("外观"));
    uk_seg(&s_ap_seg, c, 116, 85, 170, ap_lbl, 3, appearance_btn_cb);
    for (int i = 0; i < 3; i++) s_ap_btn[i] = s_ap_seg.item[i];
    {
        int sel = 0;
        for (int i = 0; i < 3; i++) if (k_off_ms[i] == s_autooff_ms) sel = i;
        highlight_off_btns(sel);
    }
    appearance_ui_sync();
    y += 60 + 120 + 10;

    uk_section(t, y, TR("电池与负载")); y += 20;
    /* 预估：公式见 estimate.c（和管理网页同一份规则） */
    c = uk_card(t, UK_MARGIN, y, UK_CARD_W, 6 * UK_ROW_H);
    static const char *const k_load_cap[6] = { N_("电池"), N_("预估"), N_("充电器"), "CPU", N_("内存"), N_("运行") };
    lv_obj_t **load_val[6] = { &s_sy_bat, &s_sy_est, &s_sy_chg, &s_sy_cpu, &s_sy_mem, &s_sy_up };
    for (int i = 0; i < 6; i++) *load_val[i] = uk_row(c, i * UK_ROW_H, TR(k_load_cap[i]), i == 0);
    lv_obj_set_style_text_font(s_sy_est, UF.cj14, 0);
    y += 6 * UK_ROW_H + 10;

    uk_section(t, y, TR("近 5 分钟")); y += 20;
    y += build_charts(t, y) + 10;

    uk_section(t, y, TR("设备")); y += 20;
    c = uk_card(t, UK_MARGIN, y, UK_CARD_W, 6 * UK_ROW_H);
    s_set_ver  = uk_row(c, 0, TR("版本"), 1);
    lv_obj_set_style_text_font(s_set_ver, UF.n12, 0);
    s_set_imei = uk_row(c, UK_ROW_H, "IMEI", 0);
    s_set_usb  = uk_row(c, 2 * UK_ROW_H, "USB", 0);
    lv_obj_set_style_text_font(s_set_usb, UF.cj14, 0);
    s_set_fw   = uk_row(c, 3 * UK_ROW_H, TR("固件"), 0);
    lv_obj_set_style_text_font(s_set_fw, UF.n12, 0);   /* English key is "Build": the OpenWrt string fits whole */
    lv_obj_set_y(s_set_fw, 3 * UK_ROW_H + 12);
    /* 健康: the device check's counts (doctor.sh via the agent); the whole row opens 告警 */
    s_set_health = uk_row_nav(c, 4 * UK_ROW_H, TR("健康"), 0, open_alerts_cb, NULL);
    /* E4 DD5: who changed what, when (datad's change log) */
    s_tile_sub[SUB_LOG] = uk_row_nav(c, 5 * UK_ROW_H, TR("改动记录"), 0, tile_click_cb, (void *)(intptr_t)SUB_LOG);
    y += 6 * UK_ROW_H + 10;

    uk_section(t, y, TR("开关")); y += 20;
    c = uk_card(t, UK_MARGIN, y, UK_CARD_W, 2 * UK_ROW_H);
    uk_label(c, UF.cj14, T->t2, UK_PAD, 11, TR("停止充电"));
    s_sy_dps_st = uk_label_r(c, UF.cj12, T->t3, UK_CARD_W - UK_PAD - 52, 13, "");
    s_sy_dps_sw = uk_toggle(c, UK_CARD_W - UK_PAD, 7, dps_cb, NULL);
    uk_sep(c, UK_ROW_H);
    uk_label(c, UF.cj14, T->t2, UK_PAD, UK_ROW_H + 11, TR("状态栏网速用 Mbps"));
    s_sy_speedunit_st = uk_label_r(c, UF.n12, T->t3, UK_CARD_W - UK_PAD - 52, UK_ROW_H + 13, "");
    s_sy_speedunit_sw = uk_toggle(c, UK_CARD_W - UK_PAD, UK_ROW_H + 7, speedunit_cb, NULL);
    y += 2 * UK_ROW_H + 10;

    uk_section(t, y, TR("插入手机等设备时")); y += 20;
    y += build_usbmode_card(t, y) + 10;

    uk_section(t, y, TR("调试")); y += 20;
    c = uk_card(t, UK_MARGIN, y, UK_CARD_W, UK_ROW_H);
    s_tile_sub[SUB_PERF] = uk_row_nav(c, 0, TR("性能测试"), 1, tile_click_cb, (void *)(intptr_t)SUB_PERF);
    lv_label_set_text(s_tile_sub[SUB_PERF], TR("调试页"));
    uk_text_color(s_tile_sub[SUB_PERF], T->t3);
    y += UK_ROW_H + 10;

    uk_section(t, y, TR("系统")); y += 20;
    c = uk_card(t, UK_MARGIN, y, UK_CARD_W, 100);
    s_vendor_btn = uk_button(c, UK_PAD, 12, UK_CARD_W - 2 * UK_PAD, 40, TR("切换到原厂界面"), UK_BTN_PLAIN,
                             act_switch_vendor, NULL, &s_vendor_lbl);
    uk_label_w(c, UF.cj12, T->t3, UK_PAD, 62, UK_CARD_W - 2 * UK_PAD, 1,
               TR("电源键：短按 亮屏/息屏  长按 电源菜单\n回到这里：长按屏幕右下角 3 秒"));
    y += 100;
    uk_scroll_extent(t, y + UK_TAB_PAD);   /* build_system: 屏幕 … 系统 */
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* ---- Charts page ----
 * The four series the old litehtml charts page plotted. lv_chart with a fixed point count
 * and lv_chart_set_next_value(): the series buffer is allocated once, values
 * shift in place, so a page that updates every second allocates nothing. */
/* Card-coloured backing so the chart's grid line does not run through the
 * text (seen on the device 10-02). */
static lv_obj_t *wait_backed(lv_obj_t *l)
{
    lv_obj_set_style_bg_color(l, lv_color_hex(T->card), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(l, 4, 0);
    return l;
}

static lv_obj_t *chart_wait(lv_obj_t *card, int x, int y)
{
    return wait_backed(uk_label(card, UF.cj12, T->t3, x, y, TR("正在收集 · 1 分钟后出现曲线")));
}

/* 原「图表」标签（2026-09-25 并进系统，放在电池与负载下面）。y0 = 第一张卡的
 * 位置，返回占的高度。 */
static int build_charts(lv_obj_t *t, int y0)
{
    /* 网速: log scale (bytes/s spans five orders of magnitude here), two
     * lines told apart by colour and dash, legend in text colours. */
    /* 网速这张在首页（2026-09-25），CPU / 内存 / 电池留在系统 */
    lv_obj_t *c = s_ch_net_card = uk_card(s_home_scroll, UK_MARGIN, 0, UK_CARD_W, 116);
    uk_label(c, UF.cj12, T->t3, 12, 9, TR("网速 · 对数刻度"));
    s_ch_net_up = uk_label_r(c, UF.n12, T->warnT, 288, 8, "");
    s_ch_net_dn = uk_label_r(c, UF.n12, T->accT, 200, 8, "");
    s_ch_net = uk_chart(c, 44, 30, 244, 60, CHART_PTS, T->blue, T->orange, &s_cs_rx, &s_cs_tx);
    /* rate_scale: 10^8 B/s = 100 → 10 MB/s at 87.5, 100 KB/s at 62.5. The
     * axis shows 50–100 (~30 KB/s to 100 MB/s): below that is idle chatter. */
    lv_chart_set_axis_range(s_ch_net, LV_CHART_AXIS_PRIMARY_Y, 50, 100);
    uk_label(c, UF.n11, T->t3, 12, 30 + 60 * 25 / 100 - 7, "10M");
    uk_label(c, UF.n11, T->t3, 12, 30 + 60 * 75 / 100 - 7, "100K");
    uk_label_r(c, UF.cj12, T->t3, 288, 94, TR("近 5 分钟"));
    s_ch_wait[0] = chart_wait(c, 56, 52);

    lv_obj_t *a = uk_card(t, UK_MARGIN, y0, 145, 110), *m = uk_card(t, UK_MARGIN + 155, y0, 145, 110);
    uk_label(a, UF.cj12, T->t3, 12, 9, "CPU");
    s_ch_cpu_t = uk_label_r(a, UF.cj12, T->t2, 133, 9, "");
    s_ch_cpu_v = uk_label(a, UF.n20, T->t1, 12, 26, "");
    s_ch_cpu = uk_chart(a, 12, 60, 121, 40, CHART_PTS, T->blue, 0, &s_cs_cpu, NULL);
    s_ch_wait[1] = wait_backed(uk_label(a, UF.cj12, T->t3, 8, 72, TR("正在收集…")));
    uk_label(m, UF.cj12, T->t3, 12, 9, TRC("图表", "内存"));   /* 145 px card: RAM in English */
    s_ch_mem_s = uk_label_r(m, UF.n12, T->t2, 133, 9, "");
    s_ch_mem_v = uk_label(m, UF.n20, T->t1, 12, 26, "");
    s_ch_mem = uk_chart(m, 12, 60, 121, 40, CHART_PTS, T->blue, 0, &s_cs_mem, NULL);
    s_ch_wait[2] = wait_backed(uk_label(m, UF.cj12, T->t3, 8, 72, TR("正在收集…")));

    lv_obj_t *b = uk_card(t, UK_MARGIN, y0 + 120, UK_CARD_W, 110);
    uk_label(b, UF.cj12, T->t3, 12, 9, TR("电池"));
    s_ch_bat_s = uk_label_r(b, UF.cj12, T->t2, 288, 9, "");
    s_ch_bat_v = uk_label(b, UF.n20, T->t1, 12, 26, "");
    s_ch_bat = uk_chart(b, 12, 56, 276, 36, CHART_PTS, T->green, 0, &s_cs_bat, NULL);
    uk_label_r(b, UF.cj12, T->t3, 288, 92, TR("近 5 分钟"));
    s_ch_wait[3] = chart_wait(b, 8, 66);
    home_reflow();
    return 120 + 110;
}

/* ---- perf test subpage ---- */
/* 性能测试: a debug page; its layout stays, only the colours follow the theme. */
static void build_sub_perf(lv_obj_t *t)
{
    lv_obj_t *c = uk_card(t, UK_MARGIN, 8, UK_CARD_W, 120);
    uk_label(c, UF.cj12, T->t3, UK_PAD, 10, TR("渲染刷新率"));
    s_t_fps = uk_label(c, UF.n20, T->okT, UK_PAD, 28, "-- FPS");
    uk_label(c, UF.cj12, T->t3, UK_PAD, 62, TR("触控上报率"));
    s_t_touch = uk_label(c, UF.n20, T->warnT, UK_PAD, 80, "-- Hz");
    s_t_box = uk_box(t, UK_MARGIN, 150, UK_CARD_W, 76, T->green, UK_R_CARD);
}

static void sms_row_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (sms_delete_tap(idx)) return;          /* second tap on an armed row: deleted */
    if (s_sms_row_id[idx] < 0) return;
    /* Open it. Reading it in full is what marks it read — tapping a row used
     * to mark it read with nothing else happening, which looked like the tap
     * did nothing, and left no way to see a long message past two lines. */
    s_smsd_id = s_sms_row_id[idx];
    s_smsd_del_arm = 0;
    sms_mark_read_id(s_smsd_id);
    lv_obj_scroll_to_y(s_smsd_scroll, 0, LV_ANIM_OFF);
    sub_open_child(SUB_SMS_DETAIL, SUB_SMS);
}

static void sms_allread_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    sms_mark_all_read();
}

/* Two taps within 4 s, same pattern as the list's long-press delete. */
static void smsd_delete_cb(lv_event_t *e)
{
    uint32_t now = lv_tick_get();
    LV_UNUSED(e);
    if (s_smsd_del_arm && now - s_smsd_del_arm > 300 && now - s_smsd_del_arm < 4000) {
        sms_delete_id(s_smsd_id);
        s_smsd_del_arm = 0;
        s_smsd_id = -1;
        sub_back();
        return;
    }
    s_smsd_del_arm = now;
}

static void sms_row_longpress_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    sms_delete_arm(idx);
}

