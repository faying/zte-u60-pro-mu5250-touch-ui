/*
 * ui_parts/usbmode.c - 插线时的 USB 用法（弹窗 + 记住选择）.
 * Part of src/ui.c, which #includes it (see power.c).
 *
 * A phone plugged into the U60 is powered by it: the port is source + host
 * and the phone gets no network. The vendor screen asks what the phone is
 * for at that moment; with ours on the panel nobody asked, so a phone only
 * ever charged (2026-10-07). Same three choices, same vendor calls, made by
 * datad's usb.attach_mode (manager docs/designs/usb-attach-mode.md):
 *   充电 + 上网   typec DR_Swap device → the phone gets a USB network
 *   快速充电宝    powerbank state 1 (18 W, no data)
 *   网口配件      nothing (stays host, for a USB Ethernet adapter)
 * devui.conf usb_attach= decides: ask (this sheet), or the remembered choice
 * done 3 s after the plug-in. Kept here, not in datad or the agent: it must
 * happen only while this UI owns the screen, and only they know that.
 *
 * SPDX-License-Identifier: MIT
 */
#define UM_ROWS        3
#define UM_ROW_H       52
#define UM_GRACE_MS    3000u    /* remembered choice: wait this long after the plug-in */
#define UM_PEND_MS     15000u   /* no readback by then: say it did not take */
#define UM_DONE_MS     1500u    /* 已生效 stays this long, then the sheet closes */
#define UM_BANNER_MS   5000u

static const char *const k_um_name[UM_ROWS] = { N_("充电 + 上网"), N_("快速充电宝"), N_("网口配件") };
static const char *const k_um_sub[UM_ROWS] = {
    N_("手机经 U60 上网，同时充电"),
    N_("18W 给手机快充，不上网"),
    N_("USB 转网口等配件；手机只充电"),
};

static ui_usb_track_t s_um_track;
static uk_sheet_t s_um_sheet;
static lv_obj_t *s_um_row[UM_ROWS], *s_um_mark[UM_ROWS], *s_um_status;
static lv_obj_t *s_um_rem_sw, *s_um_ok, *s_um_ok_lbl;
static int s_um_sel = -1;          /* chosen in the sheet, not sent yet */
static int s_um_pend = -1;         /* sent, waiting for the blocks to show it */
static int s_um_pend_auto;         /* … by the remembered choice, not the sheet */
static uint32_t s_um_pend_at, s_um_done_at, s_um_auto_at;
static int s_um_cur = -1;          /* in effect now: 0 share, 1 fast charge, -1 neither */
static int s_um_attached;          /* something in the port (cc = 1) */
static char s_um_banner[96];
static uint32_t s_um_banner_at;

/* system page card (built by build_system) */
static lv_obj_t *s_um_now, *s_um_pref_mark[4];

static int usbmode_visible(void) { return uk_sheet_visible(&s_um_sheet); }

/* A tick to keep as a time stamp: 0 means "none", so a tick of 0 is 1. */
static uint32_t um_stamp(uint32_t t) { return t ? t : 1; }

static const char *usbmode_now_text(const devui_data_t *d)
{
    if (d->usb_cc < 0) return "—";
    if (d->usb_cc != 1) return TR("没插设备");
    if (d->powerbank == 1) return TR("快速充电宝");
    if (!strcmp(d->usb_power_role, "source"))
        return !strcmp(d->usb_data_role, "device") ? TR("充电 + 上网") : TR("只充电，没有网络");
    return TR("U60 在充电");
}

static void um_status(const char *txt, uint32_t col)
{
    lv_label_set_text(s_um_status, txt);
    uk_text_color(s_um_status, col);
}

