/*
 * ui_parts/power.c - 电源键菜单.
 * Part of src/ui.c, which #includes it: the parts share ui.c's static state
 * and are one translation unit, not compiled on their own (the Makefile
 * lists only ui.c). Split 2026-09-26 so the 6000-line file stops being
 * the place every change collides; the built binary is byte-identical.
 *
 * SPDX-License-Identifier: MIT
 */
/* ---- power menu (long press on the power key): a bottom sheet ---- */
static uk_sheet_t s_pw_sheet;
static void power_menu_set(int v)
{
    uk_sheet_show(&s_pw_sheet, v);
    update_tabs();
}
static int power_menu_visible(void) { return uk_sheet_visible(&s_pw_sheet); }
static int any_sheet_open(void)
{
    return power_menu_visible();
}

/* 关机/重启: first tap arms the row (5 s), second tap runs it. Then a full
 * screen "正在关机…" is drawn before anything else happens: shutdown takes
 * 5–15 s and a menu that just sat there read as "nothing happened" (9-26
 * owner report). The vendor path is what the stock UI calls
 * (libzte_SDKowrt ZTD_RebootDevice): zte_topsw_mc tells data/nwinfo/wlan/
 * mdm to wind down, then sys_shutdown/sys_reboot. Those end in a bare
 * reboot(2) with no sync, so sync first. If the call is refused, or nothing
 * has happened after 40 s, fall back to busybox (procd runs the K scripts). */
static lv_obj_t *s_pw_lbl[2];
static lv_obj_t *s_pw_cover, *s_pw_cover_lbl;
static uint32_t s_pw_arm_ms;
static int s_pw_arm = -1;          /* 0 = 关机 armed, 1 = 重启 armed */
static int s_pw_going = -1;
static uint32_t s_pw_go_ms;
static const char *const k_pw_name[2] = { N_("关机"), N_("重启") };

int ui_powering_down(void) { return s_pw_going >= 0; }

static void power_menu_disarm(void)
{
    if (s_pw_arm < 0) return;
    lv_label_set_text(s_pw_lbl[s_pw_arm], TR(k_pw_name[s_pw_arm]));
    lv_obj_set_style_text_color(s_pw_lbl[s_pw_arm], lv_color_hex(s_pw_arm ? T->accT : T->badT), 0);
    s_pw_arm = -1;
}

static void power_run(int which)
{
    s_pw_going = which;
    s_pw_go_ms = lv_tick_get();
    power_menu_set(0);
    lv_label_set_text(s_pw_cover_lbl, which ? TR("正在重启…") : TR("正在关机…"));
    lv_obj_clear_flag(s_pw_cover, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_pw_cover);
    backlight_on();
    lv_refr_now(NULL);
    /* Through datad (E4): it writes 「重启/关机 requested」 into the change log
     * and waits for it to reach flash first. datad not there: the vendor
     * call directly, as before. Either way the 40 s last resort below stays. */
    if (data_control(which ? "device.reboot" : "device.poweroff", "{}", NULL)) return;
    char cmd[256];
    snprintf(cmd, sizeof cmd,
             "( sync; ubus -t 10 call zwrt_mc.device.manager %s '{\"moduleName\":\"zte_topsw_devui\"}'"
             " >/dev/null 2>&1 || %s ) </dev/null >/dev/null 2>&1 &",
             which ? "device_reboot" : "device_poweroff", which ? "reboot" : "poweroff");
    if (system(cmd) != 0) system(which ? "reboot" : "poweroff");
}

static void act_power(lv_event_t *e)
{
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    uint32_t now = lv_tick_get();
    if (s_pw_going >= 0) return;
    if (s_pw_arm == which && now - s_pw_arm_ms < 5000) { power_run(which); return; }
    power_menu_disarm();
    s_pw_arm = which;
    s_pw_arm_ms = now;
    lv_label_set_text(s_pw_lbl[which], which ? TR("再按一次重启") : TR("再按一次关机"));
    lv_obj_set_style_text_color(s_pw_lbl[which], lv_color_hex(T->warnT), 0);
}
static void act_cancel(lv_event_t *e)   { LV_UNUSED(e); power_menu_disarm(); power_menu_set(0); }

/* Called from key_poll_cb: expire the arm, and if a shutdown has not
 * happened after 40 s say so instead of leaving "正在关机…" up forever. */
