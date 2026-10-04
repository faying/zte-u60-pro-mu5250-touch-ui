/* ---- E4 write transactions: the 事务行 and the 事务页 ----
 * manager docs/designs/write-op-layer.md 设计评审 (DD3, DD8, DD9, DD12, DD13,
 * DD16, DD17); the data is datad's /v2/screen "op" (op_view.h), every
 * sentence comes from datad.
 *
 * - 事务行: one 40 px row under the status bar (tabs other than Home) or the
 *   subpage header, while a change is in progress or its result still wants
 *   a 知道了. The page's scroller moves down by the row's height, nothing is
 *   covered. Home shows the same through its status block (net.home), so no
 *   row there. Tap → 事务页.
 * - 事务页 (SUB_OP): conclusion → 旧 → 新 · 来源 → three steps → countdown →
 *   buttons. 退回 X (primary, left) / 保留 Y (right), both two taps, one armed
 *   at a time. A result: 知道了 (op.ack, shared with the web page); 退回也没通
 *   adds 再试一次退回 and 重启设备, 没切成 adds 重启设备 (two taps each).
 * - A brief result (已切到 Y …) shows 3 s, and only if this screen saw it end
 *   (its op_id was in active, or last changed while we watched): a restart of
 *   the screen must not flash an old 已切到 again (STATE_V2.md V2-35).
 */
#define OP_ROW_H     40
#define OP_BRIEF_MS  3000
#define OP_STALE_MS  20000   /* no new "op" for this long while counting: 停在 m:ss */
#define OP_ARM_MS    5000

enum { OPB_NONE, OPB_REVERT, OPB_KEEP, OPB_ACK, OPB_RETRY, OPB_REBOOT };
enum { OPR_NONE, OPR_LIVE, OPR_RESULT };

static char     s_op_raw[8192];
static uint32_t s_op_at;            /* lv tick of the last parse that changed it */
static int      s_op_parsed;
static char     s_op_seen_live[72]; /* an op_id this screen saw in "active" */
static char     s_op_brief_id[72];
static uint32_t s_op_brief_at;
static char     s_op_acked[72];     /* 知道了 tapped here: hide before datad says so */
static int      s_op_row_state = -1, s_op_row_shift = -1;

static lv_obj_t *s_opr, *s_opr_dot, *s_opr_lbl;
static lv_obj_t *s_opp_say, *s_opp_meta, *s_opp_note, *s_opp_left, *s_opp_why;
static lv_obj_t *s_opp_step_dot[OP_STEPS], *s_opp_step_lbl[OP_STEPS];
static lv_obj_t *s_opp_btn[3], *s_opp_btn_lbl[3];
static int      s_opp_role[3];
static int      s_opp_armed = OPB_NONE;
static uint32_t s_opp_arm_at;

static uint32_t op_mark_col(int mark)
{
    return mark == OP_MARK_OK ? T->okT : mark == OP_MARK_WARN ? T->warnT : mark == OP_MARK_BAD ? T->badT : T->accT;
}

/* The item this screen shows: the one in progress, else a result to show. */
static const op_item_t *op_shown(int *state)
{
    uint32_t now = lv_tick_get();
    const op_item_t *l = &s_op.last;

    *state = OPR_NONE;
    if (s_op.active.have) { *state = OPR_LIVE; return &s_op.active; }
    if (!l->have || !strcmp(l->op_id, s_op_acked)) return NULL;
    if (l->needs_ack || (!strcmp(l->op_id, s_op_brief_id) && now - s_op_brief_at < OP_BRIEF_MS)) {
        *state = OPR_RESULT;
        return l;
    }
    return NULL;
}

/* Time left on the countdown, counted down here between fetches; -1 = none. */
static long op_left_ms(const op_item_t *it)
{
    long left;
    if (it->remaining_ms < 0 || !it->next[0]) return -1;
    left = it->remaining_ms - (long)(lv_tick_get() - s_op_at);
    return left > 0 ? left : 0;
}

/* datad has not said anything new for a while (or is stuck): the clock stops. */
static int op_stale(void)
{
    return screen_feed_stuck() || lv_tick_get() - s_op_at >= OP_STALE_MS;
}

static void op_parse(void)
{
    const char *raw = screen_feed_op();
    char prev_last[72];

    if (s_op_parsed && !strcmp(raw, s_op_raw)) return;
    snprintf(prev_last, sizeof prev_last, "%s", s_op.last.op_id);
    snprintf(s_op_raw, sizeof s_op_raw, "%s", raw);
    op_view_parse(s_op_raw, &s_op);
    s_op_at = lv_tick_get();
    if (s_op.active.have) snprintf(s_op_seen_live, sizeof s_op_seen_live, "%s", s_op.active.op_id);
    /* a result this screen watched end: its brief 3 s start now */
    if (s_op.last.have && strcmp(s_op.last.op_id, prev_last) &&
        (s_op_parsed || !strcmp(s_op.last.op_id, s_op_seen_live))) {
        snprintf(s_op_brief_id, sizeof s_op_brief_id, "%s", s_op.last.op_id);
        s_op_brief_at = s_op_at;
    }
    s_op_parsed = 1;
}

