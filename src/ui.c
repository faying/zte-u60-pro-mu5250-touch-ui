/*
 * ui.c - U60Pro multi-page dashboard (LVGL tileview).
 *
 * Pages swipe horizontally: [Home] [Network]. Live values come from zwrt-datad
 * via data.c, refreshed at 1 Hz. Power key: short = backlight on/off,
 * long = power menu. No ubus here.
 *
 * SPDX-License-Identifier: MIT
 */
#include "ui.h"
#include "data.h"
#include "key_input.h"
#include "touch_input.h"
#include "backlight.h"
#include "chill.h"
#include "tailscale.h"
#include "scenario.h"
#include "esim.h"
#include "speedtest.h"
#include "alerts.h"
#include "ui_logic.h"
#include "ui_theme.h"
#include "ui_exec.h"
#include "ui_kit.h"
#include "lvgl.h"

#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

extern unsigned long g_frame_count;   /* defined in main.c */

/* ---- navigation model (2026-09-21) ----
 * Four swipeable top-level pages plus a subpage layer, mirroring the litehtml
 * UI this device already runs (信号 / 图表 / 功能磁贴 / 系统 + 二级页). The
 * previous flat 6-tab layout had no room for 短信/CHILL/信令读取/锁频/测速 and
 * had invented a "网络" page that only duplicated Home's cellular card. */
#define UI_TABS 4
enum { TAB_HOME, TAB_CHART, TAB_FUNC, TAB_SYS };
/* Subpages: reached from the 功能 tile wall, drawn over the tileview on the
 * active screen (so the shared top bar and tab bar, which live on
 * lv_layer_top(), still paint above them). */
enum { SUB_WIFI, SUB_SMS, SUB_CELL, SUB_LOCK, SUB_SPEED, SUB_CHILL, SUB_ESIM, SUB_PERF,
       SUB_TS,   /* not on the tile wall: opened by tapping the Home Tailscale card */
       /* Not on the tile wall either: opened by tapping a nav row on the CHILL
        * page itself (see sub_open_child()/s_sub_parent). Splitting these out
        * keeps the CHILL page itself light — it used to build a 200-slot node
        * list, a 12-button group grid, and a 6-row traffic card all on one
        * scrollable page, which is exactly the "一屏展示了很多的东西，会卡"
        * complaint from 2026-09-22: heavy even before you scroll into any of
        * it, because everything is pre-built and stacked on the same page.
        * 策略组 and 节点 stay ON THE SAME subpage (SUB_CHILL_NODES) — a
        * first cut split them into two separate subpages, which broke the
        * one relationship that actually matters: tapping a group is supposed
        * to load THAT group's member nodes right there, not send you off to
        * an unrelated page (2026-09-22 follow-up: "策略组下面展开节点啊，
        * 这两个是紧密关联的" — same shape mihomo's own dashboards use,
        * yacd/metacubexd/zashboard all show a group's proxies inline under
        * it). Only 规则→节点流量 is a genuinely separate concept (traffic
        * accounting, not node selection), so that one keeps its own page. */
       SUB_CHILL_NODES, SUB_CHILL_PAIRS,
       /* Not on the tile wall: one message in full, opened from the SMS list
        * (sub_open_child, so 返回 goes back to the list). */
       SUB_SMS_DETAIL,
       /* Not on the tile wall: opened from the status bar's alert dot or the
        * 系统 page's 健康 row. */
       SUB_ALERTS,
       SUB_N };

/* ---- shared widget handles ---- */
/* Home page: signal card (status block + carrier rows + traffic), 情景,
 * CHILL, Tailscale — stacked, reflowed every refresh (home_reflow). */
/* 5 carrier slots: 3 NR + 2 LTE covers EN-DC on this modem with headroom. */
#define CA_SLOTS 5
typedef struct {
    lv_obj_t *box, *band, *bw, *rsrp, *rsrp_c, *sinr, *sinr_c, *pci, *arfcn;   /* active: 40 px   */
    lv_obj_t *ina, *ina_tag, *ina_info;                                        /* inactive: 32 px */
    lv_obj_t *sep;
} home_ca_t;
static home_ca_t s_ca[CA_SLOTS];
static lv_obj_t *s_home_scroll, *s_cell_card, *s_cc_hint, *s_cc_tsep, *s_cc_tkey, *s_cc_traffic;
static uk_hero_t s_cc_hero;
/* 情景 — zte-agent 情景引擎的当前判定，只读。 */
#define SC_CARD_H 72
static lv_obj_t *s_sc_card, *s_sc_state, *s_sc_note;
static int s_sc_force;          /* 情景卡片要按新状态重画 */
/* 国外时点情景卡片弹出的「CHILL 出口」面板（见 build_exit_menu） */
enum { XM_PROXY, XM_GLOBAL, XM_KEEP_AI, XM_ALL, XM_OFF, XM_N };
static lv_obj_t *s_xm, *s_xm_btn[XM_N], *s_xm_lbl[XM_N], *s_xm_foot, *s_xm_ok[XM_N];
static uk_sheet_t s_xm_sheet;
static uint32_t s_xm_off_arm;   /* 「关闭」点了第一下的时刻，0 = 没准备 */
#define XM_ARM_MS 4000
static lv_obj_t *s_cp_sw;       /* CHILL 页的总开关 */
static lv_obj_t *s_cp_exit_note;  /* CHILL 页「出口」行右边的说明 */
/* Home Tailscale: a grouped list; rows past the first hide when not running. */
#define TS_HOME_ROWS 5     /* Tailscale · 本机 · 节点 · 子网 · 出口/提示 */
static lv_obj_t *s_ts_card, *s_ts_dot, *s_ts_val[TS_HOME_ROWS], *s_ts_key[TS_HOME_ROWS], *s_ts_sep[TS_HOME_ROWS];
static int s_ts_rows = 1;
/* Charts page */
/* 60 points, one per 5 s (the average of five 1 s readings) = the last 5
 * minutes, which is what the cards say. */
#define CHART_PTS   60
#define CHART_STEP  5
#define CHART_READY 12   /* points before a curve is worth showing (1 min) */
static lv_obj_t *s_ch_cpu, *s_ch_mem, *s_ch_net, *s_ch_bat;
static lv_chart_series_t *s_cs_cpu, *s_cs_mem, *s_cs_rx, *s_cs_tx, *s_cs_bat;
static lv_obj_t *s_ch_net_dn, *s_ch_net_up, *s_ch_cpu_v, *s_ch_cpu_t, *s_ch_mem_v, *s_ch_mem_s,
                *s_ch_bat_v, *s_ch_bat_s, *s_ch_wait[4];
/* Function tile wall */
static lv_obj_t *s_tile_sub[SUB_N];      /* per-tile status subtitle */
static uk_tile_t s_tile[SUB_N];
/* CHILL — home card shows the real "规则 -> 节点" traffic breakdown directly
 * (top N pairs), not a one-line "X 等 N 个" summary with the actual numbers
 * hidden a scroll away on the detail page (2026-09-22 user feedback: the fix
 * belongs on the card people actually look at). Node and group used to be
 * two independent top-N lists stacked together, which looks like row i of
 * one corresponds to row i of the other but doesn't — they're unrelated
 * rankings (2026-09-22 follow-up: "不同的分流规则到底具体走的哪个节点，
 * 没有放出来"). Now one list, keyed by (group, node), so each row directly
 * answers "this rule's traffic went through this node". */
#define CHILL_HOME_ROWS 5
#define CHILL_HOME_ROW_H 36
#define CHILL_HOME_TOP   100   /* pair rows start here */
static lv_obj_t *s_chill_card, *s_chill_state, *s_chill_rate, *s_chill_split, *s_chill_line, *s_chill_total,
                *s_chill_pair_name[CHILL_HOME_ROWS], *s_chill_pair_val[CHILL_HOME_ROWS],
                *s_chill_pair_sep[CHILL_HOME_ROWS];
static int s_chill_rows = 1;
/* 2026-09-22：新增的手动选节点组一个就有 168 个节点，8 太小——家宽/NX 节点
 * 排在后面，直接被截没，界面上看起来像是"消失了"。卡片本来就在可滚动的
 * 容器里（build_sub_chill_nodes 的 uk_scroll），提高上限只是多建几个隐藏行，
 * 没有别的副作用；配 chill.c 的 SC_MAX_NODE=200。 */
#define CHILL_MAX_NODES 200
#define CHILL_MAX_GROUPS 12
#define CHILL_GRP_COLS 3
static lv_obj_t *s_cp_core, *s_cp_conns, *s_cp_traffic,
                *s_cp_mode_btn[4], *s_cp_node_card, *s_cp_node_row[CHILL_MAX_NODES],
                *s_cp_node_name[CHILL_MAX_NODES], *s_cp_node_dl[CHILL_MAX_NODES],
                *s_cp_grp_card, *s_cp_grp_btn[CHILL_MAX_GROUPS], *s_cp_grp_lbl[CHILL_MAX_GROUPS],
                *s_cp_delay_lbl, *s_cp_node_ok[CHILL_MAX_NODES], *s_cp_node_sec, *s_cp_scroll_nodes;
static uk_hero_t s_cp_hero;
static uk_opt_t  s_cp_opt[4];
static uk_opt_t  s_cp_prof[3];     /* CHILL 页「档位」三项 */
static lv_obj_t *s_cp_prof_note;   /* 「档位」行右边：降温中 / 切换说明 */
static const char *const k_profile[3] = { "eco", "standard", "perf" };
/* 流量分布：一张"规则 -> 节点"卡，跟上面的"节点"卡不是一回事——那张卡是
 * 节点选择器（点了会切换节点），这张是只读统计，数字来自 chill_top_pair()，
 * 跟 chill.h 里的大注释对应：反映规则模式下流量实际去哪了，不是"配置了哪个
 * 节点"。曾经拆成按节点、按分流组两张独立卡，各自的 top N 排名互不相干，
 * 摆在一起容易被误读成一一对应（2026-09-22 反馈：具体哪条规则走了哪个
 * 节点，没有放出来）——改成一张卡，一行就是一对真实关系。行数跟 chill.c 的
 * SC_TOP_SHOW 对齐，改一边要记得改另一边。 */
#define CHILL_TRAF_ROWS 6
static lv_obj_t *s_cp_pair_card, *s_cp_pair_name[CHILL_TRAF_ROWS], *s_cp_pair_val[CHILL_TRAF_ROWS],
                *s_cp_pair_sep[CHILL_TRAF_ROWS];
/* CHILL page nav rows — 策略组/节点/规则→节点 used to be built inline on the
 * CHILL page itself (12 group buttons + up to 200 node rows + 6 pair rows,
 * all pre-built whether you ever scroll to them or not). One screen showing
 * everything at once was the 2026-09-22 "会卡" complaint, so the heavy
 * content moved to drill-down subpages and the CHILL page itself only keeps
 * a one-line summary + chevron per section. 策略组+节点 share ONE row/page
 * (SUB_CHILL_NODES) — they're the same picker, not two unrelated lists. */
#define CHILL_NAV_ROWS 2
enum { CHILL_NAV_NODES, CHILL_NAV_PAIRS };
static lv_obj_t *s_cp_nav_val[CHILL_NAV_ROWS];
/* WiFi page */
#define WIFI_MAX_CLI 5    /* fixed sub-card slots; backend reports up to 16 */
static lv_obj_t *s_w_ssid, *s_w_pass, *s_w_enc, *s_w_state;
static lv_obj_t *s_w_cli_card, *s_w_cli_n, *s_w_dhcp_card, *s_w_gw, *s_w_pool, *s_w_lease;
static lv_obj_t *s_w_cli[WIFI_MAX_CLI], *s_w_cli_name[WIFI_MAX_CLI],
                *s_w_cli_ip[WIFI_MAX_CLI], *s_w_cli_mac[WIFI_MAX_CLI];
static lv_obj_t *s_w_sw[5], *s_w_sw_st[5], *s_w_scroll, *s_w_cli_sec, *s_w_dhcp_sec, *s_w_cli_empty;
static lv_obj_t *s_w_cli_sep[WIFI_MAX_CLI];
/* eSIM page */
#define ESIM_MAX_ROWS 5   /* fits one screen without scrolling; backend caps at 16 */
static lv_obj_t *s_es_cur, *s_es_state, *s_es_list_card, *s_es_empty, *s_es_row_sep[ESIM_MAX_ROWS];
static uk_hero_t s_es_hero;
static lv_obj_t *s_es_row[ESIM_MAX_ROWS], *s_es_row_name[ESIM_MAX_ROWS],
                *s_es_row_sub[ESIM_MAX_ROWS], *s_es_row_tag[ESIM_MAX_ROWS];
/* System page */
static lv_obj_t *s_set_bright, *s_set_bright_v, *s_vendor_btn, *s_vendor_lbl;
static uk_seg_t  s_off_seg, s_ap_seg;
static uint32_t  s_vendor_arm;
static lv_obj_t *s_set_ver, *s_set_imei, *s_set_usb, *s_set_fw, *s_set_health;
static lv_obj_t *s_sy_bat, *s_sy_chg, *s_sy_cpu, *s_sy_mem, *s_sy_up;
static lv_obj_t *s_sy_dps_sw, *s_sy_dps_st;
static lv_obj_t *s_sy_speedunit_sw, *s_sy_speedunit_st;
/* Tailscale subpage */
static lv_obj_t *s_tp_sep[TS_PEER_MAX];
static lv_obj_t *s_tp_self[4], *s_tp_card, *s_tp_row[TS_PEER_MAX],
                *s_tp_name[TS_PEER_MAX], *s_tp_ip[TS_PEER_MAX], *s_tp_tag[TS_PEER_MAX];
/* 信令读取 subpage */
static lv_obj_t *s_sg_nr[6], *s_sg_lte, *s_sg_net[4], *s_sg_nrb, *s_sg_lteb;
/* 锁频 subpage */
#define BAND_MAX 28
#define BG_SA 0
#define BG_NSA 1
#define BG_LTE 2
typedef struct {
    lv_obj_t *card, *sec, *summary, *apply_btn, *apply_lbl;
    lv_obj_t *chip[BAND_MAX], *chip_lbl[BAND_MAX];
    int   band_no[BAND_MAX];
    char  sel[BAND_MAX];
    int   n;
    char  prefix;
    char  last_csv[256];      /* the supported-band CSV the chips were built from */
    uint32_t arm;             /* two-stage confirm timestamp */
} band_group_t;
static band_group_t s_bg[3];
static lv_obj_t *s_lk_mode_btn[4], *s_lk_mode_lbl, *s_lk_reset_lbl, *s_lk_reset_btn, *s_lk_reset_card,
                *s_lk_reset_sec, *s_lk_scroll;
static uk_seg_t  s_lk_seg;
static uint32_t  s_lk_mode_arm, s_lk_reset_arm;
static int       s_lk_mode_pending = -1;
/* SMS subpage: a toolbar (unread count + 全部已读), then one card per
 * message showing two lines; tapping a card opens SUB_SMS_DETAIL. */
#define SMS_MAX_ROWS 12
#define SMS_TOOLBAR_H 44
#define SMS_ROW_H 72
static lv_obj_t *s_sms_list, *s_al_list;
static lv_obj_t *s_sms_card, *s_sms_row[SMS_MAX_ROWS], *s_sms_num[SMS_MAX_ROWS],
                *s_sms_date[SMS_MAX_ROWS], *s_sms_body[SMS_MAX_ROWS], *s_sms_dot[SMS_MAX_ROWS];
static lv_obj_t *s_sms_count, *s_sms_allread_btn, *s_sms_empty;
static long      s_sms_row_id[SMS_MAX_ROWS];
/* SMS detail subpage */
static long      s_smsd_id = -1;
static lv_obj_t *s_smsd_scroll, *s_smsd_num, *s_smsd_date, *s_smsd_body, *s_smsd_del_btn, *s_smsd_del_lbl;
static uint32_t  s_smsd_del_arm;
static uint32_t  s_autooff_ms = 0;   /* 0 = never */
static int       s_auto_slept = 0;   /* the screen went off by itself (not the power key) */
static ui_tgate_t s_tgate;           /* touch while dark: see ui_logic.h */
/* This process's appearance, fixed at start (a switch execs a new process). */
static int         s_dark;
static ui_launch_t s_launch = { -1, 0, 0, -1, -1, { 0, 0, 0 } };
static const char *s_argv0 = "u60pro-devui";
static int         s_auto_suspended;
static int         s_wake_guard;       /* started with the screen off: wait for a real touch */
static uint32_t    s_wake_idle_last;
/* Test page */
static lv_obj_t *s_t_fps, *s_t_touch, *s_t_box;
static lv_timer_t *s_bench_timer;
/* Speedtest page */
static lv_obj_t *s_st_card, *s_st_phase, *s_st_live, *s_st_detail,
                *s_st_result, *s_st_server, *s_st_btn, *s_st_btn_lbl,
                *s_st_offline;
#define ST_SRV_ROWS (ST_MAX_SERVERS + 1)   /* +1 = "自动" 固定占第 0 行 */
static lv_obj_t *s_st_srv_card, *s_st_unit, *s_st_srv_ok[ST_SRV_ROWS];
static lv_obj_t *s_st_srv_row[ST_SRV_ROWS], *s_st_srv_name[ST_SRV_ROWS];
static int       s_box_x = 0, s_box_dir = 1;
static lv_obj_t   *s_tv, *s_tiles[UI_TABS], *s_tabs[UI_TABS];
static lv_obj_t   *s_sub_layer, *s_sub_title, *s_sub_page[SUB_N];
static int         s_sub_cur = -1;
/* -1 = s_sub_cur is a top-level subpage (opened from a tile or the Home
 * Tailscale card); otherwise the id to return to when 返回 is tapped, set by
 * sub_open_child() for the CHILL nav-row drill-down pages. One level deep
 * only — these child pages don't open further children. */
static int         s_sub_parent = -1;
static key_input_t s_key;
static lv_timer_t *s_key_timer;   /* paused while the screen is dark: main.c polls the key fd */
static lv_obj_t   *s_power_menu;
/* Global status banner (backend down) — device-wide state, so it lives in the
 * shared chrome on lv_layer_top() rather than in any one page. */
static lv_obj_t   *s_banner, *s_banner_txt;
/* Fixed top status bar — time/signal/throughput/battery, visible on every
 * page (not just Home), same "shared chrome" pattern as the tab bar. */
static lv_obj_t   *s_top_time, *s_top_net, *s_top_updown, *s_top_sig[5];
static lv_obj_t   *s_top_alert;   /* ▲ — badT: admin backend lost; warnT: unread alerts */
static int         s_alert_st;    /* 0 none, 1 unread, 2 agent lost */
static uk_battery_t s_top_batt;
static void statusbar_layout(const char *full, const char *shrt, const char *down, int pct, int chg, int alert, uint32_t alert_col);
/* Alerts subpage */
static lv_obj_t   *s_al_count, *s_al_allread_btn, *s_al_empty;
static lv_obj_t   *s_al_row[ALERTS_MAX], *s_al_label[ALERTS_MAX], *s_al_time[ALERTS_MAX],
                  *s_al_text[ALERTS_MAX], *s_al_mark[ALERTS_MAX];

/* ================= layout =================
 * Sizes, colours and fonts live in ui_kit.h / ui_theme.h; pages compose kit
 * components and never set their own radius, colour or font. These are the
 * screen regions. */
#define UI_W            UK_W
#define UI_H            UK_H
#define UI_TOPBAR_H     UK_BAR_H                 /* fixed top status bar */
/* A top-level page's area is everything under the status bar: the floating
 * tab covers content (scroll bodies keep UK_TAB_PAD clear at the end). */
#define UI_VIEW_H       (UI_H - UI_TOPBAR_H)
#define UI_SUB_HDR      (UK_NAV_H - UK_BAR_H)    /* subpage header below the status bar: ‹ + title */
#define UI_SUB_VIEW     (UI_H - UK_NAV_H)


static void banner_set(const char *txt);   /* shared chrome, defined below */

/* ---- formatting helpers ---- */
static void fmt_rate(char *out, size_t n, long bps)
{
    if (bps >= 1024 * 1024) snprintf(out, n, "%.1f MB/s", bps / 1048576.0);
    else if (bps >= 1024)   snprintf(out, n, "%.1f KB/s", bps / 1024.0);
    else                    snprintf(out, n, "%ld B/s", bps);
}
/* Total quantity, not a rate — no "/s". Used for the day/month traffic
 * counters, which run from a few MB up to hundreds of GB. */
static void fmt_bytes_total(char *out, size_t n, long b)
{
    if (b >= 1024L * 1024 * 1024) snprintf(out, n, "%.1fGB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024 * 1024)    snprintf(out, n, "%.0fMB", b / (1024.0 * 1024));
    else if (b >= 1024)           snprintf(out, n, "%.0fKB", b / 1024.0);
    else                          snprintf(out, n, "%ldB", b);
}
/* Status bar: "12.4MB" / "380KB" (bits: "12.4Mb"), or the short form
 * "12M" / "380K" (bits: "12Mb") when space runs out. The unit letter stays in
 * both, so a Mbps/MB/s switch is visible. */
static void fmt_rate_top(char *out, size_t n, long Bps, int bits, int shortf)
{
    double v = bits ? Bps * 8.0 : (double)Bps;
    double k = bits ? 1000.0 : 1024.0;
    const char *u = bits ? "b" : "B";
    if (v >= k * k) {
        if (shortf) snprintf(out, n, "%.0fM%s", v / (k * k), bits ? "b" : "");
        else        snprintf(out, n, "%.1fM%s", v / (k * k), u);
    } else if (v >= k) {
        snprintf(out, n, "%.0fK%s", v / k, shortf && !bits ? "" : u);
    } else {
        snprintf(out, n, "%.0f%s", v, u);
    }
}


/* ---- persisted UI settings, shared file with htmlmain.c's load_conf()/
 * save_conf() (src/htmlmain.c:815-844) — same devui.conf, same key set, so
 * whichever binary runs doesn't clobber the other's settings. LVGL only
 * acts on speed_bits (2026-09-21: tap-to-toggle Mbps/MB/s on the status
 * bar); the rest just round-trip verbatim through load/save. */
#ifndef DEVUI_CONF_FILE
#define DEVUI_CONF_FILE "/data/plugins/u60pro-devui/devui.conf"
#endif
static int  s_cf_theme = 0, s_cf_speed_bits = 1, s_cf_show_batpct = 1,
            s_cf_autooff_ms = 60000, s_cf_refresh_ms = 5000,
            s_cf_sig_read = 0, s_cf_sig_parse = 0, s_cf_bright = 232, s_cf_st_dur = 15;
static char s_cf_st_src[16] = "auto", s_cf_st_dir[16] = "both";
/* appearance: new-UI only. theme= (litehtml: 0 = dark) is written back to the
 * theme actually shown, never read — an old theme=0 must not turn this UI
 * dark. The litehtml UI's own save drops these keys, which falls back to light. */
static ui_appear_t s_cf_appear = UI_APPEAR_LIGHT;
static char s_cf_dark_from[8] = "19:00", s_cf_dark_to[8] = "07:00";

static void load_devui_conf(void)
{
    FILE *fp = fopen(DEVUI_CONF_FILE, "r");
    if (!fp) return;
    char line[64], sval[16];
    int v;
    while (fgets(line, sizeof line, fp)) {
        if      (sscanf(line, "theme=%d", &v) == 1)       s_cf_theme = !!v;
        else if (sscanf(line, "speed_bits=%d", &v) == 1)  s_cf_speed_bits = !!v;
        else if (sscanf(line, "show_batpct=%d", &v) == 1) s_cf_show_batpct = !!v;
        else if (sscanf(line, "autooff=%d", &v) == 1)     s_cf_autooff_ms = v;
        else if (sscanf(line, "refresh_ms=%d", &v) == 1)  s_cf_refresh_ms = v;
        else if (sscanf(line, "sig_read=%d", &v) == 1)    s_cf_sig_read = !!v;
        else if (sscanf(line, "sig_parse=%d", &v) == 1)   s_cf_sig_parse = !!v;
        else if (sscanf(line, "bright=%d", &v) == 1)      s_cf_bright = v;
        else if (sscanf(line, "st_src=%15s", sval) == 1)  snprintf(s_cf_st_src, sizeof s_cf_st_src, "%s", sval);
        else if (sscanf(line, "st_dir=%15s", sval) == 1)  snprintf(s_cf_st_dir, sizeof s_cf_st_dir, "%s", sval);
        else if (sscanf(line, "st_dur=%d", &v) == 1)      s_cf_st_dur = v;
        else if (sscanf(line, "appearance=%15s", sval) == 1) {
            if (ui_appear_parse(sval, &s_cf_appear) != 0)
                fprintf(stderr, "ui: devui.conf appearance=%s not understood, using light\n", sval);
        }
        else if (sscanf(line, "appearance_dark_from=%7s", sval) == 1) snprintf(s_cf_dark_from, sizeof s_cf_dark_from, "%.7s", sval);
        else if (sscanf(line, "appearance_dark_to=%7s", sval) == 1)   snprintf(s_cf_dark_to, sizeof s_cf_dark_to, "%.7s", sval);
    }
    fclose(fp);
}

static void save_devui_conf(void)
{
    FILE *fp = fopen(DEVUI_CONF_FILE, "w");
    if (!fp) return;
    fprintf(fp,
            "theme=%d\nspeed_bits=%d\nshow_batpct=%d\nautooff=%d\nrefresh_ms=%d\nsig_read=%d\nsig_parse=%d\nbright=%d\nst_src=%s\nst_dir=%s\nst_dur=%d\n"
            "appearance=%s\nappearance_dark_from=%s\nappearance_dark_to=%s\n",
            s_cf_theme, s_cf_speed_bits, s_cf_show_batpct, s_cf_autooff_ms, s_cf_refresh_ms,
            s_cf_sig_read, s_cf_sig_parse, s_cf_bright, s_cf_st_src, s_cf_st_dir, s_cf_st_dur,
            ui_appear_name(s_cf_appear), s_cf_dark_from, s_cf_dark_to);
    fclose(fp);
}

static void topbar_speed_unit_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    s_cf_speed_bits = !s_cf_speed_bits;
    save_devui_conf();
}

/* ---- appearance (light / dark / auto) ----
 * One theme per process: switching writes devui.conf and execs a fresh copy
 * of this binary (ui_exec.c), which picks the theme before its first object.
 * Same pid and comm, so u60-uid sees no restart. */
static void appearance_ui_sync(void);   /* the 外观 buttons, defined with the 系统 page */

/* Device-local minute of day (the clock is local time labelled UTC, so
 * localtime gives the right digits), or -1 while the clock is unsynced. */
static int now_local_min(long *wall_out)
{
    time_t now = time(NULL);
    struct tm tm;
    if (wall_out) *wall_out = (long)now;
    if (!ui_clock_sane((long)now)) return -1;
    localtime_r(&now, &tm);
    return tm.tm_hour * 60 + tm.tm_min;
}

static int appear_resolve(ui_appear_t a)
{
    return ui_is_dark(a, now_local_min(NULL), ui_hhmm_parse(s_cf_dark_from), ui_hhmm_parse(s_cf_dark_to));
}

static int cur_tab(void)
{
    lv_obj_t *act = s_tv ? lv_tileview_get_tile_active(s_tv) : NULL;
    for (int i = 0; i < UI_TABS; i++)
        if (s_tiles[i] == act) return i;
    return 0;
}

