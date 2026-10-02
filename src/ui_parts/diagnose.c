/*
 * ui_parts/diagnose.c - 网络诊断 and 摆放模式 (manager docs/designs/
 * slow-diagnosis.md §4.2, §12.2–12.5; decisions 3A 4A 5A 6A 9A 10A).
 * Part of src/ui.c, which #includes it (one translation unit, see home.c).
 *
 * SPDX-License-Identifier: MIT
 */

/* ======================= 网络诊断 =======================
 * zte-agent runs it (deep_diag.rs, about 10 s, ≤ 20 s); this page starts it,
 * reads it every second while it runs and draws what it says. The run goes on
 * when the page is left and is kept 10 minutes: coming back within that shows
 * it with when it was measured; after that, opening the page measures again.
 * Status block (72) = the main cause and one thing to do; one card, a row per
 * layer; then 加测速度 and 对 / 不对. */
#define DG_ROWS (DG_LAYERS_MAX + 1)          /* + the speed row */
#define DG_ARM_MS 5000
#define DG_NOTE_MS 4000
typedef struct { lv_obj_t *box, *sep, *name, *val, *detail, *act; int act_to; } dg_row_t;
enum { DG_ACT_NONE, DG_ACT_PLACE, DG_ACT_PROXY };
static dg_row_t  s_dg_row[DG_ROWS];
static uk_hero_t s_dg_hero;
static lv_obj_t *s_dg_scroll, *s_dg_card, *s_dg_btn, *s_dg_btn_lbl, *s_dg_note, *s_dg_fb, *s_dg_fb_lbl,
                *s_dg_yes, *s_dg_no, *s_dg_hdr, *s_dg_hdr_lbl;
static int       s_dg_pending;               /* the start tap painted "正在检查…", the reply not in yet */
static uint32_t  s_dg_arm;                   /* roaming: first tap on 加测速度 */
static uint32_t  s_dg_note_at;
static uint32_t  s_dg_speed_t0;              /* when our speed request went out (for "3 s") */
static int       s_dg_fb_local = -1;         /* tapped 对/不对 for this run id */
static long      s_dg_fb_id;
static int       s_net_roam;                 /* roaming now (datad), for the speed confirm */
static void diag_paint(void);

/* lv_label_set_text frees and reallocates even for the same text (DESIGN §4 pitfalls) */
static void lbl_set(lv_obj_t *l, const char *t)
{
    if (strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

static void dg_note(const char *t, uint32_t col)
{
    static char c[192];
    snprintf(c, sizeof c, "%s", t ? t : "");
    lv_label_set_text_static(s_dg_note, c);
    uk_text_color(s_dg_note, col);
    s_dg_note_at = c[0] ? tick_nz() : 0;
}

/* device clock seconds → "14:32" (local time labelled UTC: localtime's digits) */
static void dg_hm(long t, char *out, size_t n)
{
    time_t tt = (time_t)t;
    struct tm tm;
    snprintf(out, n, "--:--");
    if (t > 0 && localtime_r(&tt, &tm)) strftime(out, n, "%H:%M", &tm);
}

static int dg_going(const diag_run_t *r) { return r->state == DG_WAITING || r->state == DG_RUN; }

/* Paint "正在检查…" now, then send: the request is synchronous (DESIGN §4). */
static void dg_start(void)
{
    s_dg_pending = 1;
    s_dg_arm = 0;
    dg_note("", T->t3);
    diag_paint();
    lv_refr_now(NULL);
    int ok = diagnose_start();
    s_dg_pending = 0;
    if (!ok && !diagnose_agent_err()) dg_note(diagnose_error(), T->badT);
    diag_paint();
}

static void dg_start_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (dg_going(diagnose_run()) && !diagnose_agent_err()) { dg_note(TR("正在查，稍等"), T->t2); diag_paint(); return; }
    dg_start();
}

/* Opening the page: within 10 minutes of the last run show it; else measure. */
static void diag_on_open(void)
{
    /* the read is synchronous: draw what we have first, so the tap is answered */
    diag_paint();
    lv_refr_now(NULL);
    diagnose_kick();
    diagnose_poll(1);
    dg_note("", T->t3);
    s_dg_arm = 0;
    if (diagnose_idle() && !diagnose_agent_err()) dg_start();
    else diag_paint();
}

static void dg_speed_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    const diag_run_t *r = diagnose_run();
    if (diagnose_agent_err()) { dg_start(); return; }          /* the button reads 重试 then */
    if (r->state != DG_DONE) { dg_note(TR("正在查，稍等"), T->t2); diag_paint(); return; }
    if (r->has_speed && r->speed.level == DG_RUNNING) { dg_note(TR("正在测，稍等"), T->t2); diag_paint(); return; }
    uint32_t now = lv_tick_get();
    if (s_net_roam && !(s_dg_arm && now - s_dg_arm < DG_ARM_MS)) {
        s_dg_arm = (now ? now : 1);                                       /* first tap while roaming */
        diag_paint();
        return;
    }
    s_dg_arm = 0;
    s_dg_speed_t0 = (now ? now : 1);
    dg_note(TR("已发送，测速约 5 秒"), T->t2);
    diag_paint();
    lv_refr_now(NULL);
    if (!diagnose_speed()) {
        s_dg_speed_t0 = 0;
        dg_note(diagnose_error()[0] ? diagnose_error() : TR("后台没回应"), T->badT);
    }
    diag_paint();
}

