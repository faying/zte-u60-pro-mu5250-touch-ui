/*
 * ui_logic.c - pure decisions behind the touch UI; see ui_logic.h.
 * No LVGL, no I/O: everything here is exercised by tests/ui_logic_test.c.
 *
 * SPDX-License-Identifier: MIT
 */
#include "ui_logic.h"

#include <ctype.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DARK_FROM_DEFAULT (19 * 60)
#define DARK_TO_DEFAULT   (7 * 60)

int ui_appear_parse(const char *s, ui_appear_t *out)
{
    if (s && strcmp(s, "dark") == 0) { *out = UI_APPEAR_DARK; return 0; }
    if (s && strcmp(s, "auto") == 0) { *out = UI_APPEAR_AUTO; return 0; }
    *out = UI_APPEAR_LIGHT;
    return (s && strcmp(s, "light") == 0) ? 0 : -1;
}

const char *ui_appear_name(ui_appear_t a)
{
    return a == UI_APPEAR_DARK ? "dark" : a == UI_APPEAR_AUTO ? "auto" : "light";
}

int ui_hhmm_parse(const char *s)
{
    if (!s) return -1;
    /* exactly H:MM or HH:MM, digits only */
    int h = 0, m = 0, i = 0, nd = 0;
    while (s[i] >= '0' && s[i] <= '9' && nd < 2) { h = h * 10 + (s[i] - '0'); i++; nd++; }
    if (nd == 0 || s[i] != ':') return -1;
    i++;
    if (!(s[i] >= '0' && s[i] <= '9' && s[i + 1] >= '0' && s[i + 1] <= '9') || s[i + 2] != '\0') return -1;
    m = (s[i] - '0') * 10 + (s[i + 1] - '0');
    if (h > 23 || m > 59) return -1;
    return h * 60 + m;
}

static int minute_ok(int m) { return m >= 0 && m < 24 * 60; }

int ui_is_dark(ui_appear_t a, int now_min, int from_min, int to_min)
{
    if (a == UI_APPEAR_DARK) return 1;
    if (a != UI_APPEAR_AUTO) return 0;
    if (!minute_ok(from_min) || !minute_ok(to_min)) { from_min = DARK_FROM_DEFAULT; to_min = DARK_TO_DEFAULT; }
    if (!minute_ok(now_min)) return 0;
    if (from_min == to_min) return 0;
    if (from_min < to_min) return now_min >= from_min && now_min < to_min;   /* same-day window */
    return now_min >= from_min || now_min < to_min;                          /* crosses midnight */
}

int ui_legacy_theme_value(int dark) { return dark ? 0 : 1; }

/* ---- exec guard ---- */
ui_exec_verdict_t ui_exec_check(const ui_exec_hist_t *h, long now_s)
{
    if (!h || h->count <= 0 || h->first_s <= 0) return UI_EXEC_ALLOW;
    int in_window = now_s - h->first_s < UI_EXEC_WINDOW_S && now_s >= h->first_s;
    if (in_window && h->count >= UI_EXEC_STRIKES) return UI_EXEC_SUSPEND;
    if (h->last_s > 0 && now_s >= h->last_s && now_s - h->last_s < UI_EXEC_COOLDOWN_S) return UI_EXEC_COOLDOWN;
    return UI_EXEC_ALLOW;
}

ui_exec_hist_t ui_exec_next(const ui_exec_hist_t *h, long now_s)
{
    ui_exec_hist_t n = { now_s, now_s, 1 };
    if (h && h->count > 0 && h->first_s > 0 && now_s >= h->first_s && now_s - h->first_s < UI_EXEC_WINDOW_S) {
        n.first_s = h->first_s;
        n.count = h->count + 1;
    }
    return n;
}

static int arg_long(const char *a, const char *key, long *out)
{
    size_t k = strlen(key);
    if (strncmp(a, key, k) != 0 || a[k] == '\0') return 0;
    char *end = NULL;
    long v = strtol(a + k, &end, 10);
    if (!end || *end != '\0' || v < 0) return 0;
    *out = v;
    return 1;
}

