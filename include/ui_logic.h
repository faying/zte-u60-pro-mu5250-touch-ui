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

#include <stddef.h>

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

/* Operator logo for the home card (2026-09-26): MCC/MNC of the network you
 * are on → file slug under operator-logos/ (<slug>.png, <slug>-w.png for
 * dark), NULL = no logo. Same table as manager web/src/lib/operatorLogo.ts,
 * minus slugs with no file. */
const char *ui_operator_logo(int mcc, int mnc);
/* The SIM's own logo: the card's brand name (SPN) first where one PLMN is shared
 * by several brands (CMLink and CMHK are both 454-12), else by MCC/MNC. */
const char *ui_sim_logo(int mcc, int mnc, const char *spn);

/* The SIM's own network from its IMSI: MCC = 3 digits, MNC = 3 digits in
 * North America (MCC 302/310–316) and a few others, else 2. 0 = not an IMSI. */
int ui_imsi_plmn(const char *imsi, int *mcc, int *mnc);

/* DHCP pool text for the Wi-Fi page from datad's /state dhcp block:
 * ip "192.168.0.1", start "100" (host number; a full address also works),
 * limit "50" → "192.168.0.100 - 192.168.0.149" (end capped at 254). No
 * usable limit → just the start address. Bad ip/start → "". */
void ui_dhcp_pool_text(const char *ip, const char *start, const char *limit, char *out, size_t n);

/* datad /control reply → run the direct-ubus fallback? `head` is the first
 * bytes of the reply (NUL-terminated), `n` what recv() returned. Yes for
 * 503 (control queue full) and for a connection closed without a reply
 * (n == 0: datad went away mid-request). No for anything else, including
 * errors: datad ran it, running it again would not help. */
int ui_control_should_fallback(const char *head, long n);

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

/* ---- radio technology names ----
 * datad's net.type is the modem's raw network_type: "SA", "NSA", "LTE",
 * "WCDMA", "GSM", … (and variants: ENDC, LTE-A, HSPA+, EDGE, TD-SCDMA…).
 * The status bar has room for two characters, the status card for a word. */
typedef enum { UI_RAT_NONE = 0, UI_RAT_2G, UI_RAT_3G, UI_RAT_4G, UI_RAT_5G_NSA, UI_RAT_5G_SA } ui_rat_t;
ui_rat_t    ui_rat(const char *raw);
const char *ui_rat_short(const char *raw);   /* "5G" "4G" "3G" "2G"; anything else → "" */
/* The finer name under the status bar label: "5G SA" "5G NSA"
 * "4G LTE" "3G WCDMA" "3G HSPA+" "2G EDGE"…; "" when not on a network. out ≥ 24. */
void        ui_rat_long(const char *raw, int lte_active, char *out, int n);  /* 5G SA / 5G NSA · 4G 锚点 / 4G LTE-A … */

/* The label a phone would show, from the raw type and how many carriers are
 * in use: "2G" "3G" "3G+" "4G" "4G+" "5G" "5G+" "5G-A"; "" when not on a
 * network. 5G+ = 2 NR carriers aggregated, 5G-A = 3 or more (the modem has no
 * 5G-Advanced flag; three-carrier NR is how Chinese carriers deliver it), or
 * the raw type already says 5G-A. */
void ui_net_label(const char *raw, int nr_active, int lte_active, char *out, int n);
/* The radio-mode preference (`net_select`) in words: "自动" "只用 5G SA"
 * "5G NSA + 4G" "只用 4G" "4G + 3G" …; the firmware's 14 values
 * (zte_topsw_nwinfo) are all named, anything else comes back as-is, "" as "-".
 * B27 reports both WL_AND_5G and TCHGWL_5G for automatic. */
const char *ui_net_select_word(const char *sel);
/* 1 for the two automatic values (WL_AND_5G, TCHGWL_5G). */
int ui_net_select_is_auto(const char *sel);
/* The specific technology for small print: "GPRS" "EDGE" "GSM" "CDMA 1X"
 * "WCDMA" "HSPA" "HSPA+" "TD-SCDMA" "CDMA2000" "LTE" "NR"; "" if unknown. */
const char *ui_rat_family(const char *raw);

/* A band as the modem writes it ("LTE BAND 3", "B3", "3", "NR5G BAND 78",
 * "n78") → "B3" / "n78" (nr = the NR prefix). No digits → the raw text. */
void ui_band_short(const char *raw, int nr, char *out, int n);

/* ---- what the network situation means, in words ----
 * The home status card leads with this, and shows the raw figures only as
 * supporting detail (2026-09-24: "不要直接暴露参数，要有让人听得懂的解读").
 * Every situation the card can be in is decided here, in one priority order,
 * so it can be tested as a table. */