static void um_paint(void)
{
    for (int i = 0; i < UM_ROWS; i++) {
        int on = i == s_um_cur;
        uk_bg(s_um_mark[i], T->fillBlue);
        lv_obj_set_style_bg_opa(s_um_mark[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(s_um_mark[i], lv_color_hex(on ? T->fillBlue : T->t3), 0);
        uk_bg(s_um_row[i], i == s_um_sel ? T->accS : T->glass);
        lv_obj_set_style_bg_opa(s_um_row[i], i == s_um_sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
    uk_button_kind(s_um_ok, s_um_ok_lbl, s_um_sel >= 0 && s_um_pend < 0 ? UK_BTN_PRIMARY : UK_BTN_PLAIN);
}

static void um_wake(void)
{
    if (!backlight_is_on()) ui_tgate_woke(&s_tgate, lv_tick_get());
    backlight_on();
    s_auto_slept = 0;
    lv_display_trigger_activity(NULL);
}

static void usbmode_open(void)
{
    s_um_sel = -1;
    uk_toggle_set(s_um_rem_sw, 0);
    um_status(s_um_pend >= 0 ? TR("切换中…") : TR("选一项，再点「确定」"), T->t3);
    um_paint();
    um_wake();
    if (power_menu_visible()) { power_menu_disarm(); power_menu_set(0); }
    uk_sheet_show(&s_um_sheet, 1);
    update_tabs();
}

static void usbmode_close(void)
{
    uk_sheet_show(&s_um_sheet, 0);
    s_um_sel = -1;
    update_tabs();
}

static void um_cancel_cb(lv_event_t *e) { LV_UNUSED(e); usbmode_close(); }

static void um_row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_um_pend >= 0) { um_status(TR("切换中…"), T->t3); return; }
    s_um_sel = i;
    um_status(i == s_um_cur ? TR("现在就是这样 · 点「确定」也可以记住它") : TR("点「确定」切换"), T->t2);
    um_paint();
}

/* Send mode i. remembered: the automatic run (datad then leaves a USB
 * Ethernet adapter alone). Returns 0 if datad could not be asked. */
static int um_send(int i, int remembered)
{
    char params[64];
    if (i == 2) return 1;            /* 网口配件: the vendor writes nothing either */
    snprintf(params, sizeof params, "{\"mode\":\"%s\"%s}", ui_usb_pref_name((ui_usb_pref_t)(i + 1)),
             remembered ? ",\"remembered\":true" : "");
    if (!data_control("usb.attach_mode", params, NULL)) return 0;
    s_um_pend = i;
    s_um_pend_auto = remembered;
    s_um_pend_at = lv_tick_get();
    return 1;
}

/* 「选中 + 确定」 is the two-step confirm (DESIGN §4). */
static void um_ok_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_um_pend >= 0) { um_status(TR("切换中…"), T->t3); return; }
    if (s_um_sel < 0) { um_status(TR("先选一项"), T->warnT); return; }
    if (!s_um_attached) { um_status(TR("设备已拔掉"), T->warnT); return; }
    if (lv_obj_has_state(s_um_rem_sw, LV_STATE_CHECKED)) {
        s_cf_usb = (ui_usb_pref_t)(s_um_sel + 1);
        save_devui_conf();
        usbmode_pref_paint();
    }
    int i = s_um_sel;
    s_um_sel = -1;
    if (i == 2 || i == s_um_cur) {
        um_status(TR("已生效"), T->okT);
        s_um_done_at = um_stamp(lv_tick_get());
    } else if (um_send(i, 0)) {
        um_status(TR("切换中…"), T->t3);
    } else {
        um_status(TR("数据服务不可用 · 没切"), T->badT);
    }
    um_paint();
}

/* Remembered choice, done UM_GRACE_MS after the plug-in if still there. */
static void um_auto_run(void)
{
    int i = (int)s_cf_usb - 1;
    s_um_auto_at = 0;
    if (i < 0 || !s_um_attached) return;
    if (i == s_um_cur || um_send(i, 1)) {
        snprintf(s_um_banner, sizeof s_um_banner, TR("已按记住的方式：%s"), TR(k_um_name[i]));
    } else {
        snprintf(s_um_banner, sizeof s_um_banner, "%s", TR("数据服务不可用 · 没按记住的方式切"));
    }
    s_um_banner_at = um_stamp(lv_tick_get());
}