/* The page's scroll body (uk_scroll), or NULL for a page that doesn't scroll. */
static lv_obj_t *tab_scroller(int tab)
{
    lv_obj_t *t = s_tiles[tab];
    for (uint32_t i = 0; t && i < lv_obj_get_child_count(t); i++) {
        lv_obj_t *c = lv_obj_get_child(t, (int32_t)i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_SCROLLABLE)) return c;
    }
    return NULL;
}

/* Returns only if the exec failed (this process carries on unchanged). */
static int theme_exec(int automatic)
{
    ui_launch_t l = s_launch;
    l.tab = cur_tab();
    lv_obj_t *sc = tab_scroller(l.tab);
    l.scroll_y = sc ? (int)lv_obj_get_scroll_y(sc) : 0;
    if (l.scroll_y < 0) l.scroll_y = 0;
    l.screen_off = backlight_panel_lit() ? 0 : s_auto_slept ? 1 : 2;
    l.autooff_ms = (int)s_autooff_ms;
    l.bright = backlight_get();
    if (automatic) l.hist = ui_exec_next(&s_launch.hist, ui_boot_s());
    data_set_pace(1);   /* the new process sets its own pace; don't leave datad slow if it never does */
    return ui_exec_self(s_argv0, &l);
}

/* 系统 → 外观. Manual switches are never rate-limited. */
static void appearance_set(ui_appear_t a)
{
    ui_appear_t old = s_cf_appear;
    int old_theme = s_cf_theme;
    if (a == old) return;
    int dark = appear_resolve(a);
    s_cf_appear = a;
    s_cf_theme = ui_legacy_theme_value(dark);
    save_devui_conf();
    if (dark != s_dark && theme_exec(0) != 0) {
        s_cf_appear = old;              /* stay as we are, and say so in the file too */
        s_cf_theme = old_theme;
        save_devui_conf();
    }
    appearance_ui_sync();
}

/* Something a switch would cut off mid-way: a running speed test, a delay
 * test, an eSIM switch waiting for its answer. */
static int ui_busy(void)
{
    if (speedtest_running() || chill_delay_pending()) return 1;
    for (int i = 0; i < esim_profile_count(); i++) {
        esim_profile_t p;
        esim_get_profile(i, &p);
        if (p.going) return 1;
    }
    return 0;
}

static void alert_theme_paused(void)
{
    if (access("/data/u60-guard/alert-lib.sh", R_OK) != 0) return;
    if (system(". /data/u60-guard/alert-lib.sh && "
               "alert_add devui-theme-paused 'automatic light/dark switching paused until reboot (3 switches within an hour)' "
               ">/dev/null 2>&1 &") != 0)
        fprintf(stderr, "ui: could not record the devui-theme-paused alert\n");
}

/* appearance=auto: called every second from refresh_cb, which keeps running
 * while the screen is dark (ui_idle pauses only the fast timers). */
static void appearance_tick(void)
{
    if (s_cf_appear != UI_APPEAR_AUTO || s_auto_suspended) return;
    long wall;
    int min = now_local_min(&wall);
    ui_auto_in_t in = {
        .appear = s_cf_appear, .cur_dark = s_dark, .wall_s = wall, .now_min = min,
        .from_min = ui_hhmm_parse(s_cf_dark_from), .to_min = ui_hhmm_parse(s_cf_dark_to),
        .screen_off = !backlight_panel_lit(), .always_on = s_autooff_ms == 0,
        .idle_ms = (long)lv_display_get_inactive_time(NULL), .busy = ui_busy(),
        .suspended = s_auto_suspended, .hist = s_launch.hist, .now_boot_s = ui_boot_s(),
    };
    switch (ui_auto_decide(&in)) {
    case UI_AUTO_EXEC:
        s_cf_theme = ui_legacy_theme_value(!s_dark);
        save_devui_conf();
        if (theme_exec(1) != 0) {
            /* e.g. the binary was replaced on disk: retrying every second
             * would only fill the log. Next boot (or a manual switch) again. */
            s_cf_theme = ui_legacy_theme_value(s_dark);
            save_devui_conf();
            s_auto_suspended = 1;
            fprintf(stderr, "ui: automatic appearance switch off until restart (exec failed)\n");
        }
        break;
    case UI_AUTO_SUSPEND:
        s_auto_suspended = 1;
        fprintf(stderr, "ui: automatic appearance switch paused until reboot: %d switches within %d s\n",
                s_launch.hist.count, UI_EXEC_WINDOW_S);
        alert_theme_paused();
        break;
    default:
        break;
    }
}
/* Throughput on a 0..100 log scale for the chart axis: this link idles at a
 * few hundred B/s and peaks in the tens of MB/s, so a linear axis would draw
 * everything except the peak flat on the baseline. 0 B/s -> 0, 1KB/s -> 25,
 * 1MB/s -> 60, 100MB/s -> 100. */
static int32_t rate_scale(long bps)
{
    double v;
    if (bps <= 0) return 0;
    v = log10((double)bps) * 100.0 / 8.0;   /* 10^8 B/s == full scale */
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return (int32_t)v;
}

/* net.nrca / net.lteca format, per source/data-service/docs/STATE_SCHEMA.md:
 * ';'-separated carriers, each 11 ','-separated fields:
 * idx,PCI,?,band,arfcn,bw,?,rsrp,rsrq,sinr,rssi. Empty string when this
 * device isn't in carrier aggregation (the common case) — not a fixed-size
 * "always N carriers" feed, so the caller has to handle 0 results too. */
typedef struct { int pci, band, bw, active; long arfcn; double rsrp, rsrq, sinr; } ca_carrier_t;

static int parse_ca(const char *s, ca_carrier_t *out, int max)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", s);
    int n = 0;
    char *save = NULL;
    for (char *rec = strtok_r(buf, ";", &save); rec && n < max; rec = strtok_r(NULL, ";", &save)) {
        double idx, pci, unk1, band, arfcn, bw, unk2, rsrp, rsrq, sinr, rssi;
        if (sscanf(rec, "%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf",
                   &idx, &pci, &unk1, &band, &arfcn, &bw, &unk2,
                   &rsrp, &rsrq, &sinr, &rssi) != 11)
            continue;
        (void)idx; (void)unk1; (void)unk2; (void)rssi;
        out[n].pci = (int)pci;
        out[n].band = (int)band;
        out[n].arfcn = (long)arfcn;
        out[n].bw = (int)bw;
        out[n].rsrp = rsrp;
        out[n].rsrq = rsrq;
        out[n].sinr = sinr;
        /* SIGNAL-CARDS.md: RSRP <= -140 floor sentinel = configured but not
         * actively scheduled, not "very bad signal". */
        out[n].active = rsrp > -140.0;
        n++;
    }
    return n;
}

/* net.{lte_supported_bands,nr_sa_supported_bands,nr_nsa_supported_bands}
 * (aliased in this project's backend JSON as sa_bands/nsa_bands/lte_bands —
 * confirmed against a live /state response, not just the field names) are a
 * plain comma-separated capability list — what the modem *can* use, not
 * what's active right now (that's nrca/lteca, parsed separately above).
 * Reformat "1,2,3,5" into "n1 n2 n3 n5" (or "B1 B2..." for LTE) so it reads
 * as band numbers, not an opaque CSV blob — this is the actual content the
 * user asked to see ("哪些频段可用"), not decoration. */
static void fmt_band_list(char *out, size_t out_sz, const char *csv, char prefix)
{
    size_t used = 0;
    out[0] = '\0';
    const char *p = csv;
    while (*p && used + 8 < out_sz) {
        long n = strtol(p, (char **)&p, 10);
        int w = snprintf(out + used, out_sz - used, used ? " %c%ld" : "%c%ld", prefix, n);
        if (w < 0) break;
        used += (size_t)w;
        while (*p == ',') p++;
    }
}

/* lv_label_set_text() (and therefore _fmt, which funnels into it) *always*
 * frees the label's current text buffer and mallocs a new one — even when
 * the new string is byte-identical to what's already shown (confirmed by
 * reading lv_label.c's set_text_internal(), not assumed). refresh_cb updates
 * every label on every page once a second regardless of which page is
 * visible, and most fields (operator name, QCI, band, PCI, ARFCN...) barely
 * ever change — that's thousands of needless free+malloc cycles per minute
 * churning LVGL's small internal heap (LV_MEM_SIZE). This is the actual
 * mechanism behind the recurring heap-exhaustion crashes/lag (see
 * [[lvgl-heap-instability]]), not just a contributing factor. Skip the call
 * into LVGL entirely when nothing changed. Every refresh_cb label update
 * should go through this, not lv_label_set_text_fmt directly. */
/* Copy as much of `src` as fits in `cap` bytes without splitting a UTF-8
 * character (a split one renders as a broken glyph). */
static void utf8_prefix(char *out, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;   /* back off to a lead byte */
    }
    memcpy(out, src, n);
    out[n] = 0;
}

static void set_label_fmt(lv_obj_t *label, char *cache, size_t cache_sz, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (strcmp(cache, buf) != 0) {
        /* %.*s (not %s) explicitly bounds the copy for GCC's static
         * truncation analysis — cache_sz varies per call site and buf's
         * worst case (160) exceeds some of the smaller ones, matching this
         * project's existing convention for silencing that class of
         * warning by actually bounding it, not suppressing it. */
        snprintf(cache, cache_sz, "%.*s", (int)cache_sz - 1, buf);
        lv_label_set_text(label, buf);
    }
}

/* Page titles were removed 2026-09-21: the bottom tab bar already labels
 * every top-level page in Chinese, so a second title inside the page was
 * pure duplication costing 28px of the 420px content area. Subpages keep a
 * title (they have no tab of their own) — it is created inline in
 * ui_create(). */

/* DESIGN.md §4: a few dense cards, not one card per metric. Flat solid
 * panel, rounded corners, no border/shadow/glow/gradient — matches the
 * litehtml reference (`ui/01-signal.html`) that's actually live on the
 * device today. Group related fields into one card (cellular status, or
 * battery+traffic+system) instead of fragmenting into single-metric cards. */

/* Nested sub-card: a thin bordered box inside a regular card, for a list of
 * same-shape multi-field items (e.g. one component carrier's full detail)
 * where each item is a meaningful group, not a single metric — matches the
 * litehtml reference's per-carrier boxes exactly. Border only, no separate
 * fill color from its parent: differentiation comes from the outline, same
 * as the reference. */

/* mk_section_title()/mk_divider() lived here until eSIM, WiFi and Settings
 * were moved onto cards (2026-09-21) — with every page on the card system
 * there is no caller left for a bare section title or a hairline divider,
 * and DESIGN.md §4 doesn't want them back. Removed rather than kept as
 * dead code someone re-reaches for. */

/* Invisible grouping container — for the one case (Tailscale) that needs to
 * hide/show a whole section as a unit depending on runtime availability. No
 * bg, no border: it carries no visual weight of its own, only grouping. */

/* Scrollable page body for pages taller than one screen.
 *
 * The container must be exactly the *visible* area and its children must
 * extend past it — that overflow is the only thing LVGL scrolls. Home and
 * Net originally sized this container to their content height (660 / 480)
 * with every child inside it, so `lv_obj_get_scroll_bottom()` was 0: nothing
 * overflowed, the tile just clipped the bottom and no drag ever moved
 * anything. That, not a flaky gesture, is why Net's 支持频段 line could
 * never be scrolled into view during verification.
 *
 * `content_h` is pinned by a 1px invisible spacer rather than inferred from
 * the last card, so a runtime reflow (carrier count, client count) can move
 * cards around without changing the scroll range underneath the finger. */


/* ---- subpage layer ----
 * A full-content-area container stacked over the tileview on the active
 * screen. It is NOT on lv_layer_top(): the status bar and the tab bar live
 * there and must keep painting above an open subpage, exactly like the
 * litehtml UI's subpages keep its status bar. Every subpage is built once at
 * startup and hidden; opening one is a visibility flip, never a build. */
static void sub_open(int id);
static void sub_open_child(int id, int parent);
static void sub_close(void);
static void sub_back(void);
static void tile_click_cb(lv_event_t *e);   /* also used by the Home Tailscale card */
static void chill_nav_cb(lv_event_t *e);    /* CHILL page's 策略组/节点/规则→节点 rows */
static void open_alerts_cb(lv_event_t *e);  /* status-bar alert dot, 系统 page's 健康 row */
static void sc_card_cb(lv_event_t *e);      /* Home 情景 card: abroad, opens the CHILL exit menu */
static void bench_gate(void);
static void update_tabs(void);

/* The perf page's animation driver only runs while that page is open. */
static void bench_gate(void)
{
    if (!s_bench_timer) return;
    if (s_sub_cur == SUB_PERF) lv_timer_resume(s_bench_timer);
    else                       lv_timer_pause(s_bench_timer);
}

static void sub_open(int id)
{
    static const char *const k_sub_title[SUB_N] = {
        "WiFi", "\xE7\x9F\xAD\xE4\xBF\xA1" /* 短信 */,
        "\xE4\xBF\xA1\xE4\xBB\xA4\xE8\xAF\xBB\xE5\x8F\x96" /* 信令读取 */,
        "\xE9\x94\x81\xE9\xA2\x91" /* 锁频 */,
        "\xE6\xB5\x8B\xE9\x80\x9F" /* 测速 */,
        "CHILL", "eSIM",
        "\xE6\x80\xA7\xE8\x83\xBD\xE6\xB5\x8B\xE8\xAF\x95" /* 性能测试 */,
        "Tailscale",
        "\xE8\x8A\x82\xE7\x82\xB9" /* 节点 */,
        "\xE8\xA7\x84\xE5\x88\x99 \xE2\x86\x92 \xE8\x8A\x82\xE7\x82\xB9" /* 规则 → 节点 */,
        "短信详情",
        "告警",
    };
    if (id < 0 || id >= SUB_N) return;
    for (int i = 0; i < SUB_N; i++)
        if (s_sub_page[i]) {
            if (i == id) lv_obj_remove_flag(s_sub_page[i], LV_OBJ_FLAG_HIDDEN);
            else         lv_obj_add_flag(s_sub_page[i], LV_OBJ_FLAG_HIDDEN);
        }
    lv_label_set_text(s_sub_title, k_sub_title[id]);
    lv_obj_remove_flag(s_sub_layer, LV_OBJ_FLAG_HIDDEN);
    uk_anim_push(s_sub_layer);
    s_sub_cur = id;
    s_sub_parent = -1;
    bench_gate();
    update_tabs();
}

static void sub_close(void)
{
    lv_obj_add_flag(s_sub_layer, LV_OBJ_FLAG_HIDDEN);
    s_sub_cur = -1;
    s_sub_parent = -1;
    bench_gate();
    update_tabs();
}

/* Open a page one level below a top-level subpage (currently only the CHILL
 * nav rows). 返回 from here goes back to `parent`, not all the way out —
 * see sub_back(). */
static void sub_open_child(int id, int parent)
{
    sub_open(id);
    s_sub_parent = parent;
}

/* The tab bar's 返回 slot: one step back, not always a full exit — a child
 * page (opened via sub_open_child()) returns to its parent; anything else
 * closes the subpage layer entirely, same as before. */
static void sub_back(void)
{
    if (s_sub_parent >= 0) sub_open(s_sub_parent);
    else                   sub_close();
}
static void sub_back_cb(lv_event_t *e) { LV_UNUSED(e); sub_back(); }

/* Visibility predicates for the pollers (Tailscale/eSIM/CHILL only talk to
 * their backend while their own page is on a lit screen). */
static int tab_visible(int tab)
{
    return backlight_is_on() && s_sub_cur < 0 &&
           lv_tileview_get_tile_active(s_tv) == s_tiles[tab];
}

static int sub_visible(int id)
{
    return backlight_is_on() && s_sub_cur == id;
}

/* ---- page builders ----
 * Every page follows the same three zones: a top status zone, a content zone
 * built only out of cards, and the shared bottom nav (owned by ui_create, not
 * by any page). */
/* Dense stat cell: small caption line + a bigger value line under it. This
 * is the density primitive for Home — a 2-column grid of these replaces one
 * card per metric, cutting per-metric chrome (own card + own title row) to
 * zero while keeping every field. */

/* ---- Home: 信号 / 邻区 / Tailscale / CHILL ----
 * Mirrors ui/01-signal.html section for section. The headline is the
 * aggregate line ("5G SA · 3 NR 载波 · 240 MHz"): mode, carrier count and
 * TOTAL aggregated bandwidth. An earlier pass showed only the primary
 * carrier ("SA · n78 · 100") and the user immediately flagged the page as
 * having no focal point — the number people actually look for is how much
 * spectrum is currently aggregated, not which band the PCC happens to be. */
static void home_reflow(void);

static void build_home(lv_obj_t *t)
{
    /* Worst case (5 active carriers, 5 CHILL pairs, 5 Tailscale rows) is
     * ~1010; the spacer is moved by home_reflow to the real height. */
    t = s_home_scroll = uk_scroll(t, 0, UI_VIEW_H, 1100);

    /* signal card */
    s_cell_card = uk_card(t, UK_MARGIN, 4, UK_CARD_W, UK_HERO_H + 40);
    lv_obj_t *c = s_cell_card;
    uk_hero(&s_cc_hero, c, UF.n32);
    lv_label_set_text(s_cc_hero.unit, "MHz");
    s_cc_hint = uk_label_w(c, UF.cj13, T->t2, UK_PAD, UK_HERO_H + 10, UK_CARD_W - 2 * UK_PAD, 1, "");
    for (int i = 0; i < CA_SLOTS; i++) {
        home_ca_t *k = &s_ca[i];
        k->box = uk_box(c, 0, UK_HERO_H + i * 40, UK_CARD_W, 40, T->card, 0);
        lv_obj_set_style_bg_opa(k->box, LV_OPA_TRANSP, 0);
        k->sep = uk_sep(k->box, 0);
        k->band   = uk_label(k->box, UF.n17, T->t1, UK_PAD, 10, "");
        k->bw     = uk_label(k->box, UF.n12, T->t3, 60, 15, "");
        k->rsrp   = uk_label_r(k->box, UF.n15, T->t1, 144, 4, "");
        k->rsrp_c = uk_label_r(k->box, UF.n11, T->t3, 144, 23, "RSRP");
        k->sinr   = uk_label_r(k->box, UF.n17, T->okT, 190, 3, "");
        k->sinr_c = uk_label_r(k->box, UF.n11, T->t3, 190, 23, "SINR");
        k->pci    = uk_label_r(k->box, UF.n11, T->t3, UK_CARD_W - UK_PAD, 5, "");
        k->arfcn  = uk_label_r(k->box, UF.n11, T->t3, UK_CARD_W - UK_PAD, 21, "");
        k->ina      = uk_label(k->box, UF.n15, T->t3, UK_PAD, 7, "");
        k->ina_tag  = uk_label(k->box, UF.cj12, T->t3, 100, 9, "未激活");
        k->ina_info = uk_label_r(k->box, UF.n11, T->t3, UK_CARD_W - UK_PAD, 9, "");
        uk_show(k->box, 0);
    }
    s_cc_tsep = uk_box(c, 0, UK_HERO_H, UK_CARD_W, 1, T->sep, 0);
    s_cc_tkey = uk_label(c, UF.cj12, T->t3, UK_PAD, UK_HERO_H + 10, "流量");
    s_cc_traffic = uk_label_r(c, UF.n12, T->t2, UK_CARD_W - UK_PAD, UK_HERO_H + 9, "");

    /* 情景: answers "why is Wi-Fi off?". Abroad, a tap opens the CHILL exit
     * sheet (sc_card_cb); otherwise its settings live in the admin web. */
    s_sc_card = uk_card(t, UK_MARGIN, 200, UK_CARD_W, SC_CARD_H);
    uk_label(s_sc_card, UF.cj12, T->t3, UK_PAD, 10, "情景");
    s_sc_state = uk_label_w(s_sc_card, UF.cj17b, T->t1, UK_PAD, 27, UK_CARD_W - 2 * UK_PAD - 16, 0, "");
    s_sc_note = uk_label_w(s_sc_card, UF.cj12, T->t2, UK_PAD, 50, UK_CARD_W - 2 * UK_PAD - 16, 0, "");
    uk_chevron(s_sc_card, 16);
    uk_tappable(s_sc_card, sc_card_cb, NULL);
    uk_show(s_sc_card, 0);

    /* CHILL: status, live rate, exit · node · delay, connections, traffic,
     * and the real top rule → node pairs (one list keyed by the pair, so
     * each row answers "this rule's traffic went through this node"). */
    s_chill_card = uk_card(t, UK_MARGIN, 300, UK_CARD_W, CHILL_HOME_TOP + CHILL_HOME_ROW_H);
    lv_obj_t *h = s_chill_card;
    uk_label(h, UF.cj12, T->t3, UK_PAD, 10, "CHILL");
    s_chill_state = uk_label_r(h, UF.cj12, T->okT, UK_CARD_W - UK_PAD - 16, 10, "");
    uk_label_r(h, UF.cj15, T->t3, UK_CARD_W - UK_PAD, 6, "›");
    s_chill_rate  = uk_label(h, UF.n17, T->t1, UK_PAD, 27, "");
    s_chill_split = uk_label_r(h, UF.cj12, T->t2, UK_CARD_W - UK_PAD, 31, "");
    s_chill_line  = uk_label_w(h, UF.cj12, T->t2, UK_PAD, 52, 170, 0, "");
    s_chill_total = uk_label_r(h, UF.n12, T->t3, UK_CARD_W - UK_PAD, 51, "");
    uk_box(h, 0, 74, UK_CARD_W, 1, T->sep, 0);
    uk_label(h, UF.cj12, T->t3, UK_PAD, 81, "规则 → 节点");
    for (int i = 0; i < CHILL_HOME_ROWS; i++) {
        int y = CHILL_HOME_TOP + i * CHILL_HOME_ROW_H;
        s_chill_pair_sep[i] = i ? uk_sep(h, y) : NULL;
        s_chill_pair_name[i] = uk_label_w(h, UF.cj13, T->t1, UK_PAD, y + 10, 180, 0, "");
        s_chill_pair_val[i] = uk_label_r(h, UF.n12, T->t2, UK_CARD_W - UK_PAD, y + 11, "");
    }
    uk_tappable(h, tile_click_cb, (void *)(intptr_t)SUB_CHILL);
    uk_show(h, 0);

    /* Tailscale: hidden entirely when the device has no tailscaled. The card
     * is the entry to the peer list (no 功能 tile for it). */
    s_ts_card = uk_card(t, UK_MARGIN, 500, UK_CARD_W, UK_ROW_H);
    static const char *const ts_keys[TS_HOME_ROWS] = { "Tailscale", "本机", "节点", "子网", "出口" };
    for (int i = 0; i < TS_HOME_ROWS; i++) {
        if (i) s_ts_sep[i] = uk_sep(s_ts_card, i * UK_ROW_H);
        s_ts_key[i] = uk_label(s_ts_card, UF.cj14, i ? T->t2 : T->t1, UK_PAD, i * UK_ROW_H + 11, ts_keys[i]);
        s_ts_val[i] = uk_label_r(s_ts_card, i ? UF.n15 : UF.cj14, T->t1, UK_CARD_W - UK_PAD, i * UK_ROW_H + (i ? 10 : 11), "");
    }
    s_ts_dot = uk_dot(s_ts_card, 0, 17, 7, T->green);
    uk_tappable(s_ts_card, tile_click_cb, (void *)(intptr_t)SUB_TS);
    uk_show(s_ts_card, 0);
    home_reflow();
}

static int home_visible_h(lv_obj_t *o) { return lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) ? -1 : (int)lv_obj_get_style_height(o, 0); }

/* Stack the home cards under whatever the signal card is showing. Property
 * changes on existing objects only; nothing is allocated. */
static void home_reflow(void)
{
    int y = 4;
    lv_obj_t *order[4] = { s_cell_card, s_sc_card, s_chill_card, s_ts_card };
    for (int i = 0; i < 4; i++) {
        int hgt = home_visible_h(order[i]);
        if (hgt < 0) continue;
        lv_obj_set_y(order[i], y);
        y += hgt + UK_MARGIN;
    }
    uk_scroll_extent(s_home_scroll, y + UK_TAB_PAD);
}

/* No snapshot from the data service. Before the first one: 「正在读取…」.
 * After: the numbers stay where they were, dimmed, and the block says when
 * they stopped — a data-service outage is not "no signal". */
static int  s_ever_valid;
static long s_last_valid_wall;
static int  s_cc_tone = -1, s_cc_words = -1;
static void home_signal_down(void)
{
    static char c_st[64];
    uk_hero_tone(&s_cc_hero, 3);
    s_cc_tone = 3;
    if (!s_ever_valid) {
        set_label_fmt(s_cc_hero.st, c_st, sizeof c_st, "%s", "正在读取…");
        lv_label_set_text(s_cc_hero.big, "--");
        uk_show(s_cc_hero.unit, 0);
        s_cc_words = -1;
        lv_label_set_text(s_cc_hero.rtop, "");
        lv_label_set_text(s_cc_hero.r1, "");
        lv_label_set_text(s_cc_hero.r2, "");
        uk_hero_layout(&s_cc_hero);
    } else {
        char hm[8] = "--:--";
        time_t tt = (time_t)s_last_valid_wall;
        struct tm tm;
        if (localtime_r(&tt, &tm)) strftime(hm, sizeof hm, "%H:%M", &tm);
        set_label_fmt(s_cc_hero.st, c_st, sizeof c_st, "数据服务掉线 · 数字停在 %s", hm);
        lv_label_set_text(s_cc_hero.rtop, "");
    }
    for (int i = 0; i < CA_SLOTS; i++) {
        uk_text_color(s_ca[i].band, T->t3);
        uk_text_color(s_ca[i].rsrp, T->t3);
        uk_text_color(s_ca[i].sinr, T->t3);
    }
    uk_text_color(s_cc_traffic, T->t3);
    home_reflow();
}

/* ---- auxiliary device state ----
 * Things zwrt-datad's /state doesn't carry: interface up/down, WiFi power
 * save, direct-power-supply, DHCP pool text. Same one-shot shell round-trip
 * htmlmain.c's wifi_aux_refresh() uses, on the same kind of throttle, and
 * only while a page that displays it is actually visible. */
static int  s_aux_w24 = -1, s_aux_w5 = -1, s_aux_psm = -1, s_aux_dps = -1;
static char s_aux_pool[48];

static void aux_refresh(int active)
{
    static uint32_t last;
    uint32_t now = lv_tick_get();
    char line[256];
    FILE *fp;

    if (!active) return;
    if (last && now - last < 5000) return;
    last = now;

    fp = popen(
        "echo W0=$(cat /sys/class/net/wlan0/operstate 2>/dev/null);"
        "echo W2=$(cat /sys/class/net/wlan2/operstate 2>/dev/null);"
        "ps=$(iw dev wlan0 get power_save 2>/dev/null | grep -o 'o[nf]*' | tail -1);"
        "[ -z \"$ps\" ] && ps=$(iw dev wlan2 get power_save 2>/dev/null | grep -o 'o[nf]*' | tail -1);"
        "echo PSM=$([ \"$ps\" = on ] && echo 1 || echo 0);"
        "ip=$(uci -q get network.lan.ipaddr); st=$(uci -q get dhcp.lan.start); lim=$(uci -q get dhcp.lan.limit);"
        "if [ -n \"$ip\" ] && [ -n \"$st\" ]; then pre=${ip%.*}; end=$((st+lim-1)); [ $end -gt 254 ] && end=254;"
        "echo \"POOL=$pre.$st - $pre.$end\"; fi;"
        "echo DPS=$(ubus call zwrt_bsp.charger list 2>/dev/null | grep direct_power_supply_mode | grep -o 'enable\\|disable')",
        "r");
    if (!fp) return;
    while (fgets(line, sizeof line, fp)) {
        if      (!strncmp(line, "W0=", 3))   s_aux_w24 = strstr(line, "=up") != NULL;
        else if (!strncmp(line, "W2=", 3))   s_aux_w5  = strstr(line, "=up") != NULL;
        else if (!strncmp(line, "PSM=", 4))  s_aux_psm = atoi(line + 4);
        else if (!strncmp(line, "POOL=", 5)) {
            char *nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
            snprintf(s_aux_pool, sizeof s_aux_pool, "%.*s", (int)sizeof s_aux_pool - 1, line + 5);
        } else if (!strncmp(line, "DPS=", 4)) {
            if      (strstr(line, "disable")) s_aux_dps = 0;
            else if (strstr(line, "enable"))  s_aux_dps = 1;
        }
    }
    pclose(fp);
}