void ui_launch_parse(int argc, char **argv, ui_launch_t *out)
{
    ui_launch_t z = { -1, 0, 0, -1, -1, { 0, 0, 0 } };
    *out = z;
    for (int i = 1; i < argc; i++) {
        long v;
        const char *a = argv[i];
        if (!a) continue;
        if (!strcmp(a, "--screen-off"))                  out->screen_off = 1;
        else if (!strcmp(a, "--screen-off=key"))         out->screen_off = 2;
        else if (arg_long(a, "--tab=", &v))              { if (v <= 4) out->tab = (int)v; }   /* 5 个标签：0..4 */
        else if (arg_long(a, "--scroll=", &v))           { if (v <= 100000) out->scroll_y = (int)v; }
        else if (arg_long(a, "--autooff=", &v))          { if (v <= 86400000) out->autooff_ms = (int)v; }
        else if (arg_long(a, "--bright=", &v))           { if (v >= 1 && v <= 255) out->bright = (int)v; }
        else if (arg_long(a, "--exec-first=", &v))       out->hist.first_s = v;
        else if (arg_long(a, "--exec-last=", &v))        out->hist.last_s = v;
        else if (arg_long(a, "--exec-n=", &v))           { if (v <= 100) out->hist.count = (int)v; }
    }
    ui_exec_hist_t *h = &out->hist;
    if (h->count > 0 && (h->first_s <= 0 || h->last_s < h->first_s)) *h = z.hist;   /* inconsistent → forget */
    if (h->count == 0) *h = z.hist;
}

int ui_launch_argv(const ui_launch_t *l, char store[UI_LAUNCH_MAXARG][UI_LAUNCH_ARGLEN], char *argv_out[UI_LAUNCH_MAXARG])
{
    int n = 0;
#define PUT(...) do { snprintf(store[n], UI_LAUNCH_ARGLEN, __VA_ARGS__); argv_out[n] = store[n]; n++; } while (0)
    if (l->tab >= 0 && l->tab <= 4) PUT("--tab=%d", l->tab);
    if (l->scroll_y > 0)            PUT("--scroll=%d", l->scroll_y);
    if (l->screen_off == 1)         PUT("--screen-off");
    else if (l->screen_off == 2)    PUT("--screen-off=key");
    if (l->autooff_ms >= 0)         PUT("--autooff=%d", l->autooff_ms);
    if (l->bright >= 1)             PUT("--bright=%d", l->bright);
    if (l->hist.count > 0) {
        PUT("--exec-first=%ld", l->hist.first_s);
        PUT("--exec-last=%ld", l->hist.last_s);
        PUT("--exec-n=%d", l->hist.count);
    }
#undef PUT
    return n;
}

int ui_clock_sane(long wall_s) { return wall_s >= UI_CLOCK_SANE_AFTER; }

ui_auto_t ui_auto_decide(const ui_auto_in_t *in)
{
    if (in->appear != UI_APPEAR_AUTO || in->suspended) return UI_AUTO_STAY;
    if (!ui_clock_sane(in->wall_s)) return UI_AUTO_STAY;
    int want = ui_is_dark(UI_APPEAR_AUTO, in->now_min, in->from_min, in->to_min);
    if (want == !!in->cur_dark) return UI_AUTO_STAY;
    int unseen = in->screen_off || (in->always_on && in->idle_ms >= UI_AUTO_ALWAYS_ON_IDLE_MS);
    if (!unseen || in->busy) return UI_AUTO_WAIT;
    switch (ui_exec_check(&in->hist, in->now_boot_s)) {
    case UI_EXEC_SUSPEND:  return UI_AUTO_SUSPEND;
    case UI_EXEC_COOLDOWN: return UI_AUTO_WAIT;
    default:               return UI_AUTO_EXEC;
    }
}

