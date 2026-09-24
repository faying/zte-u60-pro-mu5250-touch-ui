/*
 * ui_logic.h - pure decisions behind the touch UI (no LVGL, no I/O), split out
 * of ui.c so they can be unit-tested like uid_core.c:
 *   make ui-logic-test  → tests/ui_logic_test (static, runs in an arm64 container)
 *
 * Covers: appearance (light/dark/auto with a window that may cross midnight),
 * the legacy theme= value the litehtml UI reads, the exec-restart guard used
 * when switching themes (and the argv it hands the next process), the status-bar rate fallback, battery rendering state
 * and the home signal-card state.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60PRO_UI_LOGIC_H
#define U60PRO_UI_LOGIC_H

/* ---- appearance ---- */
typedef enum { UI_APPEAR_LIGHT = 0, UI_APPEAR_DARK, UI_APPEAR_AUTO } ui_appear_t;

/* "light" / "dark" / "auto" → value. Anything else (missing key, typo) falls
 * back to light and returns -1, so a hand-edited devui.conf never turns the
 * screen dark by accident. */
int ui_appear_parse(const char *s, ui_appear_t *out);
const char *ui_appear_name(ui_appear_t a);

/* "HH:MM" (00:00–23:59) → minutes since midnight; -1 on anything else. */
int ui_hhmm_parse(const char *s);

/* Should the screen be dark right now? now_min is the device's local minute
 * of day (the device clock is local time labelled UTC, see deviceClock). The
 * dark window [from, to) may wrap past midnight (19:00–07:00). from == to
 * means "never dark" for auto. Invalid minutes fall back to 19:00–07:00. */
int ui_is_dark(ui_appear_t a, int now_min, int from_min, int to_min);

/* Value written back to the legacy theme= key (litehtml: 0 = dark, 1 = light). */
int ui_legacy_theme_value(int dark);

/* ---- exec-restart guard (theme switch = exec /proc/self/exe) ---- */
#define UI_EXEC_COOLDOWN_S   600   /* no automatic exec within 10 min of the last one        */
#define UI_EXEC_WINDOW_S     3600  /* strikes are counted over an hour                          */
#define UI_EXEC_STRIKES      3     /* 3 automatic execs within the hour → suspend (normal: 2/day) */

typedef struct {
    long first_s;   /* start of the current one-hour window (0 = none) */
    long last_s;    /* time of the last automatic exec (0 = none)        */
    int  count;     /* automatic execs inside the window                  */
} ui_exec_hist_t;

typedef enum { UI_EXEC_ALLOW = 0, UI_EXEC_COOLDOWN, UI_EXEC_SUSPEND } ui_exec_verdict_t;

/* May an automatic (appearance=auto) exec happen now? Manual switches from the
 * settings page never ask this: they are always allowed. */
ui_exec_verdict_t ui_exec_check(const ui_exec_hist_t *h, long now_s);

/* History to hand to the next process for an automatic exec at now_s. */
ui_exec_hist_t ui_exec_next(const ui_exec_hist_t *h, long now_s);

/* Boot-clock seconds (CLOCK_BOOTTIME): carries across exec, resets on reboot,
 * so "suspended until the next boot" needs no extra state, and an NTP jump
 * from 1971 to today cannot confuse the cooldown. */

/* What one process hands the next across exec, as argv:
 *   --tab=N --scroll=Y --screen-off --autooff=MS --bright=L
 *   --exec-first=T --exec-last=T --exec-n=C
 * autooff/bright are runtime-only settings (not read from devui.conf at boot),
 * so without them a theme switch would silently reset them. */
typedef struct {
    int tab;          /* 0–3, -1 = not given        */
    int scroll_y;     /* ≥ 0                         */
    int screen_off;   /* start with the backlight off: 1 = it went off by itself (auto-off),
                         2 = the power key turned it off (only the key wakes it)   */
    int autooff_ms;   /* -1 = not given              */
    int bright;       /* 1–255, -1 = not given       */
    ui_exec_hist_t hist;
} ui_launch_t;

#define UI_LAUNCH_MAXARG 8
#define UI_LAUNCH_ARGLEN 32

/* Unknown or malformed arguments are ignored (fields keep their defaults). */
void ui_launch_parse(int argc, char **argv, ui_launch_t *out);
/* Fill store[] and argv_out[] (argv_out gets at most UI_LAUNCH_MAXARG entries,
 * not NULL-terminated); returns the count. Fields at their defaults are left out. */
int ui_launch_argv(const ui_launch_t *l, char store[UI_LAUNCH_MAXARG][UI_LAUNCH_ARGLEN], char *argv_out[UI_LAUNCH_MAXARG]);

/* ---- clock ---- */
/* The device RTC reads ~1971 until the network sets it (RELIABILITY.md §4):
 * wall seconds below 2024-01-01 are not a time of day to act on. */