/* A switch row: name on the left, state text right-aligned next to the
 * switch, lv_switch on the right — all on one line. The first version
 * stacked the state under the name, which at 34px row pitch read as if the
 * state belonged to the row below. Returns the switch so the caller can
 * bind a callback; `state_out` receives the state label. */

static void sw_apply(lv_obj_t *sw, int on)
{
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else    lv_obj_remove_state(sw, LV_STATE_CHECKED);
}

/* ---- WiFi subpage ----
 * Content mirrors ui/subpages/wifi.html: credentials, the radio switches,
 * the client list, DHCP. The QR code that used to be here is gone — the
 * backend never exposes the passphrase, so it could only ever encode a
 * "join this open network" code that fails to authenticate, and nobody
 * asked for it. */
enum { WSW_MASTER, WSW_24, WSW_5, WSW_PSM, WSW_NFC };

static void wifi_sw_cb(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    int on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    const char *ic = on ? "up" : "down";
    char cmd[720];

    switch (id) {
    case WSW_MASTER:
        snprintf(cmd, sizeof cmd, "(ifconfig wlan0 %s; ifconfig wlan2 %s) >/dev/null 2>&1 &", ic, ic);
        s_aux_w24 = s_aux_w5 = on;
        break;
    case WSW_24:
        snprintf(cmd, sizeof cmd, "ifconfig wlan0 %s >/dev/null 2>&1 &", ic);
        s_aux_w24 = on;
        break;
    case WSW_5:
        snprintf(cmd, sizeof cmd, "ifconfig wlan2 %s >/dev/null 2>&1 &", ic);
        s_aux_w5 = on;
        break;
    case WSW_PSM: {
        /* Verbatim from htmlmain.c's act:psm — the switch owns
         * /etc/hotplug.d/iface/99-disable-powersave so the choice survives
         * an ifup, and applies it to the live interfaces now. */
        const char *m = on ? "on" : "off";
        snprintf(cmd, sizeof cmd,
            "(mkdir -p /etc/hotplug.d/iface; rm -f /etc/hotplug.d/iface/psm; "
            "{ echo '#!/bin/sh'; echo '[ \"$ACTION\" = ifup ] && {'; "
            "echo '  iw dev wlan0 set power_save %s 2>/dev/null'; "
            "echo '  iw dev wlan1 set power_save %s 2>/dev/null'; "
            "echo '  iw dev wlan2 set power_save %s 2>/dev/null'; "
            "echo '  iw dev wlan3 set power_save %s 2>/dev/null'; echo '}'; } "
            "> /etc/hotplug.d/iface/99-disable-powersave; "
            "chmod +x /etc/hotplug.d/iface/99-disable-powersave; "
            "for w in wlan0 wlan1 wlan2 wlan3; do iw dev $w set power_save %s 2>/dev/null; done) "
            ">/dev/null 2>&1 &", m, m, m, m, m);
        s_aux_psm = on;
        break;
    }
    case WSW_NFC:
        snprintf(cmd, sizeof cmd,
            "ubus call zwrt_nfc zwrt_nfc_wifi_set '{\"switch\":%d,\"flag\":2}' >/dev/null 2>&1 &", on);
        break;
    default:
        return;
    }
    system(cmd);
}

/* A card row with a label, a state word and a switch (WiFi, CHILL). */
static lv_obj_t *toggle_row(lv_obj_t *c, int y, const char *name, int first, lv_obj_t **state,
                            lv_event_cb_t cb, void *user)
{
    if (!first) uk_sep(c, y);
    uk_label(c, UF.cj14, T->t1, UK_PAD, y + 11, name);
    if (state) *state = uk_label_r(c, UF.cj12, T->t3, UK_CARD_W - UK_PAD - 52, y + 13, "");
    return uk_toggle(c, UK_CARD_W - UK_PAD, y + 7, cb, user);
}

#define WIFI_CLI_H 50
static void build_sub_wifi(lv_obj_t *t)
{
    int y = 4;
    t = s_w_scroll = uk_scroll(t, 0, UI_SUB_VIEW, 1000);

    uk_section(t, y, "热点"); y += 20;
    lv_obj_t *ap = uk_card(t, UK_MARGIN, y, UK_CARD_W, 3 * UK_ROW_H);
    s_w_ssid = uk_label_w(ap, UF.cj17b, T->t1, UK_PAD, 10, 200, 0, "");
    s_w_state = uk_label_r(ap, UF.cj13, T->okT, UK_CARD_W - UK_PAD, 12, "");
    s_w_enc = uk_row(ap, UK_ROW_H, "加密", 0);
    s_w_pass = uk_row(ap, 2 * UK_ROW_H, "密码", 0);
    y += 3 * UK_ROW_H + 10;

    uk_section(t, y, "开关"); y += 20;
    lv_obj_t *sws = uk_card(t, UK_MARGIN, y, UK_CARD_W, 5 * UK_ROW_H);
    static const char *const k_sw_name[5] = { "WiFi 总开关", "2.4G 频段", "5G 频段", "节能模式", "NFC 碰一碰" };
    for (int i = 0; i < 5; i++)
        s_w_sw[i] = toggle_row(sws, i * UK_ROW_H, k_sw_name[i], i == 0, &s_w_sw_st[i], wifi_sw_cb, (void *)(intptr_t)i);
    y += 5 * UK_ROW_H + 10;

    s_w_cli_sec = uk_section(t, y, "已连接设备");
    s_w_cli_n = uk_label_r(t, UF.cj12, T->t3, UK_W - UK_MARGIN - 6, y, "");
    y += 20;
    s_w_cli_card = uk_card(t, UK_MARGIN, y, UK_CARD_W, WIFI_MAX_CLI * WIFI_CLI_H);
    s_w_cli_empty = uk_label(s_w_cli_card, UF.cj14, T->t3, UK_PAD, 11, "还没有设备连上");
    for (int i = 0; i < WIFI_MAX_CLI; i++) {
        int ry = i * WIFI_CLI_H;
        s_w_cli[i] = uk_box(s_w_cli_card, 0, ry, UK_CARD_W, WIFI_CLI_H, T->card, 0);
        lv_obj_set_style_bg_opa(s_w_cli[i], LV_OPA_TRANSP, 0);
        s_w_cli_sep[i] = i ? uk_sep(s_w_cli[i], 0) : NULL;
        s_w_cli_name[i] = uk_label_w(s_w_cli[i], UF.cj14, T->t1, UK_PAD, 8, 160, 0, "");
        s_w_cli_mac[i] = uk_label(s_w_cli[i], UF.n11, T->t3, UK_PAD, 29, "");
        s_w_cli_ip[i] = uk_label_r(s_w_cli[i], UF.n15, T->t1, UK_CARD_W - UK_PAD, 15, "");
        uk_show(s_w_cli[i], 0);
    }
    y += WIFI_MAX_CLI * WIFI_CLI_H + 10;

    s_w_dhcp_sec = uk_section(t, y, "DHCP"); y += 20;
    s_w_dhcp_card = uk_card(t, UK_MARGIN, y, UK_CARD_W, 3 * UK_ROW_H);
    s_w_gw = uk_row(s_w_dhcp_card, 0, "网关", 1);
    s_w_pool = uk_row(s_w_dhcp_card, UK_ROW_H, "地址池", 0);
    s_w_lease = uk_row(s_w_dhcp_card, 2 * UK_ROW_H, "租期", 0);
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* ---- eSIM subpage ---- */
static void esim_row_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    esim_select(idx);
}

#define ESIM_ROW_H 50
static void build_sub_esim(lv_obj_t *t)
{
    t = uk_scroll(t, 0, UI_SUB_VIEW, 4 + 20 + UK_HERO_H + 10 + 20 + ESIM_MAX_ROWS * ESIM_ROW_H + 16);
    uk_section(t, 4, "当前配置");
    lv_obj_t *cur = uk_card(t, UK_MARGIN, 24, UK_CARD_W, UK_HERO_H);
    uk_hero(&s_es_hero, cur, UF.cj22b);
    uk_hero_tone(&s_es_hero, 4);
    s_es_cur = s_es_hero.big;
    s_es_state = s_es_hero.r2;
    lv_label_set_text(s_es_hero.st, "使用中");

    int y = 24 + UK_HERO_H + 10;
    uk_section(t, y, "配置列表");
    s_es_list_card = uk_card(t, UK_MARGIN, y + 20, UK_CARD_W, ESIM_MAX_ROWS * ESIM_ROW_H);
    lv_obj_set_style_clip_corner(s_es_list_card, true, 0);
    s_es_empty = uk_label_w(s_es_list_card, UF.cj14, T->t3, UK_PAD, 11, UK_CARD_W - 2 * UK_PAD, 0, "读取中…");
    for (int i = 0; i < ESIM_MAX_ROWS; i++) {
        lv_obj_t *row = uk_box(s_es_list_card, 0, i * ESIM_ROW_H, UK_CARD_W, ESIM_ROW_H, T->card, 0);
        uk_tappable(row, esim_row_cb, (void *)(intptr_t)i);
        s_es_row[i] = row;
        s_es_row_sep[i] = i ? uk_sep(row, 0) : NULL;
        s_es_row_name[i] = uk_label_w(row, UF.cj14, T->t1, UK_PAD, 8, 190, 0, "");
        s_es_row_sub[i]  = uk_label_w(row, UF.n11, T->t3, UK_PAD, 29, 200, 0, "");
        s_es_row_tag[i]  = uk_label_r(row, UF.cj13, T->accT, UK_CARD_W - UK_PAD, 16, "");
        uk_show(row, 0);
    }
}

/* ---- System page ---- */
static const uint32_t k_off_ms[3] = { 0, 30000, 120000 };  /* Never / 30s / 2m */

static void bright_label(void)
{
    char b[8];
    int max = backlight_max() > 0 ? backlight_max() : 255;
    snprintf(b, sizeof b, "%d%%", lv_slider_get_value(s_set_bright) * 100 / max);
    lv_label_set_text(s_set_bright_v, b);
}

static void bright_cb(lv_event_t *e)
{
    backlight_set(lv_slider_get_value((lv_obj_t *)lv_event_get_target(e)));
    bright_label();
}

/* Saved on release, not per step: the slider fires dozens of changes per drag. */
static void bright_save_cb(lv_event_t *e)
{
    s_cf_bright = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    save_devui_conf();
}

static void highlight_off_btns(int sel) { uk_seg_set(&s_off_seg, sel); }

static void offsel_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_autooff_ms = k_off_ms[idx];
    s_auto_slept = 0;
    highlight_off_btns(idx);
}

static void dps_cb(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    int on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    char cmd[200];
    snprintf(cmd, sizeof cmd,
        "ubus call zwrt_bsp.charger set '{\"direct_power_supply_mode\":\"%s\"}' >/dev/null 2>&1 &",
        on ? "enable" : "disable");
    system(cmd);
    s_aux_dps = on;
}

/* Same setting the status-bar tap (topbar_speed_unit_cb) flips — this is
 * just a discoverable, labelled home for it (2026-09-22: tap-to-toggle on
 * the topbar has no visible affordance beyond the accent color, easy to
 * never find). Both write the same s_cf_speed_bits + devui.conf, so
 * whichever one the user touches, the other stays in sync via refresh_cb's
 * sw_apply() below. */
static void speedunit_cb(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    s_cf_speed_bits = lv_obj_has_state(sw, LV_STATE_CHECKED);
    save_devui_conf();
}

/* Ask u60-uid (the screen-owner daemon) to switch. It stops us, waits for
 * /dev/dri/card0 to be free and starts the vendor UI — and because it asked
 * us to stop, it does not count our exit as a crash. Opening a FIFO for write
 * with O_NONBLOCK fails (ENXIO) when nobody is reading it, which is exactly
 * "u60-uid is not running". */
static int request_vendor_via_uid(void)
{
    int fd = open("/tmp/u60-uid.ctl", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return 0;
    int ok = write(fd, "vendor\n", 7) == 7;
    close(fd);
    return ok;
}

/* Switch to the vendor UI. Two-stage confirm (same pattern v1 used for
 * act:exitstock in htmlmain.c): a misfire here is expensive — the vendor UI
 * has no button back to us, so the only way home is the corner long-press
 * (u60-uid) or SSH. Without u60-uid (older installs) we do it ourselves: the
 * panel is bare DRM with no compositor, so this process must exit and close
 * /dev/dri/card0 before the vendor UI can open it — the init.d start is
 * scheduled for +2s and we exit via SIGTERM. */
static void act_switch_vendor(lv_event_t *e)
{
    static uint32_t arm;
    uint32_t now = lv_tick_get();

    LV_UNUSED(e);
    arm = s_vendor_arm;
    if (arm && now - arm < 5000) {
        lv_label_set_text(s_vendor_lbl, "切换中…");
        if (request_vendor_via_uid()) return;
        system("( sleep 2; /etc/init.d/zte_topsw_devui start ) >/dev/null 2>&1 &");
        raise(SIGTERM);
        return;
    }
    s_vendor_arm = now ? now : 1;
    uk_button_kind(s_vendor_btn, s_vendor_lbl, UK_BTN_ARMED);
    lv_label_set_text(s_vendor_lbl, "再按一次确认切换");
}

static lv_obj_t *s_ap_btn[3];   /* = s_ap_seg.item[] (the render test taps them) */
static const ui_appear_t k_ap_val[3] = { UI_APPEAR_LIGHT, UI_APPEAR_DARK, UI_APPEAR_AUTO };

static void appearance_ui_sync(void)
{
    if (!s_ap_seg.obj) return;
    for (int i = 0; i < 3; i++)
        if (k_ap_val[i] == s_cf_appear) uk_seg_set(&s_ap_seg, i);
}

static void appearance_btn_cb(lv_event_t *e)
{
    s_vendor_arm = 0;   /* a pending 切换到原厂界面 confirm does not survive a theme switch */
    appearance_set(k_ap_val[(int)(intptr_t)lv_event_get_user_data(e)]);
}

static void build_system(lv_obj_t *t)
{
    static const char *const off_lbl[3] = { "常亮", "30秒", "2分钟" };
    static const char *const ap_lbl[3] = { "浅色", "深色", "自动" };
    int y = 4;
    t = uk_scroll(t, 0, UI_VIEW_H, 1000);

    uk_section(t, y, "屏幕"); y += 20;
    lv_obj_t *c = uk_card(t, UK_MARGIN, y, UK_CARD_W, 120);
    uk_label(c, UF.cj14, T->t2, UK_PAD, 11, "亮度");
    s_set_bright = uk_slider(c, 58, 17, 172);
    lv_slider_set_range(s_set_bright, 10, backlight_max());
    lv_slider_set_value(s_set_bright, backlight_get() > 0 ? backlight_get() : backlight_max(), LV_ANIM_OFF);
    lv_obj_add_event_cb(s_set_bright, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_set_bright, bright_save_cb, LV_EVENT_RELEASED, NULL);
    s_set_bright_v = uk_label_r(c, UF.n15, T->t2, UK_CARD_W - UK_PAD, 10, "");
    bright_label();
    uk_sep(c, 40);
    uk_label(c, UF.cj14, T->t2, UK_PAD, 51, "自动息屏");
    uk_seg(&s_off_seg, c, 116, 45, 170, off_lbl, 3, offsel_cb);
    uk_sep(c, 80);
    uk_label(c, UF.cj14, T->t2, UK_PAD, 91, "外观");
    uk_seg(&s_ap_seg, c, 116, 85, 170, ap_lbl, 3, appearance_btn_cb);
    for (int i = 0; i < 3; i++) s_ap_btn[i] = s_ap_seg.item[i];
    {
        int sel = 0;
        for (int i = 0; i < 3; i++) if (k_off_ms[i] == s_autooff_ms) sel = i;
        highlight_off_btns(sel);
    }
    appearance_ui_sync();
    y += 120 + 10;

    uk_section(t, y, "电池与负载"); y += 20;
    c = uk_card(t, UK_MARGIN, y, UK_CARD_W, 5 * UK_ROW_H);
    static const char *const k_load_cap[5] = { "电池", "充电器", "CPU", "内存", "运行" };
    lv_obj_t **load_val[5] = { &s_sy_bat, &s_sy_chg, &s_sy_cpu, &s_sy_mem, &s_sy_up };
    for (int i = 0; i < 5; i++) *load_val[i] = uk_row(c, i * UK_ROW_H, k_load_cap[i], i == 0);
    y += 5 * UK_ROW_H + 10;

    uk_section(t, y, "设备"); y += 20;
    c = uk_card(t, UK_MARGIN, y, UK_CARD_W, 5 * UK_ROW_H);
    s_set_ver  = uk_row(c, 0, "版本", 1);
    lv_obj_set_style_text_font(s_set_ver, UF.n12, 0);
    s_set_imei = uk_row(c, UK_ROW_H, "IMEI", 0);
    s_set_usb  = uk_row(c, 2 * UK_ROW_H, "USB", 0);
    lv_obj_set_style_text_font(s_set_usb, UF.cj14, 0);
    s_set_fw   = uk_row(c, 3 * UK_ROW_H, "固件", 0);
    lv_obj_set_style_text_font(s_set_fw, UF.n12, 0);
    lv_obj_set_y(s_set_fw, 3 * UK_ROW_H + 12);
    /* 健康: the device check's counts (doctor.sh via the agent); the whole row opens 告警 */
    s_set_health = uk_row_nav(c, 4 * UK_ROW_H, "健康", 0, open_alerts_cb, NULL);
    y += 5 * UK_ROW_H + 10;

    uk_section(t, y, "开关"); y += 20;
    c = uk_card(t, UK_MARGIN, y, UK_CARD_W, 2 * UK_ROW_H);
    uk_label(c, UF.cj14, T->t2, UK_PAD, 11, "电源直供电");
    s_sy_dps_st = uk_label_r(c, UF.cj12, T->t3, UK_CARD_W - UK_PAD - 52, 13, "");
    s_sy_dps_sw = uk_toggle(c, UK_CARD_W - UK_PAD, 7, dps_cb, NULL);
    uk_sep(c, UK_ROW_H);
    uk_label(c, UF.cj14, T->t2, UK_PAD, UK_ROW_H + 11, "状态栏网速用 Mbps");
    s_sy_speedunit_st = uk_label_r(c, UF.n12, T->t3, UK_CARD_W - UK_PAD - 52, UK_ROW_H + 13, "");
    s_sy_speedunit_sw = uk_toggle(c, UK_CARD_W - UK_PAD, UK_ROW_H + 7, speedunit_cb, NULL);
    y += 2 * UK_ROW_H + 10;

    uk_section(t, y, "系统"); y += 20;
    c = uk_card(t, UK_MARGIN, y, UK_CARD_W, 100);
    s_vendor_btn = uk_button(c, UK_PAD, 12, UK_CARD_W - 2 * UK_PAD, 40, "切换到原厂界面", UK_BTN_PLAIN,
                             act_switch_vendor, NULL, &s_vendor_lbl);
    uk_label_w(c, UF.cj12, T->t3, UK_PAD, 62, UK_CARD_W - 2 * UK_PAD, 1,
               "电源键：短按 亮屏/息屏  长按 电源菜单\n回到这里：长按屏幕右下角 3 秒");
    y += 100;
    uk_scroll_extent(t, y + UK_TAB_PAD);
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* ---- Charts page ----
 * The four series ui/05-charts.html plots. lv_chart with a fixed point count
 * and lv_chart_set_next_value(): the series buffer is allocated once, values
 * shift in place, so a page that updates every second allocates nothing. */
static lv_obj_t *chart_wait(lv_obj_t *card, int x, int y)
{
    return uk_label(card, UF.cj12, T->t3, x, y, "正在收集 · 1 分钟后出现曲线");
}

static void build_charts(lv_obj_t *t)
{
    t = uk_scroll(t, 0, UI_VIEW_H, 4 + 116 + 10 + 110 + 10 + 110 + UK_TAB_PAD);
    /* 网速: log scale (bytes/s spans five orders of magnitude here), two
     * lines told apart by colour and dash, legend in text colours. */
    lv_obj_t *c = uk_card(t, UK_MARGIN, 4, UK_CARD_W, 116);
    uk_label(c, UF.cj12, T->t3, 12, 9, "网速 · 对数刻度");
    s_ch_net_up = uk_label_r(c, UF.n12, T->warnT, 288, 8, "");
    s_ch_net_dn = uk_label_r(c, UF.n12, T->accT, 200, 8, "");
    s_ch_net = uk_chart(c, 44, 30, 244, 60, CHART_PTS, T->blue, T->orange, &s_cs_rx, &s_cs_tx);
    /* rate_scale: 10^8 B/s = 100 → 10 MB/s at 87.5, 100 KB/s at 62.5. The
     * axis shows 50–100 (~30 KB/s to 100 MB/s): below that is idle chatter. */
    lv_chart_set_axis_range(s_ch_net, LV_CHART_AXIS_PRIMARY_Y, 50, 100);
    uk_label(c, UF.n11, T->t3, 12, 30 + 60 * 25 / 100 - 7, "10M");
    uk_label(c, UF.n11, T->t3, 12, 30 + 60 * 75 / 100 - 7, "100K");
    uk_label_r(c, UF.cj12, T->t3, 288, 94, "近 5 分钟");
    s_ch_wait[0] = chart_wait(c, 60, 52);

    lv_obj_t *a = uk_card(t, UK_MARGIN, 130, 145, 110), *m = uk_card(t, UK_MARGIN + 155, 130, 145, 110);
    uk_label(a, UF.cj12, T->t3, 12, 9, "CPU");
    s_ch_cpu_t = uk_label_r(a, UF.cj12, T->t2, 133, 9, "");
    s_ch_cpu_v = uk_label(a, UF.n20, T->t1, 12, 26, "");
    s_ch_cpu = uk_chart(a, 12, 60, 121, 40, CHART_PTS, T->blue, 0, &s_cs_cpu, NULL);
    s_ch_wait[1] = uk_label(a, UF.cj12, T->t3, 12, 72, "正在收集…");
    uk_label(m, UF.cj12, T->t3, 12, 9, "内存");
    s_ch_mem_s = uk_label_r(m, UF.n12, T->t2, 133, 9, "");
    s_ch_mem_v = uk_label(m, UF.n20, T->t1, 12, 26, "");
    s_ch_mem = uk_chart(m, 12, 60, 121, 40, CHART_PTS, T->blue, 0, &s_cs_mem, NULL);
    s_ch_wait[2] = uk_label(m, UF.cj12, T->t3, 12, 72, "正在收集…");

    lv_obj_t *b = uk_card(t, UK_MARGIN, 250, UK_CARD_W, 110);
    uk_label(b, UF.cj12, T->t3, 12, 9, "电池");
    s_ch_bat_s = uk_label_r(b, UF.cj12, T->t2, 288, 9, "");
    s_ch_bat_v = uk_label(b, UF.n20, T->t1, 12, 26, "");
    s_ch_bat = uk_chart(b, 12, 56, 276, 36, CHART_PTS, T->green, 0, &s_cs_bat, NULL);
    uk_label_r(b, UF.cj12, T->t3, 288, 92, "近 5 分钟");
    s_ch_wait[3] = chart_wait(b, 12, 66);
}

/* ---- perf test subpage ---- */
/* 性能测试: a debug page; its layout stays, only the colours follow the theme. */
static void build_sub_perf(lv_obj_t *t)
{
    lv_obj_t *c = uk_card(t, UK_MARGIN, 8, UK_CARD_W, 120);
    uk_label(c, UF.cj12, T->t3, UK_PAD, 10, "渲染刷新率");
    s_t_fps = uk_label(c, UF.n20, T->okT, UK_PAD, 28, "-- FPS");
    uk_label(c, UF.cj12, T->t3, UK_PAD, 62, "触控上报率");
    s_t_touch = uk_label(c, UF.n20, T->warnT, UK_PAD, 80, "-- Hz");
    s_t_box = uk_box(t, UK_MARGIN, 150, UK_CARD_W, 76, T->green, UK_R_CARD);
}

static void sms_row_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (sms_delete_tap(idx)) return;          /* second tap on an armed row: deleted */
    if (s_sms_row_id[idx] < 0) return;
    /* Open it. Reading it in full is what marks it read — tapping a row used
     * to mark it read with nothing else happening, which looked like the tap
     * did nothing, and left no way to see a long message past two lines. */
    s_smsd_id = s_sms_row_id[idx];
    s_smsd_del_arm = 0;
    sms_mark_read_id(s_smsd_id);
    lv_obj_scroll_to_y(s_smsd_scroll, 0, LV_ANIM_OFF);
    sub_open_child(SUB_SMS_DETAIL, SUB_SMS);
}

static void sms_allread_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    sms_mark_all_read();
}

/* Two taps within 4 s, same pattern as the list's long-press delete. */
static void smsd_delete_cb(lv_event_t *e)
{
    uint32_t now = lv_tick_get();
    LV_UNUSED(e);
    if (s_smsd_del_arm && now - s_smsd_del_arm > 300 && now - s_smsd_del_arm < 4000) {
        sms_delete_id(s_smsd_id);
        s_smsd_del_arm = 0;
        s_smsd_id = -1;
        sub_back();
        return;
    }
    s_smsd_del_arm = now;
}

static void sms_row_longpress_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    sms_delete_arm(idx);
}

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

static void build_sub_alerts(lv_obj_t *t)
{
    t = uk_scroll(t, 0, UI_SUB_VIEW, SMS_TOOLBAR_H + ALERTS_MAX * AL_ROW_H + 16);
    list_toolbar(t, &s_al_count, &s_al_allread_btn, al_allread_cb);
    s_al_empty = uk_label_w(t, UF.cj14, T->t3, UK_MARGIN + 6, SMS_TOOLBAR_H + 4, UK_CARD_W - 12, 1, "");
    s_al_list = uk_card(t, UK_MARGIN, SMS_TOOLBAR_H, UK_CARD_W, ALERTS_MAX * AL_ROW_H);
    for (int i = 0; i < ALERTS_MAX; i++) {
        lv_obj_t *c = uk_box(s_al_list, 0, i * AL_ROW_H, UK_CARD_W, AL_ROW_H, T->card, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
        s_al_row[i] = c;
        if (i) uk_sep(c, 0);
        s_al_mark[i] = uk_label(c, UF.cj12, T->warnT, UK_PAD, 12, "▲");
        s_al_label[i] = uk_label_w(c, UF.cj14, T->t1, UK_PAD + 18, 10, UK_CARD_W - 2 * UK_PAD - 18, 0, "");
        s_al_time[i] = uk_label_r(c, UF.n12, T->t3, UK_CARD_W - UK_PAD, 36, "");
        s_al_text[i] = uk_label_w(c, UF.n11, T->t3, UK_PAD + 18, 37, UK_CARD_W - 2 * UK_PAD - 18 - 90, 0, "");
        uk_show(c, 0);
    }
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
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

static const char *const k_exit_state[4] = { "proxy", "global", "direct_keep_ai", "direct_all" };
static void chill_mode_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    chill_set_exit(k_exit_state[idx]);
}

static void chill_node_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    chill_select_node(idx);
}

