/*
 * Unit tests for ui_logic.c (the touch UI's pure decisions). No device needed:
 *   scripts/test/ui_logic/run.sh   (cross-builds, runs in an arm64 busybox container)
 *
 * SPDX-License-Identifier: MIT
 */
#include "ui_logic.h"

#include <stdio.h>
#include <string.h>

static int pass, fail;

#define CHECK(desc, cond)                                              \
    do {                                                               \
        if (cond) { pass++; printf("  ok   %s\n", desc); }             \
        else      { fail++; printf("  FAIL %s  [%s]\n", desc, #cond); } \
    } while (0)

#define M(h, m) ((h) * 60 + (m))

int main(void)
{
    ui_appear_t a;

    puts("appearance parse");
    CHECK("light", ui_appear_parse("light", &a) == 0 && a == UI_APPEAR_LIGHT);
    CHECK("dark", ui_appear_parse("dark", &a) == 0 && a == UI_APPEAR_DARK);
    CHECK("auto", ui_appear_parse("auto", &a) == 0 && a == UI_APPEAR_AUTO);
    CHECK("missing key → light, reported", ui_appear_parse(NULL, &a) == -1 && a == UI_APPEAR_LIGHT);
    CHECK("typo → light, reported", ui_appear_parse("Dark", &a) == -1 && a == UI_APPEAR_LIGHT);
    CHECK("empty → light", ui_appear_parse("", &a) == -1 && a == UI_APPEAR_LIGHT);
    CHECK("names round-trip", !strcmp(ui_appear_name(UI_APPEAR_AUTO), "auto") && !strcmp(ui_appear_name(UI_APPEAR_DARK), "dark"));

    puts("HH:MM parse");
    CHECK("19:00", ui_hhmm_parse("19:00") == M(19, 0));
    CHECK("7:05", ui_hhmm_parse("7:05") == M(7, 5));
    CHECK("00:00", ui_hhmm_parse("00:00") == 0);
    CHECK("23:59", ui_hhmm_parse("23:59") == M(23, 59));
    CHECK("24:00 rejected", ui_hhmm_parse("24:00") == -1);
    CHECK("12:60 rejected", ui_hhmm_parse("12:60") == -1);
    CHECK("123:00 rejected", ui_hhmm_parse("123:00") == -1);
    CHECK("19:0 rejected", ui_hhmm_parse("19:0") == -1);
    CHECK("19:00x rejected", ui_hhmm_parse("19:00x") == -1);
    CHECK("NULL rejected", ui_hhmm_parse(NULL) == -1);

    puts("is_dark");
    int F = M(19, 0), T = M(7, 0);
    CHECK("light is never dark", !ui_is_dark(UI_APPEAR_LIGHT, M(23, 0), F, T));
    CHECK("dark is always dark", ui_is_dark(UI_APPEAR_DARK, M(12, 0), F, T));
    CHECK("auto 18:59 light", !ui_is_dark(UI_APPEAR_AUTO, M(18, 59), F, T));
    CHECK("auto 19:00 dark (start inclusive)", ui_is_dark(UI_APPEAR_AUTO, M(19, 0), F, T));
    CHECK("auto 23:59 dark", ui_is_dark(UI_APPEAR_AUTO, M(23, 59), F, T));
    CHECK("auto 00:00 dark (past midnight)", ui_is_dark(UI_APPEAR_AUTO, 0, F, T));
    CHECK("auto 06:59 dark", ui_is_dark(UI_APPEAR_AUTO, M(6, 59), F, T));
    CHECK("auto 07:00 light (end exclusive)", !ui_is_dark(UI_APPEAR_AUTO, M(7, 0), F, T));
    CHECK("auto same-day window 13:00-15:00 at 14:00", ui_is_dark(UI_APPEAR_AUTO, M(14, 0), M(13, 0), M(15, 0)));
    CHECK("auto same-day window at 16:00 light", !ui_is_dark(UI_APPEAR_AUTO, M(16, 0), M(13, 0), M(15, 0)));
    CHECK("auto from == to: never dark", !ui_is_dark(UI_APPEAR_AUTO, M(20, 0), M(19, 0), M(19, 0)));
    CHECK("auto bad window → 19:00-07:00 default", ui_is_dark(UI_APPEAR_AUTO, M(22, 0), -1, 5000));
    CHECK("auto bad now → light", !ui_is_dark(UI_APPEAR_AUTO, -5, F, T));

    puts("legacy theme=");
    CHECK("dark writes 0 (litehtml: 0 = dark)", ui_legacy_theme_value(1) == 0);
    CHECK("light writes 1", ui_legacy_theme_value(0) == 1);

    /* operator logo slug (same table as manager web operatorLogo.ts) */
    CHECK("logo 460-00", ui_operator_logo(460, 0) && !strcmp(ui_operator_logo(460, 0), "china-mobile"));
    CHECK("logo 460-11", ui_operator_logo(460, 11) && !strcmp(ui_operator_logo(460, 11), "china-telecom"));
    CHECK("logo 311-480", ui_operator_logo(311, 480) && !strcmp(ui_operator_logo(311, 480), "verizon"));
    CHECK("logo CTM", ui_operator_logo(455, 1) && !strcmp(ui_operator_logo(455, 1), "ctm"));
    CHECK("logo 3 HK", ui_operator_logo(454, 3) && !strcmp(ui_operator_logo(454, 3), "three-hk"));
    {
        int c = 0, n = 0;
        CHECK("imsi 460 2-digit", ui_imsi_plmn("460011234567890", &c, &n) && c == 460 && n == 1);
        CHECK("imsi 310 3-digit", ui_imsi_plmn("310260123456789", &c, &n) && c == 310 && n == 260);
        CHECK("imsi 466-92", ui_imsi_plmn("466920123456789", &c, &n) && c == 466 && n == 92);
        CHECK("imsi empty", !ui_imsi_plmn("", &c, &n) && !ui_imsi_plmn(NULL, &c, &n));
    }
    CHECK("logo unknown", ui_operator_logo(0, 0) == NULL && ui_operator_logo(234, 15) == NULL);

    puts("exec guard");
    ui_exec_hist_t none = { 0, 0, 0 };
    CHECK("no history: allow", ui_exec_check(&none, 1000) == UI_EXEC_ALLOW);
    CHECK("NULL history: allow", ui_exec_check(NULL, 1000) == UI_EXEC_ALLOW);
    ui_exec_hist_t h1 = ui_exec_next(&none, 1000);
    CHECK("first exec starts a window", h1.first_s == 1000 && h1.last_s == 1000 && h1.count == 1);
    CHECK("1 s later: cooldown", ui_exec_check(&h1, 1001) == UI_EXEC_COOLDOWN);
    CHECK("599 s later: cooldown", ui_exec_check(&h1, 1599) == UI_EXEC_COOLDOWN);
    CHECK("600 s later: allow", ui_exec_check(&h1, 1600) == UI_EXEC_ALLOW);
    ui_exec_hist_t h2 = ui_exec_next(&h1, 1000 + 3600);
    CHECK("exec after the hour restarts the count", h2.first_s == 4600 && h2.count == 1);
    ui_exec_hist_t s3 = { 1000, 2300, 3 };   /* 3 execs 10+ min apart, all inside the hour */
    CHECK("3 execs inside the hour: suspend", ui_exec_check(&s3, 2400) == UI_EXEC_SUSPEND);
    CHECK("suspend ends with the hour", ui_exec_check(&s3, 1000 + 3600) == UI_EXEC_ALLOW);
    ui_exec_hist_t w1 = ui_exec_next(&h1, 1600);
    ui_exec_hist_t w2 = ui_exec_next(&w1, 2200);
    CHECK("execs 10 min apart keep counting within the hour", w1.count == 2 && w2.count == 3 && w2.first_s == 1000);
    CHECK("…and the third one suspends (reachable now)", ui_exec_check(&w2, 2300) == UI_EXEC_SUSPEND);
    ui_exec_hist_t day = ui_exec_next(&(ui_exec_hist_t){ 1000, 1000, 1 }, 1000 + 12 * 3600);
    CHECK("normal day (19:00, 07:00): never more than 1 in the window", day.count == 1);
    ui_exec_hist_t in = ui_exec_next(&(ui_exec_hist_t){ 1000, 1100, 2 }, 1200);   /* (reached only via cooldown bypass) */
    CHECK("exec inside the window increments", in.first_s == 1000 && in.count == 3 && in.last_s == 1200);
    CHECK("clock went backwards: allow, no crash", ui_exec_check(&h1, 500) == UI_EXEC_ALLOW);

    puts("launch argv");
    ui_launch_t L;
    char *av1[] = { "u60pro-devui", "--tab=3", "--exec-first=1000", "--exec-last=1200", "--exec-n=2", NULL };
    ui_launch_parse(5, av1, &L);
    CHECK("history + tab", L.tab == 3 && L.hist.first_s == 1000 && L.hist.last_s == 1200 && L.hist.count == 2);
    char *av2[] = { "u60pro-devui", NULL };
    ui_launch_parse(1, av2, &L);
    CHECK("plain start: defaults", L.tab == -1 && L.scroll_y == 0 && !L.screen_off && L.autooff_ms == -1 && L.bright == -1 && L.hist.count == 0 && L.hist.first_s == 0);
    char *avt4[] = { "x", "--tab=4", NULL };   /* 系统 is the 5th tab since 2026-09-25 */
    ui_launch_parse(2, avt4, &L);
    CHECK("tab 4 (系统) accepted", L.tab == 4);
    char *avt5[] = { "x", "--tab=5", NULL };
    ui_launch_parse(2, avt5, &L);
    CHECK("tab 5 rejected", L.tab == -1);
    char *av3[] = { "x", "--tab=9", "--exec-n=abc", "--exec-first=-4", "--junk", "--bright=0", "--bright=999", "--scroll=-3", NULL };
    ui_launch_parse(8, av3, &L);
    CHECK("garbage ignored", L.tab == -1 && L.hist.count == 0 && L.hist.first_s == 0 && L.bright == -1 && L.scroll_y == 0);
    char *av4[] = { "x", "--exec-n=2", "--exec-first=900", "--exec-last=100", NULL };
    ui_launch_parse(4, av4, &L);
    CHECK("inconsistent history forgotten", L.hist.count == 0 && L.hist.first_s == 0 && L.hist.last_s == 0);
    char *av5[] = { "x", "--exec-first=900", "--exec-last=950", NULL };
    ui_launch_parse(3, av5, &L);
    CHECK("history without a count forgotten", L.hist.first_s == 0 && L.hist.last_s == 0);

    ui_launch_t src = { 2, 340, 1, 30000, 232, { 1000, 1300, 2 } }, back;
    char store[UI_LAUNCH_MAXARG][UI_LAUNCH_ARGLEN];
    char *out[UI_LAUNCH_MAXARG + 1];
    out[0] = "u60pro-devui";
    int na = ui_launch_argv(&src, store, out + 1);
    ui_launch_parse(na + 1, out, &back);
    CHECK("full round-trip: 8 args", na == 8);
    CHECK("full round-trip: fields", back.tab == 2 && back.scroll_y == 340 && back.screen_off && back.autooff_ms == 30000 && back.bright == 232 &&
          back.hist.first_s == 1000 && back.hist.last_s == 1300 && back.hist.count == 2);
    ui_launch_t zero = { -1, 0, 0, -1, -1, { 0, 0, 0 } };
    CHECK("defaults emit nothing", ui_launch_argv(&zero, store, out + 1) == 0);
    ui_launch_t keyoff = { 1, 0, 2, 30000, -1, { 0, 0, 0 } };
    na = ui_launch_argv(&keyoff, store, out + 1);
    ui_launch_parse(na + 1, out, &back);
    CHECK("--screen-off=key round-trips (only the key wakes it)", back.screen_off == 2);
    ui_launch_t off0 = { 0, 0, 0, 0, -1, { 0, 0, 0 } };
    na = ui_launch_argv(&off0, store, out + 1);
    ui_launch_parse(na + 1, out, &back);
    CHECK("autooff 0 (常亮) survives", back.autooff_ms == 0 && back.tab == 0);

    puts("clock");
    CHECK("1971 not sane", !ui_clock_sane(31536000L));
    CHECK("2023-12-31 not sane", !ui_clock_sane(1704067199L));
    CHECK("2024-01-01 sane", ui_clock_sane(1704067200L));

    puts("auto appearance");
    ui_auto_in_t ai = { UI_APPEAR_AUTO, 0, 1790000000L, M(21, 0), F, T, 1, 0, 0, 0, 0, { 0, 0, 0 }, 5000 };
    CHECK("21:00, light, screen off: exec", ui_auto_decide(&ai) == UI_AUTO_EXEC);
    ui_auto_in_t a2 = ai; a2.cur_dark = 1;
    CHECK("already dark: stay", ui_auto_decide(&a2) == UI_AUTO_STAY);
    a2 = ai; a2.appear = UI_APPEAR_LIGHT;
    CHECK("appearance light: stay", ui_auto_decide(&a2) == UI_AUTO_STAY);
    a2 = ai; a2.wall_s = 40000000L;
    CHECK("unsynced clock: stay", ui_auto_decide(&a2) == UI_AUTO_STAY);
    a2 = ai; a2.screen_off = 0;
    CHECK("screen on: wait", ui_auto_decide(&a2) == UI_AUTO_WAIT);
    a2 = ai; a2.screen_off = 0; a2.always_on = 1; a2.idle_ms = 59999;
    CHECK("常亮, 59.999 s idle: wait", ui_auto_decide(&a2) == UI_AUTO_WAIT);
    a2.idle_ms = 60000;
    CHECK("常亮, 60 s idle: exec", ui_auto_decide(&a2) == UI_AUTO_EXEC);
    a2 = ai; a2.busy = 1;
    CHECK("busy: wait", ui_auto_decide(&a2) == UI_AUTO_WAIT);
    a2 = ai; a2.suspended = 1;
    CHECK("suspended: stay", ui_auto_decide(&a2) == UI_AUTO_STAY);
    a2 = ai; a2.hist = (ui_exec_hist_t){ 4800, 4800, 1 };
    CHECK("200 s after the last exec: wait (cooldown)", ui_auto_decide(&a2) == UI_AUTO_WAIT);
    a2.now_boot_s = 5400;
    CHECK("600 s after: exec", ui_auto_decide(&a2) == UI_AUTO_EXEC);
    a2 = ai; a2.hist = (ui_exec_hist_t){ 4800, 4900, 3 };
    CHECK("3 strikes in the window: suspend", ui_auto_decide(&a2) == UI_AUTO_SUSPEND);
    a2 = ai; a2.cur_dark = 1; a2.now_min = M(7, 0);
    CHECK("07:00, dark: exec back to light", ui_auto_decide(&a2) == UI_AUTO_EXEC);

    puts("status-bar rate");
    int w[3] = { 96, 72, 40 };
    CHECK("full fits", ui_rate_pick(w, 120) == 0);
    CHECK("exactly full width fits", ui_rate_pick(w, 96) == 0);
    CHECK("short when full does not fit", ui_rate_pick(w, 80) == 1);
    CHECK("down-only when short does not fit", ui_rate_pick(w, 50) == 2);
    CHECK("down-only even if it overflows", ui_rate_pick(w, 10) == 2);

    puts("battery");
    CHECK("78 % normal", ui_bat_state(78, 0) == UI_BAT_NORMAL);
    CHECK("21 % normal", ui_bat_state(21, 0) == UI_BAT_NORMAL);
    CHECK("20 % low", ui_bat_state(20, 0) == UI_BAT_LOW);
    CHECK("18 % charging is charging, not low", ui_bat_state(18, 1) == UI_BAT_CHARGING);
    CHECK("body 27", ui_bat_body_w(78, 0) == 27);
    CHECK("body 30 at 100", ui_bat_body_w(100, 0) == 30);
    CHECK("charging keeps the width (no bolt)", ui_bat_body_w(62, 1) == 27 && ui_bat_body_w(100, 1) == 30);
    CHECK("red fill 18 % of 27 → 4", ui_bat_red_w(18, 27) == 4);
    CHECK("red fill never under 3", ui_bat_red_w(1, 27) == 3 && ui_bat_red_w(0, 27) == 3);
    CHECK("red fill never over 20 % (digits start at x≥7)", ui_bat_red_w(20, 27) == 5 && ui_bat_red_w(90, 27) == 5);

    puts("signal card");
    CHECK("boot, nothing yet: loading (not stale)", ui_sig_state(0, 0, "", 0, 0, 0) == UI_SIG_LOADING);
    CHECK("had data, now invalid: stale", ui_sig_state(1, 0, "sim ready", 4, 1, 12) == UI_SIG_STALE);
    CHECK("sim absent", ui_sig_state(1, 1, "sim absent", 0, 0, 0) == UI_SIG_NOSIM);
    CHECK("unknown sim state is not a SIM problem", ui_sig_state(1, 1, "", 4, 1, 12) == UI_SIG_GOOD);
    CHECK("0 bars with SIM: no service", ui_sig_state(1, 1, "sim ready", 0, 0, 0) == UI_SIG_NONE);
    CHECK("2 bars: weak", ui_sig_state(1, 1, "sim ready", 2, 1, 15) == UI_SIG_WEAK);
    CHECK("4 bars, SINR -2: weak", ui_sig_state(1, 1, "sim ready", 4, 1, -2) == UI_SIG_WEAK);
    CHECK("4 bars, no SINR: good", ui_sig_state(1, 1, "sim ready", 4, 0, 0) == UI_SIG_GOOD);
    CHECK("5 bars, SINR 18: good", ui_sig_state(1, 1, "sim ready", 5, 1, 18) == UI_SIG_GOOD);

    puts("touch while dark");
    {
        ui_tgate_t g = { 0 };
        long t = 10000;
#define STEP(lit, wakes, p, x, y) ui_tgate_step(&g, lit, wakes, p, x, y, t)
        CHECK("lit: a press goes through", STEP(1, 0, 1, 100, 100) == UI_TG_FORWARD);
        t += 80; CHECK("lit: release", STEP(1, 0, 0, 100, 100) == 0);

        memset(&g, 0, sizeof g);
        t += 1000; CHECK("dark (key): a press does nothing", STEP(0, 0, 1, 100, 100) == 0);
        t += 80;   STEP(0, 0, 0, 100, 100);
        t += 100;  STEP(0, 0, 1, 100, 100);
        t += 80;   CHECK("dark (key): even a double tap does nothing", STEP(0, 0, 0, 100, 100) == 0);

        memset(&g, 0, sizeof g);
        t += 1000; CHECK("dark (auto): first tap does not reach the UI", STEP(0, 1, 1, 100, 100) == 0);
        t += 80;   CHECK("dark (auto): …nor wake", STEP(0, 1, 0, 101, 100) == 0);
        t += 2000; STEP(0, 1, 1, 100, 100);
        t += 80;   CHECK("two taps 2 s apart: no wake", STEP(0, 1, 0, 100, 100) == 0);
        t += 150;  STEP(0, 1, 1, 110, 105);
        t += 90;   CHECK("double tap: wake", STEP(0, 1, 0, 110, 105) == UI_TG_WAKE);
        t += 50;   CHECK("just woken: a press is held back", STEP(1, 0, 1, 200, 200) == 0);
        t += 400;  CHECK("…until it is released", STEP(1, 0, 1, 200, 200) == 0);
        t += 10;   STEP(1, 0, 0, 200, 200);
        t += 10;   CHECK("after release + 300 ms: taps work", STEP(1, 0, 1, 200, 200) == UI_TG_FORWARD);

        memset(&g, 0, sizeof g);
        t += 1000; STEP(0, 1, 1, 100, 100);
        t += 80;   STEP(0, 1, 0, 100, 100);
        t += 150;  STEP(0, 1, 1, 100, 100);
        t += 600;  CHECK("second touch held 600 ms: not a tap, no wake", STEP(0, 1, 0, 100, 100) == 0);
        memset(&g, 0, sizeof g);
        t += 1000; STEP(0, 1, 1, 100, 100);
        t += 80;   STEP(0, 1, 0, 100, 100);
        t += 150;  STEP(0, 1, 1, 100, 100);
        t += 90;   CHECK("second touch dragged 80 px: no wake", STEP(0, 1, 0, 180, 100) == 0);
        memset(&g, 0, sizeof g);
        t += 1000; STEP(0, 1, 1, 20, 20);
        t += 80;   STEP(0, 1, 0, 20, 20);
        t += 150;  STEP(0, 1, 1, 250, 400);
        t += 80;   CHECK("two taps far apart (pocket): no wake", STEP(0, 1, 0, 250, 400) == 0);
        memset(&g, 0, sizeof g);
        t += 1000; ui_tgate_woke(&g, t);
        CHECK("power key woke it with a finger down: held back", STEP(1, 0, 1, 50, 50) == 0);
        t += 500;  STEP(1, 0, 0, 50, 50);
        t += 10;   CHECK("…then normal", STEP(1, 0, 1, 50, 50) == UI_TG_FORWARD);
#undef STEP
    }

    puts("radio technology names");
    {
        char b[32];
        CHECK("SA → 5G", !strcmp(ui_rat_short("SA"), "5G"));
        CHECK("NSA → 5G, not cut to 3 chars", !strcmp(ui_rat_short("NSA"), "5G"));
        CHECK("ENDC → 5G NSA", ui_rat("ENDC") == UI_RAT_5G_NSA);
        CHECK("LTE → 4G", !strcmp(ui_rat_short("LTE"), "4G"));
        CHECK("LTE-A → 4G", ui_rat("LTE-A") == UI_RAT_4G);
        CHECK("HSPA+ → 3G", !strcmp(ui_rat_short("HSPA+"), "3G"));
        CHECK("WCDMA → 3G", ui_rat("WCDMA") == UI_RAT_3G);
        CHECK("EDGE → 2G", !strcmp(ui_rat_short("EDGE"), "2G"));
        CHECK("GSM → 2G", ui_rat("GSM") == UI_RAT_2G);
        CHECK("empty → empty", !strcmp(ui_rat_short(""), ""));
        CHECK("LIMITED_SERVICE → nothing", !strcmp(ui_rat_short("LIMITED_SERVICE"), ""));
        CHECK("UNREGISTERED is not NR", ui_rat("UNREGISTERED") == UI_RAT_NONE);
        CHECK("LIMITED_SERVICE_SA is not 5G", ui_rat("LIMITED_SERVICE_SA") == UI_RAT_NONE);
        CHECK("NR5G_SA → 5G SA", ui_rat("NR5G_SA") == UI_RAT_5G_SA);
        ui_rat_long("NSA", 1, b, sizeof b);   CHECK("long NSA", !strcmp(b, "5G NSA · 4G 锚点"));
        ui_rat_long("SA", 1, b, sizeof b);    CHECK("long SA", !strcmp(b, "5G SA"));
        ui_rat_long("HSPA+", 1, b, sizeof b); CHECK("long HSPA+", !strcmp(b, "3G HSPA+"));
        ui_rat_long("EDGE", 1, b, sizeof b);  CHECK("long EDGE", !strcmp(b, "2G EDGE"));
        ui_rat_long("CDMA2000", 1, b, sizeof b); CHECK("long CDMA2000", !strcmp(b, "3G CDMA2000"));
        ui_rat_long("LIMITED_SERVICE", 1, b, sizeof b); CHECK("long unknown → empty", b[0] == 0);
        ui_rat_long("LTE", 1, b, sizeof b);   CHECK("long LTE", !strcmp(b, "4G LTE"));
        ui_rat_long("LTE", 2, b, sizeof b);   CHECK("long LTE CA", !strcmp(b, "4G LTE-A"));
        ui_rat_long("WCDMA", 1, b, sizeof b); CHECK("long WCDMA", !strcmp(b, "3G WCDMA"));
        ui_rat_long("GSM", 1, b, sizeof b);   CHECK("long GSM", !strcmp(b, "2G GSM"));
        ui_band_short("LTE BAND 3", 0, b, sizeof b);   CHECK("LTE BAND 3 → B3", !strcmp(b, "B3"));
        ui_band_short("NR5G BAND 78", 1, b, sizeof b); CHECK("NR5G BAND 78 → n78", !strcmp(b, "n78"));
        ui_band_short("n78", 1, b, sizeof b);          CHECK("n78 stays", !strcmp(b, "n78"));
        ui_band_short("B41", 0, b, sizeof b);          CHECK("B41 stays", !strcmp(b, "B41"));
        ui_band_short("", 0, b, sizeof b);             CHECK("empty → -", !strcmp(b, "-"));
        ui_band_short("DCS", 0, b, sizeof b);          CHECK("no digits → raw", !strcmp(b, "DCS"));
        ui_band_short("GSM 900", 0, b, sizeof b);      CHECK("GSM 900 is a frequency, kept", !strcmp(b, "GSM 900"));
        ui_band_short("n261", 1, b, sizeof b);         CHECK("n261 is a band", !strcmp(b, "n261"));
    }

    puts("network story: every situation, in priority order");
    {
        ui_net_story_t o;
        /* a good 5G SA baseline; each row changes what it says */
        ui_net_in_t b = { .ever_valid = 1, .valid = 1, .sim_state = "sim ready", .net_type = "SA", .bars = 5,
                          .data_up = 1, .roaming = 0, .n_active = 3, .nr_active = 3, .mhz = 220,
                          .sinr_valid = 1, .sinr = 17.7, .rsrp_valid = 1, .rsrp = -87, .rsrq_valid = 1, .rsrq = -11,
                          .rx_bps = 0, .ambr_dl = 1668.64, .mcc = 460, .mnc = 11, .nr_band = 78, .nr_mhz = 220,
                          .net_select = "WL_AND_5G" };
        ui_net_in_t x;
#define T_(desc, field_edits, head, tone_)                                         \
        do { x = b; field_edits; ui_net_story(&x, &o);                            \
             CHECK(desc, !strcmp(o.headline, head) && o.tone == (tone_)); } while (0)
        T_("good SA", (void)0, "顺畅", UI_NET_OK);
        ui_net_story(&b, &o);
        CHECK("SA 3 NR in mainland China = 5G-A", !strcmp(o.rat, "5G-A") && !strcmp(o.link, "3 条载波聚合 · 带宽很宽"));
        CHECK("layer words", !strcmp(o.sig, "强") && o.sig_tone == UI_NET_OK && !strcmp(o.noise, "小") && !o.load[0] &&
                              !strcmp(o.limit, "无") && o.cause == UI_CAUSE_NONE);
        CHECK("no hint when fine", o.hint[0] == 0);
        T_("loading", x.ever_valid = 0, "正在读取…", UI_NET_NEUTRAL);
        T_("data service gone", x.valid = 0, "读不到数据", UI_NET_NEUTRAL);
        T_("no SIM beats everything below", (x.sim_state = "sim absent", x.bars = 0), "无 SIM", UI_NET_BAD);
        T_("airplane", (x.airplane = 1, x.bars = 0), "移动网络已关", UI_NET_NEUTRAL);
        T_("limited service", (x.net_type = "LIMITED_SERVICE", x.bars = 0), "只能紧急呼叫", UI_NET_BAD);
        T_("no service", (x.net_type = "", x.bars = 0), "无服务", UI_NET_BAD);
        T_("registered, data down", x.data_up = 0, "没连上网", UI_NET_BAD);
        x = b; x.data_up = 0; x.roaming = 1; ui_net_story(&x, &o);
        CHECK("data down abroad mentions data roaming", strstr(o.hint, "数据漫游") != NULL);
        T_("weak by bars", x.bars = 2, "慢：信号弱", UI_NET_WARN);
        T_("noisy by SINR", x.sinr = -2.5, "慢：干扰大", UI_NET_WARN);
        T_("weak by RSRP", x.rsrp = -115, "慢：信号弱", UI_NET_WARN);
        T_("weak beats roaming", (x.bars = 1, x.roaming = 1), "慢：信号弱", UI_NET_WARN);
        T_("weak beats noisy", (x.bars = 2, x.sinr = -3), "慢：信号弱", UI_NET_WARN);
        x = b; x.sinr = -2.5; ui_net_story(&x, &o);
        CHECK("noisy keeps the signal word strong", !strcmp(o.sig, "强") && !strcmp(o.noise, "大") &&
                                                     o.cause == UI_CAUSE_NOISE && strstr(o.hint, "SINR -2.5"));
        CHECK("bars tiers", ui_bars_tier(5) == 2 && ui_bars_tier(4) == 2 && ui_bars_tier(3) == 1 &&
                            ui_bars_tier(2) == 0 && ui_bars_tier(1) == 0 && ui_bars_tier(0) == -1);
        T_("roaming", x.roaming = 1, "顺畅", UI_NET_OK);
        T_("3G", (x.net_type = "WCDMA", x.n_active = 0, x.sinr_valid = 0, x.rsrp_valid = 0), "只有 3G", UI_NET_WARN);
        T_("2G", (x.net_type = "EDGE", x.n_active = 0, x.sinr_valid = 0, x.rsrp_valid = 0), "只有 2G", UI_NET_WARN);
        x = b; x.net_type = "WCDMA"; x.n_active = 0; x.sinr_valid = x.rsrp_valid = 0; x.net_select = "Only_WCDMA";
        ui_net_story(&x, &o);
        CHECK("3G pinned says so", strstr(o.hint, "限定") != NULL && !strcmp(o.link, "这个制式没有载波聚合"));
        x = b; x.net_type = "NSA"; x.n_active = 2; x.nr_active = 1; x.lte_active = 1; x.mhz = 120; ui_net_story(&x, &o);
        CHECK("NSA words", !strcmp(o.headline, "顺畅") && !strcmp(o.rat, "5G") &&
                           !strcmp(o.link, "4G 锚点 + 5G，2 条载波 · 带宽充足"));
        x = b; x.net_type = "LTE"; x.n_active = 3; x.nr_active = 0; x.lte_active = 3; x.mhz = 60; ui_net_story(&x, &o);
        CHECK("LTE CA words (China: plain 4G)", !strcmp(o.rat, "4G") && !strcmp(o.link, "3 条载波聚合 · 带宽一般"));
        x = b; x.net_type = "LTE"; x.n_active = 1; x.nr_active = 0; x.lte_active = 1; x.mhz = 20; x.net_select = "Only_LTE"; ui_net_story(&x, &o);
        CHECK("single 20 MHz LTE is narrow", !strcmp(o.rat, "4G") && !strcmp(o.link, "单载波 · 带宽偏窄") &&
                                            !strcmp(o.headline, "慢：载波窄") && o.cause == UI_CAUSE_NARROW);
        x = b; x.net_type = "LTE"; x.n_active = 2; x.nr_active = 0; x.lte_active = 2; x.mhz = 40; x.net_select = "Only_LTE"; ui_net_story(&x, &o);
        CHECK("pinned to 4G says so", !strcmp(o.headline, "顺畅") && strstr(o.hint, "只用 4G") != NULL && o.tone == UI_NET_OK);
        x = b; x.net_type = "LTE"; x.n_active = 1; x.nr_active = 0; x.lte_active = 1; x.mhz = 0; ui_net_story(&x, &o);
        CHECK("unknown width: no width word", !strcmp(o.link, "单载波"));
        x = b; x.net_select = "TCHGWL_5G"; ui_net_story(&x, &o);
        CHECK("TCHGWL_5G is not a pinned mode", o.hint[0] == 0 || strstr(o.hint, "只用") == NULL);
        CHECK("net_select words", !strcmp(ui_net_select_word("TCHGWL_5G"), "自动") && !strcmp(ui_net_select_word("WL_AND_5G"), "自动") &&
                                  !strcmp(ui_net_select_word("Only_5G"), "只用 5G SA") && !strcmp(ui_net_select_word("LTE_AND_5G"), "只用 5G NSA") &&
                                  !strcmp(ui_net_select_word("Only_GSM_WCDMA"), "只用 3G 和 2G") && !strcmp(ui_net_select_word(""), "-") &&
                                  !strcmp(ui_net_select_word("SOMETHING_NEW"), "SOMETHING_NEW"));
        CHECK("auto values", ui_net_select_is_auto("TCHGWL_5G") && ui_net_select_is_auto("WL_AND_5G") && !ui_net_select_is_auto("Only_LTE") &&
                             !ui_net_select_is_auto(NULL));
        x = b; x.bars = 3; ui_net_story(&x, &o);    CHECK("3 bars = 中", !strcmp(o.sig, "中") && o.sig_tone == UI_NET_WARN);
        x = b; x.sinr = 5; ui_net_story(&x, &o);    CHECK("SINR 5 = 干扰中, signal still 强", !strcmp(o.noise, "中") && !strcmp(o.sig, "强"));
        x = b; x.sinr = -3; ui_net_story(&x, &o);   CHECK("SINR < 0 = 干扰大", !strcmp(o.noise, "大") && o.noise_tone == UI_NET_BAD);

        /* why it is slow (docs/designs/home-net-card.md) */
        T_("operator cap", x.ambr_dl = 5, "慢：限速", UI_NET_WARN);
        x = b; x.ambr_dl = 5; ui_net_story(&x, &o);
        CHECK("cap: cause, word, number", o.cause == UI_CAUSE_LIMIT && !strcmp(o.limit, "有") && strstr(o.hint, "5 Mbps"));
        x = b; x.ambr_dl = 0; ui_net_story(&x, &o);
        CHECK("AMBR unknown: no claim", !strcmp(o.limit, "—") && !strcmp(o.headline, "顺畅"));
        T_("cap beats weak", (x.ambr_dl = 5, x.sinr = -3), "慢：限速", UI_NET_WARN);
        T_("crowded: good RSRP, poor RSRQ, downloading", (x.rsrq = -18, x.rx_bps = 400000), "慢：疑似拥挤", UI_NET_WARN);
        x = b; x.rsrq = -18; x.rx_bps = 400000; ui_net_story(&x, &o);
        CHECK("crowded words", o.cause == UI_CAUSE_CROWD && !strcmp(o.load, "高") && !strcmp(o.sig, "强") &&
                               strstr(o.hint, "RSRQ -18"));
        T_("idle: poor RSRQ alone says nothing", (x.rsrq = -18, x.rx_bps = 20000), "顺畅", UI_NET_OK);
        x = b; x.rsrq = -18; ui_net_story(&x, &o);   CHECK("idle: no load word", !o.load[0]);
        x = b; x.rx_bps = 400000; ui_net_story(&x, &o); CHECK("busy and fine", !strcmp(o.load, "正常"));
        T_("LTE RSRQ threshold is -12", (x.net_type = "LTE", x.n_active = 2, x.lte_active = 2, x.nr_active = 0,
                                         x.mhz = 40, x.rsrq = -13, x.rx_bps = 400000), "慢：疑似拥挤", UI_NET_WARN);
        T_("noisy is not called crowded", (x.rsrp = -104, x.sinr = -1, x.rsrq = -18, x.rx_bps = 400000),
           "慢：干扰大", UI_NET_WARN);
        T_("one narrow carrier", (x.n_active = 1, x.nr_active = 1, x.mhz = 20), "慢：载波窄", UI_NET_WARN);
        x = b; x.n_active = 1; x.nr_active = 1; x.mhz = 100; ui_net_story(&x, &o);
        CHECK("one wide carrier is fine", !strcmp(o.headline, "顺畅"));
        T_("narrow beats roaming", (x.n_active = 1, x.nr_active = 1, x.mhz = 15, x.roaming = 1), "慢：载波窄", UI_NET_WARN);

        /* 2026-09-25 on the device: SA, one n5 15 MHz carrier, SINR -1.9,
         * RSRP -102, RSRQ -17. The old card said 信号偏弱 and 信号较差 at once. */
        x = b; x.bars = 3; x.n_active = 1; x.nr_active = 1; x.mhz = 15;
        x.sinr = -1.9; x.rsrp = -102; x.rsrq = -17; x.rx_bps = 0; ui_net_story(&x, &o);
        CHECK("device sample 9-25", !strcmp(o.headline, "慢：干扰大") && !strcmp(o.sig, "中") &&
                                    !strcmp(o.noise, "大") && !o.load[0] && !strcmp(o.limit, "无") &&
                                    o.cause == UI_CAUSE_NOISE && strstr(o.hint, "SINR -1.9"));
        CHECK("headlines stay short", strlen(o.headline) <= 18 && strlen(o.hint) <= 60);
#undef T_
    }

    puts("status-bar label by the network you are on (home-net-card.md)");
    {
        char b[16];
        ui_net_in_t z = { .net_type = "SA", .mcc = 460, .mnc = 11, .nr_active = 1, .nr_band = 78, .nr_mhz = 100, .mhz = 100 };
        ui_net_in_t x;
#define B_(desc, edits, want) do { x = z; edits; ui_net_badge(&x, b, sizeof b); CHECK(desc, !strcmp(b, want)); } while (0)
        B_("CN 1 carrier", (void)0, "5G");
        B_("CN 2 carriers", (x.nr_active = 2, x.nr_mhz = 200), "5G");
        B_("CN 3 carriers", x.nr_active = 3, "5G-A");
        B_("CN NSA 3 NR", (x.net_type = "NSA", x.nr_active = 3), "5G-A");
        B_("Unicom 2 carriers 200 MHz", (x.mnc = 1, x.nr_active = 2, x.nr_mhz = 200), "5G-A");
        B_("Unicom 2 carriers 160 MHz", (x.mnc = 1, x.nr_active = 2, x.nr_mhz = 160), "5G");
        B_("T-Mobile n41", (x.mcc = 310, x.mnc = 260, x.nr_band = 41), "5G UC");
        B_("T-Mobile n71", (x.mcc = 310, x.mnc = 260, x.nr_band = 71), "5G");
        B_("Verizon n77", (x.mcc = 311, x.mnc = 480, x.nr_band = 77), "5G UW");
        B_("Verizon n48", (x.mcc = 311, x.mnc = 480, x.nr_band = 48), "5G UW");
        B_("Verizon n5", (x.mcc = 311, x.mnc = 480, x.nr_band = 5, x.mhz = 10), "5G");
        B_("AT&T n77", (x.mcc = 310, x.mnc = 410, x.nr_band = 77), "5G+");
        B_("AT&T n5 + LTE 50 MHz", (x.mcc = 310, x.mnc = 410, x.nr_band = 5, x.mhz = 50), "5G+");
        B_("AT&T n5 alone", (x.mcc = 310, x.mnc = 410, x.nr_band = 5, x.mhz = 10), "5G");
        B_("Japan 5G", (x.mcc = 440, x.mnc = 10, x.nr_active = 3), "5G");
        B_("Taiwan 5G", (x.mcc = 466, x.mnc = 92, x.nr_active = 3), "5G");
        B_("HK 5G", (x.mcc = 454, x.mnc = 12, x.nr_active = 3), "5G");
        B_("UK 5G", (x.mcc = 234, x.mnc = 30, x.nr_active = 3), "5G");
        B_("Japan LTE CA", (x.net_type = "LTE", x.mcc = 440, x.nr_active = 0, x.lte_active = 2), "4G+");
        B_("Taiwan LTE CA", (x.net_type = "LTE", x.mcc = 466, x.nr_active = 0, x.lte_active = 3), "4G+");
        B_("Taiwan LTE 1", (x.net_type = "LTE", x.mcc = 466, x.nr_active = 0, x.lte_active = 1), "4G");
        B_("China LTE CA", (x.net_type = "LTE", x.nr_active = 0, x.lte_active = 3), "4G");
        B_("UK LTE CA", (x.net_type = "LTE", x.mcc = 234, x.nr_active = 0, x.lte_active = 2), "4G");
        B_("US LTE", (x.net_type = "LTE", x.mcc = 310, x.mnc = 260, x.nr_active = 0, x.lte_active = 1), "LTE");
        B_("US LTE CA", (x.net_type = "LTE", x.mcc = 311, x.mnc = 480, x.nr_active = 0, x.lte_active = 2), "LTE");
        B_("3G", (x.net_type = "HSPA+", x.nr_active = 0), "3G");
        B_("2G", (x.net_type = "EDGE", x.nr_active = 0), "2G");
        B_("emergency only", x.net_type = "LIMITED_SERVICE", "SOS");
        /* roaming follows the visited network: a China SIM on T-Mobile n41 is 5G UC */
        B_("roaming on T-Mobile n41", (x.roaming = 1, x.mcc = 310, x.mnc = 260, x.nr_band = 41), "5G UC");
        B_("roaming in Japan, 3 carriers", (x.roaming = 1, x.mcc = 440, x.mnc = 20, x.nr_active = 3), "5G");
#undef B_
        ui_net_story_t o;
        ui_net_in_t y = { .ever_valid = 1, .valid = 1, .sim_state = "sim ready", .net_type = "", .bars = 0, .data_up = 0 };
        ui_net_story(&y, &o);
        CHECK("no service: status bar says so", !strcmp(o.rat, "无服务") && !strcmp(o.headline, "无服务"));
    }

    puts("headline hold: 15 s before a new verdict shows");
    {
        ui_net_hold_t h = {0};
        CHECK("first shows at once", ui_net_hold(&h, 1, 1000) == 1);
        CHECK("same stays", ui_net_hold(&h, 1, 2000) == 1);
        CHECK("new one waits", ui_net_hold(&h, 2, 3000) == 0 && ui_net_hold(&h, 2, 17000) == 0);
        CHECK("after 15 s it shows", ui_net_hold(&h, 2, 18000) == 1);
        CHECK("flicker back resets", ui_net_hold(&h, 3, 19000) == 0 && ui_net_hold(&h, 2, 20000) == 1 &&
                                     ui_net_hold(&h, 3, 21000) == 0 && ui_net_hold(&h, 3, 35000) == 0 &&
                                     ui_net_hold(&h, 3, 36000) == 1);
    }

    puts("phone-style labels");
    {
        static const struct { const char *raw; int nr, lte; const char *want, *fam; } k[] = {
            { "GSM", 0, 0, "2G", "GSM" },       { "GPRS", 0, 0, "2G", "GPRS" },
            { "EDGE", 0, 0, "2G", "EDGE" },     { "CDMA", 0, 0, "2G", "CDMA 1X" },
            { "1xRTT", 0, 0, "2G", "CDMA 1X" }, { "WCDMA", 0, 0, "3G", "WCDMA" },
            { "UMTS", 0, 0, "3G", "WCDMA" },    { "HSPA", 0, 0, "3G", "HSPA" },
            { "HSPA+", 0, 0, "3G", "HSPA+" },   { "DC-HSPA+", 0, 0, "3G", "HSPA+" },
            { "TD-SCDMA", 0, 0, "3G", "TD-SCDMA" }, { "CDMA2000", 0, 0, "3G", "CDMA2000" },
            { "EVDO", 0, 0, "3G", "CDMA2000" }, { "eHRPD", 0, 0, "3G", "CDMA2000" },
            { "LTE", 0, 1, "4G", "LTE" },       { "TD-LTE", 0, 1, "4G", "LTE" },
            { "FDD-LTE", 0, 1, "4G", "LTE" },   { "4G", 0, 1, "4G", "LTE" },
            { "LTE", 0, 2, "4G", "LTE" },       { "LTE-A", 0, 1, "4G", "LTE" },
            { "LTE_CA", 0, 1, "4G", "LTE" },    { "4G+", 0, 1, "4G", "LTE" },
            { "SA", 1, 0, "5G", "NR" },         { "NSA", 1, 1, "5G", "NR" },
            { "ENDC", 1, 2, "5G", "NR" },       { "SA", 2, 0, "5G", "NR" },
            { "NSA", 2, 1, "5G", "NR" },        { "SA", 3, 0, "5G", "NR" },
            { "5G-A", 1, 0, "5G-A", "NR" },     { "LIMITED_SERVICE", 0, 0, "", "" },
            { "LIMITED_SERVICE_SA", 0, 0, "", "" },
            { "", 0, 0, "", "" },
        };
        char b[16], d2[64];
        for (size_t i = 0; i < sizeof k / sizeof *k; i++) {
            ui_net_label(k[i].raw, k[i].nr, k[i].lte, b, sizeof b);
            snprintf(d2, sizeof d2, "%s nr%d lte%d → %s / %s", k[i].raw, k[i].nr, k[i].lte, k[i].want, k[i].fam);
            CHECK(d2, !strcmp(b, k[i].want) && !strcmp(ui_rat_family(k[i].raw), k[i].fam));
        }
    }

    /* which card is in use */
    CHECK("iccid: trailing F padding ignored", ui_iccid_same("8986000000000000012F", "8986000000000000012"));
    CHECK("iccid: case-insensitive", ui_iccid_same("8986000000000000a12f", "8986000000000000A12"));
    CHECK("iccid: different", !ui_iccid_same("8986000000000000012", "8986000000000000013"));
    CHECK("iccid: empty never matches", !ui_iccid_same("", "") && !ui_iccid_same("FFFF", ""));
    CHECK("sim: no card", ui_sim_kind("", "", "") == UI_SIM_NONE);
    CHECK("sim: absent state, no iccid", ui_sim_kind("sim absent", "", "8986") == UI_SIM_NONE);
    CHECK("sim: plain card (no eSIM list)", ui_sim_kind("sim ready", "8986000000000000012F", "") == UI_SIM_PLAIN);
    CHECK("sim: plain card (eSIM profile is another)", ui_sim_kind("sim ready", "8986000000000000012F", "8944000000000000003") == UI_SIM_PLAIN);
    CHECK("sim: eSIM profile in use", ui_sim_kind("sim ready", "8986000000000000012F", "8986000000000000012") == UI_SIM_ESIM);
    CHECK("sim: iccid known, state not yet ready", ui_sim_kind("", "8986000000000000012", "") == UI_SIM_PLAIN);

    /* DHCP pool text (T13: computed from datad's /state dhcp block) */
    {
        char b[48];
        ui_dhcp_pool_text("192.168.0.1", "100", "50", b, sizeof b);
        CHECK("pool: start + limit", !strcmp(b, "192.168.0.100 - 192.168.0.149"));
        ui_dhcp_pool_text("192.168.0.1", "2", "252", b, sizeof b);
        CHECK("pool: capped at 254", !strcmp(b, "192.168.0.2 - 192.168.0.253"));
        ui_dhcp_pool_text("192.168.0.1", "2", "400", b, sizeof b);
        CHECK("pool: over 254 capped", !strcmp(b, "192.168.0.2 - 192.168.0.254"));
        ui_dhcp_pool_text("192.168.0.1", "192.168.0.10", "5", b, sizeof b);
        CHECK("pool: start given as full address", !strcmp(b, "192.168.0.10 - 192.168.0.14"));
        ui_dhcp_pool_text("192.168.0.1", "100", "", b, sizeof b);
        CHECK("pool: no limit → start only", !strcmp(b, "192.168.0.100"));
        ui_dhcp_pool_text("192.168.0.1", "", "50", b, sizeof b);
        CHECK("pool: no start → empty", b[0] == 0);
        ui_dhcp_pool_text("nodot", "100", "50", b, sizeof b);
        CHECK("pool: bad ip → empty", b[0] == 0);
        ui_dhcp_pool_text("192.168.0.1", "x", "50", b, sizeof b);
        CHECK("pool: garbage start → empty", b[0] == 0);
    }

    /* datad /control reply → direct-ubus fallback (T13 writes) */
    CHECK("control: 200 → no fallback", !ui_control_should_fallback("HTTP/1.1 200 OK\r\n", 17));
    CHECK("control: 503 busy → fallback", ui_control_should_fallback("HTTP/1.1 503 Service Unavailable", 32));
    CHECK("control: 400 invalid → no fallback", !ui_control_should_fallback("HTTP/1.1 400 Bad Request", 24));
    CHECK("control: 500 failed → no fallback", !ui_control_should_fallback("HTTP/1.1 500 Internal", 21));
    CHECK("control: closed without reply → fallback", ui_control_should_fallback("", 0));
    CHECK("control: recv error → no fallback", !ui_control_should_fallback("", -1));
    CHECK("control: garbage → no fallback", !ui_control_should_fallback("xx 503", 6));
    CHECK("control: HTTP/1.0 503 → fallback", ui_control_should_fallback("HTTP/1.0 503 x", 14));

    printf("passed %d, failed %d\n", pass, fail);
    return fail ? 1 : 0;
}