static void dg_fb_cb(lv_event_t *e)
{
    int right = (int)(intptr_t)lv_event_get_user_data(e);
    const diag_run_t *r = diagnose_run();
    s_dg_fb_local = right;
    s_dg_fb_id = r->id;
    diag_paint();                                   /* 已记下，谢谢 at once */
    lv_refr_now(NULL);
    if (!diagnose_feedback(right)) {
        s_dg_fb_local = -1;
        dg_note(TR("没记上：后台没回应，可再点一次"), T->badT);
        diag_paint();
    }
}

static void dg_act_cb(lv_event_t *e)
{
    int to = ((dg_row_t *)lv_event_get_user_data(e))->act_to;
    if (to == DG_ACT_PLACE) sub_open_child(SUB_PLACE, SUB_DIAG);
}

/* The title bar's 再查一次 (on the subpage header, only for this page). */
static void diag_hdr_sync(void)
{
    if (!s_dg_hdr) return;
    /* always there on this page (a tap while it runs says so); the agent not
     * answering puts 重试 on the page instead */
    uk_show(s_dg_hdr, s_sub_cur == SUB_DIAG && !diagnose_agent_err());
}

static void build_sub_diag(lv_obj_t *t)
{
    t = s_dg_scroll = uk_scroll(t, 0, UI_SUB_VIEW, 700);
    s_dg_card = uk_card(t, UK_MARGIN, 4, UK_CARD_W, UK_HERO_H + 6 * UK_ROW_H);
    lv_obj_t *c = s_dg_card;
    uk_hero(&s_dg_hero, c, UF.cj22b);
    /* main cause on top, the one thing to do under it (two lines at most) */
    lv_obj_set_pos(s_dg_hero.big, UK_PAD, 6);
    lv_obj_set_size(s_dg_hero.big, UK_CARD_W - 2 * UK_PAD, lv_font_get_line_height(UF.cj22b));
    lv_label_set_long_mode(s_dg_hero.big, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(s_dg_hero.st, UK_PAD, 38);
    lv_obj_set_width(s_dg_hero.st, UK_CARD_W - 2 * UK_PAD);
    lv_label_set_long_mode(s_dg_hero.st, LV_LABEL_LONG_MODE_WRAP);
    uk_show(s_dg_hero.dot, 0);
    uk_show(s_dg_hero.unit, 0);
    uk_show(s_dg_hero.rtop, 0);
    uk_show(s_dg_hero.r1, 0);
    uk_show(s_dg_hero.r2, 0);
    uk_hero_tone(&s_dg_hero, 3);
    for (int i = 0; i < DG_ROWS; i++) {
        dg_row_t *w = &s_dg_row[i];
        w->box = uk_box(c, 0, UK_HERO_H + i * UK_ROW_H, UK_CARD_W, UK_ROW_H, T->card, 0);
        lv_obj_set_style_bg_opa(w->box, LV_OPA_TRANSP, 0);
        w->sep = uk_sep(w->box, 0);
        w->name = uk_label(w->box, UF.cj14, T->t1, UK_PAD, 11, "");
        w->val = uk_label_r(w->box, UF.cj14, T->t3, UK_CARD_W - UK_PAD, 11, "");
        w->detail = uk_label_w(w->box, UF.cj12, T->t2, UK_PAD, 32, UK_CARD_W - 2 * UK_PAD, 0, "");
        lv_label_set_long_mode(w->detail, LV_LABEL_LONG_MODE_DOTS);
        w->act = uk_label(w->box, UF.cj13, T->accT, UK_PAD, 0, "");
        uk_tappable(w->act, dg_act_cb, w);
        lv_obj_set_ext_click_area(w->act, 12);
        uk_show(w->detail, 0);
        uk_show(w->act, 0);
        uk_show(w->box, 0);
    }
    s_dg_btn = uk_button(t, UK_MARGIN, 0, UK_CARD_W, 44, TR("加测速度 · 约 5 秒、最多 30 MB"), UK_BTN_PLAIN,
                         dg_speed_cb, NULL, &s_dg_btn_lbl);
    s_dg_note = uk_label_w(t, UF.cj12, T->t3, UK_MARGIN + 6, 0, UK_CARD_W - 12, 1, "");
    /* 「14:32 测 · 结论对吗？」 + 对 / 不对 (each ≥ 40×40) */
    s_dg_fb = uk_box(t, UK_MARGIN, 0, UK_CARD_W, 44, T->bg, 0);
    s_dg_fb_lbl = uk_label_w(s_dg_fb, UF.cj13, T->t2, 6, 12, 150, 0, "");
    s_dg_no = uk_button(s_dg_fb, UK_CARD_W - 64, 0, 64, 44, TR("不对"), UK_BTN_PLAIN, dg_fb_cb, (void *)(intptr_t)0, NULL);
    s_dg_yes = uk_button(s_dg_fb, UK_CARD_W - 64 - 8 - 64, 0, 64, 44, TR("对"), UK_BTN_PLAIN, dg_fb_cb, (void *)(intptr_t)1, NULL);
    /* title bar, right: 再查一次 (glass, like the ‹ button) */
    {
        lv_obj_t *b = s_dg_hdr = uk_box(s_sub_layer, 0, 3, 10, 30, T->glass, 15);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_black(), 0);
        lv_obj_set_style_border_opa(b, T->rim_opa, 0);
        s_dg_hdr_lbl = uk_label(b, UF.cj14, T->accT, 12, 6, TR("再查一次"));
        lv_obj_update_layout(s_dg_hdr_lbl);
        int w = (int)lv_obj_get_width(s_dg_hdr_lbl) + 24;
        lv_obj_set_size(b, w, 30);
        lv_obj_set_x(b, UK_W - UK_MARGIN - w);
        uk_tappable(b, dg_start_cb, NULL);
        lv_obj_set_ext_click_area(b, 6);
        uk_show(b, 0);
    }
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* One layer row: name left; right the mark + word (+ value when it fits on
 * the line, else on a second line). Returns the row's height. */
static int dg_row_paint(dg_row_t *w, const dg_layer_t *l, const char *name, int first, int act, const char *act_txt)
{
    char right[256];
    uint32_t col = T->t3, ncol = l->counted ? T->t1 : T->t3;
    const char *mark = diag_level_mark(l->level), *word = diag_level_word(l->level);
    int h = UK_ROW_H, two = 0;
    w->act_to = act;
    if (l->level == DG_OK)   col = T->okT;
    if (l->level == DG_WARN) col = T->warnT;
    if (l->level == DG_BAD)  col = T->badT;
    if (l->level == DG_INFO) col = T->t1;
    if (!l->counted) col = T->t3;
    if (l->level == DG_INFO)
        snprintf(right, sizeof right, "%s", l->detail);
    else if (l->level == DG_RUNNING && !strcmp(l->id, "speed") && s_dg_speed_t0)
        snprintf(right, sizeof right, TR("测试中… %u 秒"), (unsigned)((lv_tick_get() - s_dg_speed_t0) / 1000));
    else if (l->level == DG_PENDING || l->level == DG_RUNNING || !l->detail[0])
        snprintf(right, sizeof right, "%s%s%s", mark, mark[0] ? " " : "", word);
    else
        snprintf(right, sizeof right, "%s%s%s · %s", mark, mark[0] ? " " : "", word, l->detail);
    lbl_set(w->name, name);
    lv_obj_update_layout(w->name);
    int avail = UK_CARD_W - 2 * UK_PAD - (int)lv_obj_get_width(w->name) - 12;
    lv_point_t sz;
    lv_text_get_size(&sz, right, UF.cj14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (sz.x > avail && l->detail[0] && l->level != DG_PENDING && l->level != DG_RUNNING) {
        two = 1;   /* the value on its own line, the word stays beside the name */
        if (l->level == DG_INFO) snprintf(right, sizeof right, "%s", "");
        else snprintf(right, sizeof right, "%s%s%s", mark, mark[0] ? " " : "", word);
        lbl_set(w->detail, l->detail);
        uk_text_color(w->detail, l->counted && l->level != DG_NA ? T->t2 : T->t3);
        h = 54;
    }
    lbl_set(w->val, right);
    lv_obj_set_style_max_width(w->val, avail, 0);
    lv_label_set_long_mode(w->val, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(w->val, lv_font_get_line_height(UF.cj15));
    uk_text_color(w->val, col);
    uk_text_color(w->name, ncol);
    uk_show(w->detail, two);
    uk_show(w->sep, !first);
    uk_show(w->act, act != DG_ACT_NONE);
    if (act != DG_ACT_NONE) {
        lbl_set(w->act, act_txt);
        lv_obj_set_y(w->act, h - 6);
        h += 26;
    }
    lv_obj_set_height(w->box, h);
    return h;
}

static void diag_paint(void)
{
    static char c_big[128], c_st[256], c_btn[96], c_fb[96];
    if (!s_dg_card) return;
    const diag_run_t *r = diagnose_run();
    int err = diagnose_agent_err();
    uint32_t now = lv_tick_get();
    if (s_dg_arm && now - s_dg_arm >= DG_ARM_MS) s_dg_arm = 0;
    if (s_dg_note_at && now - s_dg_note_at >= DG_NOTE_MS) dg_note("", T->t3);

    /* rows: what the run has; before the first reply, the layers it will have */
    diag_run_t local;
    if (s_dg_pending || r->state == DG_NONE || r->state == DG_IDLE) {
        static const char *const ids[] = {
            "wifi", "signal", "limit", "link", "crowd",
        };
        int nids = (int)(sizeof ids / sizeof *ids);
        memset(&local, 0, sizeof local);
        local.state = s_dg_pending ? DG_RUN : r->state;
        local.feedback = -1;
        for (int i = 0; i < nids; i++) {
            snprintf(local.layer[i].id, sizeof local.layer[i].id, "%s", ids[i]);
            local.layer[i].level = DG_PENDING;
            local.layer[i].counted = 1;
        }
        local.n = local.steps = nids;
        r = &local;
    }

    /* ---- status block ---- */
    int tone = 3;
    const char *big = "", *st = "";
    char more[160] = "";
    if (err) {
        tone = 7;
        big = TR("后台没回应");
        st = err == 2 ? TR("登录管理后台失败，点下面重试") : TR("管理后台没响应，点下面重试");
    } else if (s_dg_pending || r->state == DG_RUN) {
        static char b[64];
        snprintf(b, sizeof b, TR("正在检查… %d/%d"), s_dg_pending ? 0 : r->step, r->steps);
        big = b;
        st = TR("约 10 秒，会发少量探测包");
    } else if (r->state == DG_WAITING) {
        big = TR("等另一个操作做完…");
        st = TR("做完自动开始");
    } else if (r->state == DG_DONE && r->has_main) {
        int any_ok = 0;
        for (int i = 0; i < r->n; i++) any_ok |= r->layer[i].level == DG_OK;
        tone = r->main_has_level ? (r->main_level == DG_BAD ? 7 : 6) : any_ok ? 5 : 3;
        big = r->main_text;
        st = r->main_action;
        if (r->more > 0) {
            snprintf(more, sizeof more, "%s%s", st, st[0] ? " · " : "");
            size_t n = strlen(more);
            snprintf(more + n, sizeof more - n, TR("另有 %d 处疑点"), r->more);
            st = more;
        }
    } else {
        big = TR("正在读取…");
    }
    static int c_tone = -1;
    if (tone != c_tone) { c_tone = tone; uk_hero_tone(&s_dg_hero, tone); }
    if (tone == 3) uk_text_color(s_dg_hero.st, T->t2);
    set_label_fmt(s_dg_hero.big, c_big, sizeof c_big, "%s", big);
    set_label_fmt(s_dg_hero.st, c_st, sizeof c_st, "%s", st);

    /* ---- one row per layer, then speed ---- */
    int y = UK_HERO_H, k = 0;
    for (int i = 0; i < r->n && k < DG_ROWS - 1; i++, k++) {
        const dg_layer_t *l = &r->layer[i];
        int act = DG_ACT_NONE;
        const char *act_txt = "";
        /* the signal row, when it is the trouble, leads to 摆放模式 — unless the
         * agent said its cause is something placement can't fix (narrow carrier,
         * no service: main is the signal and points nowhere) */
        if (r->state == DG_DONE && !strcmp(l->id, "signal") && (l->level == DG_WARN || l->level == DG_BAD) &&
            (strcmp(r->main_layer, "signal") || !strcmp(r->action_to, "placement"))) {
            act = DG_ACT_PLACE;
            act_txt = TR("固定位置时用摆放模式 ›");
        }
        uk_show(s_dg_row[k].box, 1);
        lv_obj_set_y(s_dg_row[k].box, y);
        y += dg_row_paint(&s_dg_row[k], l, diag_layer_name(l->id), k == 0, act, act_txt);
    }
    if (r->has_speed && r->state == DG_DONE) {
        uk_show(s_dg_row[k].box, 1);
        lv_obj_set_y(s_dg_row[k].box, y);
        y += dg_row_paint(&s_dg_row[k], &r->speed, diag_layer_name("speed"), k == 0, DG_ACT_NONE, "");
        k++;
    }
    for (; k < DG_ROWS; k++) uk_show(s_dg_row[k].box, 0);
    lv_obj_set_height(s_dg_card, y);
    y += 4 + 10;

    /* ---- 加测速度 / 重试 ---- */
    int done = r->state == DG_DONE && !s_dg_pending;
    int show_btn = err || done;
    uk_show(s_dg_btn, show_btn);
    if (show_btn) {
        uk_btn_kind_t kind = UK_BTN_PLAIN;
        if (err) { kind = UK_BTN_PRIMARY; set_label_fmt(s_dg_btn_lbl, c_btn, sizeof c_btn, "%s", TR("重试")); }
        else if (s_dg_arm) { kind = UK_BTN_ARMED; set_label_fmt(s_dg_btn_lbl, c_btn, sizeof c_btn, "%s", TR("走漫游流量，再按一次")); }
        else if (r->has_speed && r->speed.level == DG_RUNNING)
            set_label_fmt(s_dg_btn_lbl, c_btn, sizeof c_btn, "%s", TR("测速中…"));
        else set_label_fmt(s_dg_btn_lbl, c_btn, sizeof c_btn, "%s", TR("加测速度 · 约 5 秒、最多 30 MB"));
        uk_button_kind(s_dg_btn, s_dg_btn_lbl, kind);
        lv_obj_set_y(s_dg_btn, y);
        y += 44 + 6;
    }
    int has_note = lv_label_get_text(s_dg_note)[0] != 0;
    uk_show(s_dg_note, has_note);
    if (has_note) {
        lv_obj_set_y(s_dg_note, y);
        lv_obj_update_layout(s_dg_note);
        y += (int)lv_obj_get_height(s_dg_note) + 6;
    }

    /* ---- 14:32 测 · 对 / 不对 ---- */
    uk_show(s_dg_fb, done && !err);
    if (done && !err) {
        char hm[8];
        dg_hm(r->finished_at, hm, sizeof hm);
        int given = r->feedback >= 0 || (s_dg_fb_local >= 0 && s_dg_fb_id == r->id);
        set_label_fmt(s_dg_fb_lbl, c_fb, sizeof c_fb, given ? TR("%s 测 · 已记下，谢谢") : TR("%s 测 · 结论对吗？"), hm);
        lv_obj_set_width(s_dg_fb_lbl, given ? UK_CARD_W - 12 : UK_CARD_W - 2 * 64 - 8 - 12);
        uk_show(s_dg_yes, !given);
        uk_show(s_dg_no, !given);
        lv_obj_set_y(s_dg_fb, y);
        y += 44 + 10;
    }
    uk_scroll_extent(s_dg_scroll, y + 16);
    diag_hdr_sync();
}

/* ======================= 摆放模式 =======================
 * Moving a device that stays put (a window sill, a shelf): the main radio's
 * SINR, as a 3-second median, big; the best this session and how far off it
 * we are now — facts only, no "move left". A new cell (PCI, band or RAT)
 * starts the best over. The screen stays on while the page is up (power.c). */
#define PL_SAMPLES 3
static lv_obj_t *s_pl_cap, *s_pl_big, *s_pl_word, *s_pl_note, *s_pl_best, *s_pl_cmp, *s_pl_cell, *s_pl_btn;
static struct {
    int  v[PL_SAMPLES], n, head;     /* SINR in tenths of a dB, last 3 seconds   */
    unsigned long long ver;          /* datad snapshot the last sample came from */
    int  best, have_best;            /* tenths                                    */
    long best_at;                    /* wall clock                                */
    char key[48];                    /* RAT · band · PCI the best belongs to      */
    uint32_t reset_at;               /* "换了小区，重新计" shown since            */
    int  manual;                     /* the reset was the button                  */
} s_pl;

static char c_pl_cap[40], c_pl_big[16], c_pl_word[64], c_pl_note[96], c_pl_best[40], c_pl_cmp[64], c_pl_cell[48];
static uint32_t c_pl_col = 1;

static void pl_restart(void)
{
    s_pl.n = s_pl.head = 0;
    s_pl.have_best = 0;
    s_pl.reset_at = tick_nz();
}

/* Opening the page: 这次最好 is this visit's, not a leftover from the last one. */
static void place_on_open(void)
{
    pl_restart();
    s_pl.reset_at = 0;
    s_pl.key[0] = 0;
    s_pl.manual = 0;
}

static void pl_reset_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    pl_restart();
    s_pl.manual = 1;                                 /* answered at once, the numbers follow */
    set_label_fmt(s_pl_best, c_pl_best, sizeof c_pl_best, "%s", "—");
    set_label_fmt(s_pl_cmp, c_pl_cmp, sizeof c_pl_cmp, "%s", "—");
    uk_text_color(s_pl_cmp, T->t3);
    set_label_fmt(s_pl_note, c_pl_note, sizeof c_pl_note, "%s", TR("已清零，重新计"));
}

/* "17.7" / "-2.5" → tenths; 0 = no number */
static int pl_tenths(const char *s, int *out)
{
    char *end;
    if (!s || !s[0]) return 0;
    double v = strtod(s, &end);
    if (end == s) return 0;
    *out = (int)(v * 10.0 + (v < 0 ? -0.5 : 0.5));
    return 1;
}

/* tenths → "17.7" / "-2.5" (no %f: LVGL's printf has none) */
static void pl_fmt(char *out, size_t n, int t)
{
    int a = t < 0 ? -t : t;
    snprintf(out, n, "%s%d.%d", t < 0 ? "-" : "", a / 10, a % 10);
}

static void build_sub_place(lv_obj_t *t)
{
    t = uk_scroll(t, 0, UI_SUB_VIEW, 4 + 150 + 10 + 3 * UK_ROW_H + 10 + 44 + 10 + 40 + 16);
    lv_obj_t *c = uk_card(t, UK_MARGIN, 4, UK_CARD_W, 150);
    s_pl_cap = uk_label(c, UF.cj13, T->t2, 0, 14, "");
    lv_obj_set_width(s_pl_cap, UK_CARD_W);
    lv_obj_set_style_text_align(s_pl_cap, LV_TEXT_ALIGN_CENTER, 0);
    s_pl_big = uk_label(c, UF.n36, T->t1, 0, 36, "--");
    lv_obj_set_width(s_pl_big, UK_CARD_W);
    lv_obj_set_style_text_align(s_pl_big, LV_TEXT_ALIGN_CENTER, 0);
    s_pl_word = uk_label(c, UF.cj14, T->t2, 0, 92, "");
    lv_obj_set_width(s_pl_word, UK_CARD_W);
    lv_obj_set_style_text_align(s_pl_word, LV_TEXT_ALIGN_CENTER, 0);
    s_pl_note = uk_label_w(c, UF.cj12, T->t3, UK_PAD, 120, UK_CARD_W - 2 * UK_PAD, 0, "");
    lv_obj_set_style_text_align(s_pl_note, LV_TEXT_ALIGN_CENTER, 0);
    int y = 4 + 150 + 10;
    lv_obj_t *rc = uk_card(t, UK_MARGIN, y, UK_CARD_W, 3 * UK_ROW_H);
    s_pl_best = uk_row(rc, 0, TR("这次最好"), 1);
    s_pl_cmp = uk_row(rc, UK_ROW_H, TR("对比"), 0);
    s_pl_cell = uk_row(rc, 2 * UK_ROW_H, TR("在用"), 0);
    lv_obj_set_style_text_font(s_pl_cmp, UF.cj14, 0);
    y += 3 * UK_ROW_H + 10;
    s_pl_btn = uk_button(t, UK_MARGIN, y, UK_CARD_W, 44, TR("重新开始"), UK_BTN_PLAIN, pl_reset_cb, NULL, NULL);
    y += 44 + 10;
    uk_label_w(t, UF.cj12, T->t3, UK_MARGIN + 6, y, UK_CARD_W - 12, 1, TR("换了小区会重新计；这页开着不息屏"));
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* live = datad answered this second; d is its snapshot (or the last one). */
static void place_paint(const devui_data_t *d, int live)
{
    if (!s_pl_big) return;
    ui_rat_t rat = ui_rat(d->net_type);
    int nr = rat == UI_RAT_5G_SA || rat == UI_RAT_5G_NSA;
    const char *snr = nr ? d->nr_snr : d->lte_snr;
    int pci = nr ? d->nr_pci : d->lte_pci;
    char band[16] = "";
    if (nr) snprintf(band, sizeof band, "%s", d->nr_band);
    else if (d->band[0]) ui_band_short(d->band, 0, band, sizeof band);
    int v = 0, have = live && d->valid && rat != UI_RAT_NONE && pl_tenths(snr, &v);
    int lte_or_nr = nr || rat == UI_RAT_4G;

    set_label_fmt(s_pl_cap, c_pl_cap, sizeof c_pl_cap, TR("%s SINR · 越大越好"), nr ? "5G" : "4G");
    /* a different cell: the best so far was somewhere else */
    char key[48];
    snprintf(key, sizeof key, "%d·%s·%d", (int)rat, band, pci);
    if (have && strcmp(key, s_pl.key)) {
        if (s_pl.key[0]) { pl_restart(); s_pl.manual = 0; }
        snprintf(s_pl.key, sizeof s_pl.key, "%s", key);
    }
    unsigned long long ver = data_backend_version();
    if (have && lte_or_nr && ver != s_pl.ver) {
        s_pl.ver = ver;
        s_pl.v[s_pl.head] = v;
        s_pl.head = (s_pl.head + 1) % PL_SAMPLES;
        if (s_pl.n < PL_SAMPLES) s_pl.n++;
    }
    int med = 0;
    if (s_pl.n) {
        int a[PL_SAMPLES], n = s_pl.n;
        memcpy(a, s_pl.v, sizeof a);
        for (int i = 1; i < n; i++)                  /* n ≤ 3: insertion sort */
            for (int j = i; j > 0 && a[j - 1] > a[j]; j--) { int x = a[j]; a[j] = a[j - 1]; a[j - 1] = x; }
        med = a[(n - 1) / 2];                         /* two readings: the lower one */
        if (live && have && (!s_pl.have_best || med > s_pl.best)) {
            s_pl.best = med;
            s_pl.have_best = 1;
            s_pl.best_at = (long)time(NULL);
        }
    }

    char num[16];
    uint32_t col = T->t3;
    const char *word = "", *mark = "";
    if (!live && s_pl.n) {
        /* datad stopped: the last number stays, dimmed, with when */
        char hm[8] = "--:--";
        long alive = data_backend_alive_wall();
        dg_hm(alive ? alive : s_last_valid_wall, hm, sizeof hm);
        pl_fmt(num, sizeof num, med);
        set_label_fmt(s_pl_big, c_pl_big, sizeof c_pl_big, "%s", num);
        set_label_fmt(s_pl_word, c_pl_word, sizeof c_pl_word, TR("数字停在 %s"), hm);
    } else if (!have || !lte_or_nr) {
        set_label_fmt(s_pl_big, c_pl_big, sizeof c_pl_big, "%s", "--");
        set_label_fmt(s_pl_word, c_pl_word, sizeof c_pl_word, "%s", !live ? TR("读不到数据") : !lte_or_nr && rat != UI_RAT_NONE
                      ? TR("这个制式没有 SINR") : TR("没有信号"));
        s_pl.n = 0;
    } else {
        if (med >= 200)      { word = TR("信号很好"); col = T->okT; mark = "●"; }
        else if (med >= 130) { word = TR("信号良好"); col = T->okT; mark = "●"; }
        else if (med >= 0)   { word = TR("信号一般"); col = T->warnT; mark = "▲"; }
        else                 { word = TR("信号较差"); col = T->badT; mark = "■"; }
        pl_fmt(num, sizeof num, med);
        set_label_fmt(s_pl_big, c_pl_big, sizeof c_pl_big, "%s", num);
        int rsrp = nr ? d->nr_rsrp : d->lte_rsrp;
        if (rsrp) set_label_fmt(s_pl_word, c_pl_word, sizeof c_pl_word, "%s %s · RSRP %d", mark, word, rsrp);
        else      set_label_fmt(s_pl_word, c_pl_word, sizeof c_pl_word, "%s %s", mark, word);
    }
    if (col != c_pl_col) { c_pl_col = col; uk_text_color(s_pl_big, col); uk_text_color(s_pl_word, col); }

    if (s_pl.reset_at && lv_tick_get() - s_pl.reset_at < 10000)
        set_label_fmt(s_pl_note, c_pl_note, sizeof c_pl_note, "%s", s_pl.manual ? TR("已清零，重新计") : TR("换了小区，重新计"));
    else
        set_label_fmt(s_pl_note, c_pl_note, sizeof c_pl_note, "%s", "");

    if (s_pl.have_best) {
        char b[16], hm[8];
        pl_fmt(b, sizeof b, s_pl.best);
        dg_hm(s_pl.best_at, hm, sizeof hm);
        set_label_fmt(s_pl_best, c_pl_best, sizeof c_pl_best, "%s · %s", b, hm);
        int gap = s_pl.best - med;
        if (!s_pl.n) set_label_fmt(s_pl_cmp, c_pl_cmp, sizeof c_pl_cmp, "%s", "—");
        else if (gap <= 10) set_label_fmt(s_pl_cmp, c_pl_cmp, sizeof c_pl_cmp, "%s", TR("● 接近最好"));
        else { char g[16]; pl_fmt(g, sizeof g, gap); set_label_fmt(s_pl_cmp, c_pl_cmp, sizeof c_pl_cmp, TR("▲ 比最好低 %s dB"), g); }
        uk_text_color(s_pl_cmp, !s_pl.n || !live ? T->t3 : gap <= 10 ? T->okT : T->warnT);
    } else {
        set_label_fmt(s_pl_best, c_pl_best, sizeof c_pl_best, "%s", "—");
        set_label_fmt(s_pl_cmp, c_pl_cmp, sizeof c_pl_cmp, "%s", "—");
        uk_text_color(s_pl_cmp, T->t3);
    }
    uk_text_color(s_pl_best, live ? T->t1 : T->t3);
    if (band[0] && pci) set_label_fmt(s_pl_cell, c_pl_cell, sizeof c_pl_cell, "%s · PCI %d", band, pci);
    else set_label_fmt(s_pl_cell, c_pl_cell, sizeof c_pl_cell, "%s", "—");
    uk_text_color(s_pl_cell, live ? T->t1 : T->t3);
}

/* Every refresh: the page while it is up, and the 网络诊断 row on 蜂窝. */
static void diag_refresh(void)
{
    int vis = sub_visible(SUB_DIAG);
    if (diagnose_poll(vis) || vis) diag_paint();
    if (tab_visible(TAB_CELL) && s_cell_diag_sub) {
        const diag_run_t *r = diagnose_run();
        const char *t = "";
        if (dg_going(r)) t = TR("正在检查…");
        else if (r->state == DG_DONE && r->has_main && !diagnose_idle()) t = r->main_text;
        lbl_set(s_cell_diag_sub, t);
    }
}
