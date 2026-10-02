/*
 * ui_parts/apn.c - APN 子页.
 * Part of src/ui.c, which #includes it: the parts share ui.c's static state
 * and are one translation unit, not compiled on their own (the Makefile
 * lists only ui.c). Split 2026-09-26 so the 6000-line file stops being
 * the place every change collides; the built binary is byte-identical.
 *
 * SPDX-License-Identifier: MIT
 */
/* ---- APN（蜂窝 → APN，2026-09-25）----
 * 看得到数据连接正在拨哪条 APN；在「自动」和已存的手动 APN 之间切（两下确认，
 * 切换中直到读回来）。新建、修改要打字，在管理网页做。 */
#define APN_ROWS    (1 + NI_MAX_APNS)
#define APN_ROW_H   UK_ROW2_H
#define APN_NOTE_H  44
#define APN_PEND_MS 45000
static lv_obj_t *s_apn_now, *s_apn_now_sub, *s_apn_sec, *s_apn_card, *s_apn_note, *s_apn_foot, *s_apn_scroll;
static lv_obj_t *s_apn_row[APN_ROWS], *s_apn_mark[APN_ROWS], *s_apn_name[APN_ROWS], *s_apn_sub[APN_ROWS], *s_apn_tag[APN_ROWS];
static uint32_t  s_apn_arm;
static int       s_apn_arm_idx = -1;
static int       s_apn_pend = -1;             /* 0 = 自动，1.. = apns[i-1] */
static uint32_t  s_apn_pend_at;
static char      s_apn_pend_id[24], s_apn_pend_name[48];
static net_flash_t s_apn_flash;

static const char *apn_pdp(int pdp) { return pdp == 1 ? "IPv4" : pdp == 2 ? "IPv6" : pdp == 3 ? "IPv4v6" : ""; }

/* 这一行是不是现在生效的选择（自动模式 = 第 0 行；手动 = 手动模式选中的那条） */
static int apn_row_current(const netinfo_t *n, int i)
{
    if (!n->apn_known) return 0;
    if (i == 0) return !n->apn_manual;
    return i <= n->napns && n->apn_manual && n->apns[i - 1].selected;
}

static void apn_paint(int changed);

static void apn_row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    const netinfo_t *n = netinfo_get();
    if (!n->apn_known) {
        net_flash(&s_apn_flash, T->t2, 3000, "%s", TR("还没读到 APN，稍等"));
    } else if (s_apn_pend >= 0) {
        net_flash(&s_apn_flash, T->t2, 3000, TR("正在切换到「%s」，稍等"), s_apn_pend_name);
    } else if (apn_row_current(n, i)) {
        s_apn_arm_idx = -1;
        if (i == 0) net_flash(&s_apn_flash, T->t2, 4000, "%s", TR("现在已经是自动选择"));
        else        net_flash(&s_apn_flash, T->t2, 4000, TR("已经在用「%s」"), n->apns[i - 1].name);
    } else if (i > n->napns) {
        return;
    } else if (s_apn_arm_idx == i && net_confirm(s_apn_arm)) {
        s_apn_arm_idx = -1;
        s_apn_pend = i;
        s_apn_pend_at = lv_tick_get();
        snprintf(s_apn_pend_id, sizeof s_apn_pend_id, "%s", i ? n->apns[i - 1].id : "auto");
        snprintf(s_apn_pend_name, sizeof s_apn_pend_name, "%s", i ? n->apns[i - 1].name : TR("自动"));
        s_apn_flash.until = 0;
        apn_paint(1);
        lv_refr_now(NULL);
        netinfo_apn_use(s_apn_pend_id);
        const char *err = netinfo_action_error();
        if (err[0]) {
            s_apn_pend = -1;
            net_flash(&s_apn_flash, T->badT, 8000, TR("没切成：%s"), err);
        } else {
            netinfo_hurry(APN_PEND_MS / 1000);
        }
    } else {
        s_apn_arm_idx = i;
        s_apn_arm = lv_tick_get();
        s_apn_flash.until = 0;
    }
    apn_paint(1);
}