/* The top banner line for a remembered run, for a few seconds. */
static const char *usbmode_banner(void)
{
    if (!s_um_banner_at) return NULL;
    if (lv_tick_get() - s_um_banner_at >= UM_BANNER_MS) { s_um_banner_at = 0; return NULL; }
    return s_um_banner;
}

/* Every refresh with fresh data (refresh_cb). */
static void usbmode_tick(const devui_data_t *d)
{
    uint32_t now = lv_tick_get();
    int phone = !strcmp(d->usb_power_role, "source") && !strcmp(d->usb_data_role, "host");
    ui_usb_ev_t ev = ui_usb_track(&s_um_track, d->usb_cc, phone, now);

    if (d->usb_cc >= 0) s_um_attached = d->usb_cc == 1;
    s_um_cur = !s_um_attached ? -1 : d->powerbank == 1 ? 1
             : !strcmp(d->usb_power_role, "source") && !strcmp(d->usb_data_role, "device") ? 0 : -1;
    {
        static char c_now[48] = "";
        set_label_fmt(s_um_now, c_now, sizeof c_now, "%s", usbmode_now_text(d));
    }

    if (ev == UI_USB_EV_PLUGGED) {
        s_um_pend = -1;
        if (s_cf_usb == UI_USB_ASK) usbmode_open();
        else s_um_auto_at = um_stamp(now + UM_GRACE_MS);
    } else if (ev == UI_USB_EV_UNPLUGGED) {
        s_um_pend = -1;
        s_um_auto_at = 0;
        if (usbmode_visible()) usbmode_close();
    }
    if (s_um_auto_at && (int32_t)(now - s_um_auto_at) >= 0) um_auto_run();

    if (s_um_pend >= 0) {
        data_notice_t note;
        if (s_um_pend == s_um_cur) {
            s_um_pend = -1;
            if (usbmode_visible()) { um_status(TR("已生效"), T->okT); s_um_done_at = um_stamp(now); }
        } else if (data_control_notice(&note, 4000) && !strcmp(note.action, "usb.attach_mode") &&
                   (int32_t)(note.at - s_um_pend_at) >= 0) {
            s_um_pend = -1;
            if (usbmode_visible()) um_status(TR("没切成 · 可以再试一次"), T->badT);
            else if (s_um_pend_auto) {
                snprintf(s_um_banner, sizeof s_um_banner, "%s", TR("没按记住的方式切成 · 插着网口转接头？"));
                s_um_banner_at = um_stamp(now);
            }
        } else if (now - s_um_pend_at >= UM_PEND_MS) {
            s_um_pend = -1;
            if (usbmode_visible()) um_status(TR("没有生效 · 可以再试一次"), T->badT);
        }
        if (usbmode_visible()) um_paint();
    }
    if (s_um_done_at && now - s_um_done_at >= UM_DONE_MS) {
        s_um_done_at = 0;
        if (usbmode_visible()) usbmode_close();
    }
    if (usbmode_visible()) um_paint();
}

