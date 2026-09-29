/*
 * ui_parts/sms.c - 短信子页.
 * Part of src/ui.c, which #includes it: the parts share ui.c's static state
 * and are one translation unit, not compiled on their own (the Makefile
 * lists only ui.c). Split 2026-09-26 so the 6000-line file stops being
 * the place every change collides; the built binary is byte-identical.
 *
 * SPDX-License-Identifier: MIT
 */
/* ---- SMS subpage ---- */
/* A list page's first line: "N 条未读 · 共 M 条" and a 全部已读 pill. */
static void list_toolbar(lv_obj_t *t, lv_obj_t **count, lv_obj_t **btn, lv_event_cb_t cb)
{
    *count = uk_label(t, UF.cj13, T->t2, UK_MARGIN + 6, 14, "");
    lv_obj_t *l;
    *btn = uk_button(t, 0, 8, 0, 30, "全部已读", UK_BTN_PLAIN, cb, NULL, &l);
    lv_obj_set_style_text_font(l, UF.cj13, 0);
    lv_obj_align(*btn, LV_ALIGN_TOP_RIGHT, -UK_MARGIN, 8);
}

static void build_sub_sms(lv_obj_t *t)
{
    t = uk_scroll(t, 0, UI_SUB_VIEW, SMS_TOOLBAR_H + SMS_MAX_ROWS * SMS_ROW_H + 16);
    s_sms_card = t;
    list_toolbar(t, &s_sms_count, &s_sms_allread_btn, sms_allread_cb);
    s_sms_empty = uk_label_w(t, UF.cj14, T->t3, UK_MARGIN + 6, SMS_TOOLBAR_H + 4, UK_CARD_W - 12, 1,
                             "没有短信。新短信会显示在这里，点开可看全文。");
    uk_show(s_sms_empty, 0);
    s_sms_list = uk_card(t, UK_MARGIN, SMS_TOOLBAR_H, UK_CARD_W, SMS_MAX_ROWS * SMS_ROW_H);
    lv_obj_set_style_clip_corner(s_sms_list, true, 0);
    for (int i = 0; i < SMS_MAX_ROWS; i++) {
        s_sms_row_id[i] = -1;
        lv_obj_t *c = uk_box(s_sms_list, 0, i * SMS_ROW_H, UK_CARD_W, SMS_ROW_H, T->card, 0);
        uk_tappable(c, sms_row_click_cb, (void *)(intptr_t)i);
        lv_obj_add_event_cb(c, sms_row_longpress_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        s_sms_row[i] = c;
        if (i) uk_sep(c, 0);
        s_sms_dot[i] = uk_dot(c, 8, 16, 6, T->blue);
        s_sms_num[i] = uk_label_w(c, UF.cj15b, T->t1, 20, 9, 170, 0, "");
        s_sms_date[i] = uk_label_r(c, UF.n12, T->t3, UK_CARD_W - UK_PAD, 11, "");
        s_sms_body[i] = uk_label_w(c, UF.cj13, T->t2, 20, 31, UK_CARD_W - 20 - UK_PAD, 1, "");
        lv_obj_set_height(s_sms_body[i], 2 * lv_font_get_line_height(UF.cj13));
        lv_label_set_long_mode(s_sms_body[i], LV_LABEL_LONG_MODE_DOTS);
        uk_show(c, 0);
    }
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

#define AL_ROW_H 64
static void al_allread_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    alerts_mark_all_read();
}

static void open_alerts_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    sub_open(SUB_ALERTS);
}

/* 页面上半是体检（doctor.sh）里不正常的项，下半是告警记录。系统页「健康」
 * 行的「N 项注意」数的是体检，不是未读告警——以前这页只列告警，点进去全是
 * 已读，看不出注意的是什么（2026-09-25）。 */
#define HC_ROW_H 58
#define HC_TOP 24
#define AL_CHEV_W 16   /* 行尾「›」占的宽度 */

/* 点开一行看全文：列表里体检说明、告警原文都只放得下一行（2026-09-25 用户：
 * 「点进去看不了详情，只能看到预览」）。内容在点的那一刻拷下来。 */
