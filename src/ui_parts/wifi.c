/*
 * ui_parts/wifi.c - Wi-Fi 子页.
 * Part of src/ui.c, which #includes it: the parts share ui.c's static state
 * and are one translation unit, not compiled on their own (the Makefile
 * lists only ui.c). Split 2026-09-26 so the 6000-line file stops being
 * the place every change collides; the built binary is byte-identical.
 *
 * SPDX-License-Identifier: MIT
 */
/* ---- WiFi subpage ----
 * Content mirrors ui/subpages/wifi.html: credentials, the radio switches,
 * the client list, DHCP. The QR code that used to be here is gone — the
 * backend never exposes the passphrase, so it could only ever encode a
 * "join this open network" code that fails to authenticate, and nobody
 * asked for it. */
enum { WSW_MASTER, WSW_24, WSW_5, WSW_PSM, WSW_NFC };

static void wifi_sw_cb(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    int on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    const char *ic = on ? "up" : "down";
    char cmd[720];

    switch (id) {
    case WSW_MASTER:
        snprintf(cmd, sizeof cmd, "(ifconfig wlan0 %s; ifconfig wlan2 %s) >/dev/null 2>&1 &", ic, ic);
        s_aux_w24 = s_aux_w5 = on;
        break;
    case WSW_24:
        snprintf(cmd, sizeof cmd, "ifconfig wlan0 %s >/dev/null 2>&1 &", ic);
        s_aux_w24 = on;
        break;
    case WSW_5:
        snprintf(cmd, sizeof cmd, "ifconfig wlan2 %s >/dev/null 2>&1 &", ic);
        s_aux_w5 = on;
        break;
    case WSW_PSM: {
        /* Verbatim from htmlmain.c's act:psm — the switch owns
         * /etc/hotplug.d/iface/99-disable-powersave so the choice survives
         * an ifup, and applies it to the live interfaces now. */
        const char *m = on ? "on" : "off";
        snprintf(cmd, sizeof cmd,
            "(mkdir -p /etc/hotplug.d/iface; rm -f /etc/hotplug.d/iface/psm; "
            "{ echo '#!/bin/sh'; echo '[ \"$ACTION\" = ifup ] && {'; "
            "echo '  iw dev wlan0 set power_save %s 2>/dev/null'; "
            "echo '  iw dev wlan1 set power_save %s 2>/dev/null'; "
            "echo '  iw dev wlan2 set power_save %s 2>/dev/null'; "
            "echo '  iw dev wlan3 set power_save %s 2>/dev/null'; echo '}'; } "
            "> /etc/hotplug.d/iface/99-disable-powersave; "
            "chmod +x /etc/hotplug.d/iface/99-disable-powersave; "
            "for w in wlan0 wlan1 wlan2 wlan3; do iw dev $w set power_save %s 2>/dev/null; done) "
            ">/dev/null 2>&1 &", m, m, m, m, m);
        s_aux_psm = on;
        break;
    }
    case WSW_NFC: {
        char params[48];
        snprintf(cmd, sizeof cmd,
            "ubus call zwrt_nfc zwrt_nfc_wifi_set '{\"switch\":%d,\"flag\":2}' >/dev/null 2>&1 &", on);
        snprintf(params, sizeof params, "{\"enabled\":%d,\"flag\":2}", on);
        data_control("nfc.set", params, cmd);
        return;
    }
    default:
        return;
    }
    system(cmd);
}

/* A card row with a label, a state word and a switch (e.g. Wi-Fi). */
static lv_obj_t *toggle_row(lv_obj_t *c, int y, const char *name, int first, lv_obj_t **state,
                            lv_event_cb_t cb, void *user)
{
    if (!first) uk_sep(c, y);
    uk_label(c, UF.cj14, T->t1, UK_PAD, y + 11, name);
    if (state) *state = uk_label_r(c, UF.cj12, T->t3, UK_CARD_W - UK_PAD - 52, y + 13, "");
    return uk_toggle(c, UK_CARD_W - UK_PAD, y + 7, cb, user);
}