/* ---- status bar ---- */
int ui_rate_pick(const int widths[3], int maxw)
{
    for (int i = 0; i < 2; i++)
        if (widths[i] <= maxw) return i;
    return 2;
}

/* ---- battery ---- */
ui_bat_state_t ui_bat_state(int pct, int charging)
{
    if (charging) return UI_BAT_CHARGING;
    if (pct <= 20) return UI_BAT_LOW;
    return UI_BAT_NORMAL;
}

int ui_bat_body_w(int pct, int charging)
{
    (void)charging;   /* charging only recolours the body (no bolt, same width) */
    return pct >= 100 ? 30 : 27;
}

int ui_bat_red_w(int pct, int body_w)
{
    if (pct < 0) pct = 0;
    if (pct > 20) pct = 20;
    int w = body_w * pct / 100;
    return w < 3 ? 3 : w;
}

/* ---- signal card ---- */
static int sim_usable(const char *st)
{
    if (!st || !*st) return 1;                  /* unknown: don't claim a SIM problem */
    return strstr(st, "ready") != NULL;          /* datad: "sim ready" */
}

ui_sig_state_t ui_sig_state(int ever_valid, int valid, const char *sim_state, int bars, int sinr_valid, double sinr)
{
    if (!ever_valid) return UI_SIG_LOADING;
    if (!valid) return UI_SIG_STALE;
    if (!sim_usable(sim_state)) return UI_SIG_NOSIM;
    if (bars <= 0) return UI_SIG_NONE;
    if (bars <= 2 || (sinr_valid && sinr < 0)) return UI_SIG_WEAK;
    return UI_SIG_GOOD;
}

/* ---- touch while the screen is dark ---- */
static int near(int x0, int y0, int x1, int y1, int r) { return (x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) <= r * r; }

void ui_tgate_woke(ui_tgate_t *g, long now_ms)
{
    g->suppress = 1;
    g->wake_ms = now_ms;
    g->taps = 0;
}

int ui_tgate_step(ui_tgate_t *g, int lit, int touch_wakes, int pressed, int x, int y, long now_ms)
{
    int down = pressed && !g->was, up = !pressed && g->was;
    g->was = pressed;

    if (lit) {
        g->taps = 0;
        if (g->suppress) {
            /* until the waking finger is up and the quiet time is over */
            if (pressed || now_ms - g->wake_ms < UI_WAKE_QUIET_MS) return 0;
            g->suppress = 0;
        }
        return pressed ? UI_TG_FORWARD : 0;
    }

    if (!touch_wakes) { g->taps = 0; return 0; }
    if (down) {
        if (g->taps == 1 && now_ms - g->t_up <= UI_DTAP_GAP_MS && near(g->x0, g->y0, x, y, UI_DTAP_SLOP)) {
            g->taps = 2;
        } else {
            g->taps = 1;
            g->x0 = x;
            g->y0 = y;
        }
        g->px = x;
        g->py = y;
        g->t_down = now_ms;
    } else if (up && g->taps) {
        if (now_ms - g->t_down > UI_TAP_MAX_MS || !near(g->px, g->py, x, y, UI_TAP_SLOP)) {
            g->taps = 0;                        /* a press or a drag, not a tap */
        } else if (g->taps == 2) {
            ui_tgate_woke(g, now_ms);
            return UI_TG_WAKE;
        } else {
            g->t_up = now_ms;
        }
    }
    return 0;
}

/* ---- radio technology names ---- */
static int has(const char *s, const char *w) { return s && strstr(s, w) != NULL; }
static int has_ci(const char *s, const char *w)
{
    char u[48];
    int i;
    if (!s) return 0;
    for (i = 0; s[i] && i < (int)sizeof u - 1; i++) u[i] = (char)toupper((unsigned char)s[i]);
    u[i] = 0;
    return strstr(u, w) != NULL;
}
static int ci_has5a(const char *s) { return has_ci(s, "5G-A") || has_ci(s, "5GA") || has_ci(s, "5G_A") || has_ci(s, "5G-ADV"); }

