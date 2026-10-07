/*
 * ui_parts/network.c - 功能磁贴墙、网络子页.
 * Part of src/ui.c, which #includes it: the parts share ui.c's static state
 * and are one translation unit, not compiled on their own (the Makefile
 * lists only ui.c). Split 2026-09-26 so the 6000-line file stops being
 * the place every change collides; the built binary is byte-identical.
 *
 * SPDX-License-Identifier: MIT
 */
/* ---- 功能 tile wall ---- */
/* ---- 网络 subpage ----
 * 出口 IP 和归属地、原始/注册运营商、漫游、手动选网、邻小区。数据全来自
 * zte-agent 的 /api/netinfo（netinfo.c）；慢的事都在 agent 的后台线程里，
 * 这里只发起动作再轮询。碰模组的动作（搜网、注册、恢复自动）和换情景都是
 * 两步：点一下整行 / 按钮变色并写出后果，4 秒内再点才发。
 *
 * 每一次点击都要当场看得见（2026-09-25 用户：点了毫无反应）：
 * - 按下：行有底色（uk_tappable）；
 * - 第一下：当场重画，不等 1 秒的刷新；
 * - 点了已经生效的选项：一句「已经是…」，不静默忽略；
 * - 发出后：「切换中…」一直显示到真的变过去，或者超时说明原因；
 * - 被拒：原因写在这张卡片里，不只写在页顶。 */
#define NET_EXIT_H   50
#define NET_STAT_H   48
#define NET_OP_H     44
#define NET_BTN_H    52
#define NET_NBR_HDR  40
#define NET_NBR_H    28
#define NET_SCENE_ROWS (1 + NI_MAX_SCENES + 1)   /* 自动 + 各情景 + 1 行备用 */
#define NET_SC_NOTE_H  44

static lv_obj_t *s_net_sec[5], *s_net_card[5];
static lv_obj_t *s_net_sc_row[NET_SCENE_ROWS], *s_net_sc_name[NET_SCENE_ROWS], *s_net_sc_tag[NET_SCENE_ROWS];
static lv_obj_t *s_net_sc_note;
static uint32_t  s_net_arm_sc;
static int       s_net_arm_sc_idx = -1;        /* 0 = 自动，1.. = scenes[i-1] */
static lv_obj_t *s_net_ex_row[2], *s_net_ex_key[2], *s_net_ex_ip[2], *s_net_ex_sub[2];
static lv_obj_t *s_net_op[4];
static lv_obj_t *s_net_status;
static lv_obj_t *s_net_opr[NI_MAX_OPS], *s_net_opr_name[NI_MAX_OPS], *s_net_opr_det[NI_MAX_OPS], *s_net_opr_tag[NI_MAX_OPS];
static lv_obj_t *s_net_btns, *s_net_scan_btn, *s_net_scan_lbl, *s_net_auto_btn, *s_net_auto_lbl;
static lv_obj_t *s_net_nbr_state, *s_net_nbr_btn, *s_net_nbr_lbl, *s_net_nbr_row[NI_MAX_CELLS], *s_net_nbr_l[NI_MAX_CELLS], *s_net_nbr_r[NI_MAX_CELLS];
static uint32_t  s_net_arm_scan, s_net_arm_auto, s_net_arm_op, s_net_arm_nbr;
static int       s_net_arm_idx = -1;
static int       s_net_painted_arm = -2;   /* 上次画的界面状态（待确认 / 切换中 / 提示），变了就重画 */
static lv_obj_t *s_net_sc_mark[NET_SCENE_ROWS];  /* 单选圈：实心 = 现在生效的那个 */
/* 每个情景下面两行小字：什么时候进入、进入后改什么（2026-09-25 用户：光有名字看不懂） */
static lv_obj_t *s_net_sc_when[NET_SCENE_ROWS], *s_net_sc_does[NET_SCENE_ROWS];
#define NET_SC_ROW3_H 70
/* 情景切换：发出后等到真的切过去（或超时） */
static int       s_sc_pend = -1;               /* 0 = 自动，1.. = scenes[i-1] */
static uint32_t  s_sc_pend_at;
static char      s_sc_pend_id[24], s_sc_pend_name[48];
static int       s_sc_pend_wifi_off;
#define SC_PEND_MS 45000
/* 临时提示：情景卡片一条，手动选网卡片一条（类型在 eSIM 页前面） */
static net_flash_t s_sc_flash, s_ms_flash;