#define WIFI_CLI_H 50
static void build_wifi(lv_obj_t *t)
{
    int y = 4;
    t = s_w_scroll = uk_scroll(t, 0, UI_VIEW_H, 1000);

    uk_section(t, y, TR("热点")); y += 20;
    lv_obj_t *ap = uk_card(t, UK_MARGIN, y, UK_CARD_W, 3 * UK_ROW_H);
    s_w_ssid = uk_label_w(ap, UF.cj17b, T->t1, UK_PAD, 10, 200, 0, "");
    s_w_state = uk_label_r(ap, UF.cj13, T->okT, UK_CARD_W - UK_PAD, 12, "");
    s_w_enc = uk_row(ap, UK_ROW_H, TR("加密"), 0);
    s_w_pass = uk_row(ap, 2 * UK_ROW_H, TR("密码"), 0);
    y += 3 * UK_ROW_H + 10;

    uk_section(t, y, TR("开关")); y += 20;
    lv_obj_t *sws = uk_card(t, UK_MARGIN, y, UK_CARD_W, 5 * UK_ROW_H);
    static const char *const k_sw_name[5] = { N_("WiFi 总开关"), N_("2.4G 频段"), N_("5G 频段"), N_("节能模式"), N_("NFC 碰一碰") };
    for (int i = 0; i < 5; i++)
        s_w_sw[i] = toggle_row(sws, i * UK_ROW_H, TR(k_sw_name[i]), i == 0, &s_w_sw_st[i], wifi_sw_cb, (void *)(intptr_t)i);
    y += 5 * UK_ROW_H + 10;

    s_w_cli_sec = uk_section(t, y, TR("已连接设备"));
    s_w_cli_n = uk_label_r(t, UF.cj12, T->t3, UK_W - UK_MARGIN - 6, y, "");
    y += 20;
    s_w_cli_card = uk_card(t, UK_MARGIN, y, UK_CARD_W, WIFI_MAX_CLI * WIFI_CLI_H);
    s_w_cli_empty = uk_label(s_w_cli_card, UF.cj14, T->t3, UK_PAD, 11, TR("还没有设备连上"));
    for (int i = 0; i < WIFI_MAX_CLI; i++) {
        int ry = i * WIFI_CLI_H;
        s_w_cli[i] = uk_box(s_w_cli_card, 0, ry, UK_CARD_W, WIFI_CLI_H, T->card, 0);
        lv_obj_set_style_bg_opa(s_w_cli[i], LV_OPA_TRANSP, 0);
        s_w_cli_sep[i] = i ? uk_sep(s_w_cli[i], 0) : NULL;
        s_w_cli_name[i] = uk_label_w(s_w_cli[i], UF.cj14, T->t1, UK_PAD, 8, 160, 0, "");
        s_w_cli_mac[i] = uk_label_w(s_w_cli[i], UF.cj12, T->t3, UK_PAD, 29, UK_CARD_W - 2 * UK_PAD, 0, "");
        s_w_cli_ip[i] = uk_label_r(s_w_cli[i], UF.n15, T->t1, UK_CARD_W - UK_PAD, 7, "");
        s_w_cli_tot[i] = uk_label(s_w_cli[i], UF.cj12, T->t3, UK_PAD, 47, "");
        uk_show(s_w_cli[i], 0);
    }
    y += WIFI_MAX_CLI * WIFI_CLI_H + 10;

    s_w_dhcp_sec = uk_section(t, y, "DHCP"); y += 20;
    s_w_dhcp_card = uk_card(t, UK_MARGIN, y, UK_CARD_W, 3 * UK_ROW_H);
    s_w_gw = uk_row(s_w_dhcp_card, 0, TR("网关"), 1);
    s_w_pool = uk_row(s_w_dhcp_card, UK_ROW_H, TR("地址池"), 0);
    s_w_lease = uk_row(s_w_dhcp_card, 2 * UK_ROW_H, TR("租期"), 0);
    s_nh_scroll[NH_WIFI] = t;
    s_nh_base[NH_WIFI] = y + 3 * UK_ROW_H + 10;   /* 设备流量（build_sub_net）从这里起 */
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