ui_rat_t ui_rat(const char *raw)
{
    char u[32];
    int i;
    if (!raw || !raw[0]) return UI_RAT_NONE;
    for (i = 0; raw[i] && i < (int)sizeof u - 1; i++) u[i] = (char)toupper((unsigned char)raw[i]);
    u[i] = 0;
    /* measured 2026-09-25: "LIMITED_SERVICE_SA" while a manual register fails —
     * not on a network, whatever the suffix says */
    if (has(u, "LIMIT") || has(u, "EMERGENCY") || has(u, "NO_SERVICE") || has(u, "NOSERVICE")) return UI_RAT_NONE;
    if (has(u, "NSA") || has(u, "ENDC") || has(u, "EN-DC")) return UI_RAT_5G_NSA;
    /* whole-word SA / NR only: "UNREGISTERED" contains "NR" */
    if (!strcmp(u, "SA") || !strcmp(u, "NR") || has(u, "5G") || has(u, "NR5G") ||
        !strncmp(u, "SA_", 3) || !strncmp(u, "NR_", 3) || has(u, "_SA") || has(u, " SA"))
        return UI_RAT_5G_SA;
    /* LTE before 3G: "TD-LTE" must not read as TD-SCDMA */
    if (has(u, "LTE") || has(u, "4G")) return UI_RAT_4G;
    if (has(u, "WCDMA") || has(u, "UMTS") || has(u, "HSPA") || has(u, "HSDPA") || has(u, "HSUPA") ||
        has(u, "TD-SCDMA") || has(u, "TDSCDMA") || has(u, "CDMA2000") || has(u, "EVDO") ||
        has(u, "EV-DO") || has(u, "EHRPD") || has(u, "HRPD") || has(u, "3G"))
        return UI_RAT_3G;
    if (has(u, "GSM") || has(u, "GPRS") || has(u, "EDGE") || has(u, "2G") || has(u, "CDMA") ||
        has(u, "1XRTT") || has(u, "1X"))
        return UI_RAT_2G;
    return UI_RAT_NONE;
}

void ui_net_label(const char *raw, int nr_active, int lte_active, char *out, int n)
{
    ui_rat_t r = ui_rat(raw);
    const char *l = "";
    switch (r) {
    case UI_RAT_5G_SA: case UI_RAT_5G_NSA:
        l = (ci_has5a(raw) || nr_active >= 3) ? "5G-A" : nr_active == 2 ? "5G+" : "5G";
        break;
    case UI_RAT_4G:
        l = (has_ci(raw, "LTE-A") || has_ci(raw, "LTE_A") || has_ci(raw, "LTE_CA") || has_ci(raw, "4G+") ||
             has_ci(raw, "LTE+") || lte_active >= 2) ? "4G+" : "4G";
        break;
    case UI_RAT_3G: l = (has_ci(raw, "HSPA+") || has_ci(raw, "DC-HSPA") || has_ci(raw, "HSPAP")) ? "3G+" : "3G"; break;
    case UI_RAT_2G: l = "2G"; break;
    default: break;
    }
    snprintf(out, (size_t)n, "%s", l);
}

const char *ui_rat_family(const char *raw)
{
    if (!raw || !raw[0]) return "";
    if (has_ci(raw, "LTE") || has_ci(raw, "4G")) return "LTE";
    if (ui_rat(raw) == UI_RAT_5G_SA || ui_rat(raw) == UI_RAT_5G_NSA) return "NR";
    if (has_ci(raw, "TD-SCDMA") || has_ci(raw, "TDSCDMA")) return "TD-SCDMA";
    if (has_ci(raw, "CDMA2000") || has_ci(raw, "EVDO") || has_ci(raw, "EV-DO") || has_ci(raw, "HRPD")) return "CDMA2000";
    if (has_ci(raw, "HSPA+") || has_ci(raw, "DC-HSPA") || has_ci(raw, "HSPAP")) return "HSPA+";
    if (has_ci(raw, "HSPA") || has_ci(raw, "HSDPA") || has_ci(raw, "HSUPA")) return "HSPA";
    if (has_ci(raw, "WCDMA") || has_ci(raw, "UMTS")) return "WCDMA";
    if (has_ci(raw, "EDGE")) return "EDGE";
    if (has_ci(raw, "GPRS")) return "GPRS";
    if (has_ci(raw, "GSM")) return "GSM";
    if (has_ci(raw, "CDMA") || has_ci(raw, "1X")) return "CDMA 1X";
    return "";
}