static void net_paint(int changed);

static const char *net_scene_name(const netinfo_t *n, const char *id)
{
    for (int i = 0; i < n->nscenes; i++)
        if (!strcmp(n->scenes[i].id, id)) return n->scenes[i].name[0] ? n->scenes[i].name : id;
    return id;
}

static int net_armed(uint32_t at) { return at && lv_tick_get() - at < 4000; }
static int net_confirm(uint32_t at) { uint32_t d = lv_tick_get() - at; return at && d > 300 && d < 4000; }

static void net_clear_arms(void)
{
    s_net_arm_scan = s_net_arm_auto = s_net_arm_nbr = s_net_arm_op = s_net_arm_sc = 0;
    s_net_arm_idx = s_net_arm_sc_idx = -1;
}

static int net_busy(const netinfo_t *n)
{
    return !strcmp(n->scan_state, "scanning") ||
           !strcmp(n->guard_phase, "registering") || !strcmp(n->guard_phase, "reverting");
}

/* 发一个会碰模组的动作：先把「已发送」画出来再发（发的时候界面会停一下），
 * 被拒就把原因写回这张卡片 */
static void net_send(void (*fn)(void))
{
    net_flash(&s_ms_flash, T->t2, 2500, "%s", TR("已发送，等设备回应…"));
    net_paint(1);
    lv_refr_now(NULL);
    fn();
    const char *err = netinfo_action_error();
    if (err[0]) net_flash(&s_ms_flash, T->badT, 8000, TR("没执行：%s"), err);
    else netinfo_hurry(15);
    net_paint(1);
}

static const char *net_busy_what(const netinfo_t *n)
{
    return !strcmp(n->scan_state, "scanning") ? TR("正在搜索网络") :
           !strcmp(n->guard_phase, "reverting") ? TR("正在恢复自动选网") :
           !strcmp(n->guard_phase, "registering") ? TR("正在注册") : TR("正在忙");
}

static void net_scan_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    const netinfo_t *n = netinfo_get();
    if (net_busy(n)) {
        net_flash(&s_ms_flash, T->t2, 3000, TR("%s，等它做完再操作"), net_busy_what(n));
        net_paint(1);
        return;
    }
    if (net_confirm(s_net_arm_scan)) { net_clear_arms(); net_send(netinfo_scan); return; }
    net_clear_arms();
    s_net_arm_scan = lv_tick_get();
    net_paint(1);
}

static void net_auto_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    const netinfo_t *n = netinfo_get();
    if (net_busy(n)) {
        net_flash(&s_ms_flash, T->t2, 3000, TR("%s，等它做完再操作"), net_busy_what(n));
        net_paint(1);
        return;
    }
    if (!strcmp(n->selection, "auto")) {
        net_clear_arms();
        net_flash(&s_ms_flash, T->t2, 3000, "%s", TR("现在已经是自动选网，不用恢复"));
        net_paint(1);
        return;
    }
    if (net_confirm(s_net_arm_auto)) { net_clear_arms(); net_send(netinfo_auto); return; }
    net_clear_arms();
    s_net_arm_auto = lv_tick_get();
    net_paint(1);
}

static int s_net_reg_op;
static void net_register_armed(void) { netinfo_register(s_net_reg_op); }