static void build_usbmode(void)
{
    int w = UK_W - 16, y;
    uk_sheet(&s_um_sheet, 300 + 16, um_cancel_cb);   /* 16 = room under 确定, as at the top */
    lv_obj_t *p = s_um_sheet.panel;
    lv_obj_t *t = uk_label(p, UF.cj15, T->t1, 0, 0, TR("插入的设备怎么用"));
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 12);
    y = 40;
    for (int i = 0; i < UM_ROWS; i++, y += UM_ROW_H) {
        lv_obj_t *r = s_um_row[i] = uk_box(p, 0, y, w, UM_ROW_H, T->glass, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        uk_box(p, 12, y, w - 24, 1, T->sep, 0);
        s_um_mark[i] = uk_box(r, UK_PAD, (UM_ROW_H - 16) / 2, 16, 16, T->card, 8);
        lv_obj_set_style_border_width(s_um_mark[i], 2, 0);
        uk_label(r, UF.cj15, T->t1, UK_PAD + 26, 7, TR(k_um_name[i]));
        uk_label_w(r, UF.cj12, T->t3, UK_PAD + 26, 30, w - UK_PAD * 2 - 26, 0, TR(k_um_sub[i]));
        uk_tappable(r, um_row_cb, (void *)(intptr_t)i);
    }
    uk_box(p, 12, y, w - 24, 1, T->sep, 0);
    uk_label(p, UF.cj14, T->t2, UK_PAD, y + 11, TR("记住选择，以后自动这样做"));
    s_um_rem_sw = uk_toggle(p, w - UK_PAD, y + 7, NULL, NULL);
    y += UK_ROW_H;
    s_um_status = uk_label_w(p, UF.cj12, T->t3, UK_PAD, y + 4, w - 2 * UK_PAD, 0, "");
    y += 24;
    s_um_ok = uk_button(p, UK_PAD, y, w - 2 * UK_PAD, 40, TR("确定"), UK_BTN_PLAIN, um_ok_cb, NULL, &s_um_ok_lbl);
    um_paint();
}

/* ---- 系统页「插入手机等设备时」 ---- */
static void usbmode_pref_paint(void)
{
    for (int i = 0; i < 4; i++) {
        if (!s_um_pref_mark[i]) return;
        int on = (int)s_cf_usb == i;
        uk_bg(s_um_pref_mark[i], T->fillBlue);
        lv_obj_set_style_bg_opa(s_um_pref_mark[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(s_um_pref_mark[i], lv_color_hex(on ? T->fillBlue : T->t3), 0);
    }
}

static void um_pref_cb(lv_event_t *e)
{
    ui_usb_pref_t p = (ui_usb_pref_t)(intptr_t)lv_event_get_user_data(e);
    s_um_auto_at = 0;               /* a pending automatic run follows the old choice: drop it */
    if (p != s_cf_usb) {
        s_cf_usb = p;
        save_devui_conf();
    }
    usbmode_pref_paint();
}

static void um_now_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!s_um_attached) {
        snprintf(s_um_banner, sizeof s_um_banner, "%s", TR("没插设备 · 插上后可以在这里改用法"));
        s_um_banner_at = um_stamp(lv_tick_get());
        banner_set(s_um_banner);
        return;
    }
    usbmode_open();
}

/* Card on the 系统 page at y; returns its height. */
static int build_usbmode_card(lv_obj_t *t, int y)
{
    static const char *const k_pref[4] = { N_("每次询问"), N_("充电 + 上网"), N_("快速充电宝"), N_("网口配件") };
    lv_obj_t *c = uk_card(t, UK_MARGIN, y, UK_CARD_W, 5 * UK_ROW_H);
    s_um_now = uk_row_nav(c, 0, TR("现在"), 1, um_now_cb, NULL);
    lv_obj_set_style_text_font(s_um_now, UF.cj14, 0);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *r = uk_box(c, 0, (i + 1) * UK_ROW_H, UK_CARD_W, UK_ROW_H, T->card, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        uk_sep(r, 0);
        s_um_pref_mark[i] = uk_box(r, UK_PAD, (UK_ROW_H - 16) / 2, 16, 16, T->card, 8);
        lv_obj_set_style_border_width(s_um_pref_mark[i], 2, 0);
        uk_label(r, UF.cj14, T->t1, UK_PAD + 26, 11, TR(k_pref[i]));
        uk_tappable(r, um_pref_cb, (void *)(intptr_t)i);
    }
    usbmode_pref_paint();
    return 5 * UK_ROW_H;
}