static void alert_detail_open(const char *title, const char *meta, const char *body)
{
    static char c_t[96], c_m[64], c_b[320];
    set_label_fmt(s_ald_title, c_t, sizeof c_t, "%s", title);
    set_label_fmt(s_ald_meta, c_m, sizeof c_m, "%s", meta);
    set_label_fmt(s_ald_body, c_b, sizeof c_b, "%s", body[0] ? body : "（没有更多说明）");
    lv_obj_scroll_to_y(s_ald_scroll, 0, LV_ANIM_OFF);
    sub_open_child(SUB_ALERT_DETAIL, SUB_ALERTS);
}

static void hc_row_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    health_item_t h;
    if (idx >= health_count()) return;
    health_get(idx, &h);
    alert_detail_open(h.label, h.bad ? "■ 体检：异常" : "▲ 体检：需要注意", h.detail);
}

static void al_row_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    alert_item_t a;
    char meta[64];
    if (idx >= alerts_count()) return;
    alerts_get(idx, &a);
    if (a.time > 0) {
        /* Device clock = local wall time under TZ=UTC: localtime gives the right digits. */
        time_t tt = (time_t)a.time;
        struct tm tm;
        localtime_r(&tt, &tm);
        strftime(meta, sizeof meta, "告警 · %Y-%m-%d %H:%M:%S", &tm);
    } else {
        snprintf(meta, sizeof meta, "告警 · 开机后 %ld 分钟", a.uptime / 60);
    }
    alert_detail_open(a.label, meta, a.text);
}