static void net_op_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    const netinfo_t *n = netinfo_get();
    if (net_busy(n)) {
        net_flash(&s_ms_flash, T->t2, 3000, TR("%s，等它做完再操作"), net_busy_what(n));
        net_paint(1);
        return;
    }
    if (i >= n->nops) return;
    /* 当前那一行：自动选网时可以点，等于把选网固定在它上面（原厂网页也能选当前行）；
     * 已经手动在它上面就不用再注册 */
    if (!strcmp(n->ops[i].status, "2") && strcmp(n->selection, "auto")) {
        net_clear_arms();
        net_flash(&s_ms_flash, T->t2, 3000, TR("现在就在 %s 上"), n->ops[i].name[0] ? n->ops[i].name : n->ops[i].plmn);
        net_paint(1);
        return;
    }
    if (s_net_arm_idx == i && net_confirm(s_net_arm_op)) {
        net_clear_arms();
        s_net_reg_op = i;
        net_send(net_register_armed);
        return;
    }
    net_clear_arms();
    s_net_arm_idx = i;
    s_net_arm_op = lv_tick_get();
    net_paint(1);
}

static void net_nbr_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!strcmp(netinfo_get()->nbr_state, "scanning")) return;
    if (net_confirm(s_net_arm_nbr)) { net_clear_arms(); net_send(netinfo_nbr_scan); return; }
    net_clear_arms();
    s_net_arm_nbr = lv_tick_get();
    net_paint(1);
}

/* 情景行：0 = 自动，1..n = 固定到 scenes[i-1]，最后一行另有用途。
 * 换情景可能开关 Wi-Fi，所以两步：第一下整行变色、写出后果，4 秒内再点才发；
 * 发出后显示「切换中…」直到真的切过去。 */
static void net_sc_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    const netinfo_t *n = netinfo_get();
    if (i == NET_SCENE_ROWS - 1) return;
    if (i > n->nscenes) return;
    if (s_sc_pend >= 0) {
        net_flash(&s_sc_flash, T->t2, 3000, TR("正在切换到「%s」，稍等"), s_sc_pend_name);
        net_paint(1);
        return;
    }
    int already = i == 0 ? !n->scene_pin[0] : !strcmp(n->scene_pin, n->scenes[i - 1].id);
    if (already) {
        net_clear_arms();
        if (i == 0)
            net_flash(&s_sc_flash, T->t2, 4000, TR("现在已经是自动 · 按位置和 SIM 判断为「%s」"),
                      n->scene_current[0] ? net_scene_name(n, n->scene_current) : TR("判定中"));
        else
            net_flash(&s_sc_flash, T->t2, 4000, TR("已经固定在「%s」。要恢复自动判断，点「自动」"),
                      net_scene_name(n, n->scene_pin));
        net_paint(1);
        return;
    }
    if (s_net_arm_sc_idx == i && net_confirm(s_net_arm_sc)) {
        net_clear_arms();
        s_sc_pend = i;
        s_sc_pend_at = lv_tick_get();
        snprintf(s_sc_pend_id, sizeof s_sc_pend_id, "%s", i ? n->scenes[i - 1].id : "");
        snprintf(s_sc_pend_name, sizeof s_sc_pend_name, "%s", i ? net_scene_name(n, n->scenes[i - 1].id) : TR("自动"));
        s_sc_pend_wifi_off = i ? n->scenes[i - 1].wifi_off : 0;
        s_sc_flash.until = 0;
        net_paint(1);
        lv_refr_now(NULL);
        netinfo_pin(i == 0 ? NULL : n->scenes[i - 1].id);
        const char *err = netinfo_action_error();
        if (err[0]) {
            s_sc_pend = -1;
            net_flash(&s_sc_flash, T->badT, 8000, TR("没切成：%s"), err);
        } else {
            scenario_kick();
            netinfo_hurry(SC_PEND_MS / 1000);
        }
        net_paint(1);
        return;
    }
    net_clear_arms();
    s_net_arm_sc_idx = i;
    s_net_arm_sc = lv_tick_get();
    net_paint(1);
}

static void net_reflow(int err_h, int scene_rows, int exits, int ops, int cells);

/* 原「情景 · 网络」页的几块，2026-09-25 起各回各家（k_net_host）：情景 → 情景页，
 * 出口 IP → 出口标签，运营商 + 手动选网 → 运营商选择页，邻小区 → 小区信息页。
 * 设备流量并进了 Wi-Fi 标签的设备列表（refresh_wifi），这里不再建。数据和画法没变
 * （netinfo + net_paint），只是卡片挂在不同的页上，net_reflow 按页各自排。 */
