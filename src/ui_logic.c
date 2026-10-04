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

int ui_autooff_snap(long ms)
{
    if (ms == 0) return 0;
    if (ms > 0 && ms <= 30000) return 30000;
    return 120000;
}

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

int ui_net_hold(ui_net_hold_t *h, unsigned key, unsigned now_ms)
{
    if (!h->have || key == h->shown) {      /* first verdict shows at once */
        h->have = 1; h->shown = h->pending = key; h->since = now_ms;
        return 1;
    }
    if (key != h->pending) { h->pending = key; h->since = now_ms; return 0; }
    if (now_ms - h->since < UI_NET_HOLD_MS) return 0;
    h->shown = key;
    return 1;
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

void ui_dhcp_pool_text(const char *ip, const char *start, const char *limit, char *out, size_t n)
{
    const char *dot, *sd;
    char *end;
    long st, lim, last;

    if (!out || n == 0) return;
    out[0] = 0;
    if (!ip || !start) return;
    dot = strrchr(ip, '.');
    if (!dot || dot == ip) return;
    sd = strrchr(start, '.');
    if (sd) start = sd + 1;
    st = strtol(start, &end, 10);
    if (end == start || *end || st < 1 || st > 254) return;
    lim = (limit && *limit) ? strtol(limit, &end, 10) : 0;
    if (!limit || !*limit || *end || lim < 1) {
        snprintf(out, n, "%.*s.%ld", (int)(dot - ip), ip, st);
        return;
    }
    last = st + lim - 1;
    if (last > 254) last = 254;
    snprintf(out, n, "%.*s.%ld - %.*s.%ld", (int)(dot - ip), ip, st, (int)(dot - ip), ip, last);
}

ui_ctl_t ui_control_reply(const char *reply, long n)
{
    const char *sp;
    int code;

    if (n <= 0 || !reply || strncmp(reply, "HTTP/1.", 7)) return UI_CTL_NOREPLY;
    sp = strchr(reply, ' ');
    if (!sp || sp[1] < '1' || sp[1] > '5' || sp[2] < '0' || sp[2] > '9' || sp[3] < '0' || sp[3] > '9')
        return UI_CTL_NOREPLY;
    code = (sp[1] - '0') * 100 + (sp[2] - '0') * 10 + (sp[3] - '0');
    if (code >= 200 && code < 300) return UI_CTL_OK;
    if (code == 409) return UI_CTL_BUSY;
    if (code == 503) return UI_CTL_FULL;
    return UI_CTL_FAILED;
}

int ui_control_fallback_cmd(char *out, size_t n, const char *script, const char *action, const char *args)
{
    const char *p;

    if (!out || n == 0) return 0;
    out[0] = 0;
    if (!script || !script[0] || !action || !action[0] || !args) return 0;
    for (p = action; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || *p == '.' || *p == '_')) return 0;
    for (p = args; *p; p++) {
        int ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                 strchr("_.,:=+-", *p) != NULL || (*p == ' ' && p[1] != ' ' && p != args);
        if (!ok) return 0;
    }
    if (snprintf(out, n, "%s --by screen %s%s%s >/dev/null 2>&1 &", script, action,
                 args[0] ? " " : "", args) >= (int)n) {
        out[0] = 0;
        return 0;
    }
    return 1;
}