static void build_sub_alerts(lv_obj_t *t)
{
    t = s_al_scroll = uk_scroll(t, 0, UI_SUB_VIEW,
                                HC_TOP + HEALTH_MAX * HC_ROW_H + 30 + SMS_TOOLBAR_H + ALERTS_MAX * AL_ROW_H + 16);
    uk_section(t, 4, "体检");
    s_hc_card = uk_card(t, UK_MARGIN, HC_TOP, UK_CARD_W, UK_ROW_H);
    s_hc_none = uk_label_w(s_hc_card, UF.cj14, T->t2, UK_PAD, 11, UK_CARD_W - 2 * UK_PAD, 0, "读取中…");
    for (int i = 0; i < HEALTH_MAX; i++) {
        lv_obj_t *c = uk_box(s_hc_card, 0, i * HC_ROW_H, UK_CARD_W, HC_ROW_H, T->card, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
        uk_tappable(c, hc_row_click_cb, (void *)(intptr_t)i);
        s_hc_row[i] = c;
        if (i) uk_sep(c, 0);
        s_hc_mark[i] = uk_label(c, UF.cj12, T->warnT, UK_PAD, 11, "▲");
        s_hc_label[i] = uk_label_w(c, UF.cj14, T->t1, UK_PAD + 18, 9, UK_CARD_W - 2 * UK_PAD - 18 - AL_CHEV_W, 0, "");
        s_hc_detail[i] = uk_label_w(c, UF.cj12, T->t2, UK_PAD + 18, 31, UK_CARD_W - 2 * UK_PAD - 18 - AL_CHEV_W, 0, "");
        uk_label_r(c, UF.cj15, T->t3, UK_CARD_W - UK_PAD, 18, "›");
        lv_obj_set_height(s_hc_detail[i], lv_font_get_line_height(UF.cj12));
        lv_label_set_long_mode(s_hc_detail[i], LV_LABEL_LONG_MODE_DOTS);
        uk_show(c, 0);
    }
    /* 告警记录：整块随体检的高度上下移 */
    s_al_body = uk_box(t, 0, HC_TOP + UK_ROW_H + 10, UI_W, SMS_TOOLBAR_H + ALERTS_MAX * AL_ROW_H, T->bg, 0);
    lv_obj_set_style_bg_opa(s_al_body, LV_OPA_TRANSP, 0);
    t = s_al_body;
    list_toolbar(t, &s_al_count, &s_al_allread_btn, al_allread_cb);
    s_al_empty = uk_label_w(t, UF.cj14, T->t3, UK_MARGIN + 6, SMS_TOOLBAR_H + 4, UK_CARD_W - 12, 1, "");
    s_al_list = uk_card(t, UK_MARGIN, SMS_TOOLBAR_H, UK_CARD_W, ALERTS_MAX * AL_ROW_H);
    for (int i = 0; i < ALERTS_MAX; i++) {
        lv_obj_t *c = uk_box(s_al_list, 0, i * AL_ROW_H, UK_CARD_W, AL_ROW_H, T->card, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
        uk_tappable(c, al_row_click_cb, (void *)(intptr_t)i);
        s_al_row[i] = c;
        if (i) uk_sep(c, 0);
        s_al_mark[i] = uk_label(c, UF.cj12, T->warnT, UK_PAD, 12, "▲");
        s_al_label[i] = uk_label_w(c, UF.cj14, T->t1, UK_PAD + 18, 10, UK_CARD_W - 2 * UK_PAD - 18 - AL_CHEV_W, 0, "");
        s_al_time[i] = uk_label_r(c, UF.n12, T->t3, UK_CARD_W - UK_PAD - AL_CHEV_W, 36, "");
        s_al_text[i] = uk_label_w(c, UF.n11, T->t3, UK_PAD + 18, 37, UK_CARD_W - 2 * UK_PAD - 18 - 90 - AL_CHEV_W, 0, "");
        lv_obj_set_height(s_al_text[i], lv_font_get_line_height(UF.n11));
        lv_label_set_long_mode(s_al_text[i], LV_LABEL_LONG_MODE_DOTS);
        uk_label_r(c, UF.cj15, T->t3, UK_CARD_W - UK_PAD, 20, "›");
        uk_show(c, 0);
    }
    lv_obj_scroll_to_y(s_al_scroll, 0, LV_ANIM_OFF);
}

/* 同短信详情：一张卡，标题 + 类别/时间 + 全文 */
static void build_sub_alert_detail(lv_obj_t *t)
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
    s_ald_scroll = sc;

    lv_obj_t *c = uk_card(sc, 0, 0, UK_CARD_W, 10);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, UK_PAD, 0);
    lv_obj_set_style_pad_bottom(c, 18, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 6, 0);
    s_ald_title = uk_label(c, UF.cj17b, T->t1, 0, 0, "");
    lv_obj_set_width(s_ald_title, lv_pct(100));
    lv_label_set_long_mode(s_ald_title, LV_LABEL_LONG_MODE_WRAP);
    s_ald_meta = uk_label(c, UF.cj13, T->t3, 0, 0, "");
    s_ald_body = uk_label(c, UF.cj15, T->t1, 0, 0, "");
    lv_obj_set_width(s_ald_body, lv_pct(100));
    lv_obj_set_style_text_line_space(s_ald_body, 6, 0);
    lv_label_set_long_mode(s_ald_body, LV_LABEL_LONG_MODE_WRAP);
}

static void build_sub_sms_detail(lv_obj_t *t)
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
    s_smsd_scroll = sc;

    lv_obj_t *c = uk_card(sc, 0, 0, UK_CARD_W, 10);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, UK_PAD, 0);
    lv_obj_set_style_pad_bottom(c, 18, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 6, 0);
    s_smsd_num = uk_label(c, UF.cj17b, T->t1, 0, 0, "");
    s_smsd_date = uk_label(c, UF.n12, T->t3, 0, 0, "");
    s_smsd_body = uk_label(c, UF.cj15, T->t1, 0, 0, "");
    lv_obj_set_width(s_smsd_body, lv_pct(100));
    lv_obj_set_style_text_line_space(s_smsd_body, 6, 0);
    lv_label_set_long_mode(s_smsd_body, LV_LABEL_LONG_MODE_WRAP);

    s_smsd_del_btn = uk_button(sc, 0, 0, UK_CARD_W, 40, "删除这条", UK_BTN_DANGER, smsd_delete_cb, NULL, &s_smsd_del_lbl);
}