static void chill_group_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    chill_select_group(idx);
}

static void chill_delay_cb(lv_event_t *e) { LV_UNUSED(e); chill_test_delay(); }
static void chill_profile_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (!strcmp(chill_profile_raw(), k_profile[idx])) return;
    if (chill_set_profile(k_profile[idx]))
        for (int i = 0; i < 3; i++) uk_opt_set(&s_cp_prof[i], i == idx, 0);
}

#define CHILL_GRP_ROWS ((CHILL_MAX_GROUPS + CHILL_GRP_COLS - 1) / CHILL_GRP_COLS)
/* 策略组卡永远按 CHILL_MAX_GROUPS 的满槽位留高度，不跟着实际组数收缩（这张
 * 卡本来就是这么建的，不是我这次改的）——所以它的高度是个编译期常量，
 * 一个宏就够，refresh_cb 给流量卡定位时要用同一个数，不能各算各的。 */
#define CHILL_GRP_H (30 + CHILL_GRP_ROWS * 36 - 6 + 8)

/* 一张"按 X 统计流量"卡：标题 + 最多 CHILL_TRAF_ROWS 行（名字左，流量右），
 * 固定槽位建好、按实际条目数隐藏/显示——跟节点卡、策略组卡同一个规矩，不
 * 跟着数据量动态建对象。返回卡片高度，调用方拿去算下一张卡的 y。 */

/* ---- 国外时的 CHILL 出口面板 ----
 * 在国外换了当地卡：大部分流量直连就好，但 🤖 AI（按地区限制、IP 突变会触发风控）
 * 和 📞 VoWiFi（运营商的 Wi-Fi 通话通常只认本国 IP）要固定走原来的节点。四个选项就是用户实际会用的
 * 四种状态；「关闭」会让 AI 和 VoWiFi 也直连，所以要点两下并写明代价。
 * 回国（换回国内卡）后 agent 自动回到「代理」、打开在国外关掉的 CHILL。 */
static void exit_menu_refresh(void)
{
    static const char *const k_lbl[XM_N] = {
        "代理（和在国内一样）", "全局", "直连 · AI 不动", "全部直连（AI 也直连）", "关闭 CHILL（最省电）",
    };
    scenario_status_t sc;
    scenario_get_status(&sc);
    const char *x = chill_exit_raw();
    int on = sc.chill_on != 0;
    for (int i = 0; i < XM_N; i++) {
        int cur = on && ((i == XM_PROXY && !strcmp(x, "proxy")) ||
                         (i == XM_GLOBAL && !strcmp(x, "global")) ||
                         (i == XM_KEEP_AI && !strcmp(x, "direct_keep_ai")) ||
                         (i == XM_ALL && !strcmp(x, "direct_all")));
        const char *t = k_lbl[i];
        uint32_t col = on ? T->accT : T->t3;   /* CHILL 关着时前四项没意义 */
        uint32_t bg = 0;
        if (i == XM_OFF) {
            if (!on)               { t = "打开 CHILL"; col = T->okT; }
            else if (s_xm_off_arm) { t = "再点一下：关闭（AI、VoWiFi 也直连）"; col = 0xffffff; bg = T->fillOrange; }
            else                   col = T->badT;
        }
        lv_label_set_text(s_xm_lbl[i], t);
        uk_text_color(s_xm_lbl[i], col);
        lv_obj_set_style_text_font(s_xm_lbl[i], cur ? UF.cj17b : UF.cj15, 0);
        lv_obj_set_style_bg_opa(s_xm_btn[i], bg ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (bg) uk_bg(s_xm_btn[i], bg);
        /* ✓ from the symbol font: the device CJK font has none */
        uk_show(s_xm_ok[i], cur);
        if (cur) {
            lv_obj_update_layout(s_xm_lbl[i]);
            lv_obj_set_pos(s_xm_ok[i], lv_obj_get_x(s_xm_lbl[i]) - 22, 11);
        }
    }
    lv_label_set_text(s_xm_foot, sc.auto_direct
        ? "换国外卡已自动切到「直连 · AI 不动」，回国自动回到「代理」"
        : "换回国内卡后自动回到「代理」");
    uk_text_color(s_xm_foot, T->t3);
}

static void exit_menu_set(int v)
{
    s_xm_off_arm = 0;
    if (v) exit_menu_refresh();
    uk_sheet_show(&s_xm_sheet, v);
    update_tabs();
}

static void exit_menu_fail(void)
{
    lv_label_set_text(s_xm_foot, "没成功（后台没回应或 CHILL 没在运行），稍后再试");
    uk_text_color(s_xm_foot, T->warnT);
}

static void exit_pick_cb(lv_event_t *e)
{
    static const char *const k_state[4] = { "proxy", "global", "direct_keep_ai", "direct_all" };
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    scenario_status_t sc;
    scenario_get_status(&sc);
    if (i == XM_OFF) {
        if (sc.chill_on == 0) {                     /* 打开 */
            if (scenario_chill_set(1)) exit_menu_set(0); else exit_menu_fail();
            s_sc_force = 1;
            return;
        }
        uint32_t now = lv_tick_get();
        if (s_xm_off_arm && now - s_xm_off_arm < XM_ARM_MS) {
            if (scenario_chill_set(0)) exit_menu_set(0); else exit_menu_fail();
            s_sc_force = 1;
        } else {
            s_xm_off_arm = now ? now : 1;
            exit_menu_refresh();
        }
        return;
    }
    if (sc.chill_on == 0) return;
    if (chill_set_exit(k_state[i])) exit_menu_set(0); else exit_menu_fail();
    s_sc_force = 1;
}

static void exit_cancel_cb(lv_event_t *e) { LV_UNUSED(e); exit_menu_set(0); }

#define XM_TOP 62
static void build_exit_menu(void)
{
    uk_sheet(&s_xm_sheet, XM_TOP + XM_N * 40 + 4, exit_cancel_cb);
    s_xm = s_xm_sheet.scrim;
    lv_obj_t *p = s_xm_sheet.panel;
    lv_obj_t *title = uk_label(p, UF.cj13, T->t3, 0, 0, "在国外 · CHILL 怎么走");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    s_xm_foot = uk_label_w(p, UF.cj12, T->t3, 12, 27, UK_W - 16 - 24, 1, "");
    lv_obj_set_style_text_align(s_xm_foot, LV_TEXT_ALIGN_CENTER, 0);
    for (int i = 0; i < XM_N; i++) {
        s_xm_lbl[i] = uk_sheet_item(&s_xm_sheet, XM_TOP + i * 40, "", T->accT, exit_pick_cb, (void *)(intptr_t)i, &s_xm_btn[i]);
        s_xm_ok[i] = uk_label(s_xm_btn[i], &lv_font_montserrat_16, T->accT, 0, 11, LV_SYMBOL_OK);
        uk_show(s_xm_ok[i], 0);
    }
}

static void sc_card_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    scenario_status_t sc;
    scenario_get_status(&sc);
    if (sc.abroad && sc.chill_on >= 0) exit_menu_set(1);
}

/* CHILL 页总开关。agent 拒绝或连不上就把开关拨回去。 */
static void chill_master_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    int on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (!scenario_chill_set(on)) sw_apply(sw, !on);
}