/* ---- the row ---- */
static void op_row_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    sub_open(SUB_OP);
}

static void build_op_row(void)
{
    s_opr = uk_box(lv_layer_top(), 0, UK_BAR_H, UK_W, OP_ROW_H, T->card, 0);
    lv_obj_set_style_border_side(s_opr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(s_opr, 1, 0);
    lv_obj_set_style_border_color(s_opr, lv_color_hex(T->sep), 0);
    s_opr_dot = uk_dot(s_opr, UK_PAD + UK_MARGIN, (OP_ROW_H - 8) / 2, 8, T->accT);
    s_opr_lbl = uk_label(s_opr, UF.cj15, T->t1, UK_PAD + UK_MARGIN + 16, 0, "");
    lv_obj_set_width(s_opr_lbl, UK_W - (UK_PAD + UK_MARGIN + 16) - 40);
    lv_label_set_long_mode(s_opr_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_opr_lbl, LV_ALIGN_LEFT_MID, UK_PAD + UK_MARGIN + 16, 0);
    lv_obj_t *chev = uk_label(s_opr, UF.cj17b, T->t3, 0, 0, "›");
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -(UK_PAD + UK_MARGIN), 0);
    uk_tappable(s_opr, op_row_cb, NULL);
    uk_show(s_opr, 0);
}

/* Move every tab's (not Home's) and subpage's scroller down by dy so the row
 * covers nothing. Only when the row comes or goes. */
static void op_row_shift(int dy)
{
    if (dy == s_op_row_shift) return;
    s_op_row_shift = dy;
    for (int i = 0; i < UI_TABS; i++) {
        lv_obj_t *sc = i == TAB_HOME ? NULL : obj_scroller(s_tiles[i]);
        if (!sc) continue;
        lv_obj_set_y(sc, dy);
        lv_obj_set_height(sc, UI_VIEW_H - dy);
    }
    for (int i = 0; i < SUB_N; i++) {
        lv_obj_t *sc = i == SUB_OP ? NULL : obj_scroller(s_sub_page[i]);
        if (!sc) continue;
        lv_obj_set_y(sc, dy);
        lv_obj_set_height(sc, UI_SUB_VIEW - dy);
    }
}

static void op_row_paint(void)
{
    static char c_row[160];
    int state, sub = s_sub_cur >= 0;
    const op_item_t *it = op_shown(&state);
    int show = it && s_sub_cur != SUB_OP && (sub || cur_tab() != TAB_HOME);

    op_row_shift(it ? OP_ROW_H : 0);
    if (!show) { uk_show(s_opr, 0); s_op_row_state = OPR_NONE; return; }
    lv_obj_set_y(s_opr, sub ? UK_NAV_H : UK_BAR_H);
    if (state == OPR_LIVE) {
        long left = op_left_ms(it);
        char t[16];
        if (left >= 0 && !op_stale()) {
            op_fill_clock(t, sizeof t, "{t}", left);
            snprintf(c_row, sizeof c_row, "%s · %s", it->say, t);
        } else {
            snprintf(c_row, sizeof c_row, "%s", it->say);
        }
    } else {
        snprintf(c_row, sizeof c_row, "%s", it->say);
    }
    if (strcmp(lv_label_get_text(s_opr_lbl), c_row)) lv_label_set_text(s_opr_lbl, c_row);
    uk_bg(s_opr_dot, state == OPR_LIVE ? T->accT : op_mark_col(it->mark));
    uk_show(s_opr, 1);
    lv_obj_move_foreground(s_opr);
    s_op_row_state = state;
}

/* ---- the page ---- */
static void op_send(const char *action, const char *op_id)
{
    char p[128];
    snprintf(p, sizeof p, "{\"op_id\":\"%s\"}", op_id);
    data_control(action, p, NULL);
}

static void op_page_paint(void);
static void op_btn_cb(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    int role = s_opp_role[slot];
    uint32_t now = lv_tick_get();
    int state;
    const op_item_t *it = op_shown(&state);

    if (!it || role == OPB_NONE) return;
    if (role == OPB_ACK) {
        op_send("op.ack", it->op_id);
        snprintf(s_op_acked, sizeof s_op_acked, "%s", it->op_id);
        sub_back();
        return;
    }
    if (s_opp_armed != role || now - s_opp_arm_at >= OP_ARM_MS) {
        s_opp_armed = role;            /* first tap: arm, say what the second does */
        s_opp_arm_at = now ? now : 1;
        op_page_paint();               /* answered at once (DESIGN.md §4) */
        return;
    }
    s_opp_armed = OPB_NONE;
    op_page_paint();
    switch (role) {
    case OPB_REVERT: op_send("op.revert", it->op_id); break;
    case OPB_KEEP:   op_send("op.keep", it->op_id); break;
    case OPB_RETRY: {
        /* 再试一次退回到 X: a new write of X (network mode is the only item
         * with transactions; other items get no such button) */
        char p[80], fb[64];
        snprintf(p, sizeof p, "{\"mode\":\"%s\"}", it->rollback_to_raw);
        snprintf(fb, sizeof fb, "mode=%s", it->rollback_to_raw);
        data_control("network.set_mode", p, fb);
        break;
    }
    case OPB_REBOOT: power_run(1); break;
    default: break;
    }
}