#define UI_CLOCK_SANE_AFTER 1704067200L
int ui_clock_sane(long wall_s);

/* ---- automatic appearance ---- */
typedef struct {
    ui_appear_t appear;
    int  cur_dark;             /* theme this process was built with            */
    long wall_s;               /* for the sanity check only                    */
    int  now_min, from_min, to_min;
    int  screen_off;           /* backlight is off                             */
    int  always_on;            /* 自动息屏 = 常亮                              */
    long idle_ms;              /* no touch for this long                       */
    int  busy;                 /* speed test / pending /control reply          */
    int  suspended;            /* strike limit already hit this boot           */
    ui_exec_hist_t hist;
    long now_boot_s;
} ui_auto_in_t;

typedef enum {
    UI_AUTO_STAY = 0,   /* nothing to do                                        */
    UI_AUTO_WAIT,       /* the theme should change, but not now                 */
    UI_AUTO_EXEC,       /* switch now (target = !cur_dark)                      */
    UI_AUTO_SUSPEND,    /* strike limit reached: stop auto-switching this boot  */
} ui_auto_t;

#define UI_AUTO_ALWAYS_ON_IDLE_MS 60000   /* 常亮: switch after 60 s without a touch */

/* Switch only while nobody is looking (screen off, or 60 s untouched when the
 * screen never turns off), never mid-operation, never on an unsynced clock. */
ui_auto_t ui_auto_decide(const ui_auto_in_t *in);

/* ---- touch while the screen is dark ----
 * Nothing on a dark screen can be tapped. Waking it is not a tap either:
 *   - screen turned off by the power key: touch does nothing, only the key wakes it;
 *   - screen went off by itself (auto-off): a double tap wakes it;
 *   - the waking gesture (double tap, or a finger already down when the key
 *     woke it) never reaches the UI, and for 300 ms after waking nothing does.
 * Feed every touch read through ui_tgate_step; forward the press to LVGL only
 * when it returns UI_TG_FORWARD. */
#define UI_TAP_MAX_MS     350   /* longer than this is not a tap            */
#define UI_TAP_SLOP       24    /* px a tap may move                        */
#define UI_DTAP_GAP_MS    450   /* second tap must start this soon          */
#define UI_DTAP_SLOP      60    /* px between the two taps                  */
#define UI_WAKE_QUIET_MS  300   /* nothing reaches the UI right after waking */

typedef struct {
    int  was, taps, px, py, x0, y0, suppress;
    long t_down, t_up, wake_ms;
} ui_tgate_t;

enum { UI_TG_FORWARD = 1, UI_TG_WAKE = 2 };

/* lit: the backlight is on; touch_wakes: the screen went off by itself.
 * Returns UI_TG_FORWARD (pass this press on) and/or UI_TG_WAKE (light it). */
int  ui_tgate_step(ui_tgate_t *g, int lit, int touch_wakes, int pressed, int x, int y, long now_ms);
/* The screen was just lit some other way (power key): hold touches back the same way. */
void ui_tgate_woke(ui_tgate_t *g, long now_ms);

/* ---- status-bar rate ---- */
/* Given the rendered widths of the three formats (full "↓12.4MB ↑380KB",
 * short "↓12M ↑380K", down-only "↓12M") and the space available, return the
 * first index that fits; the down-only format is used even if it overflows. */
int ui_rate_pick(const int widths[3], int maxw);

/* ---- battery ---- */
typedef enum { UI_BAT_NORMAL = 0, UI_BAT_CHARGING, UI_BAT_LOW } ui_bat_state_t;

ui_bat_state_t ui_bat_state(int pct, int charging);
int ui_bat_body_w(int pct, int charging);   /* 27, 30 at 100 %; charging does not change it */
int ui_bat_red_w(int pct, int body_w);      /* low state's red fill: ≥3 px, ≤ 20 % of the body */

/* ---- home signal card ---- */
typedef enum {
    UI_SIG_LOADING = 0,   /* no snapshot since boot yet: 「正在读取…」            */
    UI_SIG_STALE,         /* had data, data service gone: 「数字停在 hh:mm」       */
    UI_SIG_NOSIM,         /* datad reports no usable SIM                          */
    UI_SIG_NONE,          /* SIM fine, no service                                 */
    UI_SIG_WEAK,
    UI_SIG_GOOD,
} ui_sig_state_t;

/* ever_valid: a valid snapshot arrived since boot; valid: the current one is
 * valid; sim_state: datad's sim.state ("sim ready", "sim absent"… or "" when
 * unknown); bars: 0–5. Weak = 1–2 bars or SINR < 0 on the serving carrier. */
ui_sig_state_t ui_sig_state(int ever_valid, int valid, const char *sim_state, int bars, int sinr_valid, double sinr);

#endif /* U60PRO_UI_LOGIC_H */