static void build_sub_chill(lv_obj_t *t)
{
    t = uk_scroll(t, 0, UI_SUB_VIEW, 4 + UK_HERO_H + 80 + 10 + 20 + 124 + 10 + 20 + 70 + 10 + 80 + 16);
    /* status block: node (big), delay, group; the master switch top right */
    lv_obj_t *st = uk_card(t, UK_MARGIN, 4, UK_CARD_W, UK_HERO_H + 2 * UK_ROW_H);
    uk_hero(&s_cp_hero, st, UF.cj22b);
    lv_obj_set_width(s_cp_hero.big, 180);
    lv_label_set_long_mode(s_cp_hero.big, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_cp_hero.big, lv_font_get_line_height(UF.cj22b));
    s_cp_core = s_cp_hero.st;
    s_cp_sw = uk_toggle(st, UK_CARD_W - UK_PAD, 6, chill_master_cb, NULL);
    uk_show(s_cp_sw, 0);          /* 知道开关状态后才显示 */
    s_sc_force = 1;               /* 下一轮按已知状态同步 */
    uk_show(s_cp_hero.rtop, 0);
    s_cp_conns = uk_row(st, UK_HERO_H, "连接", 1);
    s_cp_traffic = uk_row(st, UK_HERO_H + UK_ROW_H, "流量", 0);
    int y = 4 + UK_HERO_H + 2 * UK_ROW_H + 10;

    uk_section(t, y, "出口");
    s_cp_exit_note = uk_label_r(t, UF.cj12, T->t3, UK_W - UK_MARGIN - 6, y, "");
    y += 20;
    lv_obj_t *md = uk_card(t, UK_MARGIN, y, UK_CARD_W, 124);
    static const char *const labels[4] = { "代理", "全局", "直连·AI 不动", "全部直连" };
    static const char *const subs[4] = { "规则分流", "全部走代理", "国外卡用", "AI 也直连" };
    for (int i = 0; i < 4; i++) {
        uk_opt(&s_cp_opt[i], md, 12 + (i % 2) * 142, 12 + (i / 2) * 54, labels[i], subs[i], chill_mode_cb, (void *)(intptr_t)i);
        s_cp_mode_btn[i] = s_cp_opt[i].obj;
        uk_opt_set(&s_cp_opt[i], 0, 0);
    }
    y += 124 + 10;

    /* 档位：三项一排（uk_opt 默认 134 宽是两列用的，这里改成 88） */
    uk_section(t, y, "档位");
    s_cp_prof_note = uk_label_r(t, UF.cj12, T->t3, UK_W - UK_MARGIN - 6, y, "");
    y += 20;
    lv_obj_t *pf = uk_card(t, UK_MARGIN, y, UK_CARD_W, 70);
    static const char *const plabels[3] = { "省电", "标准", "性能" };
    static const char *const psubs[3] = { "更凉更省", "默认", "切换更快" };
    for (int i = 0; i < 3; i++) {
        uk_opt(&s_cp_prof[i], pf, 12 + i * 94, 12, plabels[i], psubs[i], chill_profile_cb, (void *)(intptr_t)i);
        lv_obj_set_width(s_cp_prof[i].obj, 88);
        uk_opt_set(&s_cp_prof[i], i == 1, 0);
    }
    y += 70 + 10;

    lv_obj_t *nav = uk_card(t, UK_MARGIN, y, UK_CARD_W, CHILL_NAV_ROWS * UK_ROW_H);
    static const char *const k_nav_cap[CHILL_NAV_ROWS] = { "节点", "规则 → 节点" };
    static const int k_nav_child[CHILL_NAV_ROWS] = { SUB_CHILL_NODES, SUB_CHILL_PAIRS };
    for (int i = 0; i < CHILL_NAV_ROWS; i++)
        s_cp_nav_val[i] = uk_row_nav(nav, i * UK_ROW_H, k_nav_cap[i], i == 0, chill_nav_cb, (void *)(intptr_t)k_nav_child[i]);
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* 策略组 chips flow like band chips; the node list follows them. */
static void chill_nodes_layout(int ng)
{
    int x = UK_PAD, y = 12, maxx = UK_CARD_W - UK_PAD;
    for (int i = 0; i < ng; i++) {
        lv_obj_update_layout(s_cp_grp_lbl[i]);
        int w = lv_obj_get_width(s_cp_grp_lbl[i]) + 22;
        lv_obj_set_width(s_cp_grp_btn[i], w);
        if (x + w > maxx) { x = UK_PAD; y += 36; }
        lv_obj_set_pos(s_cp_grp_btn[i], x, y);
        x += w + 6;
    }
    int gh = y + 28 + 12;
    lv_obj_set_height(s_cp_grp_card, gh);
    int ny = 24 + gh + 10;
    lv_obj_set_y(s_cp_node_sec, ny);
    lv_obj_align(s_cp_delay_lbl, LV_ALIGN_TOP_RIGHT, -(UK_MARGIN + 6), ny);
    lv_obj_set_y(s_cp_node_card, ny + 20);
}

static void build_sub_chill_nodes(lv_obj_t *t)
{
    t = s_cp_scroll_nodes = uk_scroll(t, 0, UI_SUB_VIEW, 200 + CHILL_MAX_NODES * UK_ROW_H);
    uk_section(t, 4, "策略组");
    s_cp_grp_card = uk_card(t, UK_MARGIN, 24, UK_CARD_W, 52);
    for (int i = 0; i < CHILL_MAX_GROUPS; i++) {
        s_cp_grp_btn[i] = uk_chip(s_cp_grp_card, UK_PAD, 12, "", &s_cp_grp_lbl[i]);
        lv_obj_set_style_text_font(s_cp_grp_lbl[i], UF.cj13, 0);
        lv_obj_set_y(s_cp_grp_lbl[i], 6);
        uk_tappable(s_cp_grp_btn[i], chill_group_cb, (void *)(intptr_t)i);
        uk_show(s_cp_grp_btn[i], 0);
    }
    s_cp_node_sec = uk_section(t, 90, "节点");
    s_cp_delay_lbl = uk_label(t, UF.cj13, T->accT, 0, 90, "测延迟");
    lv_obj_add_flag(s_cp_delay_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_cp_delay_lbl, 14);
    lv_obj_add_event_cb(s_cp_delay_lbl, chill_delay_cb, LV_EVENT_CLICKED, NULL);
    s_cp_node_card = uk_card(t, UK_MARGIN, 110, UK_CARD_W, UK_ROW_H);
    for (int i = 0; i < CHILL_MAX_NODES; i++) {
        lv_obj_t *row = uk_box(s_cp_node_card, 0, i * UK_ROW_H, UK_CARD_W, UK_ROW_H, T->card, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        uk_tappable(row, chill_node_cb, (void *)(intptr_t)i);
        s_cp_node_row[i] = row;
        if (i) uk_sep(row, 0);
        s_cp_node_ok[i] = uk_label(row, &lv_font_montserrat_14, T->accT, UK_PAD, 12, LV_SYMBOL_OK);
        s_cp_node_name[i] = uk_label_w(row, UF.cj14, T->t1, UK_PAD + 22, 11, UK_CARD_W - 2 * UK_PAD - 22 - 70, 0, "");
        s_cp_node_dl[i] = uk_label_r(row, UF.n15, T->t3, UK_CARD_W - UK_PAD, 10, "");
        uk_show(row, 0);
    }
    chill_nodes_layout(0);
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

static void build_sub_chill_pairs(lv_obj_t *t)
{
    t = uk_scroll(t, 0, UI_SUB_VIEW, 24 + CHILL_TRAF_ROWS * UK_ROW_H + 16);
    uk_section(t, 4, "规则 → 节点流量");
    s_cp_pair_card = uk_card(t, UK_MARGIN, 24, UK_CARD_W, CHILL_TRAF_ROWS * UK_ROW_H);
    for (int i = 0; i < CHILL_TRAF_ROWS; i++) {
        s_cp_pair_sep[i] = i ? uk_sep(s_cp_pair_card, i * UK_ROW_H) : NULL;
        s_cp_pair_name[i] = uk_label_w(s_cp_pair_card, UF.cj13, T->t1, UK_PAD, i * UK_ROW_H + 12, 190, 0, "");
        s_cp_pair_val[i] = uk_label_r(s_cp_pair_card, UF.n12, T->t2, UK_CARD_W - UK_PAD, i * UK_ROW_H + 13, "");
        uk_show(s_cp_pair_name[i], 0);
        uk_show(s_cp_pair_val[i], 0);
    }
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

#define TS_PEER_H 50
static void build_sub_ts(lv_obj_t *t)
{
    t = uk_scroll(t, 0, UI_SUB_VIEW, 24 + 4 * UK_ROW_H + 10 + 20 + TS_PEER_MAX * TS_PEER_H + 16);
    uk_section(t, 4, "本机");
    lv_obj_t *self = uk_card(t, UK_MARGIN, 24, UK_CARD_W, 4 * UK_ROW_H);
    static const char *const k_self_cap[4] = { "主机名", "IP", "DERP", "子网路由" };
    for (int i = 0; i < 4; i++) s_tp_self[i] = uk_row(self, i * UK_ROW_H, k_self_cap[i], i == 0);
    lv_obj_set_style_text_font(s_tp_self[0], UF.cj14, 0);
    int y = 24 + 4 * UK_ROW_H + 10;
    uk_section(t, y, "节点");
    s_tp_card = uk_card(t, UK_MARGIN, y + 20, UK_CARD_W, TS_PEER_MAX * TS_PEER_H);
    for (int i = 0; i < TS_PEER_MAX; i++) {
        s_tp_row[i] = uk_box(s_tp_card, 0, i * TS_PEER_H, UK_CARD_W, TS_PEER_H, T->card, 0);
        lv_obj_set_style_bg_opa(s_tp_row[i], LV_OPA_TRANSP, 0);
        s_tp_sep[i] = i ? uk_sep(s_tp_row[i], 0) : NULL;
        s_tp_name[i] = uk_label_w(s_tp_row[i], UF.cj14, T->t1, UK_PAD, 8, 190, 0, "");
        s_tp_ip[i] = uk_label(s_tp_row[i], UF.n11, T->t3, UK_PAD, 29, "");
        s_tp_tag[i] = uk_label_r(s_tp_row[i], UF.cj13, T->t3, UK_CARD_W - UK_PAD, 16, "");
        uk_show(s_tp_row[i], 0);
    }
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

static void build_sub_cell(lv_obj_t *t)
{
    int y = 4;
    t = uk_scroll(t, 0, UI_SUB_VIEW, 1000);

    uk_section(t, y, "5G 服务小区"); y += 20;
    lv_obj_t *nr = uk_card(t, UK_MARGIN, y, UK_CARD_W, 6 * UK_ROW_H);
    static const char *const k_nr_cap[6] = { "频段", "ARFCN", "PCI", "Cell ID", "PLMN", "RSRP / RSRQ / SINR" };
    for (int i = 0; i < 6; i++) s_sg_nr[i] = uk_row(nr, i * UK_ROW_H, k_nr_cap[i], i == 0);
    y += 6 * UK_ROW_H + 10;

    uk_section(t, y, "LTE"); y += 20;
    lv_obj_t *lte = uk_card(t, UK_MARGIN, y, UK_CARD_W, UK_ROW_H);
    s_sg_lte = uk_label_w(lte, UF.n12, T->t1, UK_PAD, 12, UK_CARD_W - 2 * UK_PAD, 0, "");
    y += UK_ROW_H + 10;

    uk_section(t, y, "网络"); y += 20;
    lv_obj_t *net = uk_card(t, UK_MARGIN, y, UK_CARD_W, 4 * UK_ROW_H);
    static const char *const k_net_cap[4] = { "选网方式", "WAN", "制式", "高铁模式" };
    for (int i = 0; i < 4; i++) s_sg_net[i] = uk_row(net, i * UK_ROW_H, k_net_cap[i], i == 0);
    lv_obj_set_style_text_font(s_sg_net[3], UF.cj14, 0);
    y += 4 * UK_ROW_H + 10;

    uk_section(t, y, "支持频段"); y += 20;
    lv_obj_t *cap = uk_card(t, UK_MARGIN, y, UK_CARD_W, 150);
    uk_label(cap, UF.cj13, T->t2, UK_PAD, 11, "5G");
    s_sg_nrb = uk_label_w(cap, UF.n12, T->t1, UK_PAD + 40, 11, UK_CARD_W - 2 * UK_PAD - 40, 1, "");
    uk_sep(cap, 74);
    uk_label(cap, UF.cj13, T->t2, UK_PAD, 85, "LTE");
    s_sg_lteb = uk_label_w(cap, UF.n12, T->t1, UK_PAD + 40, 85, UK_CARD_W - 2 * UK_PAD - 40, 1, "");
    y += 150 + 10;

    uk_section(t, y, "邻小区 / 调度明细"); y += 20;
    lv_obj_t *note = uk_card(t, UK_MARGIN, y, UK_CARD_W, 70);
    uk_label_w(note, UF.cj13, T->t2, UK_PAD, 12, UK_CARD_W - 2 * UK_PAD, 1,
               "拿不到：本机的 zwrt-datad 没有 /modem/latest-signals 接口，MIMO/层数/RB/BLER 和邻小区列表都读不到。");
    y += 70;
    uk_scroll_extent(t, y + 16);
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* ---- 锁频 subpage ----
 * Mirrors the litehtml 锁频 page: network-mode selector, one band picker per
 * technology, reset-to-default. Every action that actually reaches the modem
 * is two-stage (tap to arm, tap again within 5s to send) — a mis-tap here
 * drops the connection, and the same pattern already guards the
 * switch-to-vendor-UI button. */
static void band_group_apply(int gi)
{
    band_group_t *g = &s_bg[gi];
    char csv[256] = "";
    char cmd[400];
    int o = 0, n = 0;

    for (int i = 0; i < g->n; i++)
        if (g->sel[i]) {
            o += snprintf(csv + o, sizeof csv - (size_t)o, "%s%d", n ? "," : "", g->band_no[i]);
            n++;
        }
    if (!n) return;   /* locking zero bands would strand the modem */
    if (gi == BG_LTE)
        snprintf(cmd, sizeof cmd,
                 "ubus call zte_nwinfo_api nwinfo_set_lte_ext_band '{\"lte_band\":\"%s\"}' >/dev/null 2>&1 &",
                 csv);
    else
        snprintf(cmd, sizeof cmd,
                 "ubus call zte_nwinfo_api nwinfo_set_nrbandlock '{\"nr5g_type\":\"%s\",\"nr5g_band\":\"%s\"}' >/dev/null 2>&1 &",
                 gi == BG_SA ? "sa" : "nsa", csv);
    system(cmd);
}

static void band_summary_set(int gi)
{
    band_group_t *g = &s_bg[gi];
    char buf[200];
    int o = 0, n = 0, all = 1;

    for (int i = 0; i < g->n; i++) {
        if (!g->sel[i]) { all = 0; continue; }
        n++;
        if (o < (int)sizeof buf - 8)
            o += snprintf(buf + o, sizeof buf - (size_t)o, "%s%c%d",
                          o ? " " : "", g->prefix, g->band_no[i]);
    }
    if (!g->n)      lv_label_set_text(g->summary, "—");
    else if (all)   lv_label_set_text(g->summary, "\xE5\x85\xA8\xE9\x83\xA8\xE9\xA2\x91\xE6\xAE\xB5\xEF\xBC\x88\xE6\x9C\xAA\xE9\x94\x81\xE5\xAE\x9A\xEF\xBC\x89" /* 全部频段（未锁定） */);
    else if (!n)    lv_label_set_text(g->summary, "\xE6\x9C\xAA\xE9\x80\x89\xE9\xA2\x91\xE6\xAE\xB5" /* 未选频段 */);
    else            lv_label_set_text(g->summary, buf);
}

static void band_chip_paint(int gi, int i)
{
    band_group_t *g = &s_bg[gi];
    /* Every band selected = not locked, the default: muted (accS), since 21
     * saturated chips would read as 21 alarms. A real lock selection is
     * fillBlue with white text. */
    int all = 1;
    for (int k = 0; k < g->n; k++) if (!g->sel[k]) all = 0;
    if (g->sel[i] && all) {
        uk_bg(g->chip[i], T->accS);
        uk_text_color(g->chip_lbl[i], T->accT);
    } else {
        uk_chip_set(g->chip[i], g->chip_lbl[i], g->sel[i]);
    }
}
static void band_chip_cb(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    int gi = code / 64, i = code % 64;
    if (gi < 0 || gi > 2 || i >= s_bg[gi].n) return;
    s_bg[gi].sel[i] = !s_bg[gi].sel[i];
    for (int k = 0; k < s_bg[gi].n; k++) band_chip_paint(gi, k);
    band_summary_set(gi);
}

static void band_apply_cb(lv_event_t *e)
{
    int gi = (int)(intptr_t)lv_event_get_user_data(e);
    band_group_t *g = &s_bg[gi];
    uint32_t now = lv_tick_get();

    if (g->arm && now - g->arm < 5000) {
        g->arm = 0;
        lv_label_set_text(g->apply_lbl, "已下发…");
        uk_button_kind(g->apply_btn, g->apply_lbl, UK_BTN_PLAIN);
        band_group_apply(gi);
        return;
    }
    g->arm = now ? now : 1;
    lv_label_set_text(g->apply_lbl, "再按一次确认");
    uk_button_kind(g->apply_btn, g->apply_lbl, UK_BTN_ARMED);
}

static void lk_mode_cb(lv_event_t *e)
{
    static const char *const k_mode_v[4] = { "WL_AND_5G", "Only_5G", "LTE_AND_5G", "Only_LTE" };
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    uint32_t now = lv_tick_get();
    char cmd[200];

    if (s_lk_mode_pending == idx && s_lk_mode_arm && now - s_lk_mode_arm < 5000) {
        snprintf(cmd, sizeof cmd,
                 "ubus call zte_nwinfo_api nwinfo_set_netselect '{\"net_select\":\"%s\"}' >/dev/null 2>&1 &",
                 k_mode_v[idx]);
        system(cmd);
        s_lk_mode_arm = 0;
        s_lk_mode_pending = -1;
        s_lk_seg.sel = -2;   /* repaint from the modem's answer */
        lv_label_set_text(s_lk_mode_lbl, "已下发…");
        return;
    }
    s_lk_mode_arm = now ? now : 1;
    s_lk_mode_pending = idx;
    lv_label_set_text(s_lk_mode_lbl, "再按一次确认切换");
    uk_seg_set(&s_lk_seg, -1);
    uk_seg_arm(&s_lk_seg, idx);
}

static void lk_reset_cb(lv_event_t *e)
{
    uint32_t now = lv_tick_get();
    LV_UNUSED(e);
    if (s_lk_reset_arm && now - s_lk_reset_arm < 5000) {
        s_lk_reset_arm = 0;
        system("ubus call zte_nwinfo_api nwinfo_reset_band_cell_setting '{}' >/dev/null 2>&1 &");
        lv_label_set_text(s_lk_reset_lbl, "已恢复默认");
        uk_button_kind(s_lk_reset_btn, s_lk_reset_lbl, UK_BTN_DANGER);
        return;
    }
    s_lk_reset_arm = now ? now : 1;
    lv_label_set_text(s_lk_reset_lbl, "再按一次确认");
    uk_button_kind(s_lk_reset_btn, s_lk_reset_lbl, UK_BTN_ARMED);
}

/* Chips flow left to right, as many per line as fit; the card grows with them. */
static int band_group_layout(int gi)
{
    band_group_t *g = &s_bg[gi];
    int x = UK_PAD, y = 36, maxx = UK_CARD_W - UK_PAD;
    for (int i = 0; i < g->n; i++) {
        lv_obj_update_layout(g->chip_lbl[i]);
        int w = lv_obj_get_width(g->chip_lbl[i]) + 22;
        lv_obj_set_width(g->chip[i], w);
        if (x + w > maxx) { x = UK_PAD; y += 36; }
        lv_obj_set_pos(g->chip[i], x, y);
        x += w + 6;
    }
    y += g->n ? 36 : 0;
    lv_obj_set_y(g->apply_btn, y + 4);
    int h = y + 4 + 36 + 12;
    lv_obj_set_height(g->card, h);
    return h;
}

static void lock_reflow(void)
{
    int y = 4 + 20 + 84 + 10;
    for (int gi = 0; gi < 3; gi++) {
        lv_obj_set_y(s_bg[gi].sec, y);
        lv_obj_set_y(s_bg[gi].card, y + 20);
        y += 20 + (int)lv_obj_get_style_height(s_bg[gi].card, 0) + 10;
    }
    lv_obj_set_y(s_lk_reset_sec, y);
    lv_obj_set_y(s_lk_reset_card, y + 20);
    uk_scroll_extent(s_lk_scroll, y + 20 + 88 + 16);
}

static void band_group_build(lv_obj_t *t, int gi, const char *title, char prefix)
{
    band_group_t *g = &s_bg[gi];
    g->prefix = prefix;
    g->sec = uk_section(t, 0, title);
    g->card = uk_card(t, UK_MARGIN, 0, UK_CARD_W, 100);
    g->summary = uk_label_w(g->card, UF.cj13, T->t2, UK_PAD, 11, UK_CARD_W - 2 * UK_PAD, 0, "—");
    for (int i = 0; i < BAND_MAX; i++) {
        g->chip[i] = uk_chip(g->card, UK_PAD, 36, "", &g->chip_lbl[i]);
        uk_tappable(g->chip[i], band_chip_cb, (void *)(intptr_t)(gi * 64 + i));
        uk_show(g->chip[i], 0);
    }
    g->apply_btn = uk_button(g->card, UK_PAD, 40, UK_CARD_W - 2 * UK_PAD, 36, "应用锁频", UK_BTN_PLAIN,
                             band_apply_cb, (void *)(intptr_t)gi, &g->apply_lbl);
    band_group_layout(gi);
}

static void build_sub_lock(lv_obj_t *t)
{
    t = s_lk_scroll = uk_scroll(t, 0, UI_SUB_VIEW, 1400);
    uk_section(t, 4, "选网方式");
    lv_obj_t *md = uk_card(t, UK_MARGIN, 24, UK_CARD_W, 84);
    static const char *const k_mode_lab[4] = { "自动", "5G SA", "5G NSA", "4G" };
    uk_seg(&s_lk_seg, md, UK_PAD, 12, UK_CARD_W - 2 * UK_PAD, k_mode_lab, 4, lk_mode_cb);
    for (int i = 0; i < 4; i++) s_lk_mode_btn[i] = s_lk_seg.item[i];
    s_lk_mode_lbl = uk_label_w(md, UF.cj12, T->t3, UK_PAD, 54, UK_CARD_W - 2 * UK_PAD, 0, "切换会短暂断网，需要按两次确认");

    band_group_build(t, BG_SA,  "5G SA 频段", 'n');
    band_group_build(t, BG_NSA, "5G NSA 频段", 'n');
    band_group_build(t, BG_LTE, "4G 频段", 'B');

    s_lk_reset_sec = uk_section(t, 0, "恢复");
    s_lk_reset_card = uk_card(t, UK_MARGIN, 0, UK_CARD_W, 88);
    s_lk_reset_btn = uk_button(s_lk_reset_card, UK_PAD, 12, UK_CARD_W - 2 * UK_PAD, 36, "恢复默认配置", UK_BTN_DANGER,
                               lk_reset_cb, NULL, &s_lk_reset_lbl);
    uk_label_w(s_lk_reset_card, UF.cj12, T->t3, UK_PAD, 58, UK_CARD_W - 2 * UK_PAD, 0, "清掉全部锁频和锁小区设置，回到自动选网");
    lock_reflow();
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

static void band_group_sync(int gi, const char *csv)
{
    band_group_t *g = &s_bg[gi];
    char buf[256];
    char *save = NULL;

    if (!strcmp(g->last_csv, csv)) return;
    snprintf(g->last_csv, sizeof g->last_csv, "%s", csv);
    snprintf(buf, sizeof buf, "%s", csv);
    g->n = 0;
    for (char *tk = strtok_r(buf, ",", &save); tk && g->n < BAND_MAX; tk = strtok_r(NULL, ",", &save)) {
        int b = atoi(tk);
        if (b <= 0) continue;
        g->band_no[g->n] = b;
        g->sel[g->n] = 1;               /* all bands selected == not locked */
        lv_label_set_text_fmt(g->chip_lbl[g->n], "%c%d", g->prefix, b);
        lv_obj_remove_flag(g->chip[g->n], LV_OBJ_FLAG_HIDDEN);
        band_chip_paint(gi, g->n);
        g->n++;
    }
    for (int i = g->n; i < BAND_MAX; i++) lv_obj_add_flag(g->chip[i], LV_OBJ_FLAG_HIDDEN);
    band_summary_set(gi);
    band_group_layout(gi);
    lock_reflow();
}

/* ---- 测速 subpage ----
 * 2026-09-22: rebuilt on top of zte-agent's own speed test engine (see
 * speedtest.h/.c) instead of the old, never-installed better-speedtest
 * plugin — this used to be a permanent "not installed" placeholder. */
static void speedtest_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (speedtest_running()) speedtest_stop();
    else                     speedtest_start();
}

static void speedtest_srv_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    speedtest_select_server(idx - 1);   /* row 0 = 自动 = index -1 */
}

static void build_sub_speed(lv_obj_t *t)
{
    t = uk_scroll(t, 0, UI_SUB_VIEW, 4 + 220 + 10 + 20 + ST_SRV_ROWS * UK_ROW_H + 16);

    s_st_card = uk_card(t, UK_MARGIN, 4, UK_CARD_W, 220);
    uk_label(s_st_card, UF.cj12, T->t3, UK_PAD, 10, "网络测速");
    s_st_phase = uk_label_r(s_st_card, UF.cj12, T->t2, UK_CARD_W - UK_PAD, 10, "");
    s_st_live = uk_label(s_st_card, UF.n36, T->t1, UK_PAD, 30, "--");
    s_st_unit = uk_label(s_st_card, UF.n15, T->t3, 100, 50, "Mbps");
    s_st_detail = uk_label_w(s_st_card, UF.cj13, T->t2, UK_PAD, 82, UK_CARD_W - 2 * UK_PAD, 0, "");
    s_st_result = uk_label_w(s_st_card, UF.n15, T->t1, UK_PAD, 104, UK_CARD_W - 2 * UK_PAD, 0, "");
    s_st_server = uk_label_w(s_st_card, UF.cj12, T->t3, UK_PAD, 128, UK_CARD_W - 2 * UK_PAD, 0, "");
    s_st_btn = uk_button(s_st_card, UK_PAD, 160, UK_CARD_W - 2 * UK_PAD, 42, "开始测速", UK_BTN_PRIMARY,
                         speedtest_btn_cb, NULL, &s_st_btn_lbl);
    s_st_offline = uk_label_w(s_st_card, UF.cj13, T->badT, UK_PAD, 190, UK_CARD_W - 2 * UK_PAD, 1, "");
    uk_show(s_st_offline, 0);

    uk_section(t, 4 + 220 + 10, "服务器");
    s_st_srv_card = uk_card(t, UK_MARGIN, 4 + 220 + 10 + 20, UK_CARD_W, ST_SRV_ROWS * UK_ROW_H);
    for (int i = 0; i < ST_SRV_ROWS; i++) {
        lv_obj_t *row = uk_box(s_st_srv_card, 0, i * UK_ROW_H, UK_CARD_W, UK_ROW_H, T->card, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        uk_tappable(row, speedtest_srv_cb, (void *)(intptr_t)i);
        s_st_srv_row[i] = row;
        if (i) uk_sep(row, 0);
        s_st_srv_ok[i] = uk_label(row, &lv_font_montserrat_14, T->accT, UK_PAD, 12, LV_SYMBOL_OK);
        s_st_srv_name[i] = uk_label_w(row, UF.cj14, T->t1, UK_PAD + 22, 11, UK_CARD_W - 2 * UK_PAD - 22, 0, "");
        if (i > 0) uk_show(row, 0);   /* shown once the list arrives */
    }
    lv_label_set_text(s_st_srv_name[0], "自动（最佳服务器）");
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* Every tile now has a real page, so the "not built yet" placeholder body
 * is gone. If a future tile lands before its page does, write the page with
 * an honest description of what it needs (see build_sub_speed) rather than
 * a generic 开发中 box. */

/* ---- 功能 tile wall ---- */
static void tile_click_cb(lv_event_t *e)
{
    sub_open((int)(intptr_t)lv_event_get_user_data(e));
}

static void chill_nav_cb(lv_event_t *e)
{
    sub_open_child((int)(intptr_t)lv_event_get_user_data(e), SUB_CHILL);
}

static void build_func(lv_obj_t *t)
{
    static const char *const k_tile_name[SUB_N] = {
        "WiFi", "\xE7\x9F\xAD\xE4\xBF\xA1" /* 短信 */,
        "\xE4\xBF\xA1\xE4\xBB\xA4\xE8\xAF\xBB\xE5\x8F\x96" /* 信令读取 */,
        "\xE9\x94\x81\xE9\xA2\x91" /* 锁频 */, "\xE6\xB5\x8B\xE9\x80\x9F" /* 测速 */,
        "CHILL", "eSIM", "\xE6\x80\xA7\xE8\x83\xBD\xE6\xB5\x8B\xE8\xAF\x95" /* 性能测试 */,
        "Tailscale",
        "\xE8\x8A\x82\xE7\x82\xB9" /* 节点 */,
        "\xE8\xA7\x84\xE5\x88\x99 \xE2\x86\x92 \xE8\x8A\x82\xE7\x82\xB9" /* 规则 → 节点 */,
    };
    static const char *const k_tile_static[SUB_N] = {
        NULL, NULL,
        "\xE5\xB0\x8F\xE5\x8C\xBA/\xE9\x82\xBB\xE5\x8C\xBA/\xE6\x94\xAF\xE6\x8C\x81\xE9\xA2\x91\xE6\xAE\xB5", /* 小区/邻区/支持频段 */
        NULL,   /* 锁频: refresh_cb writes the live 选网方式 */
        /* 2026-09-22: this used to be a hardcoded "插件未安装" — stale as
         * soon as speedtest.c stopped depending on that plugin. Now NULL,
         * refresh_cb writes a live status same as the other dynamic tiles. */
        NULL,
        NULL, NULL,
        "\xE8\xB0\x83\xE8\xAF\x95\xE9\xA1\xB5", /* 调试页 */
        NULL, NULL, NULL,   /* SUB_TS/NODES/PAIRS: 不在磁贴墙上 */
        NULL,               /* SUB_SMS_DETAIL: 从短信列表点进去 */
        NULL,               /* SUB_ALERTS: 顶栏圆点 / 系统页「健康」 */
    };
    /* Explicit list, not 0..SUB_N: SUB_TS has a subpage but no tile (it is
     * opened from the Tailscale card on Home). */
    static const int k_tiles[] = { SUB_WIFI, SUB_SMS, SUB_CELL, SUB_LOCK,
                                   SUB_SPEED, SUB_CHILL, SUB_ESIM, SUB_PERF };
    static const char *const k_icon[SUB_N] = {
        [SUB_WIFI] = LV_SYMBOL_WIFI, [SUB_SMS] = LV_SYMBOL_ENVELOPE, [SUB_CELL] = LV_SYMBOL_LIST,
        [SUB_LOCK] = LV_SYMBOL_GPS, [SUB_SPEED] = LV_SYMBOL_CHARGE, [SUB_CHILL] = LV_SYMBOL_SHUFFLE,
        [SUB_ESIM] = LV_SYMBOL_SD_CARD, [SUB_PERF] = LV_SYMBOL_SETTINGS,
    };
    int ntile = (int)(sizeof k_tiles / sizeof k_tiles[0]);
    t = uk_scroll(t, 0, UI_VIEW_H, 4 + 4 * 88 + UK_TAB_PAD);
    for (int k = 0; k < ntile; k++) {
        int i = k_tiles[k];
        uk_tile(&s_tile[i], t, UK_MARGIN + (k % 2) * 155, 4 + (k / 2) * 88, k_icon[i], k_tile_name[i],
                tile_click_cb, (void *)(intptr_t)i);
        s_tile_sub[i] = s_tile[i].sub;
        /* 性能测试 is a debug page: its title in t3 says so */
        uk_tile_set(&s_tile[i], 0, i == SUB_PERF, 0);
        if (k_tile_static[i]) lv_label_set_text(s_tile_sub[i], k_tile_static[i]);
    }
}

static void bench_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    s_box_x += s_box_dir * 4;
    if (s_box_x >= 16) { s_box_x = 16; s_box_dir = -1; }
    if (s_box_x <= 0)  { s_box_x = 0;  s_box_dir = 1; }
    lv_obj_set_pos(s_t_box, UK_MARGIN + s_box_x, 150);
    lv_obj_invalidate(s_sub_page[SUB_PERF]);  /* force a full-screen redraw */
    lv_refr_now(NULL);                         /* render it now -> counts a frame */
}

#ifdef DEVUI_PERF_BENCH_ON_START
static void perf_bench_jump_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    lv_tileview_set_tile_by_index(s_tv, TAB_FUNC, 0, LV_ANIM_OFF);
    sub_open(SUB_PERF);
    lv_timer_resume(s_bench_timer);
}
#endif

static void refresh_wifi(const devui_data_t *d)
{
    static char c_ssid[72] = "", c_pass[80] = "", c_enc[40] = "", c_state[24] = "";
    /* "Open" means the *encryption mode* says so. An empty wifi_key does
     * NOT mean open: zwrt-datad's `wlan` section reports ssid/enc/enabled
     * and deliberately never includes the passphrase, so keying off the
     * empty password labelled this sae-mixed network as 开放 and, worse,
     * built a `T:nopass` QR that cannot actually join it. */
    int open = d->wifi_enc[0] == 0 || strstr(d->wifi_enc, "none") != NULL;

    set_label_fmt(s_w_ssid, c_ssid, sizeof c_ssid, "%s", d->wifi_ssid[0] ? d->wifi_ssid : "—");
    set_label_fmt(s_w_pass, c_pass, sizeof c_pass, "%s",
                  d->wifi_key[0] ? d->wifi_key
                  : open          ? "\xE6\x97\xA0\xE5\xAF\x86\xE7\xA0\x81"                   /* 无密码 */
                                  : "\xE5\xAF\x86\xE7\xA0\x81\xE6\x9C\xAA\xE5\x85\xAC\xE5\xBC\x80"); /* 密码未公开 */
    /* The encryption mode as the backend reports it, not a hand-rolled
     * "WPA"/"open" guess — psk2/sae/sae-mixed are meaningfully different
     * and this is the only place in the UI that can tell you which one. */
    set_label_fmt(s_w_enc, c_enc, sizeof c_enc, "%s", open ? "\xE5\xBC\x80\xE6\x94\xBE" /* 开放 */
                                                          : d->wifi_enc);
    set_label_fmt(s_w_state, c_state, sizeof c_state, "%s",
                  d->wifi_enabled ? "\xE5\xB7\xB2\xE5\xBC\x80\xE5\x90\xAF"   /* 已开启 */
                                  : "\xE5\xB7\xB2\xE5\x85\xB3\xE9\x97\xAD"); /* 已关闭 */
    uk_text_color(s_w_state, d->wifi_enabled ? T->okT : T->t3);

    /* Client list — fixed slots, hidden/shown, never created per tick. */
    static char c_cli_n[24] = "";
    static char c_cli_name[WIFI_MAX_CLI][48], c_cli_ip[WIFI_MAX_CLI][28],
                c_cli_mac[WIFI_MAX_CLI][24];
    int n = d->client_n > WIFI_MAX_CLI ? WIFI_MAX_CLI : d->client_n;
    if (d->client_n > WIFI_MAX_CLI)
        set_label_fmt(s_w_cli_n, c_cli_n, sizeof c_cli_n, "%d/%d \xE5\x8F\xB0" /* 台 */, n, d->client_n);
    else
        set_label_fmt(s_w_cli_n, c_cli_n, sizeof c_cli_n, "%d \xE5\x8F\xB0", d->client_n);
    for (int i = 0; i < WIFI_MAX_CLI; i++) {
        if (i >= n) { uk_show(s_w_cli[i], 0); continue; }
        lv_obj_remove_flag(s_w_cli[i], LV_OBJ_FLAG_HIDDEN);
        set_label_fmt(s_w_cli_name[i], c_cli_name[i], sizeof c_cli_name[i], "%s",
                      d->client[i].name[0] ? d->client[i].name : "?");
        set_label_fmt(s_w_cli_ip[i], c_cli_ip[i], sizeof c_cli_ip[i], "%s", d->client[i].ip);
        set_label_fmt(s_w_cli_mac[i], c_cli_mac[i], sizeof c_cli_mac[i], "%s", d->client[i].mac);
    }
    /* Reflow: the card shrinks to the rows shown, DHCP follows. */
    uk_show(s_w_cli_empty, n == 0);
    int cli_h = n ? n * WIFI_CLI_H : UK_ROW_H;
    lv_obj_set_height(s_w_cli_card, cli_h);
    int dy = lv_obj_get_style_y(s_w_cli_card, 0) + cli_h + 10;
    lv_obj_set_y(s_w_dhcp_sec, dy);
    lv_obj_set_y(s_w_dhcp_card, dy + 20);
    uk_scroll_extent(s_w_scroll, dy + 20 + 3 * UK_ROW_H + 16);

    /* DHCP — mirrors htmlmain.c's own summary formatting. */
    static char c_gw[28] = "", c_pool[48] = "", c_lease[24] = "";
    set_label_fmt(s_w_gw, c_gw, sizeof c_gw, "%s", d->dhcp_ip[0] ? d->dhcp_ip : "-");
    set_label_fmt(s_w_pool, c_pool, sizeof c_pool, "%s \xC2\xB7 \xE5\x85\xB1 %s" /* · 共 */,
                  d->dhcp_start[0] ? d->dhcp_start : "-", d->dhcp_limit[0] ? d->dhcp_limit : "-");
    long lt = atol(d->dhcp_leasetime);
    if (lt >= 3600)     set_label_fmt(s_w_lease, c_lease, sizeof c_lease, "%ld \xE5\xB0\x8F\xE6\x97\xB6", lt / 3600);
    else if (lt >= 60)  set_label_fmt(s_w_lease, c_lease, sizeof c_lease, "%ld \xE5\x88\x86\xE9\x92\x9F", lt / 60);
    else if (lt > 0)    set_label_fmt(s_w_lease, c_lease, sizeof c_lease, "%ld \xE7\xA7\x92", lt);
    else                set_label_fmt(s_w_lease, c_lease, sizeof c_lease, "%s", "-");
}

/* ---- refresh ---- */
static void refresh_cb(lv_timer_t *t)
{
    appearance_tick();
    LV_UNUSED(t);
    static devui_data_t d;

    /* perf counters (per-second deltas) — these two legitimately change
     * almost every tick, so the dirty-check below is just a cheap no-op
     * guard here, not a real win — kept for consistency (every refresh_cb
     * label update goes through set_label_fmt, no exceptions to remember). */
    static unsigned long last_f, last_r;
    static char c_fps[16] = "", c_touch[16] = "";
    unsigned long f = g_frame_count, r = touch_input_report_count();
    set_label_fmt(s_t_fps, c_fps, sizeof c_fps, "%lu FPS", f - last_f);
    set_label_fmt(s_t_touch, c_touch, sizeof c_touch, "%lu Hz", r - last_r);
    last_f = f; last_r = r;

    /* Wall clock, independent of the backend — matches htmlmain.c's own
     * status-bar clock (time(NULL) + localtime_r), so it keeps ticking even
     * when zwrt-datad is down. */
    {
        static char c_time[8] = "";
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        set_label_fmt(s_top_time, c_time, sizeof c_time, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    }

    /* zte-agent health: read from the scenario poller (which now polls on
     * every page). The Wi-Fi watchdog keeps Wi-Fi up without the agent; this
     * is only so the owner knows. Runs before the datad check below, so the
     * dot stays right while the data service is down too. */
    /* Polled here, before the datad early-return below: agent_health() is
     * "time since the last successful read", so skipping the read whenever
     * datad is down would turn a datad outage into a false "agent lost". */
    data_set_pace(backlight_panel_lit());
    int sc_changed = scenario_poll(tab_visible(TAB_HOME));
    if (s_xm_off_arm && lv_tick_get() - s_xm_off_arm >= XM_ARM_MS) { s_xm_off_arm = 0; exit_menu_refresh(); }
    if (s_vendor_arm && lv_tick_get() - s_vendor_arm >= 5000) {
        s_vendor_arm = 0;
        uk_button_kind(s_vendor_btn, s_vendor_lbl, UK_BTN_PLAIN);
        lv_label_set_text(s_vendor_lbl, "切换到原厂界面");
    }
    {
        /* 情景卡片上写着 CHILL 的出口，出口变了也要重画 */
        static char last_exit[20];
        if (strcmp(last_exit, chill_exit_raw())) {
            snprintf(last_exit, sizeof last_exit, "%s", chill_exit_raw());
            s_sc_force = 1;
        }
    }
    if (s_sc_force) { s_sc_force = 0; sc_changed = 1; }
    agent_health_t ah;
    agent_health(&ah);
    {
        s_alert_st = ah.lost_secs ? 2 : ah.unread > 0 ? 1 : 0;
    }

    {
        static char c_h[40] = "";
        static uint32_t c_hcol;
        uint32_t col = T->t3;
        if (!ah.checked) {
            set_label_fmt(s_set_health, c_h, sizeof c_h, "%s", "—");
        } else if (ah.bad) {
            col = T->badT;
            set_label_fmt(s_set_health, c_h, sizeof c_h, "■ %d 项异常", ah.bad);
        } else if (ah.warn) {
            col = T->warnT;
            set_label_fmt(s_set_health, c_h, sizeof c_h, "▲ %d 项注意", ah.warn);
        } else {
            col = T->okT;
            set_label_fmt(s_set_health, c_h, sizeof c_h, "%s", "● 一切正常");
        }
        if (col != c_hcol) {
            c_hcol = col;
            uk_text_color(s_set_health, col);
        }
    }

    if (!data_refresh(&d)) {
        /* Backend down is a device-wide condition, so it is reported once in
         * the shared banner instead of overwriting home-page content. */
        banner_set("后台数据服务不可用");
        home_signal_down();
        return;
    }
    if (ah.lost_secs) {
        /* Same banner, lower priority than "data service down" above. */
        char msg[64];
        snprintf(msg, sizeof msg, "管理后台失联 %ld 分钟", ah.lost_secs / 60);
        banner_set(msg);
    } else if (d.bat_temp >= 50) {
        banner_set("电池过热，拔掉充电器放到阴凉处");
    } else if (d.bat_percent <= 10 && !(d.charger_connect && d.chg_uv > 1000000)) {
        banner_set("电量低，尽快充电");
    } else {
        banner_set(NULL);
    }

    /* Top status bar — same signal-strength color tiers as the Home card's
     * dots, reused here for the always-visible summary. */
    {
        static char c_net[16] = "";
        uint32_t sig_col = d.bars <= 1 ? T->red : d.bars <= 2 ? T->orange : T->green;
        static uint32_t c_sig[5];
        for (int i = 0; i < 5; i++) {
            uint32_t col = i < d.bars ? sig_col : T->track;
            if (col != c_sig[i]) { c_sig[i] = col; uk_bg(s_top_sig[i], col); }
        }
        set_label_fmt(s_top_net, c_net, sizeof c_net, "%s", d.net_type);
        char dn[16], up[16], dn2[16], up2[16], full[48], shrt[48], down[24];
        fmt_rate_top(dn, sizeof dn, d.rx_speed, s_cf_speed_bits, 0);
        fmt_rate_top(up, sizeof up, d.tx_speed, s_cf_speed_bits, 0);
        fmt_rate_top(dn2, sizeof dn2, d.rx_speed, s_cf_speed_bits, 1);
        fmt_rate_top(up2, sizeof up2, d.tx_speed, s_cf_speed_bits, 1);
        snprintf(full, sizeof full, "\xE2\x86\x93%s \xE2\x86\x91%s", dn, up);    /* ↓ ↑ */
        snprintf(shrt, sizeof shrt, "\xE2\x86\x93%s \xE2\x86\x91%s", dn2, up2);
        snprintf(down, sizeof down, "\xE2\x86\x93%s", dn2);
        /* Charging = charger_connect only: d.charging is a status code
         * ("2" = discharging on this hardware), truthy almost always. */
        int chg = d.charger_connect && d.chg_uv > 1000000;
        static int c_pct = -1, c_chg = -1;
        if (d.bat_percent != c_pct || chg != c_chg) {
            c_pct = d.bat_percent; c_chg = chg;
            uk_battery_set(&s_top_batt, d.bat_percent, chg);
            uk_show(s_top_batt.body, 1);
            uk_show(s_top_batt.nub, 1);
        }
        statusbar_layout(full, shrt, down, d.bat_percent, chg, s_alert_st != 0, s_alert_st == 2 ? T->badT : T->warnT);
    }

    /* ---- Home: signal card ----
     * Status block: state words (not colour alone), QCI · AMBR, total MHz,
     * RAT, operator · carrier count. Then one row per carrier: the serving
     * cell first from nr_* (its live signal is only there), then every
     * nrca / lteca entry as-is — the serving band may appear again as a
     * -140-floor "inactive" entry, which is what the modem reports. The
     * total counts every carrier reported, active or not (htmlmain.c's
     * total_show_bw). */
    s_ever_valid = 1;
    s_last_valid_wall = time(NULL);
    {
        static char c_rtop[48], c_big[16], c_r1[20], c_r2[96], c_st[48];
        static char c_ca_band[CA_SLOTS][16], c_ca_bw[CA_SLOTS][12], c_ca_rsrp[CA_SLOTS][12],
                    c_ca_sinr[CA_SLOTS][12], c_ca_pci[CA_SLOTS][16], c_ca_arfcn[CA_SLOTS][24],
                    c_ca_ina[CA_SLOTS][24], c_ca_info[CA_SLOTS][40];
        ca_carrier_t ca[CA_SLOTS];
        char pfx[CA_SLOTS];
        int ca_n = 0;
        if (d.nr_rsrp != 0) {
            ca[0].band = 0;   /* band name comes from d.nr_band below */
            ca[0].pci = d.nr_pci;
            ca[0].arfcn = d.nr_channel;
            ca[0].bw = atoi(d.nr_bw);
            ca[0].rsrp = d.nr_rsrp;
            ca[0].rsrq = d.nr_rsrq;
            ca[0].sinr = atof(d.nr_snr[0] ? d.nr_snr : "0");
            ca[0].active = 1;
            pfx[0] = 'n';
            ca_n = 1;
        }
        int nr_extra = parse_ca(d.nrca, ca + ca_n, CA_SLOTS - ca_n);
        for (int i = 0; i < nr_extra; i++) pfx[ca_n + i] = 'n';
        ca_n += nr_extra;
        if (ca_n < CA_SLOTS) {
            int lte_n = parse_ca(d.lteca, ca + ca_n, CA_SLOTS - ca_n);
            for (int i = 0; i < lte_n; i++) pfx[ca_n + i] = 'B';
            ca_n += lte_n;
        }
        int total_bw = 0, nr_cc = 0, lte_cc = 0;
        for (int i = 0; i < ca_n; i++) {
            total_bw += ca[i].bw;
            if (pfx[i] == 'n') nr_cc++; else lte_cc++;
        }
        char cnt[48];
        if (lte_cc && nr_cc) snprintf(cnt, sizeof cnt, "%d LTE + %d NR 载波", lte_cc, nr_cc);
        else if (nr_cc)      snprintf(cnt, sizeof cnt, "%d NR 载波", nr_cc);
        else if (lte_cc)     snprintf(cnt, sizeof cnt, "%d LTE 载波", lte_cc);
        else                 snprintf(cnt, sizeof cnt, "无载波");

        double sinr0 = ca_n ? ca[0].sinr : 0;
        ui_sig_state_t sst = ui_sig_state(1, 1, d.sim_state, d.bars, ca_n > 0, sinr0);
        static uint32_t nosig_since;
        if (sst == UI_SIG_NONE) { if (!nosig_since) nosig_since = lv_tick_get() ? lv_tick_get() : 1; }
        else nosig_since = 0;
        int tone = sst == UI_SIG_GOOD ? 0 : sst == UI_SIG_WEAK ? 1 : 2;
        if (tone != s_cc_tone) { s_cc_tone = tone; uk_hero_tone(&s_cc_hero, tone); }
        const char *hint = "";
        int words = 0;   /* the big field shows words, not a number */
        if (sst == UI_SIG_NOSIM) {
            set_label_fmt(s_cc_hero.st, c_st, sizeof c_st, "%s", "没有 SIM 卡");
            set_label_fmt(s_cc_hero.rtop, c_rtop, sizeof c_rtop, "%s", "");
            set_label_fmt(s_cc_hero.big, c_big, sizeof c_big, "%s", "无 SIM");
            set_label_fmt(s_cc_hero.r1, c_r1, sizeof c_r1, "%s", "");
            set_label_fmt(s_cc_hero.r2, c_r2, sizeof c_r2, "%s", "");
            hint = "插卡，或在「功能 → eSIM」启用一个配置";
            words = 1;
        } else if (sst == UI_SIG_NONE) {
            uint32_t mins = (lv_tick_get() - nosig_since) / 60000;
            set_label_fmt(s_cc_hero.st, c_st, sizeof c_st, "%s", "没有信号");
            if (mins) set_label_fmt(s_cc_hero.rtop, c_rtop, sizeof c_rtop, "已 %u 分钟", (unsigned)mins);
            else      set_label_fmt(s_cc_hero.rtop, c_rtop, sizeof c_rtop, "%s", "刚刚");
            set_label_fmt(s_cc_hero.big, c_big, sizeof c_big, "%s", "无服务");
            set_label_fmt(s_cc_hero.r1, c_r1, sizeof c_r1, "%s", "正在搜网");
            set_label_fmt(s_cc_hero.r2, c_r2, sizeof c_r2, "%s", d.operator_name[0] ? d.operator_name : "SIM 正常");
            hint = "换个位置试试；锁过频就去「锁频」恢复默认配置";
            words = 1;
        } else {
            set_label_fmt(s_cc_hero.st, c_st, sizeof c_st, "%s", sst == UI_SIG_GOOD ? "信号良好" : "信号偏弱");
            set_label_fmt(s_cc_hero.rtop, c_rtop, sizeof c_rtop, "QCI %d · AMBR %d/%d", d.qci, (int)d.ambr_dl, (int)d.ambr_ul);
            set_label_fmt(s_cc_hero.big, c_big, sizeof c_big, "%d", total_bw);
            set_label_fmt(s_cc_hero.r1, c_r1, sizeof c_r1, "%s", d.net_type[0] ? d.net_type : "-");
            set_label_fmt(s_cc_hero.r2, c_r2, sizeof c_r2, "%s · %s", d.operator_name[0] ? d.operator_name : "-", cnt);
        }
        if (words != s_cc_words) {
            s_cc_words = words;
            lv_obj_set_style_text_font(s_cc_hero.big, words ? UF.cj24b : UF.n32, 0);
            lv_obj_set_y(s_cc_hero.big, words ? 34 : 30);
            uk_show(s_cc_hero.unit, !words);
        }
        uk_hero_layout(&s_cc_hero);

        int y = UK_HERO_H;
        uk_show(s_cc_hint, hint[0] != 0);
        if (hint[0]) {
            static char c_hint[96];
            set_label_fmt(s_cc_hint, c_hint, sizeof c_hint, "%s", hint);
            lv_obj_set_y(s_cc_hint, y + 10);
            y += 46;
        }
        for (int i = 0; i < CA_SLOTS; i++) {
            home_ca_t *k = &s_ca[i];
            if (i >= ca_n) { uk_show(k->box, 0); continue; }
            uk_show(k->box, 1);
            uk_show(k->sep, i > 0 || hint[0]);
            int act = ca[i].active;
            char band_s[16];
            if (ca[i].band)         snprintf(band_s, sizeof band_s, "%c%d", pfx[i], ca[i].band);
            else if (pfx[i] == 'n') snprintf(band_s, sizeof band_s, "%s", d.nr_band[0] ? d.nr_band : "-");
            else                    snprintf(band_s, sizeof band_s, "%s", d.band[0] ? d.band : "-");
            const char *fl = pfx[i] == 'n' ? "ARFCN" : "EARFCN";
            lv_obj_t *on[8] = { k->band, k->bw, k->rsrp, k->rsrp_c, k->sinr, k->sinr_c, k->pci, k->arfcn };
            for (int j2 = 0; j2 < 8; j2++) uk_show(on[j2], act);
            uk_show(k->ina, !act); uk_show(k->ina_tag, !act); uk_show(k->ina_info, !act);
            if (act) {
                /* floats through snprintf, never %f in set_label_fmt ([[lvgl-sprintf-no-float]]) */
                char rsrp_s[12], sinr_s[12];
                snprintf(rsrp_s, sizeof rsrp_s, "%.0f", ca[i].rsrp);
                snprintf(sinr_s, sizeof sinr_s, "%.1f", ca[i].sinr);
                set_label_fmt(k->band, c_ca_band[i], sizeof c_ca_band[i], "%s", band_s);
                set_label_fmt(k->bw, c_ca_bw[i], sizeof c_ca_bw[i], "%dM", ca[i].bw);
                lv_obj_update_layout(k->band);
                lv_obj_set_x(k->bw, UK_PAD + 3 + lv_obj_get_width(k->band));
                set_label_fmt(k->rsrp, c_ca_rsrp[i], sizeof c_ca_rsrp[i], "%s", rsrp_s);
                set_label_fmt(k->sinr, c_ca_sinr[i], sizeof c_ca_sinr[i], "%s", sinr_s);
                set_label_fmt(k->pci, c_ca_pci[i], sizeof c_ca_pci[i], "PCI %d", ca[i].pci);
                set_label_fmt(k->arfcn, c_ca_arfcn[i], sizeof c_ca_arfcn[i], "%s %ld", fl, ca[i].arfcn);
                uk_text_color(k->band, T->t1);
                uk_text_color(k->rsrp, T->t1);
                uk_text_color(k->sinr, ca[i].sinr >= 13 ? T->okT : ca[i].sinr >= 0 ? T->warnT : T->badT);
                lv_obj_set_height(k->box, 40);
            } else {
                set_label_fmt(k->ina, c_ca_ina[i], sizeof c_ca_ina[i], "%s %dM", band_s, ca[i].bw);
                set_label_fmt(k->ina_info, c_ca_info[i], sizeof c_ca_info[i], "%s %ld · PCI %d", fl, ca[i].arfcn, ca[i].pci);
                lv_obj_set_height(k->box, 32);
            }
            lv_obj_set_y(k->box, y);
            y += act ? 40 : 32;
        }
        /* 今日/本月：固件（zwrt_data）按日历日/月累计的计数器，不是本次开机的 rx/tx */
        {
            char c_day[32], c_month[32];
            static char c_traf[80] = "";
            fmt_bytes_total(c_day, sizeof c_day, d.day_rx_bytes + d.day_tx_bytes);
            fmt_bytes_total(c_month, sizeof c_month, d.month_rx_bytes + d.month_tx_bytes);
            set_label_fmt(s_cc_traffic, c_traf, sizeof c_traf, "今日 %s · 本月 %s", c_day, c_month);
            lv_obj_set_y(s_cc_tsep, y);
            lv_obj_set_y(s_cc_tkey, y + 10);
            lv_obj_set_y(s_cc_traffic, y + 9);
            uk_text_color(s_cc_traffic, T->t2);
            y += 34;
        }
        lv_obj_set_height(s_cell_card, y);
    }

    /* ---- 情景 (Home) ---- */
    {
        if (sc_changed) {
            static char c_ss[64] = "", c_sn[96] = "";
            scenario_status_t sc;
            scenario_get_status(&sc);
            if (!sc.available) {
                lv_obj_add_flag(s_sc_card, LV_OBJ_FLAG_HIDDEN);
            } else {
                char when[24] = "";
                lv_obj_remove_flag(s_sc_card, LV_OBJ_FLAG_HIDDEN);
                if (!sc.enabled)
                    set_label_fmt(s_sc_state, c_ss, sizeof c_ss, "已停用");
                else
                    set_label_fmt(s_sc_state, c_ss, sizeof c_ss, "%s%s",
                                  sc.name[0] ? sc.name : "判定中",
                                  sc.pin[0] ? " · 已固定" : "");
                uk_text_color(s_sc_state, sc.enabled ? T->t1 : T->t3);
                if (sc.last_switch > 0) {
                    time_t tt = (time_t)sc.last_switch;
                    struct tm tm;
                    localtime_r(&tt, &tm);
                    strftime(when, sizeof when, "%m-%d %H:%M", &tm);
                }
                /* 在家：解释 Wi-Fi 为什么没了；判定中：为什么还没结论；
                 * 其他：上次什么时候切过来的 */
                uint32_t note_col = T->t2;
                if (sc.enabled && !sc.name[0])
                    set_label_fmt(s_sc_note, c_sn, sizeof c_sn, "开机后要连续两次扫描确认位置");
                else if (sc.abroad && sc.chill_on == 1) {
                    /* 在国外：直接写 CHILL 现在怎么走，点卡片改 */
                    note_col = T->accT;
                    set_label_fmt(s_sc_note, c_sn, sizeof c_sn, "CHILL：%s · 点这里改", chill_mode());
                } else if (sc.abroad && sc.chill_on == 0) {
                    note_col = T->accT;
                    set_label_fmt(s_sc_note, c_sn, sizeof c_sn, "%s",
                                  sc.chill_back ? "CHILL 已关 · 回国自动打开 · 点这里改" : "CHILL 已关 · 点这里改");
                } else
                    set_label_fmt(s_sc_note, c_sn, sizeof c_sn, "%s%s%s",
                                  sc.wifi_off ? "Wi-Fi 已关 · " : (when[0] ? "上次切换 " : ""),
                                  when, sc.wifi_off && !when[0] ? "手机走家里网络" : "");
                uk_text_color(s_sc_note, note_col);
            }
            /* CHILL 页的总开关跟着 agent 报的状态走（旧 agent 不报 → 不显示） */
            if (s_cp_sw) {
                if (sc.chill_on < 0) lv_obj_add_flag(s_cp_sw, LV_OBJ_FLAG_HIDDEN);
                else {
                    lv_obj_remove_flag(s_cp_sw, LV_OBJ_FLAG_HIDDEN);
                    sw_apply(s_cp_sw, sc.chill_on);
                }
            }
        }
    }

    /* ---- Tailscale (Home) ---- */
    {
        if (tailscale_poll(tab_visible(TAB_HOME))) {
            tailscale_status_t ts;
            tailscale_get_status(&ts);
            if (!ts.available) {
                lv_obj_add_flag(s_ts_card, LV_OBJ_FLAG_HIDDEN);
            } else {
                const char *state_txt, *note = "";
                uint32_t state_col, dot_col;
                int running = ts.ok && !strcmp(ts.state, "Running");
                lv_obj_remove_flag(s_ts_card, LV_OBJ_FLAG_HIDDEN);
                if (!ts.ok)                                    { state_txt = "未运行";   state_col = T->t3; }
                else if (running)                              { state_txt = ts.self_online ? "已连接" : "离线";
                                                                   state_col = ts.self_online ? T->t1 : T->badT; }
                else if (!strcmp(ts.state, "Starting"))         { state_txt = "连接中";   state_col = T->warnT; }
                else if (!strcmp(ts.state, "NeedsLogin"))       { state_txt = "需要登录"; state_col = T->warnT;
                                                                   note = "在管理网页打开登录链接"; }
                else if (!strcmp(ts.state, "NeedsMachineAuth")) { state_txt = "等待批准"; state_col = T->warnT;
                                                                   note = "在 Tailscale 后台批准这台设备"; }
                else if (!strcmp(ts.state, "Stopped"))          { state_txt = "已停止";   state_col = T->t3; }
                else                                            { state_txt = ts.state[0] ? ts.state : "-"; state_col = T->t3; }
                dot_col = running && ts.self_online ? T->green : state_col == T->warnT ? T->orange : state_col == T->badT ? T->red : T->t3;
                lv_label_set_text(s_ts_val[0], state_txt);
                uk_text_color(s_ts_val[0], state_col);
                uk_bg(s_ts_dot, dot_col);
                lv_obj_update_layout(s_ts_val[0]);
                lv_obj_set_x(s_ts_dot, lv_obj_get_x(s_ts_val[0]) - 12);
                int rows = 1;
                if (running) {
                    lv_label_set_text_fmt(s_ts_val[1], "%s%s%s%s", ts.ip,
                        ts.ip[0] && ts.relay[0] ? " · " : "", ts.relay[0] ? "DERP " : "", ts.relay);
                    if (ts.peers_online == 0)
                        lv_label_set_text(s_ts_val[2], "只有本机在线");
                    else
                        lv_label_set_text_fmt(s_ts_val[2], "在线 %d/%d · 直连 %d · 中继 %d",
                                              ts.peers_online, ts.peers, ts.direct, ts.active - ts.direct);
                    lv_label_set_text(s_ts_val[3], ts.routes[0] ? ts.routes : "-");
                    rows = 4;
                    /* health beats the exit node: it is the actionable one */
                    if (ts.health[0]) {
                        lv_label_set_text(s_ts_key[4], "提示");
                        lv_label_set_text(s_ts_val[4], ts.health);
                        uk_text_color(s_ts_val[4], T->warnT);
                        rows = 5;
                    } else if (ts.exit_node[0]) {
                        lv_label_set_text(s_ts_key[4], "出口节点");
                        lv_label_set_text_fmt(s_ts_val[4], "%s%s", ts.exit_node, ts.exit_online ? "" : " · 离线");
                        uk_text_color(s_ts_val[4], T->t1);
                        rows = 5;
                    }
                } else if (note[0]) {
                    lv_label_set_text(s_ts_key[1], "下一步");
                    lv_label_set_text(s_ts_val[1], note);
                    rows = 2;
                }
                if (!running) lv_label_set_text(s_ts_key[1], "下一步");
                else          lv_label_set_text(s_ts_key[1], "本机");
                for (int i = 1; i < TS_HOME_ROWS; i++) {
                    uk_show(s_ts_key[i], i < rows); uk_show(s_ts_val[i], i < rows); uk_show(s_ts_sep[i], i < rows);
                }
                lv_obj_set_width(s_ts_val[4], LV_SIZE_CONTENT);
                s_ts_rows = rows;
                lv_obj_set_height(s_ts_card, rows * UK_ROW_H);
                /* Subpage: own identity + the peer list. */
                lv_label_set_text(s_tp_self[0], ts.name[0] ? ts.name : "-");
                lv_label_set_text(s_tp_self[1], ts.ip[0] ? ts.ip : "-");
                lv_label_set_text(s_tp_self[2], ts.relay[0] ? ts.relay : "-");
                lv_label_set_text(s_tp_self[3], ts.routes[0] ? ts.routes : "-");
                int pn = tailscale_peer_count();
                if (pn > TS_PEER_MAX) pn = TS_PEER_MAX;
                for (int i = 0; i < TS_PEER_MAX; i++) {
                    if (i >= pn) { uk_show(s_tp_row[i], 0); continue; }
                    tailscale_peer_t pe;
                    tailscale_get_peer(i, &pe);
                    lv_obj_remove_flag(s_tp_row[i], LV_OBJ_FLAG_HIDDEN);
                    lv_label_set_text(s_tp_name[i], pe.name[0] ? pe.name : "-");
                    lv_label_set_text(s_tp_ip[i], pe.ip);
                    /* One tag, most specific first: how it is connected beats
                     * plain online/offline. */
                    const char *tag = pe.active
                        ? (pe.direct ? "\xE7\x9B\xB4\xE8\xBF\x9E" /* 直连 */
                                     : "\xE4\xB8\xAD\xE7\xBB\xA7" /* 中继 */)
                        : (pe.online ? "\xE5\x9C\xA8\xE7\xBA\xBF" /* 在线 */
                                     : "\xE7\xA6\xBB\xE7\xBA\xBF" /* 离线 */);
                    lv_label_set_text(s_tp_tag[i], tag);
                    uk_text_color(s_tp_tag[i], pe.active ? T->okT : pe.online ? T->t2 : T->t3);
                    uk_text_color(s_tp_name[i], pe.online ? T->t1 : T->t3);
                }
                lv_obj_set_height(s_tp_card, (pn ? pn : 1) * TS_PEER_H);
            }
        }
    }

    /* ---- CHILL (Home card + subpage) ---- */
    {
        int on_home = tab_visible(TAB_HOME);
        int on_page = sub_visible(SUB_CHILL) || sub_visible(SUB_CHILL_NODES) ||
                      sub_visible(SUB_CHILL_PAIRS);
        if (chill_poll(on_home || on_page)) {
            static char c_cs[32] = "", c_cr[48] = "", c_csp[32] = "", c_ctt[40] = "", c_cl[160] = "";
            int online = chill_online();
            scenario_status_t csc;
            scenario_get_status(&csc);
            /* Hidden only when there is no CHILL at all; stopped still shows,
             * with the way back (the card opens the page with the switch). */
            uk_show(s_chill_card, online || csc.chill_on == 0);
            if (!online) {
                set_label_fmt(s_chill_state, c_cs, sizeof c_cs, "%s", "已关闭");
                uk_text_color(s_chill_state, T->t3);
                set_label_fmt(s_chill_rate, c_cr, sizeof c_cr, "%s", "—");
                set_label_fmt(s_chill_split, c_csp, sizeof c_csp, "%s", "");
                set_label_fmt(s_chill_line, c_cl, sizeof c_cl, "%s",
                              csc.chill_back ? "在国外关掉了 · 回国自动打开" : "点这里打开");
                set_label_fmt(s_chill_total, c_ctt, sizeof c_ctt, "%s", "");
                s_chill_rows = 0;
            } else {
                set_label_fmt(s_chill_state, c_cs, sizeof c_cs, "%s", chill_core());
                uk_text_color(s_chill_state, T->okT);
                set_label_fmt(s_chill_rate, c_cr, sizeof c_cr, "%s", chill_speed());
                set_label_fmt(s_chill_split, c_csp, sizeof c_csp, "%s", chill_conn_split());
                set_label_fmt(s_chill_total, c_ctt, sizeof c_ctt, "%s", chill_traffic());
                /* 出口 · 节点 · 延迟: the main selector's node, and its last
                 * measured delay when the node list has it. */
                char dl[24] = "";
                for (int i = 0; i < chill_node_count(); i++) {
                    chill_node_info_t ni;
                    chill_get_node(i, &ni);
                    if (ni.selected && !strcmp(ni.name, chill_node())) {
                        if (ni.delay > 0) snprintf(dl, sizeof dl, " · %dms", ni.delay);
                        else if (ni.delay == 0) snprintf(dl, sizeof dl, " · 超时");
                        break;
                    }
                }
                set_label_fmt(s_chill_line, c_cl, sizeof c_cl, "%s%s%s%s", chill_mode(),
                              chill_node()[0] ? " · " : "", chill_node(), dl);
                s_chill_rows = chill_top_pair_count();
            }
            static char c_tpn[CHILL_HOME_ROWS][96], c_tpv[CHILL_HOME_ROWS][40];
            int rows = s_chill_rows > CHILL_HOME_ROWS ? CHILL_HOME_ROWS : s_chill_rows;
            if (rows < 1) rows = 1;
            for (int i = 0; i < CHILL_HOME_ROWS; i++) {
                int vis = i < rows;
                uk_show(s_chill_pair_name[i], vis);
                uk_show(s_chill_pair_val[i], vis);
                uk_show(s_chill_pair_sep[i], vis);
                if (!vis) continue;
                if (i >= s_chill_rows) {   /* up but idle: one placeholder row */
                    set_label_fmt(s_chill_pair_name[i], c_tpn[i], sizeof c_tpn[i], "%s", online ? "暂无活跃连接" : "—");
                    set_label_fmt(s_chill_pair_val[i], c_tpv[i], sizeof c_tpv[i], "%s", "");
                    uk_text_color(s_chill_pair_name[i], T->t3);
                } else {
                    chill_traffic_item_t it;
                    chill_get_top_pair(i, &it);
                    set_label_fmt(s_chill_pair_name[i], c_tpn[i], sizeof c_tpn[i], "%s", it.name);
                    set_label_fmt(s_chill_pair_val[i], c_tpv[i], sizeof c_tpv[i], "%s", it.traffic);
                    uk_text_color(s_chill_pair_name[i], T->t1);
                }
            }
            lv_obj_set_height(s_chill_card, CHILL_HOME_TOP + rows * CHILL_HOME_ROW_H + 2);
            /* Subpage mirrors the same values plus the node list. */
            static char c_pc[24] = "", c_pv[48] = "", c_pt[48] = "";
            set_label_fmt(s_cp_core, c_pc, sizeof c_pc, "%s", chill_core());
            {
                static int c_on = -1;
                if (online != c_on) { c_on = online; uk_hero_tone(&s_cp_hero, online ? 0 : 3); }
                static char c_big[96], c_r1[24], c_r2[80];
                char dl[24] = "-";
                for (int i = 0; i < chill_node_count(); i++) {
                    chill_node_info_t ni;
                    chill_get_node(i, &ni);
                    if (ni.selected && !strcmp(ni.name, chill_node())) {
                        if (ni.delay > 0) snprintf(dl, sizeof dl, "%d ms", ni.delay);
                        else if (ni.delay == 0) snprintf(dl, sizeof dl, "超时");
                        break;
                    }
                }
                set_label_fmt(s_cp_hero.big, c_big, sizeof c_big, "%s", online && chill_node()[0] ? chill_node() : "—");
                set_label_fmt(s_cp_hero.r1, c_r1, sizeof c_r1, "%s", online ? dl : "");
                set_label_fmt(s_cp_hero.r2, c_r2, sizeof c_r2, "%s", online ? chill_group() : "打开右上角开关启动");
            }
            set_label_fmt(s_cp_conns, c_pv, sizeof c_pv, "%s", chill_conn_split());
            set_label_fmt(s_cp_traffic, c_pt, sizeof c_pt, "%s", chill_traffic());
            const char *xraw = chill_exit_raw();
            for (int i = 0; i < 4; i++) uk_opt_set(&s_cp_opt[i], !strcmp(xraw, k_exit_state[i]), 0);
            {
                static char c_xn[48] = "";
                set_label_fmt(s_cp_exit_note, c_xn, sizeof c_xn, "%s",
                              !strcmp(xraw, "direct_all") ? "AI、VoWiFi 也直连" : "");
            }
            {
                const char *pr = chill_profile_raw();
                for (int i = 0; i < 3; i++) uk_opt_set(&s_cp_prof[i], !strcmp(pr, k_profile[i]), 0);
                static char c_pn[48] = "";
                int hot = chill_thermal_eco() && strcmp(chill_profile_effective_raw(), pr);
                set_label_fmt(s_cp_prof_note, c_pn, sizeof c_pn, "%s", hot ? "太热 · 暂按省电" : "进出省电断网约 10 秒");
                uk_text_color(s_cp_prof_note, hot ? T->warnT : T->t3);
            }
            int ng = chill_group_count();
            if (ng > CHILL_MAX_GROUPS) ng = CHILL_MAX_GROUPS;
            static char c_gname[CHILL_MAX_GROUPS][48];
            for (int i = 0; i < CHILL_MAX_GROUPS; i++) {
                if (i >= ng) { uk_show(s_cp_grp_btn[i], 0); continue; }
                chill_group_info_t gi;
                chill_get_group(i, &gi);
                lv_obj_remove_flag(s_cp_grp_btn[i], LV_OBJ_FLAG_HIDDEN);
                set_label_fmt(s_cp_grp_lbl[i], c_gname[i], sizeof c_gname[i], "%s", gi.name);
                uk_chip_set(s_cp_grp_btn[i], s_cp_grp_lbl[i], gi.selected);
                if (gi.auto_pick && !gi.selected) uk_text_color(s_cp_grp_lbl[i], T->t3);
            }

            {
                static int c_ng = -1;
                static char c_gsig[CHILL_MAX_GROUPS * 48];
                char sig[CHILL_MAX_GROUPS * 48] = "";
                for (int i = 0; i < ng; i++) strncat(sig, c_gname[i], sizeof sig - strlen(sig) - 1);
                if (ng != c_ng || strcmp(sig, c_gsig)) {
                    c_ng = ng;
                    snprintf(c_gsig, sizeof c_gsig, "%s", sig);
                    chill_nodes_layout(ng);
                }
            }
            int pending = chill_delay_pending();
            lv_label_set_text(s_cp_delay_lbl, pending
                ? "\xE6\xB5\x8B\xE8\xAF\x95\xE4\xB8\xAD\xE2\x80\xA6" /* 测试中… */
                : "\xE6\xB5\x8B\xE5\xBB\xB6\xE8\xBF\x9F" /* 测延迟 */);
            uk_text_color(s_cp_delay_lbl, pending ? T->t3 : T->accT);

            int nn = chill_node_count();
            if (nn > CHILL_MAX_NODES) nn = CHILL_MAX_NODES;
            static char c_nname[CHILL_MAX_NODES][48], c_ndl[CHILL_MAX_NODES][16];
            for (int i = 0; i < CHILL_MAX_NODES; i++) {
                if (i >= nn) { uk_show(s_cp_node_row[i], 0); continue; }
                chill_node_info_t ni;
                chill_get_node(i, &ni);
                lv_obj_remove_flag(s_cp_node_row[i], LV_OBJ_FLAG_HIDDEN);
                set_label_fmt(s_cp_node_name[i], c_nname[i], sizeof c_nname[i], "%s", ni.name);
                /* chill.c's convention: >0 = ms, 0 = timed out, -1 = not
                 * measured yet. Same 150/400ms tiers the HTML list uses. */
                if (ni.delay > 0)
                    set_label_fmt(s_cp_node_dl[i], c_ndl[i], sizeof c_ndl[i], "%d ms", ni.delay);
                else
                    set_label_fmt(s_cp_node_dl[i], c_ndl[i], sizeof c_ndl[i], "%s",
                                  ni.delay == 0 ? "\xE8\xB6\x85\xE6\x97\xB6" /* 超时 */ : "-");
                uk_text_color(s_cp_node_dl[i], ni.delay == 0 ? T->badT : ni.delay < 0 ? T->t3
                              : ni.delay < 150 ? T->okT : ni.delay < 400 ? T->warnT : T->badT);
                uk_show(s_cp_node_ok[i], ni.selected);
            }
            lv_obj_set_height(s_cp_node_card, (nn ? nn : 1) * UK_ROW_H);
            uk_scroll_extent(s_cp_scroll_nodes, (int)lv_obj_get_style_y(s_cp_node_card, 0) + (nn ? nn : 1) * UK_ROW_H + 16);

            /* 流量分布——数据来自这次已经拉过的 /connections，chill_poll()
             * 内部顺带算好了，这里不额外发请求。行数固定建好，按实际条目数
             * 隐藏/显示；卡片高度按实际行数收缩。一行是一个 (规则, 节点)
             * 组合，不是两张各自独立排名、容易被误读成一一对应的卡
             * （2026-09-22 反馈）。现在自己单独一个二级页，不用再跟着节点卡
             * 的高度重新定位。 */
            int tpn = chill_top_pair_count();
            static char c_tpname[CHILL_TRAF_ROWS][96], c_tpval[CHILL_TRAF_ROWS][40];
            for (int i = 0; i < CHILL_TRAF_ROWS; i++) {
                if (i >= tpn) {
                    lv_obj_add_flag(s_cp_pair_name[i], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(s_cp_pair_val[i], LV_OBJ_FLAG_HIDDEN);
                    continue;
                }
                chill_traffic_item_t it;
                chill_get_top_pair(i, &it);
                lv_obj_remove_flag(s_cp_pair_name[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_cp_pair_val[i], LV_OBJ_FLAG_HIDDEN);
                set_label_fmt(s_cp_pair_name[i], c_tpname[i], sizeof c_tpname[i], "%s", it.name);
                set_label_fmt(s_cp_pair_val[i], c_tpval[i], sizeof c_tpval[i], "%s", it.traffic);
            }
            for (int i = 1; i < CHILL_TRAF_ROWS; i++) uk_show(s_cp_pair_sep[i], i < tpn);
            lv_obj_set_height(s_cp_pair_card, (tpn ? tpn : 1) * UK_ROW_H);

            /* CHILL 页两行摘要——纯计数，不复用 chill_group()/chill_node()
             * 那套"当前配置的节点选择"语义，避免又把"点进去能改什么"和
             * "流量实际去哪了"这两件事混到一起（就是这轮反馈本身在说的
             * 问题）。节点行顺带带上组数，因为这一行现在是策略组+节点
             * 合并页的入口。 */
            static char c_ngc[CHILL_NAV_ROWS][24];
            set_label_fmt(s_cp_nav_val[CHILL_NAV_NODES], c_ngc[CHILL_NAV_NODES],
                          sizeof c_ngc[CHILL_NAV_NODES], "%d \xE7\xBB\x84 \xC2\xB7 %d \xE4\xB8\xAA" /* 组 · 个 */, ng, nn);
            set_label_fmt(s_cp_nav_val[CHILL_NAV_PAIRS], c_ngc[CHILL_NAV_PAIRS],
                          sizeof c_ngc[CHILL_NAV_PAIRS], "%d \xE6\x9D\xA1" /* 条 */, tpn);
        }
    }

    home_reflow();

    /* ---- eSIM subpage ---- */
    {
        if (esim_poll(sub_visible(SUB_ESIM))) {
            int n = esim_profile_count();
            int locked = esim_locked();

            const char *cur = esim_current();
            lv_label_set_text(s_es_cur, cur[0] ? cur : "—");
            lv_label_set_text(s_es_state, esim_state());
            lv_label_set_text(s_es_hero.rtop, "");
            uk_show(s_es_empty, n == 0);
            if (n == 0) {
                const char *st = esim_state();
                lv_label_set_text(s_es_empty, !esim_loaded() && strcmp(st, "就绪") == 0 ? "读取中…"
                                             : strcmp(st, "就绪") ? st : "还没有 eSIM 配置 · 用管理网页添加");
            }
            for (int i = 0; i < ESIM_MAX_ROWS; i++) {
                if (i >= n) { uk_show(s_es_row[i], 0); continue; }
                esim_profile_t p;
                esim_get_profile(i, &p);
                uk_show(s_es_row[i], 1);
                lv_label_set_text(s_es_row_name[i], p.name);
                lv_label_set_text(s_es_row_sub[i], p.sub);
                if (p.enabled) lv_label_set_text(s_es_hero.rtop, p.sub);
                lv_label_set_text(s_es_row_tag[i],
                    p.going ? "切换中…" : p.armed ? "再点一次确认切换" : p.enabled ? "使用中" : "");
                if (locked || p.enabled) lv_obj_remove_flag(s_es_row[i], LV_OBJ_FLAG_CLICKABLE);
                else                     lv_obj_add_flag(s_es_row[i], LV_OBJ_FLAG_CLICKABLE);
                uk_bg(s_es_row[i], p.armed ? T->fillOrange : p.enabled ? T->accS : T->card);
                uint32_t fg = p.armed ? 0xffffff : T->t1;
                uk_text_color(s_es_row_name[i], fg);
                uk_text_color(s_es_row_sub[i], p.armed ? T->onFill : T->t3);
                uk_text_color(s_es_row_tag[i], p.armed ? 0xffffff : p.enabled ? T->accT : T->t2);
            }
            lv_obj_set_height(s_es_list_card, n ? n * ESIM_ROW_H : UK_ROW_H);
        }
    }

    /* ---- Speedtest subpage ---- */
    {
        /* Also polled while just the 功能 tile wall is up (not only the
         * subpage itself) — the tile's own subtitle (功能 tile subtitles,
         * below) needs live data to replace the old hardcoded "插件未安装"
         * text, same as WiFi/SMS/CHILL/eSIM/锁频 already do. */
        if (speedtest_poll(sub_visible(SUB_SPEED) || tab_visible(TAB_FUNC))) {
            int online = speedtest_agent_reachable();
            if (!online) {
                lv_obj_add_flag(s_st_live, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_st_detail, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_st_result, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_st_server, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_st_btn, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_st_offline, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(s_st_offline, UK_PAD, 32);
                uk_show(s_st_unit, 0);
                lv_label_set_text(s_st_phase, "");
                lv_label_set_text(s_st_offline,
                    "连不上测速服务（zte-agent）。\n"
                    "检查 zte-agent 有没有在跑，以及 /data/zte-agent.env\n"
                    "（或 start_zte_agent.sh）里有没有 ZTE_AGENT_PASSWORD。");
            } else {
                lv_obj_remove_flag(s_st_live, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_st_detail, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_st_result, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_st_server, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_st_btn, LV_OBJ_FLAG_HIDDEN);
                uk_show(s_st_unit, 1);

                static char c_ph[24] = "", c_live[32] = "", c_detail[64] = "",
                            c_result[64] = "", c_srv[96] = "";
                speedtest_phase_t ph = speedtest_phase();
                double dl = speedtest_download_mbps(), ul = speedtest_upload_mbps();
                double ping = speedtest_ping_ms(), jitter = speedtest_jitter_ms();

                set_label_fmt(s_st_phase, c_ph, sizeof c_ph, "%s", speedtest_phase_label());

                if (ph == ST_DOWNLOAD || ph == ST_UPLOAD)
                    set_label_fmt(s_st_live, c_live, sizeof c_live, "%.1f", speedtest_live_mbps());
                else if (ph == ST_COMPLETE)
                    set_label_fmt(s_st_live, c_live, sizeof c_live, "%.1f", dl >= 0 ? dl : 0.0);
                else
                    set_label_fmt(s_st_live, c_live, sizeof c_live, "%s", "--");

                if (ping >= 0)
                    set_label_fmt(s_st_detail, c_detail, sizeof c_detail,
                                  "延迟 %.0fms · 抖动 %.0fms", ping, jitter >= 0 ? jitter : 0.0);
                else
                    set_label_fmt(s_st_detail, c_detail, sizeof c_detail, "%s",
                                  ph == ST_IDLE && dl < 0 ? "还没测过" : "");

                if (dl >= 0 || ul >= 0)
                    set_label_fmt(s_st_result, c_result, sizeof c_result,
                                  "\xE2\x86\x93 %.1f Mbps  \xE2\x86\x91 %.1f Mbps",
                                  dl >= 0 ? dl : 0.0, ul >= 0 ? ul : 0.0);
                else
                    set_label_fmt(s_st_result, c_result, sizeof c_result, "%s", "");

                set_label_fmt(s_st_server, c_srv, sizeof c_srv, "%s", speedtest_server());

                int running = speedtest_running();
                lv_label_set_text(s_st_btn_lbl,
                    running ? "停止" : ph == ST_COMPLETE ? "重新测速" : "开始测速");
                uk_bg(s_st_btn, running ? T->fillRed : T->fillBlue);
                lv_obj_update_layout(s_st_live);
                lv_obj_set_x(s_st_unit, UK_PAD + lv_obj_get_width(s_st_live) + 4);

                if (ph == ST_ERROR && speedtest_error()[0]) {
                    lv_obj_remove_flag(s_st_offline, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_pos(s_st_offline, UK_PAD, 206);
                    lv_label_set_text(s_st_offline, speedtest_error());
                } else {
                    lv_obj_add_flag(s_st_offline, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }

        /* 服务器列表：只在二级页真正打开时拉（不像 progress 那样功能磁贴墙
         * 打开也拉——列表要不了那么勤，磁贴副标题不需要它）。 */
        if (speedtest_servers_poll(sub_visible(SUB_SPEED))) {
            int n = speedtest_servers_count();
            static char c_sn[ST_SRV_ROWS][112];
            for (int i = 1; i < ST_SRV_ROWS; i++) {
                if (i > n) { lv_obj_add_flag(s_st_srv_row[i], LV_OBJ_FLAG_HIDDEN); continue; }
                speedtest_server_t s;
                speedtest_get_server(i - 1, &s);
                lv_obj_remove_flag(s_st_srv_row[i], LV_OBJ_FLAG_HIDDEN);
                set_label_fmt(s_st_srv_name[i], c_sn[i], sizeof c_sn[i], "%s \xC2\xB7 %s, %s",
                              s.sponsor, s.name, s.country);
            }
            lv_obj_set_height(s_st_srv_card, ((n > 0 ? n : 0) + 1) * UK_ROW_H);
        }
        {
            int sel = speedtest_selected_index();
            for (int i = 0; i < ST_SRV_ROWS; i++) {
                if (!s_st_srv_row[i]) continue;
                int is_sel = (sel < 0 && i == 0) || (sel >= 0 && i == sel + 1);
                uk_show(s_st_srv_ok[i], is_sel);
            }
        }
    }

    /* ---- System page ---- */
    {
        static char c_sbat[40] = "", c_schg[48] = "", c_scpu[40] = "", c_smem[40] = "", c_sup[32] = "";
        set_label_fmt(s_sy_bat, c_sbat, sizeof c_sbat, "%d%% \xC2\xB7 %d\xC2\xB0""C %d.%02ldV %ldmA",
                      d.bat_percent, d.bat_temp, (int)(d.bat_uv / 1000000),
                      (d.bat_uv / 10000) % 100, d.bat_ua / 1000);
        /* charger_connect alone still reads 1 with nothing plugged in on
         * this hardware (shows 0.03V/0mA), so gate on a real voltage. */
        if (d.chg_uv > 1000000)
            set_label_fmt(s_sy_chg, c_schg, sizeof c_schg, "%d.%02ldV %ldmA",
                          (int)(d.chg_uv / 1000000), (d.chg_uv / 10000) % 100, d.chg_ua / 1000);
        else
            set_label_fmt(s_sy_chg, c_schg, sizeof c_schg, "%s", "\xE6\x9C\xAA\xE6\x8E\xA5\xE5\x85\xA5" /* 未接入 */);
        set_label_fmt(s_sy_cpu, c_scpu, sizeof c_scpu,
                      "\xE5\x8D\xA0\xE7\x94\xA8 %ld%% \xC2\xB7 %ld\xC2\xB0""C", d.cpu_usage, d.cpu_temp);
        set_label_fmt(s_sy_mem, c_smem, sizeof c_smem, "%ld%% \xC2\xB7 %ldM / %ldM",
                      d.mem_used_pct, (d.mem_total - d.mem_avail) / 1048576, d.mem_total / 1048576);
        long up = d.uptime;
        set_label_fmt(s_sy_up, c_sup, sizeof c_sup, "%ldh%02ldm", up / 3600, (up / 60) % 60);

        static char c_ver[40] = "", c_imei[32] = "", c_usb[24] = "", c_fw[96] = "";
        set_label_fmt(s_set_ver, c_ver, sizeof c_ver, "%s", d.sw_version[0] ? d.sw_version : "-");
        if (strlen(d.imei) >= 8) {
            char masked[32];
            size_t len = strlen(d.imei);
            snprintf(masked, sizeof masked, "%.4s********%s", d.imei, d.imei + len - 3);
            set_label_fmt(s_set_imei, c_imei, sizeof c_imei, "%s", masked);
        } else {
            set_label_fmt(s_set_imei, c_imei, sizeof c_imei, "%s", "-");
        }
        set_label_fmt(s_set_usb, c_usb, sizeof c_usb, "%s",
                      !strcmp(d.usb_mode, "debug") ? "\xE8\xB0\x83\xE8\xAF\x95\xE5\xB7\xB2\xE5\xBC\x80"
                      : d.usb_mode[0]              ? "\xE8\xB0\x83\xE8\xAF\x95\xE5\x85\xB3\xE9\x97\xAD"
                                                   : "-");
        set_label_fmt(s_set_fw, c_fw, sizeof c_fw, "%s", d.fw[0] ? d.fw : "-");
    }

    /* ---- charts ----
     * Fed every tick whatever page is showing, so 图表 opens on real history.
     * One point per CHART_STEP ticks: the average of those readings. */
    {
        static long acc_cpu, acc_mem, acc_rx, acc_tx, acc_bat;
        static int acc_n, pts;
        acc_cpu += d.cpu_usage < 0 ? 0 : d.cpu_usage; acc_mem += d.mem_used_pct;
        acc_rx += d.rx_speed; acc_tx += d.tx_speed; acc_bat += d.bat_percent;
        if (++acc_n >= CHART_STEP) {
            lv_chart_set_next_value(s_ch_cpu, s_cs_cpu, (int32_t)(acc_cpu / acc_n));
            lv_chart_set_next_value(s_ch_mem, s_cs_mem, (int32_t)(acc_mem / acc_n));
            lv_chart_set_next_value(s_ch_net, s_cs_rx, rate_scale(acc_rx / acc_n));
            lv_chart_set_next_value(s_ch_net, s_cs_tx, rate_scale(acc_tx / acc_n));
            lv_chart_set_next_value(s_ch_bat, s_cs_bat, (int32_t)(acc_bat / acc_n));
            acc_cpu = acc_mem = acc_rx = acc_tx = acc_bat = 0;
            acc_n = 0;
            if (pts < CHART_READY) {
                pts++;
                for (int i = 0; i < 4; i++) uk_show(s_ch_wait[i], pts < CHART_READY);
            }
        }
        static char c_cpu[16], c_cput[16], c_mem[16], c_mems[32], c_dn[32], c_up[32], c_bat[16], c_bats[40];
        set_label_fmt(s_ch_cpu_v, c_cpu, sizeof c_cpu, "%ld%%", d.cpu_usage < 0 ? 0 : d.cpu_usage);
        set_label_fmt(s_ch_cpu_t, c_cput, sizeof c_cput, "%ld°C", d.cpu_temp);
        set_label_fmt(s_ch_mem_v, c_mem, sizeof c_mem, "%ld%%", d.mem_used_pct);
        set_label_fmt(s_ch_mem_s, c_mems, sizeof c_mems, "%ldM / %ldM",
                      (d.mem_total - d.mem_avail) / 1048576, d.mem_total / 1048576);
        char dn2[32], up2[32];
        fmt_rate(dn2, sizeof dn2, d.rx_speed);
        fmt_rate(up2, sizeof up2, d.tx_speed);
        set_label_fmt(s_ch_net_dn, c_dn, sizeof c_dn, "↓ %s", dn2);
        set_label_fmt(s_ch_net_up, c_up, sizeof c_up, "↑ %s", up2);
        lv_obj_update_layout(s_ch_net_up);
        lv_obj_align(s_ch_net_dn, LV_ALIGN_TOP_RIGHT, -(12 + lv_obj_get_width(s_ch_net_up) + 8), 8);
        int chg = d.charger_connect && d.chg_uv > 1000000;
        set_label_fmt(s_ch_bat_v, c_bat, sizeof c_bat, "%d%%", d.bat_percent);
        set_label_fmt(s_ch_bat_s, c_bats, sizeof c_bats, "%s · %d°C", chg ? "充电中" : "放电中", d.bat_temp);
    }

    /* ---- SMS subpage ---- */
    {
        static char c_num[SMS_MAX_ROWS][48], c_date[SMS_MAX_ROWS][24], c_body[SMS_MAX_ROWS][160];
        static char c_cnt[40];
        int n = d.sms_n > SMS_MAX_ROWS ? SMS_MAX_ROWS : d.sms_n, unread = 0;
        for (int i = 0; i < d.sms_n; i++) unread += d.sms[i].unread ? 1 : 0;
        if (d.sms_n == 0) set_label_fmt(s_sms_count, c_cnt, sizeof c_cnt, "%s", "");
        else if (unread) set_label_fmt(s_sms_count, c_cnt, sizeof c_cnt, "%d 条未读 · 共 %d 条", unread, d.sms_n);
        else set_label_fmt(s_sms_count, c_cnt, sizeof c_cnt, "共 %d 条，都已读", d.sms_n);
        if (unread) lv_obj_remove_flag(s_sms_allread_btn, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(s_sms_allread_btn, LV_OBJ_FLAG_HIDDEN);
        uk_show(s_sms_empty, d.sms_n == 0);
        uk_show(s_sms_list, n > 0);
        if (n) lv_obj_set_height(s_sms_list, n * SMS_ROW_H);
        for (int i = 0; i < SMS_MAX_ROWS; i++) {
            if (i >= n) {
                s_sms_row_id[i] = -1;
                uk_show(s_sms_row[i], 0);
                continue;
            }
            char pv[150];
            s_sms_row_id[i] = d.sms[i].id;
            lv_obj_remove_flag(s_sms_row[i], LV_OBJ_FLAG_HIDDEN);
            set_label_fmt(s_sms_num[i], c_num[i], sizeof c_num[i], "%s", d.sms[i].num);
            set_label_fmt(s_sms_date[i], c_date[i], sizeof c_date[i], "%s", d.sms[i].date);
            /* Only two lines show; cut on a UTF-8 boundary so a Chinese
             * character is never split into a broken glyph (snprintf alone
             * cuts bytes). The label adds the "…". */
            utf8_prefix(pv, sizeof pv, d.sms[i].text);
            set_label_fmt(s_sms_body[i], c_body[i], sizeof c_body[i], "%s", pv);
            if (d.sms[i].unread) lv_obj_remove_flag(s_sms_dot[i], LV_OBJ_FLAG_HIDDEN);
            else                 lv_obj_add_flag(s_sms_dot[i], LV_OBJ_FLAG_HIDDEN);
            uk_bg(s_sms_row[i], sms_delete_armed(i) ? T->washB : T->card);
        }
    }

    /* ---- SMS detail subpage ---- */
    if (s_sub_cur == SUB_SMS_DETAIL) {
        static char c_dn[48], c_dd[24], c_del[32];
        static char c_dbody[DEVUI_SMS_TEXT_MAX];
        int k = -1;
        for (int i = 0; i < d.sms_n; i++)
            if (d.sms[i].id == s_smsd_id) { k = i; break; }
        if (k < 0) {
            /* Deleted (here or elsewhere) while open: nothing left to show. */
            s_smsd_id = -1;
            sub_back();
        } else {
            set_label_fmt(s_smsd_num, c_dn, sizeof c_dn, "%s", d.sms[k].num);
            set_label_fmt(s_smsd_date, c_dd, sizeof c_dd, "%s", d.sms[k].date);
            if (strcmp(c_dbody, d.sms[k].text)) {   /* full text: too big for set_label_fmt */
                snprintf(c_dbody, sizeof c_dbody, "%s", d.sms[k].text);
                lv_label_set_text(s_smsd_body, c_dbody);
            }
            int armed = s_smsd_del_arm && lv_tick_get() - s_smsd_del_arm < 4000;
            if (!armed) s_smsd_del_arm = 0;
            set_label_fmt(s_smsd_del_lbl, c_del, sizeof c_del, "%s", armed ? "再点一次确认删除" : "删除这条");
            uk_button_kind(s_smsd_del_btn, s_smsd_del_lbl, armed ? UK_BTN_ARMED : UK_BTN_DANGER);
        }
    }

    /* ---- Alerts subpage ---- */
    if (alerts_poll(sub_visible(SUB_ALERTS))) {
        static char c_ac[48], c_al[ALERTS_MAX][96], c_at[ALERTS_MAX][24], c_ax[ALERTS_MAX][128];
        int n = alerts_count(), un = alerts_unread();
        const char *err = alerts_error();
        if (err[0]) set_label_fmt(s_al_count, c_ac, sizeof c_ac, "%s", err);
        else if (!n) set_label_fmt(s_al_count, c_ac, sizeof c_ac, "%s", "");
        else if (un) set_label_fmt(s_al_count, c_ac, sizeof c_ac, "%d 条未读", un);
        else set_label_fmt(s_al_count, c_ac, sizeof c_ac, "%s", "都已读");
        if (un && !err[0]) lv_obj_remove_flag(s_al_allread_btn, LV_OBJ_FLAG_HIDDEN);
        else               lv_obj_add_flag(s_al_allread_btn, LV_OBJ_FLAG_HIDDEN);
        if (!n && !err[0]) {
            lv_label_set_text(s_al_empty, "● 一切正常，没有告警。程序崩溃、Wi-Fi 被看门狗打开这类事会记在这里。");
            lv_obj_remove_flag(s_al_empty, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_al_empty, LV_OBJ_FLAG_HIDDEN);
        }
        uk_show(s_al_list, n > 0);
        if (n) lv_obj_set_height(s_al_list, (n > ALERTS_MAX ? ALERTS_MAX : n) * AL_ROW_H);
        for (int i = 0; i < ALERTS_MAX; i++) {
            alert_item_t a;
            char when[40];
            if (i >= n) { uk_show(s_al_row[i], 0); continue; }
            alerts_get(i, &a);
            lv_obj_remove_flag(s_al_row[i], LV_OBJ_FLAG_HIDDEN);
            if (a.time > 0) {
                /* Device clock = local wall time under TZ=UTC: localtime gives the right digits. */
                time_t tt = (time_t)a.time;
                struct tm tm;
                localtime_r(&tt, &tm);
                strftime(when, sizeof when, "%m-%d %H:%M", &tm);
            } else {
                snprintf(when, sizeof when, "开机后%ld分", a.uptime / 60);
            }
            set_label_fmt(s_al_label[i], c_al[i], sizeof c_al[i], "%s", a.label);
            set_label_fmt(s_al_time[i], c_at[i], sizeof c_at[i], "%s", when);
            set_label_fmt(s_al_text[i], c_ax[i], sizeof c_ax[i], "%s", a.text);
            if (a.unread) lv_obj_remove_flag(s_al_mark[i], LV_OBJ_FLAG_HIDDEN);
            else          lv_obj_add_flag(s_al_mark[i], LV_OBJ_FLAG_HIDDEN);
            uk_text_color(s_al_label[i], a.unread ? T->t1 : T->t2);
        }
    }

    /* ---- 信令读取 subpage ---- */
    {
        static char c_sg[6][64], c_sgl[80], c_sgn[4][48], c_nrb[160], c_lteb[200];
        set_label_fmt(s_sg_nr[0], c_sg[0], sizeof c_sg[0], "%s  %s MHz",
                      d.nr_band[0] ? d.nr_band : "-", d.nr_bw[0] ? d.nr_bw : "-");
        set_label_fmt(s_sg_nr[1], c_sg[1], sizeof c_sg[1], "%ld", d.nr_channel);
        set_label_fmt(s_sg_nr[2], c_sg[2], sizeof c_sg[2], "%d", d.nr_pci);
        set_label_fmt(s_sg_nr[3], c_sg[3], sizeof c_sg[3], "%ld", d.nr_cell_id);
        set_label_fmt(s_sg_nr[4], c_sg[4], sizeof c_sg[4], "%d-%02d %s",
                      d.mcc, d.mnc, d.operator_name);
        set_label_fmt(s_sg_nr[5], c_sg[5], sizeof c_sg[5], "%d / %d / %s",
                      d.nr_rsrp, d.nr_rsrq, d.nr_snr[0] ? d.nr_snr : "-");
        if (d.lte_rsrp != 0)
            set_label_fmt(s_sg_lte, c_sgl, sizeof c_sgl, "RSRP %d  RSRQ %d  SINR %s  RSSI %d",
                          d.lte_rsrp, d.lte_rsrq, d.lte_snr[0] ? d.lte_snr : "-", d.lte_rssi);
        else
            set_label_fmt(s_sg_lte, c_sgl, sizeof c_sgl, "%s",
                          "\xE6\x9C\xAA\xE8\x81\x9A\xE5\x90\x88" /* 未聚合 */);
        set_label_fmt(s_sg_net[0], c_sgn[0], sizeof c_sgn[0], "%s",
                      d.net_select[0] ? d.net_select : "-");
        set_label_fmt(s_sg_net[1], c_sgn[1], sizeof c_sgn[1], "%s",
                      d.wan_status[0] ? d.wan_status : "-");
        set_label_fmt(s_sg_net[2], c_sgn[2], sizeof c_sgn[2], "%s", d.net_type);
        set_label_fmt(s_sg_net[3], c_sgn[3], sizeof c_sgn[3], "%s",
                      d.hsr ? "\xE5\xBC\x80\xE5\x90\xAF" : "\xE5\x85\xB3\xE9\x97\xAD");
        char nrf[160], ltef[200];
        fmt_band_list(nrf, sizeof nrf, d.sa_bands, 'n');
        fmt_band_list(ltef, sizeof ltef, d.lte_bands, 'B');
        set_label_fmt(s_sg_nrb, c_nrb, sizeof c_nrb, "%s", nrf[0] ? nrf : "-");
        set_label_fmt(s_sg_lteb, c_lteb, sizeof c_lteb, "%s", ltef[0] ? ltef : "-");
    }

    /* ---- 锁频 subpage ---- */
    band_group_sync(BG_SA,  d.sa_bands);
    band_group_sync(BG_NSA, d.nsa_bands);
    band_group_sync(BG_LTE, d.lte_bands);
    /* Highlight whichever 选网方式 the modem is actually on. Skipped while a
     * tap is armed so the orange "confirm?" state isn't repainted away by
     * the next refresh tick. */
    if (s_lk_mode_pending < 0) {
        static const char *const k_mode_v[4] = { "WL_AND_5G", "Only_5G", "LTE_AND_5G", "Only_LTE" };
        int sel = -1;
        for (int i = 0; i < 4; i++) if (!strcmp(d.net_select, k_mode_v[i])) sel = i;
        if (sel != s_lk_seg.sel) uk_seg_set(&s_lk_seg, sel);
    } else if (lv_tick_get() - s_lk_mode_arm >= 5000) {
        s_lk_mode_pending = -1;          /* confirm window lapsed */
        s_lk_seg.sel = -2;               /* repaint the real selection next tick */
        lv_label_set_text(s_lk_mode_lbl, "切换会短暂断网，需要按两次确认");
    }

    /* ---- 功能 tile subtitles ---- */
    {
        static char c_t0[40] = "", c_t1[40] = "", c_t5[40] = "", c_t6[40] = "";
        /* 「开着」(WiFi up, CHILL running) = green dot + green subtitle; the
         * SMS tile carries the unread count as a badge. */
        {
            static int c_on[3] = { -1, -1, -1 };
            int wifi_on = s_aux_w24 == 1 || s_aux_w5 == 1 || (s_aux_w24 < 0 && s_aux_w5 < 0 && d.wifi_enabled);
            int chill_on = chill_online();
            int unread = d.sms_unread;
            if (wifi_on != c_on[0]) { c_on[0] = wifi_on; uk_tile_set(&s_tile[SUB_WIFI], wifi_on, 0, 0); }
            if (chill_on != c_on[1]) { c_on[1] = chill_on; uk_tile_set(&s_tile[SUB_CHILL], chill_on, 0, 0); }
            if (unread != c_on[2]) { c_on[2] = unread; uk_tile_set(&s_tile[SUB_SMS], 0, 0, unread); }
        }
        set_label_fmt(s_tile_sub[SUB_WIFI], c_t0, sizeof c_t0, "%s \xC2\xB7 %d \xE5\x8F\xB0",
                      d.wifi_ssid[0] ? d.wifi_ssid : "-", d.client_n);
        if (d.sms_unread)
            set_label_fmt(s_tile_sub[SUB_SMS], c_t1, sizeof c_t1,
                          "%d \xE6\x9D\xA1 \xC2\xB7 %d \xE6\x9C\xAA\xE8\xAF\xBB", d.sms_n, d.sms_unread);
        else
            set_label_fmt(s_tile_sub[SUB_SMS], c_t1, sizeof c_t1, "%d \xE6\x9D\xA1", d.sms_n);
        set_label_fmt(s_tile_sub[SUB_CHILL], c_t5, sizeof c_t5, "%s \xC2\xB7 %s",
                      chill_core(), chill_mode());
        set_label_fmt(s_tile_sub[SUB_ESIM], c_t6, sizeof c_t6, "%s", esim_current());
        static char c_t3[40] = "";
        set_label_fmt(s_tile_sub[SUB_LOCK], c_t3, sizeof c_t3, "%s",
                      d.net_select[0] ? d.net_select : "-");
        static char c_t4[40] = "";
        if (!speedtest_agent_reachable())
            set_label_fmt(s_tile_sub[SUB_SPEED], c_t4, sizeof c_t4, "%s",
                          "\xE6\x9C\x8D\xE5\x8A\xA1\xE4\xB8\x8D\xE5\x8F\xAF\xE7\x94\xA8" /* 服务不可用 */);
        else if (speedtest_running())
            set_label_fmt(s_tile_sub[SUB_SPEED], c_t4, sizeof c_t4, "%s",
                          speedtest_phase_label());
        else if (speedtest_phase() == ST_COMPLETE)
            set_label_fmt(s_tile_sub[SUB_SPEED], c_t4, sizeof c_t4,
                          "\xE2\x86\x93 %.0f \xC2\xB7 \xE2\x86\x91 %.0f Mbps",
                          speedtest_download_mbps() >= 0 ? speedtest_download_mbps() : 0.0,
                          speedtest_upload_mbps() >= 0 ? speedtest_upload_mbps() : 0.0);
        else
            set_label_fmt(s_tile_sub[SUB_SPEED], c_t4, sizeof c_t4, "%s",
                          "\xE7\x82\xB9\xE5\x87\xBB\xE6\xB5\x8B\xE9\x80\x9F" /* 点击测速 */);
    }

    /* ---- WiFi subpage ---- */
    aux_refresh(sub_visible(SUB_WIFI) || tab_visible(TAB_SYS));
    refresh_wifi(&d);
    {
        static char c_wsw[5][32];
        int st[5] = { (s_aux_w24 == 1 || s_aux_w5 == 1), s_aux_w24 == 1, s_aux_w5 == 1,
                      s_aux_psm == 1, d.nfc_switch };
        for (int i = 0; i < 5; i++) {
            sw_apply(s_w_sw[i], st[i]);
            set_label_fmt(s_w_sw_st[i], c_wsw[i], sizeof c_wsw[i], "%s",
                          st[i] ? "\xE5\xB7\xB2\xE5\xBC\x80\xE5\x90\xAF" : "\xE5\xB7\xB2\xE5\x85\xB3\xE9\x97\xAD");
        }
        if (s_aux_pool[0]) {
            static char c_pool2[48] = "";
            set_label_fmt(s_w_pool, c_pool2, sizeof c_pool2, "%s", s_aux_pool);
        }
        static char c_dps[40] = "";
        sw_apply(s_sy_dps_sw, s_aux_dps == 1);
        set_label_fmt(s_sy_dps_st, c_dps, sizeof c_dps, "%s",
                      s_aux_dps < 0 ? "—"
                      : s_aux_dps   ? "\xE5\xB7\xB2\xE5\xBC\x80\xE5\x90\xAF"
                                    : "\xE5\xB7\xB2\xE5\x85\xB3\xE9\x97\xAD");
        /* Reflects whichever of {this switch, the topbar tap} was touched
         * last — both just write s_cf_speed_bits, this only redraws it. */
        sw_apply(s_sy_speedunit_sw, s_cf_speed_bits);
        lv_label_set_text(s_sy_speedunit_st, s_cf_speed_bits ? "Mbps" : "MB/s");
    }
}

/* ---- power menu (long press on the power key): a bottom sheet ---- */
static uk_sheet_t s_pw_sheet;
static void power_menu_set(int v)
{
    uk_sheet_show(&s_pw_sheet, v);
    update_tabs();
}
static int power_menu_visible(void) { return uk_sheet_visible(&s_pw_sheet); }
static int any_sheet_open(void)
{
    return power_menu_visible() || (s_xm && !lv_obj_has_flag(s_xm, LV_OBJ_FLAG_HIDDEN));
}

static void act_poweroff(lv_event_t *e) { LV_UNUSED(e); system("poweroff"); }
static void act_reboot(lv_event_t *e)   { LV_UNUSED(e); system("reboot"); }
static void act_cancel(lv_event_t *e)   { LV_UNUSED(e); power_menu_set(0); }

static void build_power_menu(void)
{
    uk_sheet(&s_pw_sheet, 120, act_cancel);
    s_power_menu = s_pw_sheet.scrim;
    lv_obj_t *t = uk_label(s_pw_sheet.panel, UF.cj13, T->t3, 0, 0, "电源");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 12);
    uk_sheet_item(&s_pw_sheet, 38, "关机", T->badT, act_poweroff, NULL, NULL);
    uk_sheet_item(&s_pw_sheet, 78, "重启", T->accT, act_reboot, NULL, NULL);
}

static void key_poll_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    int ev = key_input_poll(&s_key, lv_tick_get());
    if (ev != KEY_EV_NONE) {
        /* The key is not an LVGL input device, so a press does not reset the
         * inactivity timer. Without this, waking an auto-slept screen with the
         * key turned it on and the auto-off below turned it straight back off
         * in the same pass — a one-frame flash (2026-09-23, owner report). */
        lv_display_trigger_activity(NULL);
    }
    if (ev == KEY_EV_SHORT) {
        /* Decide from the real brightness, not our remembered state: the vendor
         * key daemon (zte_topsw_key) also sees the power key, and if anything
         * else changed the backlight our flag would make this toggle the wrong
         * way and the press would seem to do nothing. */
        if (backlight_is_lit()) backlight_off();
        else { backlight_on(); ui_tgate_woke(&s_tgate, lv_tick_get()); }
        s_auto_slept = 0;
    } else if (ev == KEY_EV_LONG) {
        if (!backlight_is_lit()) ui_tgate_woke(&s_tgate, lv_tick_get());
        backlight_on();
        s_auto_slept = 0;
        power_menu_set(!power_menu_visible());
    }

    /* Auto screen-off after inactivity. Waking is the key, or a double tap
     * (ui_touch_filter); touches on a dark screen never reach the UI, so they
     * do not count as activity here either. */
    if (s_autooff_ms) {
        uint32_t idle = lv_display_get_inactive_time(NULL);
        /* Exec'd with the screen off (theme switch): inactivity restarted at
         * zero with the new process, which would read as "just touched" and
         * wake the screen. Wait for a touch that happened after the start. */
        if (s_wake_guard && idle + 50 < lv_tick_elaps(s_wake_idle_last)) s_wake_guard = 0;
        if (s_wake_guard) return;
        if (idle > s_autooff_ms) {
            if (backlight_is_on()) { backlight_off(); s_auto_slept = 1; }
        } else if (s_auto_slept) {   /* activity with the screen dark: only the key gets here */
            backlight_on();
            ui_tgate_woke(&s_tgate, lv_tick_get());
            s_auto_slept = 0;
        }
    }
}

/* ---- shared chrome: floating tab capsule ----
 * Four top-level pages, text only (the icon row read as guesses). The
 * capsule floats over the page (glass, rim, soft shadow; no real blur: it is
 * in every frame). It hides while a subpage or a sheet is open: subpages have
 * their own ‹ back button, top-left, and sheets cover the bottom. */
static const char *k_tab_names[UI_TABS] = { "首页", "图表", "功能", "系统" };
static lv_obj_t *s_tab_bar, *s_tab_pill[UI_TABS];

static int any_sheet_open(void);

static void update_tabs(void)
{
    /* sub_close() runs once during ui_create() before build_tabbar(): the
     * tab objects don't exist yet (a NULL deref here once killed the UI on
     * start, 2026-09-21). */
    if (!s_tab_bar) return;
    lv_obj_t *act = lv_tileview_get_tile_active(s_tv);
    for (int i = 0; i < UI_TABS; i++) {
        int on = s_tiles[i] == act;
        uk_show(s_tab_pill[i], on);
        lv_obj_set_style_text_font(s_tabs[i], on ? UF.cj15b : UF.cj13, 0);
        uk_text_color(s_tabs[i], on ? T->accT : T->t2);
    }
    uk_show(s_tab_bar, s_sub_cur < 0 && !any_sheet_open());
}

static void tab_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    sub_close();
    lv_tileview_set_tile_by_index(s_tv, idx, 0, LV_ANIM_OFF);
    update_tabs();
}

static void build_tabbar(void)
{
    const int w = UK_W - 2 * UK_MARGIN, cw = (w - 8) / UI_TABS;
    lv_obj_t *t = uk_box(lv_layer_top(), UK_MARGIN, UK_H - UK_TAB_GAP - UK_TAB_H, w, UK_TAB_H, T->glass, 22);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_border_color(t, lv_color_black(), 0);
    lv_obj_set_style_border_opa(t, T->rim_opa, 0);
    lv_obj_set_style_shadow_width(t, 16, 0);
    lv_obj_set_style_shadow_offset_y(t, 6, 0);
    lv_obj_set_style_shadow_color(t, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(t, 36, 0);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);   /* taps between cells don't fall through to the page */
    if (T->hl_opa) {
        lv_obj_t *hl = uk_box(t, 20, 0, w - 40, 1, 0xffffff, 0);
        lv_obj_set_style_bg_opa(hl, T->hl_opa, 0);
    }
    s_tab_bar = t;
    for (int i = 0; i < UI_TABS; i++) {
        lv_obj_t *cell = uk_box(t, 4 + i * cw, 4, cw, 36, T->accS, 18);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        uk_tappable(cell, tab_click_cb, (void *)(intptr_t)i);
        s_tab_pill[i] = uk_box(cell, 0, 0, cw, 36, T->accS, 18);
        lv_obj_set_style_border_width(s_tab_pill[i], 1, 0);
        lv_obj_set_style_border_color(s_tab_pill[i], lv_color_black(), 0);
        lv_obj_set_style_border_opa(s_tab_pill[i], T->rim_opa, 0);
        s_tabs[i] = uk_label(cell, UF.cj13, T->t2, 0, 0, k_tab_names[i]);
        lv_obj_center(s_tabs[i]);
    }
    update_tabs();
}

/* ---- shared chrome: status bar (26 px, straight on the canvas) ----
 * time · 5 signal dots · RAT · rate [tap: Mbps/MB/s] · ▲ [tap: alerts] · battery.
 * The rate's right edge follows the battery (whose width depends on % and
 * charging) and the ▲; when it does not fit it drops to a shorter form
 * (ui_rate_pick). */
#define TOP_BATT_XR 308
#define TOP_LEFT_END 132   /* time + dots + RAT end here; the rate never crosses it */

static void build_statusbar(void)
{
    lv_obj_t *bar = uk_box(lv_layer_top(), 0, 0, UK_W, UK_BAR_H, T->bg, 0);
    s_top_time = uk_label(bar, UF.n15, T->t1, 12, 4, "--:--");
    for (int i = 0; i < 5; i++) s_top_sig[i] = uk_dot(bar, 60 + i * 8, 11, 5, T->track);
    s_top_net = uk_label(bar, UF.n12, T->t1, 104, 6, "");
    lv_obj_set_width(s_top_net, TOP_LEFT_END - 104 - 2);
    lv_label_set_long_mode(s_top_net, LV_LABEL_LONG_MODE_CLIP);
    s_top_updown = uk_label(bar, UF.n12, T->t2, 200, 6, "");
    lv_obj_add_flag(s_top_updown, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_top_updown, 10);   /* reaches down to ~40 px */
    lv_obj_add_event_cb(s_top_updown, topbar_speed_unit_cb, LV_EVENT_CLICKED, NULL);
    s_top_alert = uk_label(bar, UF.cj12, T->warnT, 260, 5, "▲");
    lv_obj_add_flag(s_top_alert, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_top_alert, 14);
    lv_obj_add_event_cb(s_top_alert, open_alerts_cb, LV_EVENT_CLICKED, NULL);
    uk_show(s_top_alert, 0);
    uk_battery(&s_top_batt, bar, TOP_BATT_XR, 6);
    uk_battery_set(&s_top_batt, 0, 0);
    uk_show(s_top_batt.body, 0);   /* no fake "0" before the first snapshot */
    uk_show(s_top_batt.nub, 0);
}

static int text_w(const char *t, const lv_font_t *f)
{
    lv_point_t sz;
    lv_text_get_size(&sz, t, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

/* Place the rate and ▲ between the RAT and the battery. */
static void statusbar_layout(const char *full, const char *shrt, const char *down, int pct, int chg, int alert, uint32_t alert_col)
{
    static char shown[48];
    int right = TOP_BATT_XR - uk_battery_w(pct, chg) - 6;
    uk_show(s_top_alert, alert);
    if (alert) {
        uk_text_color(s_top_alert, alert_col);
        int aw = text_w("▲", UF.cj12);
        lv_obj_set_x(s_top_alert, right - aw);
        right -= aw + 4;
    }
    const char *f[3] = { full, shrt, down };
    int w[3];
    for (int i = 0; i < 3; i++) w[i] = text_w(f[i], UF.n12);
    int k = ui_rate_pick(w, right - TOP_LEFT_END);
    if (strcmp(shown, f[k])) {
        snprintf(shown, sizeof shown, "%s", f[k]);
        lv_label_set_text(s_top_updown, shown);
    }
    lv_obj_set_x(s_top_updown, right - w[k]);
}

/* ---- shared chrome: status banner (data service down / agent lost) ---- */
static void banner_set(const char *txt)
{
    static char shown[96] = "";
    if (!s_banner) return;
    if (txt) {
        if (strcmp(shown, txt)) {
            snprintf(shown, sizeof shown, "%s", txt);
            lv_label_set_text(s_banner_txt, txt);
        }
    }
    uk_show(s_banner, txt != NULL);
}

static void build_banner(void)
{
    s_banner = uk_box(lv_layer_top(), UK_MARGIN, UK_H - UK_TAB_GAP - UK_TAB_H - 8 - 30, UK_CARD_W, 30, T->washB, 15);
    lv_obj_set_style_border_width(s_banner, 1, 0);
    lv_obj_set_style_border_color(s_banner, lv_color_hex(T->badT), 0);
    lv_obj_set_style_border_opa(s_banner, 90, 0);
    s_banner_txt = uk_label(s_banner, UF.cj13, T->badT, 0, 0, "");
    lv_obj_center(s_banner_txt);
    uk_show(s_banner, 0);
}

static void tv_changed_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    update_tabs();
    /* The benchmark burns a full redraw per millisecond, so it only runs
     * while its own subpage is open. tv_changed_cb fires on page changes;
     * sub_open()/sub_close() call bench_gate() for the subpage case. */
    bench_gate();
}

int ui_key_fd(void) { return s_key.fd; }

int ui_touch_filter(int pressed, int x, int y)
{
    int f = ui_tgate_step(&s_tgate, backlight_is_on(), s_auto_slept && s_autooff_ms, pressed, x, y, (long)lv_tick_get());
    if (f & UI_TG_WAKE) {
        backlight_on();
        s_auto_slept = 0;
        s_wake_guard = 0;
        lv_display_trigger_activity(NULL);
    }
    return (f & UI_TG_FORWARD) != 0;
}

/* U60_DEVUI_TAPLOG helper: where the pieces a tap should hit actually are. */
void ui_debug_tap(void)
{
    lv_area_t b, t, sc;
    lv_obj_t *sys = tab_scroller(TAB_SYS);
    lv_obj_get_coords(s_ap_btn[1], &b);
    lv_obj_get_coords(s_tiles[TAB_SYS], &t);
    if (sys) lv_obj_get_coords(sys, &sc);
    fprintf(stderr, "tap:   深色 [%d,%d %d,%d] sys-tile [%d,%d %d,%d] tv scroll_x=%d act=%d sys-scroll y=%d [%d,%d] sub=%d hidden=%d\n",
            (int)b.x1, (int)b.y1, (int)b.x2, (int)b.y2, (int)t.x1, (int)t.y1, (int)t.x2, (int)t.y2,
            (int)lv_obj_get_scroll_x(s_tv), cur_tab(), sys ? (int)lv_obj_get_scroll_y(sys) : -1,
            sys ? (int)sc.x1 : -1, sys ? (int)sc.y1 : -1, s_sub_cur, lv_obj_has_flag(s_sub_layer, LV_OBJ_FLAG_HIDDEN));
    lv_obj_t *hit = lv_indev_get_active_obj();
    fprintf(stderr, "tap:   hit=%p tv=%p tile3=%p sysscroll=%p sublayer=%p\n", (void *)hit, (void *)s_tv,
            (void *)s_tiles[TAB_SYS], (void *)sys, (void *)s_sub_layer);
    for (lv_obj_t *o = s_ap_btn[1]; o; o = lv_obj_get_parent(o))
        fprintf(stderr, "tap:   chain %p clickable=%d hidden=%d scroll=%d,%d\n", (void *)o,
                lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE), lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN),
                (int)lv_obj_get_scroll_x(o), (int)lv_obj_get_scroll_y(o));
}

void ui_idle(int dark)
{
    if (!s_key_timer) return;
    if (dark) lv_timer_pause(s_key_timer);
    else      lv_timer_resume(s_key_timer);
}

void ui_set_launch(int argc, char **argv)
{
    if (argc > 0 && argv[0] && argv[0][0]) s_argv0 = argv[0];
    ui_launch_parse(argc, argv, &s_launch);
}

void ui_create(void)
{
    /* Settings and theme before the first object: the first frame is already
     * in the right colours, with no flash of the other theme. */
    load_devui_conf();
    s_dark = appear_resolve(s_cf_appear);
    ui_theme_select(s_dark);
    if (s_cf_theme != ui_legacy_theme_value(s_dark)) {
        s_cf_theme = ui_legacy_theme_value(s_dark);   /* litehtml fallback shows the same theme */
        save_devui_conf();
    }
    backlight_init();
    if (s_launch.autooff_ms >= 0) s_autooff_ms = (uint32_t)s_launch.autooff_ms;
    if (s_launch.bright > 0) backlight_remember(s_launch.bright);
    else if (!backlight_panel_lit() && s_cf_bright > 0) backlight_remember(s_cf_bright);   /* started dark: wake to the saved level, not max */
    if (s_launch.screen_off && !backlight_panel_lit()) {
        s_auto_slept = s_launch.screen_off == 1 && s_autooff_ms != 0;
        s_wake_guard = 1;
        s_wake_idle_last = lv_tick_get();
    }
    fprintf(stderr, "ui: appearance=%s dark=%d tab=%d screen_off=%d exec_n=%d\n",
            ui_appear_name(s_cf_appear), s_dark, s_launch.tab, s_launch.screen_off, s_launch.hist.count);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(T->bg), 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(T->t1), 0);

    /* Fonts come from the device (CJK) and the plugin's fonts/ dir (Nunito);
     * the repo ships no proprietary font. */
    ui_fonts_load();

    s_tv = lv_tileview_create(scr);
    /* Leave room at the top for the fixed status bar (built later, on
     * lv_layer_top()) — lv_tileview_create() defaults to 100% of the
     * screen, which would otherwise draw every page's content starting at
     * y=0, right under the status bar. */
    lv_obj_set_size(s_tv, UI_W, UI_H - UI_TOPBAR_H);
    lv_obj_align(s_tv, LV_ALIGN_TOP_MID, 0, UI_TOPBAR_H);
    lv_obj_set_style_bg_color(s_tv, lv_color_hex(T->bg), 0);
    lv_obj_set_style_bg_opa(s_tv, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);

    /* No lv_theme_*_init() is called anywhere in this project, so LV_STYLE_BG_OPA
     * defaults to transparent (lv_style.c) on every tile. Left alone, each tile
     * only repaints the pixels its own widgets happen to cover — any gap (T29:
     * eSIM's card doesn't span the full 320px width) shows through whatever the
     * previous tile last drew there via the DRM partial-dirty-rect flush. Force
     * every tile opaque here, once, so no future page can reintroduce this. */
    for (int i = 0; i < UI_TABS; i++) {
        s_tiles[i] = lv_tileview_add_tile(s_tv, i, 0, LV_DIR_HOR);
        lv_obj_set_style_bg_color(s_tiles[i], lv_color_hex(T->bg), 0);
        lv_obj_set_style_bg_opa(s_tiles[i], LV_OPA_COVER, 0);
    }
    build_home(s_tiles[TAB_HOME]);
    build_charts(s_tiles[TAB_CHART]);
    build_func(s_tiles[TAB_FUNC]);
    build_system(s_tiles[TAB_SYS]);
    lv_obj_add_event_cb(s_tv, tv_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Subpage layer: a sibling of the tileview, created after it so it paints
     * on top, but still below lv_layer_top()'s status bar and tab bar. */
    s_sub_layer = lv_obj_create(scr);
    lv_obj_remove_style_all(s_sub_layer);
    lv_obj_remove_flag(s_sub_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_sub_layer, UI_W, UI_H - UI_TOPBAR_H);
    lv_obj_align(s_sub_layer, LV_ALIGN_TOP_LEFT, 0, UI_TOPBAR_H);
    lv_obj_set_style_bg_color(s_sub_layer, lv_color_hex(T->bg), 0);
    lv_obj_set_style_bg_opa(s_sub_layer, LV_OPA_COVER, 0);
    /* Subpage header: solid bg (content scrolls under it, not through it),
     * 1 px separator, a round ‹ back button and the title in 17 px bold. */
    {
        lv_obj_t *hdr = uk_box(s_sub_layer, 0, 0, UI_W, UI_SUB_HDR, T->bg, 0);
        uk_box(hdr, 0, UI_SUB_HDR - 1, UI_W, 1, T->sep, 0);
        lv_obj_t *bk = uk_box(hdr, 10, 3, 30, 30, T->glass, 15);
        lv_obj_set_style_border_width(bk, 1, 0);
        lv_obj_set_style_border_color(bk, lv_color_black(), 0);
        lv_obj_set_style_border_opa(bk, T->rim_opa, 0);
        lv_obj_t *c = uk_label(bk, UF.cj17b, T->t1, 0, 0, "‹");
        lv_obj_align(c, LV_ALIGN_CENTER, -1, -1);
        uk_tappable(bk, sub_back_cb, NULL);
        lv_obj_set_ext_click_area(bk, 10);
        s_sub_title = uk_label(hdr, UF.cj17b, T->t1, 48, 6, "");
    }
    for (int i = 0; i < SUB_N; i++) {
        lv_obj_t *pg = lv_obj_create(s_sub_layer);
        lv_obj_remove_style_all(pg);
        lv_obj_remove_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(pg, UI_W, UI_H - UI_TOPBAR_H - UI_SUB_HDR);
        lv_obj_align(pg, LV_ALIGN_TOP_LEFT, 0, UI_SUB_HDR);
        lv_obj_add_flag(pg, LV_OBJ_FLAG_HIDDEN);
        s_sub_page[i] = pg;
    }
    build_sub_wifi(s_sub_page[SUB_WIFI]);
    build_sub_sms(s_sub_page[SUB_SMS]);
    build_sub_cell(s_sub_page[SUB_CELL]);
    build_sub_lock(s_sub_page[SUB_LOCK]);
    build_sub_speed(s_sub_page[SUB_SPEED]);
    build_sub_ts(s_sub_page[SUB_TS]);
    build_sub_chill(s_sub_page[SUB_CHILL]);
    build_sub_chill_nodes(s_sub_page[SUB_CHILL_NODES]);
    build_sub_chill_pairs(s_sub_page[SUB_CHILL_PAIRS]);
    build_sub_esim(s_sub_page[SUB_ESIM]);
    build_sub_perf(s_sub_page[SUB_PERF]);
    build_sub_sms_detail(s_sub_page[SUB_SMS_DETAIL]);
    build_sub_alerts(s_sub_page[SUB_ALERTS]);
    sub_close();

    /* Seed the tileview's "active tile" pointer. lv_tileview_add_tile()
     * never sets it — `tile_act` stays NULL until the first
     * lv_tileview_set_tile*() call or the first scroll-end
     * (lv_tileview.c:92,181). So on a fresh start, while Home is plainly
     * the visible page, lv_tileview_get_tile_active() returns NULL and
     * every `== s_tiles[0]` visibility gate in refresh_cb reads false.
     * That is why the Tailscale card stayed empty on a freshly started UI
     * and only filled in after swiping to another page and back: its
     * poller is gated on exactly that comparison. */
    /* Lay everything out first: switching tile on a tileview whose tiles have
     * no size yet shows the right page but leaves its scroll state stale, and
     * taps on that page's content then go nowhere until the next tab tap
     * (seen on the device after a theme exec with --tab=3). */
    lv_obj_update_layout(scr);
    lv_tileview_set_tile_by_index(s_tv, s_launch.tab > 0 ? s_launch.tab : 0, 0, LV_ANIM_OFF);
    if (s_launch.scroll_y > 0) {
        lv_obj_t *sc = tab_scroller(s_launch.tab > 0 ? s_launch.tab : 0);
        if (sc) { lv_obj_update_layout(sc); lv_obj_scroll_to_y(sc, s_launch.scroll_y, LV_ANIM_OFF); }
    }

    /* benchmark driver — paused until the test page is shown */
    s_bench_timer = lv_timer_create(bench_cb, 1, NULL);
    lv_timer_pause(s_bench_timer);
#ifdef DEVUI_PERF_BENCH_ON_START
    /* One-shot, fired after the first normal refresh cycle so the tileview's
     * layout/scroll state is valid before we jump to a non-zero tile. */
    lv_timer_t *jump = lv_timer_create(perf_bench_jump_cb, 500, NULL);
    lv_timer_set_repeat_count(jump, 1);
#endif

    /* shared chrome (top layer): icon tab bar + global status banner */
    build_statusbar();
    build_tabbar();
    build_banner();

    lv_timer_create(refresh_cb, 1000, NULL);
    refresh_cb(NULL);
    fprintf(stderr, "ui: first refresh done\n");
    fflush(stderr);

    /* Startup breadcrumbs: stderr goes to the launcher's log file, which for
     * the side-by-side test lives under /data (not /tmp — /tmp is a RAM disk
     * and a crash-triggered reboot erases exactly the log that would explain
     * the crash; learned the hard way on 2026-09-21). */
    fprintf(stderr, "ui: pages+chrome built\n");
    fflush(stderr);

    build_power_menu();
    build_exit_menu();
    key_input_init(&s_key);
    s_key_timer = lv_timer_create(key_poll_cb, 50, NULL);
}