typedef struct {
    int  ever_valid, valid;     /* datad snapshot state                        */
    const char *sim_state;      /* "sim ready" …                               */
    int  airplane;              /* operate_mode LPM / OFFLINE                  */
    const char *net_type;       /* raw network_type                            */
    int  bars;                  /* 0–5                                         */
    int  data_up;               /* WAN connected (wan_status …connected)       */
    int  roaming;               /* 1 / 0 / -1 unknown                          */
    int  n_active;              /* active carriers                             */
    int  nr_active, lte_active; /* of which NR / LTE (for 5G+ / 5G-A / 4G+)    */
    int  mhz;                   /* their total bandwidth, 0 = unknown          */
    int  sinr_valid;  double sinr;
    int  rsrp_valid;  int rsrp;
    int  rsrq_valid;  int rsrq; /* serving cell, dB                            */
    int  mcc, mnc;              /* the network you are on (roaming: the visited one) */
    int  nr_band;               /* primary NR band number (41, 77 …), 0 = none  */
    int  nr_mhz;                /* active NR bandwidth                         */
    long rx_bps;                /* cellular download now, bytes/s              */
    double ambr_dl;             /* operator cap, Mbps; 0 = unknown             */
    const char *net_select;     /* radio-mode preference (Only_LTE, WL_AND_5G…) */
} ui_net_in_t;

typedef enum { UI_NET_OK = 0, UI_NET_WARN, UI_NET_BAD, UI_NET_NEUTRAL } ui_net_tone_t;

/* Why it is slow (2026-09-25, docs/designs/home-net-card.md): the first
 * that holds, in this order. Congestion is only guessed while you are
 * downloading — with no traffic there is nothing to tell it by. */
typedef enum { UI_CAUSE_NONE = 0, UI_CAUSE_LIMIT, UI_CAUSE_WEAK, UI_CAUSE_NOISE, UI_CAUSE_CROWD, UI_CAUSE_NARROW } ui_net_cause_t;

typedef struct {
    ui_net_tone_t tone;
    ui_net_cause_t cause;
    char headline[32];      /* 顺畅 / 慢：信号弱 / 慢：干扰大 / 慢：疑似拥挤 / 漫游中 / 无服务 … */
    char hint[128];         /* what it means / what to do; "" when all is fine */
    char rat[32];           /* the phone-style label: 5G-A / 5G+ / 5G / 4G+ / 4G / 3G+ / 3G / 2G */
    char link[96];          /* 3 条载波聚合 · 带宽很宽 / 单载波 · 带宽一般 / 不支持载波聚合 */
    /* three separate things, one word each; none implies another
     * (strong signal can still be noisy or crowded) */
    char sig[8];            /* 强 / 中 / 弱 — from the status-bar bars, so the two always agree */
    ui_net_tone_t sig_tone;
    char noise[8];          /* 干扰 小 / 中 / 大 from SINR; "" without SINR */
    ui_net_tone_t noise_tone;
    char load[12];          /* 负载 正常 / 高; "" when idle or signal too weak to tell */
    char limit[8];          /* 无 / 有 / — (AMBR unknown)                   */
} ui_net_story_t;

void ui_net_story(const ui_net_in_t *in, ui_net_story_t *out);

/* Signal tier from the status-bar bars: 2 强 (4–5), 1 中 (3), 0 弱 (1–2), -1 none.
 * The status bar colours its dots by this and the Home card writes the word. */
int ui_bars_tier(int bars);

/* The status-bar label (docs/designs/home-net-card.md「状态栏和首页顶行的制式
 * 叫法」): plain 5G / 4G / 3G / 2G, plus the operator's own name where the
 * network you are on has one — 5G-A (mainland China: ≥3 NR carriers, or
 * China Unicom 2 carriers ≥ 200 MHz), 5G UC / 5G UW / 5G+ (T-Mobile n41,
 * Verizon n77/n48, AT&T n77 or ≥ 50 MHz), 4G+ (Taiwan, Japan: LTE CA),
 * LTE (US). Roaming uses the visited network's rules: they describe the
 * network you are actually on. The U60 Pro has no mmWave, so no mmWave rule. */
void ui_net_badge(const ui_net_in_t *in, char *out, int n);

/* A new verdict has to last 15 s before the headline changes, so it does
 * not flicker at a threshold. key = anything that tells verdicts apart
 * (a hash of the headline); returns 1 when the caller should show it. */
typedef struct { unsigned shown, pending; unsigned since; int have; } ui_net_hold_t;
#define UI_NET_HOLD_MS 15000u
int ui_net_hold(ui_net_hold_t *h, unsigned key, unsigned now_ms);

/* Which card the modem is using. The SIM slot can hold a plain SIM or an
 * eUICC (eSIM card); it is the eSIM when the modem's ICCID is the ICCID of
 * the profile lpac reports enabled. ICCIDs compare without the trailing F
 * padding the modem adds to 19-digit ones, case-insensitively. */
typedef enum { UI_SIM_NONE = 0, UI_SIM_PLAIN, UI_SIM_ESIM } ui_sim_kind_t;
int ui_iccid_same(const char *a, const char *b);
ui_sim_kind_t ui_sim_kind(const char *sim_state, const char *modem_iccid, const char *esim_enabled_iccid);

#endif /* U60PRO_UI_LOGIC_H */