const char *ui_rat_short(const char *raw)
{
    switch (ui_rat(raw)) {
    case UI_RAT_5G_SA: case UI_RAT_5G_NSA: return "5G";
    case UI_RAT_4G: return "4G";
    case UI_RAT_3G: return "3G";
    case UI_RAT_2G: return "2G";
    default: return "";   /* LIMITED_SERVICE and the like: the dots already say it */
    }
}

void ui_rat_long(const char *raw, char *out, int n)
{
    /* the finer name under the status bar's short label: the status bar
     * already says 5G-A / 4G+ …, so this says how (SA/NSA) or which (WCDMA…) */
    const char *fam = ui_rat_family(raw);
    switch (ui_rat(raw)) {
    case UI_RAT_5G_SA:  snprintf(out, (size_t)n, "5G SA"); break;
    case UI_RAT_5G_NSA: snprintf(out, (size_t)n, "5G NSA"); break;
    case UI_RAT_4G:     snprintf(out, (size_t)n, "4G LTE"); break;
    case UI_RAT_3G:     snprintf(out, (size_t)n, "3G%s%s", fam[0] ? " " : "", fam); break;
    case UI_RAT_2G:     snprintf(out, (size_t)n, "2G%s%s", fam[0] ? " " : "", fam); break;
    default:            snprintf(out, (size_t)n, "%s", ""); break;
    }
}

void ui_band_short(const char *raw, int nr, char *out, int n)
{
    const char *p, *last = NULL;
    if (!raw || !raw[0]) { snprintf(out, (size_t)n, "-"); return; }
    /* the last run of digits: "NR5G BAND 78" → 78, not 5 */
    for (p = raw; *p; p++)
        if (isdigit((unsigned char)*p) && (p == raw || !isdigit((unsigned char)p[-1]))) last = p;
    /* no number, or a frequency rather than a band ("GSM 900", "DCS 1800"):
     * band numbers stop at n261, frequencies start at 450 MHz */
    if (!last || atoi(last) >= 450) { snprintf(out, (size_t)n, "%s", raw); return; }
    snprintf(out, (size_t)n, "%c%d", nr ? 'n' : 'B', atoi(last));
}

/* ---- what the network situation means, in words ---- */
static int ci_has(const char *s, const char *w)
{
    char u[48];
    int i;
    if (!s) return 0;
    for (i = 0; s[i] && i < (int)sizeof u - 1; i++) u[i] = (char)toupper((unsigned char)s[i]);
    u[i] = 0;
    return strstr(u, w) != NULL;
}

/* The radio mode is pinned to fewer technologies than the modem supports
 * (Only_LTE, Only_WCDMA, …). Auto / "5G+4G" style preferences are not. */
static const struct { const char *v, *word; } k_net_select[] = {
    { "WL_AND_5G", "自动" },            { "TCHGWL_5G", "自动" },
    { "Only_5G", "只用 5G SA" },        { "LTE_AND_5G", "只用 5G NSA" },
    { "4G_AND_5G", "4G + 5G" },         { "WL_AND_NSA", "5G NSA + 4G + 3G" },
    { "Only_LTE", "只用 4G" },          { "WCDMA_AND_LTE", "4G + 3G" },
    { "GSM_AND_LTE", "4G + 2G" },       { "TDSCDMA_AND_LTE", "4G + TD-SCDMA" },
    { "Only_WCDMA", "只用 3G" },        { "Only_GSM_WCDMA", "只用 3G 和 2G" },
    { "Only_TDSCDMA", "只用 TD-SCDMA" }, { "Only_GSM", "只用 2G" },
};