static void power_menu_tick(uint32_t now)
{
    if (s_pw_arm >= 0 && now - s_pw_arm_ms >= 5000) power_menu_disarm();
    if (s_pw_going >= 0 && now - s_pw_go_ms >= 40000) {
        if (now - s_pw_go_ms < 41000) {   /* once: last resort */
            system(s_pw_going ? "reboot" : "poweroff");
            lv_label_set_text(s_pw_cover_lbl, s_pw_going ? TR("重启没成功，再试一次…") : TR("关机没成功，再试一次…"));
        } else if (now - s_pw_go_ms >= 80000) {
            lv_obj_add_flag(s_pw_cover, LV_OBJ_FLAG_HIDDEN);
            s_pw_going = -1;
        }
    }
}

static void build_power_menu(void)
{
    uk_sheet(&s_pw_sheet, 120, act_cancel);
    s_power_menu = s_pw_sheet.scrim;
    lv_obj_t *t = uk_label(s_pw_sheet.panel, UF.cj13, T->t3, 0, 0, TR("电源"));
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 12);
    s_pw_lbl[0] = uk_sheet_item(&s_pw_sheet, 38, TR("关机"), T->badT, act_power, (void *)(intptr_t)0, NULL);
    s_pw_lbl[1] = uk_sheet_item(&s_pw_sheet, 78, TR("重启"), T->accT, act_power, (void *)(intptr_t)1, NULL);

    s_pw_cover = uk_box(lv_layer_top(), 0, 0, UK_W, UK_H, T->bg, 0);
    lv_obj_add_flag(s_pw_cover, LV_OBJ_FLAG_CLICKABLE);   /* swallow taps */
    s_pw_cover_lbl = uk_label(s_pw_cover, UF.cj15, T->t1, 0, 0, "");
    lv_obj_center(s_pw_cover_lbl);
    lv_obj_add_flag(s_pw_cover, LV_OBJ_FLAG_HIDDEN);
}

static void key_poll_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    int ev = key_input_poll(&s_key, lv_tick_get());
    if (ev != KEY_EV_NONE) {
        /* The key is not an LVGL input device, so a press does not reset the
         * inactivity timer. Without this, waking an auto-slept screen with the
         * key turned it on and the auto-off below turned it straight back off
         * in the same pass — a one-frame flash (2026-09-23, owner report). */
        lv_display_trigger_activity(NULL);
    }
    power_menu_tick(lv_tick_get());
    if (s_pw_going >= 0) return;   /* shutting down: keep "正在关机…" lit */
    if (ev == KEY_EV_SHORT) {
        /* Decide from the real brightness, not our remembered state: the vendor
         * key daemon (zte_topsw_key) also sees the power key, and if anything
         * else changed the backlight our flag would make this toggle the wrong
         * way and the press would seem to do nothing. */
        if (backlight_is_lit()) {
            backlight_off();
            if (power_menu_visible()) { power_menu_disarm(); power_menu_set(0); }
        }
        else { backlight_on(); ui_tgate_woke(&s_tgate, lv_tick_get()); }
        s_auto_slept = 0;
    } else if (ev == KEY_EV_LONG) {
        if (!backlight_is_lit()) ui_tgate_woke(&s_tgate, lv_tick_get());
        backlight_on();
        s_auto_slept = 0;
        power_menu_disarm();
        power_menu_set(!power_menu_visible());
    }

    /* Auto screen-off after inactivity. Waking is the key, or a double tap
     * (ui_touch_filter); touches on a dark screen never reach the UI, so they
     * do not count as activity here either. Not while the power menu is up:
     * a menu that went dark under the finger read as a dead menu. */
    /* 摆放模式 is watched while the device is moved: no auto screen-off while it
     * is up (idle time keeps counting, so leaving it lets the timer act again) */
    if (s_autooff_ms && !power_menu_visible() && s_sub_cur != SUB_PLACE) {
        uint32_t idle = lv_display_get_inactive_time(NULL);
        /* Exec'd with the screen off (theme switch): inactivity restarted at
         * zero with the new process, which would read as "just touched" and
         * wake the screen. Wait for a touch that happened after the start. */
        if (s_wake_guard && idle + 50 < lv_tick_elaps(s_wake_idle_last)) s_wake_guard = 0;
        if (s_wake_guard) return;
        if (idle > s_autooff_ms) {
            if (backlight_is_on()) { backlight_off(); s_auto_slept = 1; }
        } else if (s_auto_slept) {   /* activity with the screen dark: only the key gets here */
            backlight_on();
            ui_tgate_woke(&s_tgate, lv_tick_get());
            s_auto_slept = 0;
        }
    }
}

