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

    printf("passed %d, failed %d\n", pass, fail);
    return fail ? 1 : 0;
}