static void build_sub_apn(lv_obj_t *t)
{
    t = s_apn_scroll = uk_scroll(t, 0, UI_SUB_VIEW, 800);
    uk_section(t, 4, TR("正在用"));
    lv_obj_t *c = uk_card(t, UK_MARGIN, 24, UK_CARD_W, UK_HERO_H);
    s_apn_now = uk_label_w(c, UF.cj17b, T->t1, UK_PAD, 12, UK_CARD_W - 2 * UK_PAD, 0, TR("读取中…"));
    s_apn_now_sub = uk_label_w(c, UF.cj13, T->t2, UK_PAD, 42, UK_CARD_W - 2 * UK_PAD, 0, "");
    int y = 24 + UK_HERO_H + 10;
    s_apn_sec = uk_section(t, y, TR("选择"));
    c = s_apn_card = uk_card(t, UK_MARGIN, y + 20, UK_CARD_W, APN_ROW_H + APN_NOTE_H);
    for (int i = 0; i < APN_ROWS; i++) {
        lv_obj_t *r = s_apn_row[i] = uk_box(c, 0, i * APN_ROW_H, UK_CARD_W, APN_ROW_H, T->card, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        if (i) uk_sep(r, 0);
        s_apn_mark[i] = uk_box(r, UK_PAD, (APN_ROW_H - 16) / 2, 16, 16, T->card, 8);
        lv_obj_set_style_border_width(s_apn_mark[i], 2, 0);
        s_apn_name[i] = uk_label_w(r, UF.cj14, T->t1, UK_PAD + 26, 6, 170, 0, "");
        s_apn_sub[i]  = uk_label_w(r, UF.cj12, T->t3, UK_PAD + 26, 27, 170, 0, "");
        s_apn_tag[i]  = uk_label_r(r, UF.cj13, T->t3, UK_CARD_W - UK_PAD, 16, "");
        uk_tappable(r, apn_row_cb, (void *)(intptr_t)i);
        uk_show(r, i == 0);
    }
    s_apn_note = uk_label_w(c, UF.cj12, T->t3, UK_PAD, APN_ROW_H + 8, UK_CARD_W - 2 * UK_PAD, 1, "");
    s_apn_foot = uk_label_w(t, UF.cj12, T->t3, UK_MARGIN + 6, 0, UK_CARD_W - 12, 1,
                            TR("新建或修改 APN 请用管理网页的「APN」页"));
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

static void apn_paint(int changed)
{
    static char c_now[64], c_nsub[96], c_n[APN_ROWS][48], c_s[APN_ROWS][64], c_t[APN_ROWS][32], c_note[160], c_row[64];
    static int painted = -2;
    const netinfo_t *n = netinfo_get();
    int arm = (s_apn_arm_idx >= 0 && net_armed(s_apn_arm)) ? s_apn_arm_idx : -1;
    char buf[160];

    if (s_apn_pend >= 0) {
        int done = s_apn_pend == 0 ? (n->apn_known && !n->apn_manual)
                                   : (n->apn_manual && !strcmp(n->apn_in_use.id, s_apn_pend_id));
        if (done) {
            net_flash(&s_apn_flash, T->okT, 6000, TR("已切换：现在用「%s」"),
                      n->apn_in_use.name[0] ? n->apn_in_use.name : s_apn_pend_name);
            s_apn_pend = -1;
        } else if (n->apn_switch_err[0]) {
            net_flash(&s_apn_flash, T->badT, 10000, TR("没切成：%s"), n->apn_switch_err);
            s_apn_pend = -1;
        } else if (lv_tick_get() - s_apn_pend_at > APN_PEND_MS) {
            net_flash(&s_apn_flash, T->warnT, 10000, "%s", TR("设备没确认这次切换，看一下上面「正在用」再试"));
            s_apn_pend = -1;
        }
    }
    int key = arm + (s_apn_pend >= 0 ? 1000 + s_apn_pend : 0) + (net_flash_on(&s_apn_flash) ? 10000 : 0);
    if (!changed && key == painted) return;
    painted = key;

    /* 蜂窝标签上那一行 */
    if (s_tile_sub[SUB_APN]) {
        if (!n->apn_known) snprintf(buf, sizeof buf, "%s", n->err[0] ? "—" : "");
        else snprintf(buf, sizeof buf, "%s · %s", n->apn_in_use.name[0] ? n->apn_in_use.name : TR("未拨号"),
                      n->apn_manual ? TR("手动") : TR("自动"));
        set_label_fmt(s_tile_sub[SUB_APN], c_row, sizeof c_row, "%s", buf);
    }

    if (!n->apn_known) snprintf(buf, sizeof buf, "%s", n->err[0] ? TR("读不到 APN") : TR("读取中…"));
    else snprintf(buf, sizeof buf, "%s", n->apn_in_use.name[0] ? n->apn_in_use.name : TR("没有拨号"));
    set_label_fmt(s_apn_now, c_now, sizeof c_now, "%s", buf);
    if (!n->apn_known) buf[0] = 0;
    else if (n->apn_in_use.id[0])
        snprintf(buf, sizeof buf, "%s%s%s · %s", n->apn_in_use.apn, n->apn_in_use.pdp ? " · " : "",
                 apn_pdp(n->apn_in_use.pdp), n->apn_manual ? TR("手动指定") : TR("自动选择"));
    else snprintf(buf, sizeof buf, "%s", n->apn_manual ? TR("手动模式") : TR("自动模式"));
    set_label_fmt(s_apn_now_sub, c_nsub, sizeof c_nsub, "%s", buf);

    int rows = 0;
    for (int i = 0; i < APN_ROWS; i++) {
        int show = i == 0 || (n->apn_known && i <= n->napns);
        uk_show(s_apn_row[i], show);
        if (!show) continue;
        const ni_apn_t *a = i ? &n->apns[i - 1] : NULL;
        int sel = apn_row_current(n, i);
        const char *tag = "";
        uint32_t tag_col = T->t3;
        if (i == 0) {
            set_label_fmt(s_apn_name[i], c_n[i], sizeof c_n[i], "%s", TR("自动"));
            set_label_fmt(s_apn_sub[i], c_s[i], sizeof c_s[i], "%s", TR("按 SIM 卡自动选"));
        } else {
            set_label_fmt(s_apn_name[i], c_n[i], sizeof c_n[i], "%s", a->name[0] ? a->name : a->id);
            snprintf(buf, sizeof buf, "%s%s%s", a->apn, a->pdp ? " · " : "", apn_pdp(a->pdp));
            set_label_fmt(s_apn_sub[i], c_s[i], sizeof c_s[i], "%s", buf);
            if (a->in_use) { tag = TR("在用"); tag_col = T->okT; }
        }
        if (i == 0 && sel && n->apn_in_use.id[0]) { tag = TR("在用"); tag_col = T->okT; }
        if (s_apn_pend == i) { tag = TR("切换中…"); tag_col = T->accT; }
        if (arm == i) { tag = TR("再点一次确认"); tag_col = T->warnT; }
        uk_bg(s_apn_row[i], T->washW);
        lv_obj_set_style_bg_opa(s_apn_row[i], arm == i ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        uk_bg(s_apn_mark[i], T->fillBlue);
        lv_obj_set_style_bg_opa(s_apn_mark[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(s_apn_mark[i], lv_color_hex(sel ? T->fillBlue : T->t3), 0);
        lv_obj_set_y(s_apn_row[i], rows * APN_ROW_H);
        rows++;
        set_label_fmt(s_apn_tag[i], c_t[i], sizeof c_t[i], "%s", tag);
        uk_text_color(s_apn_tag[i], tag_col);
    }

    uint32_t col = T->t3;
    if (s_apn_pend >= 0)
        { snprintf(buf, sizeof buf, TR("正在切换到「%s」…数据连接会重拨，断几秒"), s_apn_pend_name); col = T->accT; }
    else if (net_flash_on(&s_apn_flash))
        { snprintf(buf, sizeof buf, "%s", s_apn_flash.txt); col = s_apn_flash.col; }
    else if (arm == 0)
        { snprintf(buf, sizeof buf, "%s", TR("回到自动：按 SIM 卡选 APN，会断网几秒")); col = T->warnT; }
    else if (arm > 0 && arm <= n->napns)
        { snprintf(buf, sizeof buf, TR("改用「%s」：会断网几秒，之后换卡也一直用它，点「自动」才恢复"),
                   n->apns[arm - 1].name); col = T->warnT; }
    else if (!n->apn_known)
        snprintf(buf, sizeof buf, "%s", n->err[0] ? n->err : TR("读取中…"));
    else if (!n->napns)
        snprintf(buf, sizeof buf, "%s", TR("还没有手动 APN。要用自定义 APN，先在管理网页里新建"));
    else if (n->apn_manual)
        snprintf(buf, sizeof buf, "%s", TR("手动：一直用选中的这条，换卡也不变"));
    else
        snprintf(buf, sizeof buf, "%s", TR("点一条手动 APN 可以改用它（两下确认）"));
    set_label_fmt(s_apn_note, c_note, sizeof c_note, "%s", buf);
    uk_text_color(s_apn_note, col);

    int ch = rows * APN_ROW_H + APN_NOTE_H;
    lv_obj_set_height(s_apn_card, ch);
    lv_obj_set_y(s_apn_note, rows * APN_ROW_H + 8);
    int y = 24 + UK_HERO_H + 10 + 20 + ch + 10;
    lv_obj_set_y(s_apn_foot, y);
    uk_scroll_extent(s_apn_scroll, y + 40 + 16);
}

static void net_paint(int changed)
{
    static char c_err[100], c_key[2][24], c_ip[2][48], c_sub[2][160], c_op[4][96], c_st[200];
    static char c_on[NI_MAX_OPS][64], c_od[NI_MAX_OPS][64], c_ot[NI_MAX_OPS][24];
    static char c_ns[96], c_nl[NI_MAX_CELLS][48], c_nr[NI_MAX_CELLS][24], c_sb[24], c_ab[24], c_nb[24];
    const netinfo_t *n = netinfo_get();
    const char *aerr = netinfo_action_error();
    int arm = net_armed(s_net_arm_scan) ? 100 : net_armed(s_net_arm_auto) ? 101 :
              net_armed(s_net_arm_nbr) ? 102 :
              (s_net_arm_sc_idx >= 0 && net_armed(s_net_arm_sc)) ? 200 + s_net_arm_sc_idx :
              (s_net_arm_idx >= 0 && net_armed(s_net_arm_op)) ? s_net_arm_idx : -1;
    int busy = net_busy(n);
    char buf[256];

    apn_paint(changed);

    /* 情景切换有没有真的过去 */
    if (s_sc_pend >= 0) {
        int done = s_sc_pend == 0 ? !n->scene_pin[0]
                                  : !strcmp(n->scene_pin, s_sc_pend_id) && !strcmp(n->scene_current, s_sc_pend_id);
        if (done) {
            if (s_sc_pend == 0)
                net_flash(&s_sc_flash, T->okT, 6000, TR("已恢复自动判断 · 现在是「%s」"),
                          n->scene_current[0] ? net_scene_name(n, n->scene_current) : TR("判定中"));
            else
                net_flash(&s_sc_flash, T->okT, 6000, TR("已切换到「%s」，一直保持到你点「自动」"), s_sc_pend_name);
            s_sc_pend = -1;
        } else if (lv_tick_get() - s_sc_pend_at > SC_PEND_MS) {
            if (s_sc_pend && !strcmp(n->scene_pin, s_sc_pend_id))
                net_flash(&s_sc_flash, T->warnT, 10000, TR("已固定「%s」，但设备还没切过去；Wi-Fi 看门狗可能正在接管，过一会再看"),
                          s_sc_pend_name);
            else
                net_flash(&s_sc_flash, T->warnT, 10000, "%s", TR("设备没确认这次切换，再试一次"));
            s_sc_pend = -1;
        }
    }
    /* 画的依据不只是数据：待确认、切换中、临时提示变了也要重画 */
    int key = arm + (s_sc_pend >= 0 ? 1000 + s_sc_pend : 0) +
              (net_flash_on(&s_sc_flash) ? 10000 : 0) + (net_flash_on(&s_ms_flash) ? 20000 : 0);
    if (!changed && key == s_net_painted_arm) return;
    s_net_painted_arm = key;
    if (arm < 0) { s_net_arm_idx = -1; s_net_arm_sc_idx = -1; }

    set_label_fmt(s_net_err, c_err, sizeof c_err, "%s", n->err);

    /* 情景：自动 + 各情景（可固定） */
    static char c_scn[NET_SCENE_ROWS][48], c_sct[NET_SCENE_ROWS][32], c_scnote[160];
    static char c_scw[NET_SCENE_ROWS][160], c_scd[NET_SCENE_ROWS][160];
    int scene_px = 0;
    {
        const char *cur_name = "";
        int cur_abroad = 0;
        for (int i = 0; i < n->nscenes; i++)
            if (!strcmp(n->scenes[i].id, n->scene_current)) { cur_name = n->scenes[i].name; cur_abroad = n->scenes[i].abroad; }
        (void)cur_abroad;
        int usable = n->scene_known && n->scene_enabled && n->nscenes > 0;
        for (int i = 0; i < NET_SCENE_ROWS; i++) {
            int show = 0;
            const char *name = "", *tag = "", *when = "", *does = "";
            uint32_t tag_col = T->t3;
            char buf2[48];
            if (i == 0) {
                show = 1;
                name = TR("自动");
                if (usable) { when = TR("按下面每个情景的条件自己切换"); does = TR("条件变了，1–2 分钟内跟着换"); }
                if (!usable) tag = "—";
                else if (!n->scene_pin[0]) {
                    snprintf(buf2, sizeof buf2, TR("现在：%s"), cur_name[0] ? cur_name : TR("判定中"));
                    tag = buf2; tag_col = T->accT;
                }
            } else if (i <= n->nscenes && usable) {
                const ni_scene_t *sc = &n->scenes[i - 1];
                show = 1;
                name = sc->name[0] ? sc->name : sc->id;
                when = sc->when;
                does = sc->does;
                if (!strcmp(n->scene_pin, sc->id)) { tag = TR("已固定"); tag_col = T->accT; }
                else if (!strcmp(n->scene_current, sc->id)) { tag = TR("现在"); tag_col = T->accT; }
                else if (sc->wifi_off && !does[0]) tag = TR("会关 Wi-Fi");
            }
            int is_exit = i == NET_SCENE_ROWS - 1;
            int sel = usable && !is_exit &&
                      (i == 0 ? !n->scene_pin[0] : i <= n->nscenes && !strcmp(n->scene_pin, n->scenes[i - 1].id));
            if (s_sc_pend == i) { tag = TR("切换中…"); tag_col = T->accT; }
            if (arm == 200 + i) { tag = TR("再点一次确认"); tag_col = T->warnT; }
            uk_show(s_net_sc_row[i], show);
            if (!show) continue;
            /* 待确认：整行浅黄底（不用彩色左边框，DESIGN.md §6） */
            uk_bg(s_net_sc_row[i], T->washW);
            lv_obj_set_style_bg_opa(s_net_sc_row[i], arm == 200 + i ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            /* 单选圈：实心 = 现在生效的那个（没固定时是「自动」） */
            uk_show(s_net_sc_mark[i], usable && !is_exit);
            uk_bg(s_net_sc_mark[i], T->fillBlue);
            lv_obj_set_style_bg_opa(s_net_sc_mark[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(s_net_sc_mark[i], lv_color_hex(sel ? T->fillBlue : T->t3), 0);
            lv_obj_set_x(s_net_sc_name[i], usable && !is_exit ? UK_PAD + 26 : UK_PAD);
            /* 有说明的行三行高，圈对齐第一行；没说明的（读不到时）一行高 */
            /* 条件那行放不下就折成两行（Wi-Fi 名可以很长），再长末尾「…」 */
            int lh = lv_font_get_line_height(UF.cj12), wl = 1;
            if (when[0]) {
                lv_point_t sz;
                lv_text_get_size(&sz, when, UF.cj12, 0, 0, UK_CARD_W - 2 * UK_PAD - 26, LV_TEXT_FLAG_NONE);
                if (sz.y > lh) wl = 2;
            }
            int rh = !when[0] && !does[0] ? UK_ROW_H : 32 + (when[0] ? wl * lh : 0) + (does[0] ? lh + 2 : 0) + 8;
            lv_obj_set_height(s_net_sc_row[i], rh);
            lv_obj_set_y(s_net_sc_mark[i], rh == UK_ROW_H ? (UK_ROW_H - 16) / 2 : 12);
            lv_obj_set_height(s_net_sc_when[i], wl * lh);
            lv_obj_set_y(s_net_sc_does[i], 32 + wl * lh + 2);
            set_label_fmt(s_net_sc_when[i], c_scw[i], sizeof c_scw[i], "%s", when);
            set_label_fmt(s_net_sc_does[i], c_scd[i], sizeof c_scd[i], "%s", does[0] ? does : "");
            uk_show(s_net_sc_when[i], when[0] != 0);
            uk_show(s_net_sc_does[i], does[0] != 0);
            lv_obj_set_y(s_net_sc_row[i], scene_px);
            scene_px += rh;
            set_label_fmt(s_net_sc_name[i], c_scn[i], sizeof c_scn[i], "%s", name);
            set_label_fmt(s_net_sc_tag[i], c_sct[i], sizeof c_sct[i], "%s", tag);
            uk_text_color(s_net_sc_tag[i], tag_col);
        }
        uint32_t note_col = T->t3;
        if (!n->scene_known)
            snprintf(buf, sizeof buf, "%s", n->err[0] ? TR("读不到情景") : TR("读取中…"));
        else if (!n->scene_enabled)
            snprintf(buf, sizeof buf, "%s", TR("情景引擎已停用，在管理网页的「情景」页打开"));
        else if (!n->nscenes)
            snprintf(buf, sizeof buf, "%s", TR("还没配置情景，在管理网页的「情景」页设置"));
        else if (s_sc_pend > 0)
            { snprintf(buf, sizeof buf, TR("正在切换到「%s」…%s"), s_sc_pend_name,
                       s_sc_pend_wifi_off ? TR("会关掉 Wi-Fi") : TR("开关 Wi-Fi 要 10–30 秒")); note_col = T->accT; }
        else if (s_sc_pend == 0)
            { snprintf(buf, sizeof buf, "%s", TR("正在恢复自动判断…")); note_col = T->accT; }
        else if (net_flash_on(&s_sc_flash))
            { snprintf(buf, sizeof buf, "%s", s_sc_flash.txt); note_col = s_sc_flash.col; }
        else if (n->scene_takeover && n->scene_pin[0])
            { snprintf(buf, sizeof buf, "%s", TR("Wi-Fi 看门狗接管中，固定暂时不生效，先按「外出」")); note_col = T->warnT; }
        else if (arm == 200)
            { snprintf(buf, sizeof buf, "%s", TR("恢复自动：按位置和 SIM 重新判断\n可能会开关 Wi-Fi")); note_col = T->warnT; }
        else if (arm > 200 && arm - 200 <= n->nscenes) {
            const ni_scene_t *sc = &n->scenes[arm - 201];
            snprintf(buf, sizeof buf, TR("切到「%s」：%s\n之后一直固定，点「自动」才恢复"),
                     sc->name[0] ? sc->name : sc->id,
                     sc->wifi_off ? TR("会关掉 Wi-Fi") : sc->abroad ? TR("按国外设置") : TR("Wi-Fi 开着"));
            note_col = T->warnT;
        }
        else if (n->scene_pin[0])
            snprintf(buf, sizeof buf, "%s", TR("手动固定后一直保持（重启也是），点「自动」才恢复自动判断"));
        else
            snprintf(buf, sizeof buf, "%s", TR("情景只管 Wi-Fi 这些，不碰蜂窝网络。点一个情景就固定在它，点「自动」恢复"));
        set_label_fmt(s_net_sc_note, c_scnote, sizeof c_scnote, "%s", buf);
        uk_text_color(s_net_sc_note, note_col);
    }

    /* 出口：蜂窝直连出去的公网 IP，一行（第二行备用，不显示） */
    int two = 0;
    for (int i = 0; i < 1; i++) {
        const ni_exit_t *e = &n->direct;
        const char *key = TR("出口 IP");
        char extra[80] = "";
        snprintf(extra, sizeof extra, "%s", e->isp);
        net_exit_sub(buf, sizeof buf, e, extra);
        set_label_fmt(s_net_ex_key[i], c_key[i], sizeof c_key[i], "%s", key);
        set_label_fmt(s_net_ex_ip[i], c_ip[i], sizeof c_ip[i], "%s",
                      e->ip[0] ? e->ip : (e->present || n->err[0]) ? "—" : TR("查询中…"));
        set_label_fmt(s_net_ex_sub[i], c_sub[i], sizeof c_sub[i], "%s", buf);
    }
    uk_show(s_net_ex_row[1], two);

    /* 运营商 */
    net_oper_text(buf, sizeof buf, &n->home);
    set_label_fmt(s_net_op[0], c_op[0], sizeof c_op[0], "%s", buf);
    net_oper_text(buf, sizeof buf, &n->serving);
    set_label_fmt(s_net_op[1], c_op[1], sizeof c_op[1], "%s", buf);
    set_label_fmt(s_net_op[2], c_op[2], sizeof c_op[2], "%s", n->roaming > 0 ? TR("漫游中") : n->roaming == 0 ? TR("本地") : "—");
    uk_text_color(s_net_op[2], n->roaming > 0 ? T->warnT : T->t1);
    if (!strcmp(n->guard_phase, "registering"))
        snprintf(buf, sizeof buf, TR("正在注册 %s…"), n->guard_target);
    else if (!strcmp(n->guard_phase, "reverting"))
        snprintf(buf, sizeof buf, "%s", TR("正在恢复自动…"));
    else
        snprintf(buf, sizeof buf, "%s", !strcmp(n->selection, "auto") ? TR("自动") : !strcmp(n->selection, "manual") ? TR("手动") : "—");
    set_label_fmt(s_net_op[3], c_op[3], sizeof c_op[3], "%s", buf);

    /* 手动选网：状态一句话，按「正在发生的事」优先 */
    uint32_t st_col = T->t2;
    if (net_flash_on(&s_ms_flash))
        { snprintf(buf, sizeof buf, "%s", s_ms_flash.txt); st_col = s_ms_flash.col; }
    else if (!strcmp(n->guard_phase, "registering"))
        snprintf(buf, sizeof buf, TR("正在注册到 %s。没注册上会自动回到自动选网。"), n->guard_target);
    else if (!strcmp(n->guard_phase, "reverting"))
        snprintf(buf, sizeof buf, "%s", TR("正在回到自动选网…"));
    else if (!strcmp(n->scan_state, "scanning"))
        snprintf(buf, sizeof buf, "%s", TR("正在搜索，数据连接会断 1–3 分钟…"));
    else if (arm == 100)
        { snprintf(buf, sizeof buf, "%s", TR("搜索时会断网 1–3 分钟，再点一次开始。")); st_col = T->warnT; }
    else if (arm == 101)
        { snprintf(buf, sizeof buf, "%s", TR("回到自动选网，可能短暂断网，再点一次确认。")); st_col = T->warnT; }
    else if (arm >= 0 && arm < n->nops)   /* 只有运营商行；情景（200+）、按钮（100+）不是 */
        { snprintf(buf, sizeof buf, !strcmp(n->ops[arm].status, "2")
                       ? TR("再点一次把选网固定在 %s（改成手动）。要回自动点「恢复自动」。")
                       : TR("再点一次注册到 %s。没注册上会自动回到自动选网。"),
                   n->ops[arm].name[0] ? n->ops[arm].name : n->ops[arm].plmn); st_col = T->warnT; }
    else if (aerr[0])
        { snprintf(buf, sizeof buf, "%s", aerr); st_col = T->badT; }
    else if (!strcmp(n->guard_phase, "revert_failed"))
        { snprintf(buf, sizeof buf, TR("%s。重启设备也会回到自动选网。"), n->guard_reason); st_col = T->badT; }
    else if (!strcmp(n->guard_phase, "reverted")) {
        /* the user asked for it: no reason to give. By code (agent reason_code);
         * an agent from before L2 has none and its reason is still the Chinese */
        int asked = n->guard_reason_code[0] ? !strcmp(n->guard_reason_code, "manual_auto")
                                            : !strcmp(n->guard_reason, "手动恢复自动");
        if (asked) snprintf(buf, sizeof buf, "%s", TR("已回到自动选网。"));
        else       snprintf(buf, sizeof buf, TR("%s，已回到自动选网。"), n->guard_reason);
    }
    else if (!strcmp(n->guard_phase, "ok"))
        { snprintf(buf, sizeof buf, TR("已注册到 %s。要回自动选网点「恢复自动」。"), n->guard_target); st_col = T->okT; }
    else if (!strcmp(n->scan_state, "error"))
        { snprintf(buf, sizeof buf, TR("搜索失败：%s"), n->scan_err); st_col = T->badT; }
    else if (!strcmp(n->scan_state, "done"))
        snprintf(buf, sizeof buf, "%s", TR("点一个网络，再点一次确认注册。"));
    else
        snprintf(buf, sizeof buf, "%s", TR("手动指定注册的网络。搜索会断网 1–3 分钟。"));
    /* 一句话放不下时，句号会单独掉到第二行：不要句号（英文也一样，agent 的
     * 原文错误可能带英文句号） */
    size_t bl = strlen(buf);
    if (bl >= 3 && !strcmp(buf + bl - 3, "。")) buf[bl - 3] = 0;
    else if (lang_is_en() && bl >= 1 && buf[bl - 1] == '.') buf[bl - 1] = 0;
    set_label_fmt(s_net_status, c_st, sizeof c_st, "%s", buf);
    uk_text_color(s_net_status, st_col);

    int ops = (!strcmp(n->scan_state, "done") && !busy) ? n->nops : 0;
    for (int i = 0; i < NI_MAX_OPS; i++) {
        const ni_scan_op_t *o = &n->ops[i];
        if (i >= ops) { uk_show(s_net_opr[i], 0); continue; }
        uk_show(s_net_opr[i], 1);
        set_label_fmt(s_net_opr_name[i], c_on[i], sizeof c_on[i], "%s", o->name[0] ? o->name : o->plmn);
        snprintf(buf, sizeof buf, "%s · %s%s%s", o->plmn, net_rat_name(o->rat),
                 o->country[0] ? " · " : "", o->country);
        set_label_fmt(s_net_opr_det[i], c_od[i], sizeof c_od[i], "%s", buf);
        const char *tag = arm == i ? TR("再点一次确认") : !strcmp(o->status, "2") ? TR("当前") : !strcmp(o->status, "3") ? TR("禁止") : "";
        uk_bg(s_net_opr[i], T->washW);
        lv_obj_set_style_bg_opa(s_net_opr[i], arm == i ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        set_label_fmt(s_net_opr_tag[i], c_ot[i], sizeof c_ot[i], "%s", tag);
        uk_text_color(s_net_opr_tag[i], arm == i ? T->warnT : T->t3);
        uk_text_color(s_net_opr_name[i], !strcmp(o->status, "3") ? T->t3 : T->t1);
    }
    set_label_fmt(s_net_scan_lbl, c_sb, sizeof c_sb, "%s", arm == 100 ? TR("再点开始") : TR("搜索网络"));
    uk_button_kind(s_net_scan_btn, s_net_scan_lbl, arm == 100 ? UK_BTN_ARMED : UK_BTN_PLAIN);
    set_label_fmt(s_net_auto_lbl, c_ab, sizeof c_ab, "%s", arm == 101 ? TR("再点确认") : TR("恢复自动"));
    uk_button_kind(s_net_auto_btn, s_net_auto_lbl, arm == 101 ? UK_BTN_ARMED : UK_BTN_PLAIN);

    /* 邻小区。原厂扫描会断网且拿不到数据（2026-09-25 实测），agent 报
     * unsupported：不给按钮，写明原因 */
    int nbr_off = !strcmp(n->nbr_state, "unsupported");
    uk_show(s_net_nbr_btn, !nbr_off);
    lv_obj_set_width(s_net_nbr_state, UK_CARD_W - 2 * UK_PAD - (nbr_off ? 0 : 80));
    if (nbr_off)
        snprintf(buf, sizeof buf, "%s", n->nbr_err[0] ? n->nbr_err : TR("本机暂时读不到邻区"));
    else if (!strcmp(n->nbr_state, "scanning"))
        snprintf(buf, sizeof buf, "%s", TR("扫描中…"));
    else if (arm == 102)
        snprintf(buf, sizeof buf, "%s", TR("可能短暂影响网速"));
    else if (!strcmp(n->nbr_state, "error"))
        snprintf(buf, sizeof buf, "%s", TR("扫描失败"));
    else if (n->nbr_at > 0) {
        time_t tt = (time_t)n->nbr_at;   /* 设备时钟 = 当地时间标成 UTC，localtime 给出对的数字 */
        struct tm tm;
        char hm[8];
        localtime_r(&tt, &tm);
        strftime(hm, sizeof hm, "%H:%M", &tm);
        snprintf(buf, sizeof buf, TR("%s 扫描 · %d 个"), hm, n->ncells);
    } else
        snprintf(buf, sizeof buf, "%s", TR("还没扫过，点「扫描」读一次"));
    set_label_fmt(s_net_nbr_state, c_ns, sizeof c_ns, "%s", buf);
    uk_text_color(s_net_nbr_state, arm == 102 ? T->warnT : T->t2);
    set_label_fmt(s_net_nbr_lbl, c_nb, sizeof c_nb, "%s", arm == 102 ? TR("再点开始") : TR("扫描"));
    uk_button_kind(s_net_nbr_btn, s_net_nbr_lbl, arm == 102 ? UK_BTN_ARMED : UK_BTN_PLAIN);
    for (int i = 0; i < NI_MAX_CELLS; i++) {
        const ni_cell_t *cl = &n->cells[i];
        if (i >= n->ncells) { uk_show(s_net_nbr_row[i], 0); continue; }
        uk_show(s_net_nbr_row[i], 1);
        set_label_fmt(s_net_nbr_l[i], c_nl[i], sizeof c_nl[i], "%-3s PCI %s  %s", cl->rat, cl->pci, cl->arfcn);
        set_label_fmt(s_net_nbr_r[i], c_nr[i], sizeof c_nr[i], "%s%s", cl->rsrp[0] ? cl->rsrp : "-", cl->rsrp[0] ? " dBm" : "");
    }

    /* 已连接设备流量 */
    {
        static char c_cls[96], c_cn[NI_MAX_CLIENTS][48], c_ct[NI_MAX_CLIENTS][40], c_cs[NI_MAX_CLIENTS][96];
        int k = n->nclients;
        s_net_ncl = k;
        uk_show(s_net_cl_state, k == 0);
        set_label_fmt(s_net_cl_state, c_cls, sizeof c_cls, "%s",
                      !n->clients_known ? TR("读取中…") : TR("现在没有 Wi-Fi 设备连着（网线和 USB 连的设备不在这里）"));
        for (int i = 0; i < NI_MAX_CLIENTS; i++) {
            const ni_client_t *cl = &n->clients[i];
            char a[16], b[16], ra[16], rb[16];
            if (i >= k) { uk_show(s_net_cl_row[i], 0); continue; }
            uk_show(s_net_cl_row[i], 1);
            set_label_fmt(s_net_cl_name[i], c_cn[i], sizeof c_cn[i], "%s", cl->name[0] ? cl->name : cl->ip[0] ? cl->ip : cl->mac);
            fmt_bytes_total(a, sizeof a, (long)cl->down);
            fmt_bytes_total(b, sizeof b, (long)cl->up);
            set_label_fmt(s_net_cl_tot[i], c_ct[i], sizeof c_ct[i], "↓%s ↑%s", a, b);
            if (cl->down_rate >= 0) fmt_rate_top(ra, sizeof ra, cl->down_rate, s_cf_speed_bits, 0); else snprintf(ra, sizeof ra, "-");
            if (cl->up_rate >= 0)   fmt_rate_top(rb, sizeof rb, cl->up_rate, s_cf_speed_bits, 0);   else snprintf(rb, sizeof rb, "-");
            /* 「5 GHz · ↓12K/s ↑3K/s · 信号很好」：频段在前（2.4 还是 5，2026-09-25 用户要的），
             * 信号说成话；Wi-Fi 几代和协商速率放不下，管理网页的已连设备页有 */
            char band[16] = "", sig[48] = "";
            if (cl->band[0]) snprintf(band, sizeof band, "%s · ", cl->band);
            if (wifi_sig_word(cl->signal_tier))
                snprintf(sig, sizeof sig, " · %s", wifi_sig_word(cl->signal_tier));
            if (cl->down_rate < 0 && cl->up_rate < 0)
                set_label_fmt(s_net_cl_sub[i], c_cs[i], sizeof c_cs[i], TR("%s速率稍后显示%s"), band, sig);
            else
                set_label_fmt(s_net_cl_sub[i], c_cs[i], sizeof c_cs[i], "%s↓%s/s ↑%s/s%s", band, ra, rb, sig);
        }
    }

    net_reflow(n->err[0] ? 22 : 0, scene_px, two ? 2 : 1, ops, n->ncells);
}

static void tile_click_cb(lv_event_t *e)
{
    sub_open((int)(intptr_t)lv_event_get_user_data(e));
}


/* 标签页上的一张「›」行卡片：每行开一个二级页，右边是 refresh_cb 写的状态字。 */
static lv_obj_t *s_nav_sec, *s_nav_card;   /* 最近一张（出口标签要跟着出口 IP 挪） */
static int nav_card(lv_obj_t *t, int y, const char *section, const int *ids, const char *const *names, int n)
{
    lv_obj_t *c;
    /* section and names are N_() literals: shown through TR here */
    s_nav_sec = uk_section(t, y, TR(section));
    s_nav_card = c = uk_card(t, UK_MARGIN, y + 20, UK_CARD_W, n * UK_ROW_H);
    for (int k = 0; k < n; k++)
        s_tile_sub[ids[k]] = uk_row_nav(c, k * UK_ROW_H, TR(names[k]), k == 0, tile_click_cb, (void *)(intptr_t)ids[k]);
    return y + 20 + n * UK_ROW_H + 10;
}

/* 蜂窝：跟这张卡、这个运营商有关的都在这里。网络模式直接在标签上切（两下确认）。 */
