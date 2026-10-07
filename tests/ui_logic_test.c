/*
 * Unit tests for ui_logic.c (the touch UI's pure decisions). No device needed:
 *   scripts/test/ui_logic/run.sh   (cross-builds, runs in an arm64 busybox container)
 *
 * SPDX-License-Identifier: MIT
 */
#include "ui_logic.h"
#include "key_input.h"

#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>

#include <stdio.h>
#include <string.h>

static int pass, fail;

#define CHECK(desc, cond)                                              \
    do {                                                               \
        if (cond) { pass++; printf("  ok   %s\n", desc); }             \
        else      { fail++; printf("  FAIL %s  [%s]\n", desc, #cond); } \
    } while (0)

#define M(h, m) ((h) * 60 + (m))

/* Power key: feed evdev records through a pipe (same layout key_input reads). */
struct tkev { unsigned long sec, usec; uint16_t type, code; int32_t value; };
static void key_feed(int fd, int value)
{
    struct tkev e = { 0, 0, 1 /* EV_KEY */, 116 /* KEY_POWER */, value };
    if (write(fd, &e, sizeof e) != (ssize_t)sizeof e) perror("write");
}

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

    /* operator logos, IMSI → PLMN: zwrt-datad screen.rs (tests there) */

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
        CHECK("ENDC → 5G NSA", ui_rat("ENDC") == UI_RAT_5G_NSA);
        CHECK("LTE-A → 4G", ui_rat("LTE-A") == UI_RAT_4G);
        CHECK("WCDMA → 3G", ui_rat("WCDMA") == UI_RAT_3G);
        CHECK("GSM → 2G", ui_rat("GSM") == UI_RAT_2G);
        CHECK("UNREGISTERED is not NR", ui_rat("UNREGISTERED") == UI_RAT_NONE);
        CHECK("LIMITED_SERVICE_SA is not 5G", ui_rat("LIMITED_SERVICE_SA") == UI_RAT_NONE);
        CHECK("NR5G_SA → 5G SA", ui_rat("NR5G_SA") == UI_RAT_5G_SA);
        ui_band_short("LTE BAND 3", 0, b, sizeof b);   CHECK("LTE BAND 3 → B3", !strcmp(b, "B3"));
        ui_band_short("NR5G BAND 78", 1, b, sizeof b); CHECK("NR5G BAND 78 → n78", !strcmp(b, "n78"));
        ui_band_short("n78", 1, b, sizeof b);          CHECK("n78 stays", !strcmp(b, "n78"));
        ui_band_short("B41", 0, b, sizeof b);          CHECK("B41 stays", !strcmp(b, "B41"));
        ui_band_short("", 0, b, sizeof b);             CHECK("empty → -", !strcmp(b, "-"));
        ui_band_short("DCS", 0, b, sizeof b);          CHECK("no digits → raw", !strcmp(b, "DCS"));
        ui_band_short("GSM 900", 0, b, sizeof b);      CHECK("GSM 900 is a frequency, kept", !strcmp(b, "GSM 900"));
        ui_band_short("n261", 1, b, sizeof b);         CHECK("n261 is a band", !strcmp(b, "n261"));
    }

    /* the verdict, the status-bar label, the tiers: zwrt-datad screen.rs,
     * tested there (the cases that used to be here moved with the rules) */
    /* radio-mode preference words: zwrt-datad screen.rs */

    puts("screen-off time from devui.conf");
    CHECK("0 = never", ui_autooff_snap(0) == 0);
    CHECK("30 s", ui_autooff_snap(30000) == 30000);
    CHECK("2 min", ui_autooff_snap(120000) == 120000);
    CHECK("old default 60 s rounds up to 2 min", ui_autooff_snap(60000) == 120000);
    CHECK("10 s rounds up to 30 s", ui_autooff_snap(10000) == 30000);
    CHECK("negative → 2 min, not never", ui_autooff_snap(-5) == 120000);
    CHECK("an hour → 2 min", ui_autooff_snap(3600000) == 120000);

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

    /* phone-style labels and technology names: zwrt-datad screen.rs */

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

    /* datad /control reply (E4 T8): nothing here runs the emergency script */
    CHECK("control: 200 → ok", ui_control_reply("HTTP/1.1 200 OK\r\n", 17) == UI_CTL_OK);
    CHECK("control: 409 → busy", ui_control_reply("HTTP/1.1 409 Conflict", 21) == UI_CTL_BUSY);
    CHECK("control: 503 → queue full", ui_control_reply("HTTP/1.1 503 Service Unavailable", 32) == UI_CTL_FULL);
    CHECK("control: HTTP/1.0 503 → queue full", ui_control_reply("HTTP/1.0 503 x", 14) == UI_CTL_FULL);
    CHECK("control: 400 → failed", ui_control_reply("HTTP/1.1 400 Bad Request", 24) == UI_CTL_FAILED);
    CHECK("control: 502 → failed", ui_control_reply("HTTP/1.1 502 Bad Gateway", 24) == UI_CTL_FAILED);
    CHECK("control: closed without reply", ui_control_reply("", 0) == UI_CTL_NOREPLY);
    CHECK("control: recv error", ui_control_reply("", -1) == UI_CTL_NOREPLY);
    CHECK("control: garbage", ui_control_reply("xx 503", 6) == UI_CTL_NOREPLY);
    CHECK("control: short status", ui_control_reply("HTTP/1.1 50", 11) == UI_CTL_NOREPLY);
    {
        char c[256];
        const char *sc = "/data/u60-guard/u60-fallback.sh";
        CHECK("fallback: command", ui_control_fallback_cmd(c, sizeof c, sc, "network.set_mode", "mode=WL_AND_5G") &&
              !strcmp(c, "/data/u60-guard/u60-fallback.sh --by screen network.set_mode mode=WL_AND_5G >/dev/null 2>&1 &"));
        CHECK("fallback: no args", ui_control_fallback_cmd(c, sizeof c, sc, "band.reset", "") &&
              !strcmp(c, "/data/u60-guard/u60-fallback.sh --by screen band.reset >/dev/null 2>&1 &"));
        CHECK("fallback: two args", ui_control_fallback_cmd(c, sizeof c, sc, "wifi.radio", "ap_2g=1 ap_5g=0"));
        CHECK("fallback: bands list", ui_control_fallback_cmd(c, sizeof c, sc, "band.set_lte", "bands=1,3,28"));
        CHECK("fallback: quote refused", !ui_control_fallback_cmd(c, sizeof c, sc, "network.set_mode", "mode='x'") && !c[0]);
        CHECK("fallback: ; refused", !ui_control_fallback_cmd(c, sizeof c, sc, "band.reset", "a=1;reboot"));
        CHECK("fallback: $ refused", !ui_control_fallback_cmd(c, sizeof c, sc, "band.reset", "a=$(x)"));
        CHECK("fallback: bad action", !ui_control_fallback_cmd(c, sizeof c, sc, "band.reset;x", ""));
        CHECK("fallback: double space refused", !ui_control_fallback_cmd(c, sizeof c, sc, "wifi.radio", "a=1  b=2"));
        CHECK("fallback: no args pointer", !ui_control_fallback_cmd(c, sizeof c, sc, "band.reset", NULL));
        CHECK("fallback: too long", !ui_control_fallback_cmd(c, 20, sc, "band.reset", "") && !c[0]);
    }

    /* Power key (9-26: a long press on a dark screen did nothing) */
    {
        int p[2];
        key_input_t k = { 0 };
        if (pipe(p) == 0) {
            fcntl(p[0], F_SETFL, O_NONBLOCK);
            k.fd = p[0];
            key_feed(p[1], 1);
            CHECK("key: press → nothing yet", key_input_poll(&k, 1000) == KEY_EV_NONE);
            key_feed(p[1], 0);
            CHECK("key: quick release → short", key_input_poll(&k, 1300) == KEY_EV_SHORT);
            key_feed(p[1], 1);
            CHECK("key: press again", key_input_poll(&k, 2000) == KEY_EV_NONE);
            CHECK("key: held past 1.2 s while polled → long", key_input_poll(&k, 3300) == KEY_EV_LONG);
            key_feed(p[1], 0);
            CHECK("key: release after long → nothing", key_input_poll(&k, 3500) == KEY_EV_NONE);
            /* not polled while held (UI asleep): press and release read together */
            key_feed(p[1], 1);
            CHECK("key: dark press seen", key_input_poll(&k, 5000) == KEY_EV_NONE);
            key_feed(p[1], 0);
            CHECK("key: released 2 s later, never polled while held → long", key_input_poll(&k, 7000) == KEY_EV_LONG);
            close(p[0]); close(p[1]);
        } else CHECK("key: pipe", 0);
    }

    /* ---- 插线时的 USB 用法 ---- */
    {
        ui_usb_pref_t p = UI_USB_ASK;
        CHECK("usb pref: parse share", ui_usb_pref_parse("share\n", &p) == 0 && p == UI_USB_SHARE);
        CHECK("usb pref: parse fast_charge", ui_usb_pref_parse("fast_charge", &p) == 0 && p == UI_USB_FAST);
        CHECK("usb pref: bad value keeps", ui_usb_pref_parse("shares", &p) == -1 && p == UI_USB_FAST);
        CHECK("usb pref: names round-trip", ui_usb_pref_parse(ui_usb_pref_name(UI_USB_ACCESSORY), &p) == 0 &&
                                             p == UI_USB_ACCESSORY && !strcmp(ui_usb_pref_name(UI_USB_ASK), "ask"));

        ui_usb_track_t t = { 0, 0, 0 };
        CHECK("usb: phone already in at start is not a plug-in", ui_usb_track(&t, 1, 1, 100) == UI_USB_EV_NONE);
        CHECK("usb: unknown changes nothing", ui_usb_track(&t, -1, 0, 200) == UI_USB_EV_NONE && !t.seen0);
        ui_usb_track_t u = { 0, 0, 0 };
        CHECK("usb: unplugged at start", ui_usb_track(&u, 0, 0, 100) == UI_USB_EV_NONE && u.seen0);
        CHECK("usb: charger (sink) is not a phone", ui_usb_track(&u, 1, 0, 200) == UI_USB_EV_NONE && u.seen0);
        CHECK("usb: phone 0 → 1 is a plug-in", ui_usb_track(&u, 1, 1, 300) == UI_USB_EV_PLUGGED);
        CHECK("usb: still in, nothing again", ui_usb_track(&u, 1, 0, 400) == UI_USB_EV_NONE);
        CHECK("usb: a short drop is not an unplug", ui_usb_track(&u, 0, 0, 1000) == UI_USB_EV_NONE &&
                                                     ui_usb_track(&u, 0, 0, 2999) == UI_USB_EV_NONE);
        CHECK("usb: back within 2 s, no second plug-in", ui_usb_track(&u, 1, 1, 3000) == UI_USB_EV_NONE);
        ui_usb_track(&u, 0, 0, 5000);
        CHECK("usb: 0 for 2 s is an unplug", ui_usb_track(&u, 0, 0, 7000) == UI_USB_EV_UNPLUGGED && !u.attached);
        CHECK("usb: plug in again", ui_usb_track(&u, 1, 1, 8000) == UI_USB_EV_PLUGGED);
    }

    printf("passed %d, failed %d\n", pass, fail);
    /* ---- two-tap confirm ---- */
    {
        ui_arm_t a = { 0, 0 };
        CHECK("arm: first tap arms", ui_arm_tap(&a, 1, 1000, 5000) == 0 && ui_arm_live(&a, 1, 1000, 5000));
        CHECK("arm: second tap in time confirms", ui_arm_tap(&a, 1, 5999, 5000) == 1 && !ui_arm_live(&a, -1, 5999, 5000));
        ui_arm_tap(&a, 1, 1000, 5000);
        CHECK("arm: second tap too late re-arms", ui_arm_tap(&a, 1, 6000, 5000) == 0 && a.at == 6000);
        CHECK("arm: another id re-arms instead", ui_arm_tap(&a, 2, 6001, 5000) == 0 && a.id == 2);
        CHECK("arm: live any id", ui_arm_live(&a, -1, 6002, 5000) && !ui_arm_live(&a, 1, 6002, 5000));
        CHECK("arm: tick 0 still arms", ui_arm_tap(&a, 3, 0, 4000) == 0 && a.at == 1);
        CHECK("arm: not lapsed yet", ui_arm_expire(&a, 4000, 4000) == 0 && a.at);
        CHECK("arm: lapses once", ui_arm_expire(&a, 4001, 4000) == 1 && ui_arm_expire(&a, 4002, 4000) == 0);
        ui_arm_tap(&a, 3, 10, 4000);
        ui_arm_clear(&a);
        CHECK("arm: cleared", !ui_arm_live(&a, -1, 11, 4000));
        a.at = 0xfffffff0u; a.id = 4;   /* tick wraps */
        CHECK("arm: survives tick wrap", ui_arm_live(&a, 4, 0x10u, 5000));
    }

    return fail ? 1 : 0;
}