static void build_sub_net(lv_obj_t *t)
{
    static const char *const k_sec[5] = { N_("选择"), N_("出口 IP"), N_("运营商"), N_("手动选网"), N_("邻小区") };
    static const char *const k_op_cap[4] = { N_("原始运营商"), N_("注册运营商"), N_("漫游"), N_("选网") };
    lv_obj_t *c, *host[5];

    t = s_net_scroll = s_nh_scroll[NH_OPER] = uk_scroll(t, 0, UI_SUB_VIEW, 1400);
    s_nh_scroll[NH_SCENE] = uk_scroll(s_sub_page[SUB_SCENE], 0, UI_SUB_VIEW, 600);
    s_nh_base[NH_SCENE] = s_nh_base[NH_OPER] = 4;
    for (int i = 0; i < 5; i++) host[i] = s_nh_scroll[k_net_host[i]];
    s_net_err = uk_label_w(t, UF.cj13, T->badT, UK_MARGIN + 6, 4, UK_CARD_W - 12, 1, "");
    for (int i = 0; i < 5; i++) s_net_sec[i] = uk_section(host[i], 0, TR(k_sec[i]));

    c = s_net_card[0] = uk_card(host[0], UK_MARGIN, 0, UK_CARD_W, UK_ROW_H + NET_SC_NOTE_H);
    for (int i = 0; i < NET_SCENE_ROWS; i++) {
        lv_obj_t *r = s_net_sc_row[i] = uk_box(c, 0, i * UK_ROW_H, UK_CARD_W, UK_ROW_H, T->card, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        if (i) uk_sep(r, 0);
        s_net_sc_mark[i] = uk_box(r, UK_PAD, (UK_ROW_H - 16) / 2, 16, 16, T->card, 8);
        lv_obj_set_style_border_width(s_net_sc_mark[i], 2, 0);
        s_net_sc_name[i] = uk_label_w(r, UF.cj14, T->t1, UK_PAD + 26, 11, 140, 0, "");
        s_net_sc_tag[i]  = uk_label_r(r, UF.cj13, T->t3, UK_CARD_W - UK_PAD, 12, "");
        s_net_sc_when[i] = uk_label_w(r, UF.cj12, T->t2, UK_PAD + 26, 32, UK_CARD_W - 2 * UK_PAD - 26, 1, "");
        s_net_sc_does[i] = uk_label_w(r, UF.cj12, T->t3, UK_PAD + 26, 50, UK_CARD_W - 2 * UK_PAD - 26, 0, "");
        for (int k = 0; k < 2; k++) {
            lv_obj_t *l = k ? s_net_sc_does[i] : s_net_sc_when[i];
            lv_obj_set_height(l, lv_font_get_line_height(UF.cj12));
            lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        }
        uk_tappable(r, net_sc_cb, (void *)(intptr_t)i);
        uk_show(r, i == 0);
    }
    s_net_sc_note = uk_label_w(c, UF.cj12, T->t3, UK_PAD, UK_ROW_H + 8, UK_CARD_W - 2 * UK_PAD, 1, "");

    c = s_net_card[1] = uk_card(host[1], UK_MARGIN, 0, UK_CARD_W, 2 * NET_EXIT_H);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *r = s_net_ex_row[i] = uk_box(c, 0, i * NET_EXIT_H, UK_CARD_W, NET_EXIT_H, T->card, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        if (i) uk_sep(r, 0);
        s_net_ex_key[i] = uk_label(r, UF.cj14, T->t2, UK_PAD, 7, "");
        s_net_ex_ip[i]  = uk_label_r(r, UF.n15, T->t1, UK_CARD_W - UK_PAD, 6, "");
        s_net_ex_sub[i] = uk_label_w(r, UF.cj12, T->t3, UK_PAD, 29, UK_CARD_W - 2 * UK_PAD, 0, "");
    }

    c = s_net_card[2] = uk_card(host[2], UK_MARGIN, 0, UK_CARD_W, 4 * UK_ROW_H);
    for (int i = 0; i < 4; i++) {
        s_net_op[i] = uk_row(c, i * UK_ROW_H, TR(k_op_cap[i]), i == 0);
        lv_obj_set_style_text_font(s_net_op[i], UF.cj14, 0);
    }

    c = s_net_card[3] = uk_card(host[3], UK_MARGIN, 0, UK_CARD_W, NET_STAT_H + NET_BTN_H);
    s_net_status = uk_label_w(c, UF.cj13, T->t2, UK_PAD, 8, UK_CARD_W - 2 * UK_PAD, 1, "");
    for (int i = 0; i < NI_MAX_OPS; i++) {
        lv_obj_t *r = s_net_opr[i] = uk_box(c, 0, NET_STAT_H + i * NET_OP_H, UK_CARD_W, NET_OP_H, T->card, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        uk_sep(r, 0);
        s_net_opr_name[i] = uk_label_w(r, UF.cj14, T->t1, UK_PAD, 5, UK_CARD_W - 2 * UK_PAD - 70, 0, "");
        s_net_opr_det[i]  = uk_label_w(r, UF.cj12, T->t3, UK_PAD, 25, UK_CARD_W - 2 * UK_PAD - 70, 0, "");
        s_net_opr_tag[i]  = uk_label_r(r, UF.cj13, T->t3, UK_CARD_W - UK_PAD, 13, "");
        uk_tappable(r, net_op_cb, (void *)(intptr_t)i);
        uk_show(r, 0);
    }
    s_net_btns = uk_box(c, 0, NET_STAT_H, UK_CARD_W, NET_BTN_H, T->card, 0);
    lv_obj_set_style_bg_opa(s_net_btns, LV_OPA_TRANSP, 0);
    uk_sep(s_net_btns, 0);
    s_net_scan_btn = uk_button(s_net_btns, UK_PAD, 10, (UK_CARD_W - 2 * UK_PAD - 8) / 2, 32, TR("搜索网络"),
                               UK_BTN_PLAIN, net_scan_cb, NULL, &s_net_scan_lbl);
    s_net_auto_btn = uk_button(s_net_btns, UK_PAD + (UK_CARD_W - 2 * UK_PAD - 8) / 2 + 8, 10,
                               (UK_CARD_W - 2 * UK_PAD - 8) / 2, 32, TR("恢复自动"),
                               UK_BTN_PLAIN, net_auto_cb, NULL, &s_net_auto_lbl);

    c = s_net_card[4] = uk_card(host[4], UK_MARGIN, 0, UK_CARD_W, NET_NBR_HDR);
    s_net_nbr_state = uk_label_w(c, UF.cj13, T->t2, UK_PAD, 12, UK_CARD_W - 2 * UK_PAD - 80, 0, "");
    s_net_nbr_btn = uk_button(c, UK_CARD_W - UK_PAD - 72, 6, 72, 28, TR("扫描"), UK_BTN_PLAIN, net_nbr_cb, NULL, &s_net_nbr_lbl);
    for (int i = 0; i < NI_MAX_CELLS; i++) {
        lv_obj_t *r = s_net_nbr_row[i] = uk_box(c, 0, NET_NBR_HDR + i * NET_NBR_H, UK_CARD_W, NET_NBR_H, T->card, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        if (!i) uk_sep(r, 0);
        s_net_nbr_l[i] = uk_label(r, UF.n12, T->t1, UK_PAD, 7, "");
        s_net_nbr_r[i] = uk_label_r(r, UF.n12, T->t2, UK_CARD_W - UK_PAD, 7, "");
        uk_show(r, 0);
    }

    net_reflow(0, UK_ROW_H, 1, 0, 0);
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* Cards move with the lists above them: lay each page out from its base. */
static int s_nr_last[5] = { 0, UK_ROW_H, 1, 0, 0 };
/* scene_px：情景各行加起来的高度（行高不一样，2026-09-25 起按像素传） */
static void net_reflow(int err_h, int scene_rows, int exits, int ops, int cells)
{
    int a[5] = { err_h, scene_rows, exits, ops, cells };
    memcpy(s_nr_last, a, sizeof a);
    int y[NH_N];
    for (int k = 0; k < NH_N; k++) y[k] = s_nh_base[k];
    y[NH_OPER] += err_h;
    int h[5] = {
        scene_rows + NET_SC_NOTE_H,
        exits * NET_EXIT_H,
        4 * UK_ROW_H,
        NET_STAT_H + ops * NET_OP_H + NET_BTN_H,
        NET_NBR_HDR + cells * NET_NBR_H + (cells ? 6 : 0),
    };
    for (int i = 0; i < 5; i++) {
        if (lv_obj_has_flag(s_net_card[i], LV_OBJ_FLAG_HIDDEN)) continue;
        int *yy = &y[k_net_host[i]];
        lv_obj_set_y(s_net_sec[i], *yy);
        lv_obj_set_y(s_net_card[i], *yy + 20);
        lv_obj_set_height(s_net_card[i], h[i]);
        *yy += 20 + h[i] + 10;
    }
    lv_obj_set_y(s_net_btns, NET_STAT_H + ops * NET_OP_H);
    lv_obj_set_y(s_net_sc_note, scene_rows + 8);
    lv_obj_set_y(s_exit_nav_sec, y[NH_EXIT]);
    lv_obj_set_y(s_exit_nav_card, y[NH_EXIT] + 20);
    y[NH_EXIT] += 20 + (int)lv_obj_get_style_height(s_exit_nav_card, 0) + 10;
    for (int k = 0; k < NH_N; k++)
        uk_scroll_extent(s_nh_scroll[k], y[k] - 10 + (k == NH_EXIT || k == NH_WIFI ? UK_TAB_PAD : 16));
}

/* 某一页上面的内容高度变了（Wi-Fi 设备列表）：按上次的参数重排 */
static void net_relayout(void)
{
    if (!s_net_card[0]) return;   /* 还没建好 */
    net_reflow(s_nr_last[0], s_nr_last[1], s_nr_last[2], s_nr_last[3], s_nr_last[4]);
}

/* 搜网结果的 m_rat：按原厂网页（mobile_network.js）的表，不是 27.007——13 算 4G、9 算 5G。
 * 表外的代号原样显示，和网页一样 */
static const char *net_rat_name(const char *rat)
{
    if (!strcmp(rat, "9") || !strcmp(rat, "11") || !strcmp(rat, "12")) return "5G";
    if (!strcmp(rat, "7") || !strcmp(rat, "13")) return "4G";
    if (!strcmp(rat, "2")) return "3G";
    if (!strcmp(rat, "0")) return "2G";
    return rat;
}

/* "中国联通 46001"；国外的带上国家："SoftBank（日本）44020"。国内看 agent 的
 * country_iso（英文模式下 country 是 China）；旧 agent 没有它，还比中文名 */
static int net_oper_home_country(const ni_oper_t *o)
{
    return o->country_iso[0] ? !strcmp(o->country_iso, "CN") : !strcmp(o->country, "中国");
}

static void net_oper_text(char *out, size_t n, const ni_oper_t *o)
{
    const char *name = o->name[0] ? o->name : TR("未知");
    if (!o->mcc[0] && !o->name[0]) { snprintf(out, n, "—"); return; }
    if (o->country[0] && !net_oper_home_country(o))
        snprintf(out, n, TR("%s（%s）%s%s"), name, o->country, o->mcc, o->mnc);
    else
        snprintf(out, n, "%s %s%s", name, o->mcc, o->mnc);
}

/* 第二行：归属地 · 运营商或节点；查不到时写原因，旧结果刷新失败时标一下 */
static void net_exit_sub(char *out, size_t n, const ni_exit_t *e, const char *extra)
{
    /* 原始错误（"ipapi.co: io: unexpected end of file"）只是最后一家的失败；
     * 几家都失败多半是刚换网/节点不通，半分钟后会再查。 */
    if (!e->ip[0] && e->err[0]) { snprintf(out, n, "%s", TR("暂时查不到，稍后自动重试")); return; }
    snprintf(out, n, "%s%s%s%s", e->geo, e->geo[0] && extra[0] ? " · " : "", extra,
             e->err[0] ? TR(" · 刷新失败") : "");
}