const char *ui_net_select_word(const char *sel)
{
    if (!sel || !sel[0]) return "-";
    for (size_t i = 0; i < sizeof k_net_select / sizeof k_net_select[0]; i++)
        if (!strcmp(sel, k_net_select[i].v)) return k_net_select[i].word;
    return sel;
}

int ui_net_select_is_auto(const char *sel)
{
    return sel && (!strcmp(sel, "WL_AND_5G") || !strcmp(sel, "TCHGWL_5G"));
}

static const char *pinned_mode(const char *sel)
{
    if (!sel || !sel[0] || !ci_has(sel, "ONLY")) return NULL;
    if (!strcmp(sel, "Only_GSM_WCDMA")) return "只用 3G 和 2G";
    if (ci_has(sel, "GSM") || ci_has(sel, "2G")) return "只用 2G";
    if (ci_has(sel, "WCDMA") || ci_has(sel, "3G") || ci_has(sel, "TD")) return "只用 3G";
    if (ci_has(sel, "LTE") || ci_has(sel, "4G")) return "只用 4G";
    if (ci_has(sel, "5G") || ci_has(sel, "NR")) return "只用 5G";
    return "限定了制式";
}

void ui_net_story(const ui_net_in_t *in, ui_net_story_t *o)
{
    ui_rat_t rat = ui_rat(in->net_type);
    const char *pin = pinned_mode(in->net_select);
    int limited = ci_has(in->net_type, "LIMIT") || ci_has(in->net_type, "EMERGENCY");

    memset(o, 0, sizeof *o);
    o->tone = UI_NET_OK;

    /* the label a phone would show */
    ui_net_label(in->net_type, in->nr_active, in->lte_active, o->rat, sizeof o->rat);

    /* the link, as a judgement: SA / NSA, how many carriers, how wide */
    if (rat == UI_RAT_2G) {
        snprintf(o->link, sizeof o->link, "这个制式没有载波聚合");
    } else if (rat == UI_RAT_3G) {
        snprintf(o->link, sizeof o->link, "这个制式没有载波聚合");
    } else if (in->n_active > 0) {
        const char *w = in->mhz >= 200 ? "带宽很宽" : in->mhz >= 100 ? "带宽充足" :
                        in->mhz >= 40 ? "带宽一般" : in->mhz > 0 ? "带宽偏窄" : "";
        char n[64];
        /* SA / NSA is already in the line above (ui_rat_long) */
        if (rat == UI_RAT_5G_NSA)
            snprintf(n, sizeof n, "4G 锚点 + 5G，%d 条载波", in->n_active);
        else if (in->n_active > 1) snprintf(n, sizeof n, "%d 条载波聚合", in->n_active);
        else snprintf(n, sizeof n, "单载波");
        snprintf(o->link, sizeof o->link, "%s%s%s", n, w[0] ? " · " : "", w);
    }

    /* signal quality, as a judgement (SINR first: it is what limits speed) */
    if (in->sinr_valid || in->rsrp_valid) {
        int q;   /* 3 very good … 0 poor */
        if (in->sinr_valid) q = in->sinr >= 20 ? 3 : in->sinr >= 13 ? 2 : in->sinr >= 0 ? 1 : 0;
        else q = 2;
        if (in->rsrp_valid && in->rsrp < -110 && q > 0) q = 0;
        else if (in->rsrp_valid && in->rsrp < -100 && q > 1) q = 1;
        static const char *const words[4] = { "信号较差", "信号一般", "信号良好", "信号很好" };
        snprintf(o->quality, sizeof o->quality, "%s", words[q]);
        o->quality_tone = q >= 2 ? UI_NET_OK : q == 1 ? UI_NET_WARN : UI_NET_BAD;
    }

    /* the headline, in priority order: the first thing that is wrong wins */
#define SAY(t, h, ...) do { o->tone = (t); snprintf(o->headline, sizeof o->headline, "%s", (h)); \
                            snprintf(o->hint, sizeof o->hint, __VA_ARGS__); return; } while (0)
    if (!in->ever_valid) SAY(UI_NET_NEUTRAL, "正在读取…", "%s", "");
    if (!in->valid)      SAY(UI_NET_NEUTRAL, "读不到数据", "%s", "数据服务没响应，下面的数字停在最后一次");
    if (!sim_usable(in->sim_state))
        SAY(UI_NET_BAD, "无 SIM", "%s", "插卡，或在「功能 → eSIM」启用一个配置");
    if (in->airplane)
        SAY(UI_NET_NEUTRAL, "移动网络已关", "%s", "飞行模式开着：在管理网页「移动网络」里关掉");
    if (limited)
        SAY(UI_NET_BAD, "只能紧急呼叫", "%s", in->roaming == 1
            ? "卡没注册上：在国外要这张卡开了漫游，或换当地卡"
            : "卡没注册上运营商：可能欠费、停机，或这里没有这家的网");
    if (in->bars <= 0 || rat == UI_RAT_NONE)
        SAY(UI_NET_BAD, "无服务", "%s", pin
            ? "正在搜网。制式被限定了，去「锁频」改回自动试试"
            : "正在搜网。换个位置试试；锁过频就去「锁频」恢复默认");
    if (!in->data_up)
        SAY(UI_NET_BAD, "没连上网", "%s", in->roaming == 1
            ? "已注册但数据没拨上：确认这张卡开了数据漫游，设备也允许漫游"
            : "已注册但数据没拨上：检查流量开关、APN，或是否欠费");
    {
        int weak = in->bars <= 2 || (in->sinr_valid && in->sinr < 0) || (in->rsrp_valid && in->rsrp < -110);
        if (weak)
            SAY(UI_NET_WARN, "信号偏弱", "%s", in->roaming == 1
                ? "网速会受影响。换个位置试试，靠窗通常更好；现在是漫游，注意流量"
                : "网速会受影响。换个位置试试，靠窗通常更好");
    }
    if (rat == UI_RAT_2G)
        SAY(UI_NET_WARN, "只有 2G", "%s", pin ? "制式被限定为只用 2G，去「锁频」改回自动"
                                              : "只能打电话发短信，上网会非常慢；附近可能没有 4G/5G");
    if (rat == UI_RAT_3G)
        SAY(UI_NET_WARN, !strcmp(o->rat, "3G+") ? "只有 3G+" : "只有 3G", "%s",
            pin ? "制式被限定为只用 3G，去「锁频」改回自动" : "能上网但比较慢；附近可能没有 4G/5G");
    if (in->roaming == 1)
        SAY(UI_NET_WARN, "漫游中", "%s", "按漫游计费，注意流量用量");
    if (pin && rat == UI_RAT_4G)
        SAY(UI_NET_OK, "网络正常", "%s", "制式被限定为只用 4G；想用 5G 去「锁频」改回自动");
    SAY(UI_NET_OK, "网络正常", "%s", "");
#undef SAY
}

static size_t iccid_len(const char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == 'F' || s[n - 1] == 'f' || s[n - 1] == ' ')) n--;
    return n;
}

int ui_iccid_same(const char *a, const char *b)
{
    if (!a || !b) return 0;
    size_t la = iccid_len(a), lb = iccid_len(b);
    if (!la || la != lb) return 0;
    for (size_t i = 0; i < la; i++) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return 0;
    }
    return 1;
}

ui_sim_kind_t ui_sim_kind(const char *sim_state, const char *modem_iccid, const char *esim_enabled_iccid)
{
    int ready = sim_state && strstr(sim_state, "ready") != NULL;
    if (!ready && !(modem_iccid && iccid_len(modem_iccid))) return UI_SIM_NONE;
    return ui_iccid_same(modem_iccid, esim_enabled_iccid) ? UI_SIM_ESIM : UI_SIM_PLAIN;
}