static void build_sub_op(lv_obj_t *t)
{
    lv_obj_t *sc = lv_obj_create(t);
    lv_obj_remove_style_all(sc);
    lv_obj_set_size(sc, UI_W, UI_SUB_VIEW);
    lv_obj_set_scroll_dir(sc, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sc, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sc, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(sc, 4, 0);
    lv_obj_set_style_pad_bottom(sc, 24, 0);
    lv_obj_set_style_pad_row(sc, 10, 0);

    lv_obj_t *c = uk_card(sc, 0, 0, UK_CARD_W, 10);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, UK_PAD, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 6, 0);
    /* 1 conclusion */
    s_opp_say = uk_label(c, UF.cj24b, T->t1, 0, 0, "");
    lv_obj_set_width(s_opp_say, lv_pct(100));
    lv_label_set_long_mode(s_opp_say, LV_LABEL_LONG_MODE_WRAP);
    /* 2 旧 → 新 · 来源 */
    s_opp_meta = uk_label(c, UF.cj13, T->t3, 0, 0, "");
    lv_obj_set_width(s_opp_meta, lv_pct(100));
    lv_label_set_long_mode(s_opp_meta, LV_LABEL_LONG_MODE_WRAP);
    s_opp_note = uk_label(c, UF.cj13, T->warnT, 0, 0, "");
    /* 3 three steps */
    for (int i = 0; i < OP_STEPS; i++) {
        lv_obj_t *r = uk_box(c, 0, 0, lv_pct(100), 26, T->card, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        s_opp_step_dot[i] = uk_dot(r, 0, 9, 8, T->t3);
        s_opp_step_lbl[i] = uk_label(r, UF.cj15, T->t2, 16, 2, "");
    }
    /* 4 countdown */
    s_opp_left = uk_label(c, UF.cj15, T->t1, 0, 0, "");
    lv_obj_set_width(s_opp_left, lv_pct(100));
    lv_label_set_long_mode(s_opp_left, LV_LABEL_LONG_MODE_WRAP);
    s_opp_why = uk_label(c, UF.cj13, T->t2, 0, 0, "");
    lv_obj_set_width(s_opp_why, lv_pct(100));
    lv_label_set_long_mode(s_opp_why, LV_LABEL_LONG_MODE_WRAP);

    /* 5 buttons: two side by side, a third under them */
    lv_obj_t *b = uk_box(sc, 0, 0, UK_CARD_W, 44 + 10 + 44, T->bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    for (int i = 0; i < 3; i++) {
        int w = i < 2 ? (UK_CARD_W - 10) / 2 : UK_CARD_W;
        s_opp_btn[i] = uk_button(b, i == 1 ? w + 10 : 0, i < 2 ? 0 : 54, w, 44, "", UK_BTN_PLAIN,
                                 op_btn_cb, (void *)(intptr_t)i, &s_opp_btn_lbl[i]);
        lv_label_set_long_mode(s_opp_btn_lbl[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(s_opp_btn_lbl[i], w - 16, lv_font_get_line_height(UF.cj15b));
        lv_obj_set_style_text_align(s_opp_btn_lbl[i], LV_TEXT_ALIGN_CENTER, 0);
        uk_show(s_opp_btn[i], 0);
    }
}

/* Where a button sits: half = left / right half of row `row`, or the full width (-1). */
static void op_btn_place(int slot, int row, int half)
{
    const int hw = (UK_CARD_W - 10) / 2;
    int w = half < 0 ? UK_CARD_W : hw;
    lv_obj_set_pos(s_opp_btn[slot], half == 1 ? hw + 10 : 0, row * 54);
    lv_obj_set_width(s_opp_btn[slot], w);
    lv_obj_set_width(s_opp_btn_lbl[slot], w - 16);
    lv_obj_center(s_opp_btn_lbl[slot]);
}

static void op_btn_set(int slot, int role, const char *text, uk_btn_kind_t kind)
{
    static char c_btn[3][80];
    int armed = role != OPB_NONE && role == s_opp_armed;
    s_opp_role[slot] = role;
    uk_show(s_opp_btn[slot], role != OPB_NONE);
    if (role == OPB_NONE) return;
    snprintf(c_btn[slot], sizeof c_btn[slot], "%s", armed ? TR("再按一次") : text);
    if (strcmp(lv_label_get_text(s_opp_btn_lbl[slot]), c_btn[slot]))
        lv_label_set_text(s_opp_btn_lbl[slot], c_btn[slot]);
    uk_button_kind(s_opp_btn[slot], s_opp_btn_lbl[slot], armed ? UK_BTN_ARMED : kind);
}

static void op_page_paint(void)
{
    static char c_say[96], c_meta[200], c_left[160], c_why[200], c_title[64];
    int state;
    const op_item_t *it = op_shown(&state);
    uint32_t now = lv_tick_get();

    if (s_opp_armed != OPB_NONE && now - s_opp_arm_at >= OP_ARM_MS) s_opp_armed = OPB_NONE;
    if (!it) {
        /* nothing left to show (acked elsewhere, or a brief result timed out) */
        set_label_fmt(s_opp_say, c_say, sizeof c_say, "%s", TR("没有进行中的改动"));
        uk_text_color(s_opp_say, T->t2);
        lv_label_set_text(s_opp_meta, "");
        uk_show(s_opp_note, 0);
        for (int i = 0; i < OP_STEPS; i++) { uk_show(s_opp_step_dot[i], 0); uk_show(s_opp_step_lbl[i], 0); }
        uk_show(s_opp_left, 0);
        uk_show(s_opp_why, 0);
        for (int i = 0; i < 3; i++) op_btn_set(i, OPB_NONE, "", UK_BTN_PLAIN);
        return;
    }
    /* the title is what is happening (正在换制式 / the item) */
    set_label_fmt(s_sub_title, c_title, sizeof c_title, "%s", state == OPR_LIVE ? it->say : it->what);
    set_label_fmt(s_opp_say, c_say, sizeof c_say, "%s", it->say);
    uk_text_color(s_opp_say, state == OPR_LIVE ? T->t1 : op_mark_col(it->mark));
    set_label_fmt(s_opp_meta, c_meta, sizeof c_meta, "%s → %s · %s", it->old_v, it->target, it->source);
    uk_show(s_opp_note, it->note[0] != 0);
    if (it->note[0]) lv_label_set_text(s_opp_note, it->note);
    for (int i = 0; i < OP_STEPS; i++) {
        uk_show(s_opp_step_dot[i], it->step[i][0] != 0);
        uk_show(s_opp_step_lbl[i], it->step[i][0] != 0);
        if (strcmp(lv_label_get_text(s_opp_step_lbl[i]), it->step[i])) lv_label_set_text(s_opp_step_lbl[i], it->step[i]);
        uk_bg(s_opp_step_dot[i], it->step_done[i] ? T->okT : T->t3);
        uk_text_color(s_opp_step_lbl[i], it->step_done[i] ? T->t1 : T->t3);
    }
    /* countdown: counted down here; stale → stopped and greyed (DD12) */
    c_left[0] = 0;
    if (state == OPR_LIVE && op_left_ms(it) >= 0) {
        if (op_stale()) {
            char t[16];
            op_fill_clock(t, sizeof t, "{t}", op_left_ms(it));
            snprintf(c_left, sizeof c_left, TR("停在 %s · 等设备响应"), t);
        } else {
            op_fill_clock(c_left, sizeof c_left, it->next, op_left_ms(it));
        }
    }
    uk_show(s_opp_left, c_left[0] != 0);
    if (c_left[0] && strcmp(lv_label_get_text(s_opp_left), c_left)) lv_label_set_text(s_opp_left, c_left);
    uk_text_color(s_opp_left, op_stale() ? T->t3 : T->t1);

    /* what the armed button will do, or the advice for this result */
    c_why[0] = 0;
    if (s_opp_armed == OPB_KEEP)
        snprintf(c_why, sizeof c_why, "%s", TR("不再自动退回；还没确认通，没网要你自己改回去"));
    else if (s_opp_armed == OPB_REVERT)
        snprintf(c_why, sizeof c_why, TR("马上改回「%s」，会重新注册，断网几十秒"), it->rollback_to);
    else if (s_opp_armed == OPB_RETRY)
        snprintf(c_why, sizeof c_why, TR("再发一次改回「%s」"), it->rollback_to);
    else if (s_opp_armed == OPB_REBOOT)
        snprintf(c_why, sizeof c_why, "%s", TR("重启设备，一两分钟没网"));
    else if (state == OPR_LIVE && !strcmp(it->phase, "verifying") && !s_op.rollback_enabled)
        snprintf(c_why, sizeof c_why, "%s", TR("自动退回没开：没通也会保持"));
    else if (state == OPR_RESULT && !strcmp(it->phase, "not_applied"))
        snprintf(c_why, sizeof c_why, "%s", TR("基带崩过以后常见，重启后再切一次"));
    else if (state == OPR_RESULT && !strcmp(it->phase, "rollback_failed"))
        snprintf(c_why, sizeof c_why, TR("现在：%s · 上次确认：%s"),
                 it->readback[0] ? it->readback : TR("当前设置未知"), it->rollback_to);
    uk_show(s_opp_why, c_why[0] != 0);
    if (c_why[0] && strcmp(lv_label_get_text(s_opp_why), c_why)) lv_label_set_text(s_opp_why, c_why);

    /* buttons */
    /* DD13: 退回 X left (primary), 保留 Y right. DD9: 再试一次退回 on its own
     * row (the value can be long), 重启设备 and 知道了 under it. */
    if (state == OPR_LIVE) {
        op_btn_place(0, 0, 0); op_btn_place(1, 0, 1);
        op_btn_set(0, it->can_revert ? OPB_REVERT : OPB_NONE, it->revert_label, UK_BTN_PRIMARY);
        op_btn_set(1, it->can_keep ? OPB_KEEP : OPB_NONE, it->keep_label, UK_BTN_PLAIN);
        op_btn_set(2, OPB_NONE, "", UK_BTN_PLAIN);
    } else if (!strcmp(it->phase, "rollback_failed")) {
        char r[96];
        int retry = !strcmp(it->item, "network.mode") && it->rollback_to_raw[0];
        snprintf(r, sizeof r, TR("再试一次退回到%s"), it->rollback_to);
        op_btn_place(0, 0, -1); op_btn_place(1, retry, 0); op_btn_place(2, retry, 1);
        op_btn_set(0, retry ? OPB_RETRY : OPB_NONE, r, UK_BTN_PRIMARY);
        op_btn_set(1, OPB_REBOOT, TR("重启设备"), UK_BTN_DANGER);
        op_btn_set(2, OPB_ACK, TR("知道了"), UK_BTN_PLAIN);
    } else if (!strcmp(it->phase, "not_applied")) {
        op_btn_place(1, 0, 0); op_btn_place(2, 0, 1);
        op_btn_set(0, OPB_NONE, "", UK_BTN_PLAIN);
        op_btn_set(1, OPB_REBOOT, TR("重启设备"), UK_BTN_DANGER);
        op_btn_set(2, OPB_ACK, TR("知道了"), UK_BTN_PLAIN);
    } else {
        op_btn_place(2, 0, -1);
        op_btn_set(0, OPB_NONE, "", UK_BTN_PLAIN);
        op_btn_set(1, OPB_NONE, "", UK_BTN_PLAIN);
        op_btn_set(2, it->needs_ack ? OPB_ACK : OPB_NONE, TR("知道了"), UK_BTN_PLAIN);
    }
}

/* The 事务页 just opened: paint it now, not at the next refresh tick. */
static void op_on_open(void)
{
    s_opp_armed = OPB_NONE;
    op_parse();
    op_page_paint();
}

static void op_guard_paint(void);
static void op_owner_refresh(void);

/* ---- DD18: 自动退回已打开 · 知道了, once, on Home ----
 * datad's op.notice = "rollback_on" while automatic revert is on and nobody
 * has acknowledged it (here or on the web page). 知道了 sends op.notice_ack
 * and hides the line at once; it comes back only if datad drops the notice
 * and raises it again (turned off, then on). */
static int s_hn_acked;

static void op_notice_paint(void)
{
    int want = !strcmp(s_op.notice, "rollback_on");
    if (!want) s_hn_acked = 0;
    want = want && !s_hn_acked;
    if (want == (home_visible_h(s_hn_box) >= 0)) return;
    uk_show(s_hn_box, want);
    home_reflow();
}

static void op_notice_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    data_control("op.notice_ack", "{}", NULL);
    s_hn_acked = 1;
    op_notice_paint();                  /* answered at once (DESIGN.md §4) */
}

/* Every refresh tick. */
static void op_refresh(void)
{
    op_parse();
    op_guard_paint();
    op_notice_paint();
    op_row_paint();
    op_owner_refresh();
    if (s_sub_cur == SUB_OP) op_page_paint();
}

/* The network-mode segment (lock.c): datad's own verdict while it has a
 * transaction for that item (replaces the screen's 20 s read-back guess), and
 * its result if that ended (seen by this screen) at or after tick `since`.
 * Returns 1 and fills `out` when it does. */
static int op_mode_line(char *out, size_t n, uint32_t since)
{
    const op_item_t *a = &s_op.active, *l = &s_op.last;
    if (a->have && !strcmp(a->item, "network.mode")) {
        long left = op_left_ms(a);
        char t[16];
        if (left >= 0 && !op_stale()) {
            op_fill_clock(t, sizeof t, "{t}", left);
            snprintf(out, n, "%s · %s", a->say, t);
        } else {
            snprintf(out, n, "%s", a->say);
        }
        return 1;
    }
    if (since && l->have && !strcmp(l->item, "network.mode") && !strcmp(l->op_id, s_op_brief_id) &&
        (int32_t)(s_op_brief_at - since) >= 0) {
        snprintf(out, n, "%s", l->say);
        return 1;
    }
    return 0;
}

/* ---- 系统 › 改动记录 (DD5, DD10, DD11) ----
 * datad's journal.list, each line already in words (STATE_V2.md V2-41).
 * Fetched when the page opens (and on 刷新 by reopening); a failed fetch
 * keeps what was shown, dimmed, and says so. Two lines a row: what · change,
 * then time · source · result. A row opens its detail, where 撤销 / 重做 is
 * (two taps) or, greyed, why not. */
#define LOG_ROW_H 56
static op_log_t s_log[OP_LOG_MAX];
static op_owner_t s_own_mode;      /* owners["network.mode"], read with the log */
static int      s_log_n = -1, s_log_err, s_log_sel = -1;
static lv_obj_t *s_log_status, *s_log_card, *s_log_row[OP_LOG_MAX], *s_log_l1[OP_LOG_MAX],
                *s_log_l2[OP_LOG_MAX], *s_log_dot[OP_LOG_MAX], *s_log_foot;
static lv_obj_t *s_logd_what, *s_logd_change, *s_logd_meta, *s_logd_result, *s_logd_btn, *s_logd_btn_lbl,
                *s_logd_why;
static uint32_t s_logd_arm;

/* POST /control journal.list on the UI thread: datad answers from its files,
 * fast; the IO limit keeps a stuck datad from holding the screen long. */
static int op_log_fetch(void)
{
    static char buf[57344];
    static op_log_t got[OP_LOG_MAX];
    char req[256];
    const char *body = "{\"action\":\"journal.list\",\"source\":\"screen\",\"params\":{\"limit\":30}}";
    http_resp_t r;
    int fd, n;

    if (!http_build(req, sizeof req, "POST", "/control", "127.0.0.1", "", body)) return 0;
    if ((fd = http_connect_tcp("127.0.0.1", 9460, 300)) < 0) return 0;
    http_exchange(fd, req, buf, sizeof buf, 600, &r);
    if (r.status != 200 || !r.body) return 0;
    n = op_log_parse(r.body, got, OP_LOG_MAX);
    if (n < 0) return 0;
    memcpy(s_log, got, sizeof s_log);
    s_log_n = n;
    op_owner_parse(r.body, "network.mode", &s_own_mode);
    return 1;
}

/* ---- 「上次改动」 under 网络模式 on the 蜂窝 tab (DD5, T15) ----
 * datad's owners say who last wrote the item and when. Read with the change
 * log (one journal.list) when the tab comes into view and whenever datad's op
 * block changes while it is in view (every write of that item is a
 * transaction, so a new owner always comes with an op change). A failed read
 * keeps what was shown. owners only has 网络模式 for now; other settings get
 * the same row when datad starts owning them. */
static void op_owner_paint(void)
{
    static char c_val[64];
    static int was = -1;
    int show = s_own_mode.have;
    if (show) {
        char v[64];
        snprintf(v, sizeof v, "%s · %s", s_own_mode.when, s_own_mode.source);
        if (strcmp(v, c_val)) { snprintf(c_val, sizeof c_val, "%s", v); lv_label_set_text(s_md_own_val, c_val); }
    }
    if (show != was) { was = show; cell_own_layout(show); }
}

static void op_owner_refresh(void)
{
    static int seen;
    static char key[200];
    char k[200];
    int vis = tab_visible(TAB_CELL);

    snprintf(k, sizeof k, "%s|%s|%s|%s", s_op.active.op_id, s_op.active.phase, s_op.last.op_id, s_op.last.phase);
    if (vis && (!seen || strcmp(k, key))) {
        snprintf(key, sizeof key, "%s", k);
        if (op_log_fetch()) op_owner_paint();
    }
    seen = vis;
}

/* Open that change in 系统 › 改动记录 (返回 lands on the list); not among the
 * rows read (older than 20, or no op_id): the list itself. */
static void op_owner_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    sub_open(SUB_LOG);                  /* reads the log (and owners) afresh */
    op_owner_paint();
    s_log_sel = -1;
    for (int i = 0; s_own_mode.op_id[0] && i < (s_log_n < 0 ? 0 : s_log_n); i++)
        if (!strcmp(s_log[i].op_id, s_own_mode.op_id)) { s_log_sel = i; break; }
    if (s_log_sel >= 0) sub_open_child(SUB_LOG_DETAIL, SUB_LOG);
}

static void log_row_cb(lv_event_t *e)
{
    s_log_sel = (int)(intptr_t)lv_event_get_user_data(e);
    sub_open_child(SUB_LOG_DETAIL, SUB_LOG);
}

static void build_sub_log(lv_obj_t *t)
{
    lv_obj_t *sc = uk_scroll(t, 0, UI_SUB_VIEW, 24 + 20 + OP_LOG_MAX * LOG_ROW_H + 60);
    s_log_status = uk_label_w(sc, UF.cj13, T->t3, UK_MARGIN + 4, 4, UK_CARD_W - 8, 1, "");
    s_log_card = uk_card(sc, UK_MARGIN, 28, UK_CARD_W, LOG_ROW_H);
    for (int i = 0; i < OP_LOG_MAX; i++) {
        lv_obj_t *r = uk_box(s_log_card, 0, i * LOG_ROW_H, UK_CARD_W, LOG_ROW_H, T->card, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        if (i) uk_sep(s_log_card, i * LOG_ROW_H);
        s_log_l1[i] = uk_label(r, UF.cj15, T->t1, UK_PAD, 8, "");
        lv_obj_set_width(s_log_l1[i], UK_CARD_W - 2 * UK_PAD - 14);
        lv_label_set_long_mode(s_log_l1[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(s_log_l1[i], lv_font_get_line_height(UF.cj15));
        s_log_dot[i] = uk_dot(r, UK_PAD, 35, 7, T->t3);
        s_log_l2[i] = uk_label(r, UF.cj13, T->t3, UK_PAD + 12, 30, "");
        lv_obj_set_width(s_log_l2[i], UK_CARD_W - 2 * UK_PAD - 26);
        lv_label_set_long_mode(s_log_l2[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(s_log_l2[i], lv_font_get_line_height(UF.cj13));
        uk_label(r, UF.cj17b, T->t3, UK_CARD_W - UK_PAD - 8, 16, "›");
        uk_tappable(r, log_row_cb, (void *)(intptr_t)i);
        s_log_row[i] = r;
        uk_show(r, 0);
    }
    s_log_foot = uk_label(sc, UF.cj12, T->t3, UK_MARGIN + 4, 0, "");
}

static void log_paint(void)
{
    static char c_status[160], c_foot[64];
    int n = s_log_n < 0 ? 0 : s_log_n;

    if (s_log_err && s_log_n < 0) snprintf(c_status, sizeof c_status, "%s", TR("读不到改动记录 · 数据服务没响应"));
    else if (s_log_err) snprintf(c_status, sizeof c_status, "%s", TR("读不到改动记录 · 下面是上次读到的"));
    else if (!n) snprintf(c_status, sizeof c_status, "%s", TR("还没有改动 · 触屏、网页、情景、定时任务改的设置都会记在这里"));
    else c_status[0] = 0;
    lv_label_set_text(s_log_status, c_status);
    uk_show(s_log_card, n > 0);
    lv_obj_set_y(s_log_card, c_status[0] ? 28 + 2 * 18 : 28);
    lv_obj_set_height(s_log_card, (n ? n : 1) * LOG_ROW_H);
    lv_obj_set_style_opa(s_log_card, s_log_err ? LV_OPA_50 : LV_OPA_COVER, 0);
    for (int i = 0; i < OP_LOG_MAX; i++) {
        uk_show(s_log_row[i], i < n);
        if (i >= n) continue;
        char l1[160], l2[200];
        const op_log_t *o = &s_log[i];
        if (o->change[0]) snprintf(l1, sizeof l1, "%s · %s", o->what, o->change);
        else snprintf(l1, sizeof l1, "%s", o->what);
        /* 「情景跳过 ×5（…）」 already names who: don't say 情景 twice */
        int named = o->source[0] && !strncmp(o->result, o->source, strlen(o->source));
        if (o->source[0] && !named) snprintf(l2, sizeof l2, "%s · %s · %s", o->when, o->source, o->result);
        else snprintf(l2, sizeof l2, "%s · %s", o->when, o->result);
        lv_label_set_text(s_log_l1[i], l1);
        lv_label_set_text(s_log_l2[i], l2);
        uk_bg(s_log_dot[i], op_mark_col(o->mark));
    }
    snprintf(c_foot, sizeof c_foot, TR("只显示最近 %d 条"), OP_LOG_MAX);
    lv_label_set_text(s_log_foot, n ? c_foot : "");
    lv_obj_set_y(s_log_foot, lv_obj_get_y(s_log_card) + n * LOG_ROW_H + 8);
}

static void op_log_on_open(void)
{
    s_log_err = !op_log_fetch();
    log_paint();
}

static void logd_undo_cb(lv_event_t *e)
{
    uint32_t now = lv_tick_get();
    const op_log_t *o;
    LV_UNUSED(e);
    if (s_log_sel < 0 || s_log_sel >= (s_log_n < 0 ? 0 : s_log_n)) return;
    o = &s_log[s_log_sel];
    if (!o->undo_ok) return;
    if (!s_logd_arm || now - s_logd_arm >= OP_ARM_MS) {
        s_logd_arm = now ? now : 1;     /* first tap: arm (DD10: 两段) */
        lv_label_set_text(s_logd_btn_lbl, TR("再按一次"));
        uk_button_kind(s_logd_btn, s_logd_btn_lbl, UK_BTN_ARMED);
        return;
    }
    s_logd_arm = 0;
    data_control_undo(o->undo_action, o->undo_params);
    sub_back();                         /* the 事务行 takes it from here */
    sub_back();
}

static void build_sub_log_detail(lv_obj_t *t)
{
    lv_obj_t *sc = lv_obj_create(t);
    lv_obj_remove_style_all(sc);
    lv_obj_set_size(sc, UI_W, UI_SUB_VIEW);
    lv_obj_set_scroll_dir(sc, LV_DIR_VER);
    lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sc, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(sc, 4, 0);
    lv_obj_set_style_pad_row(sc, 10, 0);
    lv_obj_t *c = uk_card(sc, 0, 0, UK_CARD_W, 10);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, UK_PAD, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 6, 0);
    s_logd_what = uk_label(c, UF.cj17b, T->t1, 0, 0, "");
    s_logd_change = uk_label(c, UF.cj15, T->t1, 0, 0, "");
    lv_obj_set_width(s_logd_change, lv_pct(100));
    lv_label_set_long_mode(s_logd_change, LV_LABEL_LONG_MODE_WRAP);
    s_logd_meta = uk_label(c, UF.cj13, T->t3, 0, 0, "");
    s_logd_result = uk_label(c, UF.cj15, T->t1, 0, 0, "");
    lv_obj_set_width(s_logd_result, lv_pct(100));
    lv_label_set_long_mode(s_logd_result, LV_LABEL_LONG_MODE_WRAP);
    s_logd_btn = uk_button(sc, 0, 0, UK_CARD_W, 44, "", UK_BTN_PLAIN, logd_undo_cb, NULL, &s_logd_btn_lbl);
    s_logd_why = uk_label(sc, UF.cj13, T->t3, 0, 0, "");
}

static void op_logd_on_open(void)
{
    const op_log_t *o;
    s_logd_arm = 0;
    if (s_log_sel < 0 || s_log_sel >= (s_log_n < 0 ? 0 : s_log_n)) return;
    o = &s_log[s_log_sel];
    lv_label_set_text(s_logd_what, o->what);
    lv_label_set_text(s_logd_change, o->change);
    uk_show(s_logd_change, o->change[0] != 0);
    lv_label_set_text_fmt(s_logd_meta, "%s · %s", o->when, o->source);
    lv_label_set_text(s_logd_result, o->result);
    uk_text_color(s_logd_result, op_mark_col(o->mark));
    /* DD10: no undo for writes that are not transactions (重启、eSIM …) */
    uk_show(s_logd_btn, o->undo_have);
    uk_show(s_logd_why, o->undo_have && !o->undo_ok);
    if (o->undo_have) {
        lv_label_set_text(s_logd_btn_lbl, o->undo_label[0] ? o->undo_label : TR("撤销"));
        uk_button_kind(s_logd_btn, s_logd_btn_lbl, o->undo_ok ? UK_BTN_PLAIN : UK_BTN_PLAIN);
        lv_obj_set_style_opa(s_logd_btn, o->undo_ok ? LV_OPA_COVER : LV_OPA_40, 0);
        lv_label_set_text(s_logd_why, o->undo_why);
    }
}

/* ---- DD8: write controls grey while datad is stuck ----
 * datad answers but its executor has not moved for 20 s (V2-40): a tap
 * would only queue behind it. The banner says why; the controls that write
 * through datad are disabled until it moves again. */
static void op_guard_paint(void)
{
    static int was = -1;
    int stuck = screen_feed_stuck();
    lv_obj_t *o[24];
    int n = 0;

    if (stuck == was) return;
    was = stuck;
    for (int i = 0; i < s_lk_seg.n && n < 24; i++) o[n++] = s_lk_seg.item[i];
    for (int i = 0; i < 3 && n < 24; i++) o[n++] = s_bg[i].apply_btn;
    o[n++] = s_lk_reset_btn;
    o[n++] = s_md_sw[MD_DATA];
    o[n++] = s_md_sw[MD_ROAM];
    o[n++] = s_w_sw[WSW_MASTER];
    o[n++] = s_w_sw[WSW_24];
    o[n++] = s_w_sw[WSW_5];
    o[n++] = s_w_sw[WSW_PSM];   /* through datad since 10-04 (wifi.power_save) */
    o[n++] = s_w_sw[WSW_NFC];
    o[n++] = s_sy_dps_sw;
    for (int i = 0; i < n; i++) {
        if (!o[i]) continue;
        if (stuck) lv_obj_add_state(o[i], LV_STATE_DISABLED);
        else       lv_obj_remove_state(o[i], LV_STATE_DISABLED);
        lv_obj_set_style_opa(o[i], stuck ? LV_OPA_50 : LV_OPA_COVER, 0);
    }
}
