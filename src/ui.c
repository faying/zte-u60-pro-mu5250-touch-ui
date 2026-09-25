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
#include "tailscale.h"
#include "scenario.h"
#include "esim.h"
#include "speedtest.h"
#include "alerts.h"
#include "netinfo.h"
#include "ui_logic.h"
#include "ui_theme.h"
#include "ui_exec.h"
#include "battery_est.h"
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
 * previous flat 6-tab layout had no room for 短信/信令读取/锁频/测速 and
 * had invented a "网络" page that only duplicated Home's cellular card. */
/* 2026-09-25 起按「我想做什么」分 5 个标签（docs/designs/touch-menu-tabs.md，
 * 在 manager 仓库）：每个功能只有一个家。原「图表」并进系统，原「功能」磁贴墙
 * 拆到蜂窝 / 出口 / 系统，Wi-Fi 页升为标签。 */
#define UI_TABS 5
enum { TAB_HOME, TAB_CELL, TAB_WIFI, TAB_EXIT, TAB_SYS };
/* Subpages: opened from a tab's › rows, drawn over the tileview on the
 * active screen (so the shared top bar and tab bar, which live on
 * lv_layer_top(), still paint above them). */
enum { SUB_SMS, SUB_CELL, SUB_LOCK, SUB_SPEED, SUB_ESIM, SUB_PERF,
       SUB_TS,   /* not on the tile wall: opened by tapping the Home Tailscale card */
       /* Not on the tile wall: one message in full, opened from the SMS list
        * (sub_open_child, so 返回 goes back to the list). */
       SUB_SMS_DETAIL,
       /* Not on the tile wall: opened from the status bar's alert dot or the
        * 系统 page's 健康 row. */
       SUB_ALERTS,
       /* 网络：出口 IP/运营商/手动选网/邻区（netinfo.c）。放在最后，前面的
        * 数组都按下标顺序填，插在中间会把磁贴名字整体错位。 */
       SUB_NET,
       /* 2026-09-25：情景单独一页（只管 Wi-Fi、Tailscale 这些，不碰蜂窝），
        * APN 在蜂窝标签下 */
       SUB_SCENE, SUB_APN,
       /* 健康与告警里点一行：体检项或告警的全文（列表里只放得下一行） */
       SUB_ALERT_DETAIL,
       SUB_N };

/* ---- shared widget handles ---- */
/* Home page: signal card (status block + carrier rows + traffic), 情景,
 * Tailscale — stacked, reflowed every refresh (home_reflow). */
/* 5 carrier slots: 3 NR + 2 LTE covers EN-DC on this modem with headroom. */
#define CA_SLOTS 5
typedef struct {
    lv_obj_t *box, *band, *bw, *rsrp, *rsrp_c, *sinr, *sinr_c, *pci, *arfcn;   /* active: 40 px   */
    lv_obj_t *ina, *ina_tag, *ina_info;                                        /* inactive: 32 px */
    lv_obj_t *sep;
} home_ca_t;
static home_ca_t s_ca[CA_SLOTS];
static lv_obj_t *s_home_scroll, *s_cell_card, *s_cc_hint;
static uk_hero_t s_cc_hero;
/* 首页层级（2026-09-24 评审：Wi-Fi 用户视角 + 设计视角，用户选「结论优先 +
 * 双磁贴」）：拿起设备先要知道「能不能上网、好不好、会不会多花钱」，所以
 * 状态卡的大字是一句结论，不是聚合带宽；下面三行是 Wi-Fi 和设备数、流量、
 * 载波摘要（聚合带宽降到这里，点它滚到载波明细）。下面是情景磁贴和
 * Tailscale 卡。 */
typedef struct { lv_obj_t *box, *key, *val; } home_row_t;
static home_row_t s_hr_wifi, s_hr_traf, s_hr_ca, s_hr_exit;
/* 载波、出口两行各带一行小字（2026-09-25 home-net-card.md）：
 * 载波 = 基站配了几条 / 在用几条 + 下行、上行各用哪条；
 * 出口 = 流量从哪出去的一句话，有另一条路时小字补上 */
static lv_obj_t *s_hr_ca_sub, *s_hr_exit_sub;
static lv_obj_t *s_ch_net_card;                 /* 网速图（首页，原图表页第一张） */
static lv_obj_t *s_cell_scroll, *s_cell_rest;  /* 蜂窝标签：载波卡在上，其余跟在下面 */
static void cell_reflow(void);
static lv_obj_t *s_ca_card, *s_ca_qos;          /* 载波明细卡 */
#define CA_CARD_TOP 34
#define HOME_TILE_W 145
#define HOME_TILE_H 92
/* 情景 — zte-agent 情景引擎的当前判定，只读。 */
#define SC_CARD_H 72
/* 网络：注册运营商 + 漫游（datad）、出口 IP 和归属地（zte-agent 缓存）。
 * 点开「情景 · 网络」页的网络部分。 */
static lv_obj_t *s_nh_card, *s_nh_ip, *s_nh_geo, *s_nh_tsrow, *s_nh_tsval;
static void nh_card_cb(lv_event_t *e);
static lv_obj_t *s_sc_card, *s_sc_state, *s_sc_note;
static int s_sc_force;          /* 情景卡片要按新状态重画 */
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
static lv_obj_t *s_net_scroll, *s_net_err;
/* 网络各块挂在哪一页（build_sub_net 的注释） */
enum { NH_SCENE, NH_EXIT, NH_OPER, NH_CELL, NH_WIFI, NH_N };
static const int k_net_host[6] = { NH_SCENE, NH_EXIT, NH_OPER, NH_OPER, NH_CELL, NH_WIFI };
static lv_obj_t *s_nh_scroll[NH_N];
static int       s_nh_base[NH_N];              /* 那一页里网络部分从哪开始 */
static void net_relayout(void);
static lv_obj_t *s_exit_nav_sec, *s_exit_nav_card;

/* 标签页上「›」行右边的状态字（原功能磁贴的副标题） */
static lv_obj_t *s_tile_sub[SUB_N];
/* WiFi page */
#define WIFI_MAX_CLI 5    /* fixed sub-card slots; backend reports up to 16 */
static lv_obj_t *s_w_ssid, *s_w_pass, *s_w_enc, *s_w_state;
static lv_obj_t *s_w_cli_card, *s_w_cli_n, *s_w_dhcp_card, *s_w_gw, *s_w_pool, *s_w_lease;
static lv_obj_t *s_w_cli[WIFI_MAX_CLI], *s_w_cli_name[WIFI_MAX_CLI],
                *s_w_cli_ip[WIFI_MAX_CLI], *s_w_cli_mac[WIFI_MAX_CLI];
static lv_obj_t *s_w_cli_tot[8];   /* 连上以来的总流量（netinfo） */
static lv_obj_t *s_w_sw[5], *s_w_sw_st[5], *s_w_scroll, *s_w_cli_sec, *s_w_dhcp_sec, *s_w_cli_empty;
static lv_obj_t *s_w_cli_sep[WIFI_MAX_CLI];
/* eSIM page */
#define ESIM_MAX_ROWS 5   /* fits one screen without scrolling; backend caps at 16 */
static lv_obj_t *s_es_cur, *s_es_state, *s_es_list_card, *s_es_empty, *s_es_row_sep[ESIM_MAX_ROWS];
static uk_hero_t s_es_hero;
/* 卡信息（实体 SIM 和 eSIM 都有）：号码 / ICCID / IMSI */
static lv_obj_t *s_es_info[3], *s_es_list_sec;
static struct {
    ui_sim_kind_t kind;
    char oper[48], iccid[24], imsi[20], msisdn[24];
} s_sim;
static lv_obj_t *s_es_row[ESIM_MAX_ROWS], *s_es_row_name[ESIM_MAX_ROWS],
                *s_es_row_sub[ESIM_MAX_ROWS], *s_es_row_tag[ESIM_MAX_ROWS];
/* System page */
static lv_obj_t *s_set_bright, *s_set_bright_v, *s_vendor_btn, *s_vendor_lbl;
static uk_seg_t  s_off_seg, s_ap_seg;
static uint32_t  s_vendor_arm;
static lv_obj_t *s_set_ver, *s_set_imei, *s_set_usb, *s_set_fw, *s_set_health;
static lv_obj_t *s_sy_bat, *s_sy_est, *s_sy_chg, *s_sy_cpu, *s_sy_mem, *s_sy_up;
static lv_obj_t *s_sy_dps_sw, *s_sy_dps_st;
static lv_obj_t *s_sy_speedunit_sw, *s_sy_speedunit_st;
/* Tailscale subpage */
static lv_obj_t *s_tp_sep[TS_PEER_MAX];
#define TS_SELF_ROWS 8
static lv_obj_t *s_tp_self[TS_SELF_ROWS], *s_tp_card, *s_tp_row[TS_PEER_MAX],
                *s_tp_name[TS_PEER_MAX], *s_tp_ip[TS_PEER_MAX], *s_tp_tag[TS_PEER_MAX], *s_tp_link[TS_PEER_MAX];
/* 信令读取 subpage */
static lv_obj_t *s_sg_nr[6], *s_sg_lt[6], *s_sg_lte_sec, *s_sg_nr_sec, *s_sg_net[4], *s_sg_nrb, *s_sg_lteb;
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
 * sub_open_child() for drill-down pages (短信详情 etc.). One level deep
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
static char s_top_label[32];   /* 顶栏制式：信号卡算出来的 5G-A / 5G+ / 4G+ … */
/* Alerts subpage */
static lv_obj_t   *s_al_count, *s_al_allread_btn, *s_al_empty, *s_al_body, *s_al_scroll;
static lv_obj_t   *s_ald_title, *s_ald_meta, *s_ald_body, *s_ald_scroll;
static lv_obj_t   *s_hc_card, *s_hc_row[HEALTH_MAX], *s_hc_mark[HEALTH_MAX], *s_hc_label[HEALTH_MAX],
                  *s_hc_detail[HEALTH_MAX], *s_hc_none;
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

static lv_obj_t *obj_scroller(lv_obj_t *t);
static lv_obj_t *tab_scroller(int tab) { return obj_scroller(s_tiles[tab]); }

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
    if (speedtest_running()) return 1;
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
static void tile_click_cb(lv_event_t *e);   /* › rows and Home cards: open a subpage */
static void tab_go_cb(lv_event_t *e);       /* Home summary rows: jump to a tab */
static void open_alerts_cb(lv_event_t *e);  /* status-bar alert dot, 系统 page's 健康 row */
static void sc_card_cb(lv_event_t *e);      /* Home 情景 card: opens the 情景 page */
static void tab_go(int idx);
static void bench_gate(void);
static void update_tabs(void);

/* The perf page's animation driver only runs while that page is open. */
static void bench_gate(void)
{
    if (!s_bench_timer) return;
    if (s_sub_cur == SUB_PERF) lv_timer_resume(s_bench_timer);
    else                       lv_timer_pause(s_bench_timer);
}

/* The page's scroll body (uk_scroll), or NULL for a page that doesn't scroll. */
static lv_obj_t *obj_scroller(lv_obj_t *t)
{
    for (uint32_t i = 0; t && i < lv_obj_get_child_count(t); i++) {
        lv_obj_t *c = lv_obj_get_child(t, (int32_t)i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_SCROLLABLE)) return c;
    }
    return NULL;
}

/* Show a subpage as it was left (返回 from a child page lands here). */
static void sub_show(int id)
{
    /* 标题 = 入口上的字。按下标写，插页不会错位。 */
    static const char *const k_sub_title[SUB_N] = {
        [SUB_SMS] = "短信", [SUB_CELL] = "小区信息", [SUB_LOCK] = "锁频",
        [SUB_SPEED] = "测速", [SUB_ESIM] = "SIM 与 eSIM",
        [SUB_PERF] = "性能测试", [SUB_TS] = "Tailscale", [SUB_SMS_DETAIL] = "短信详情",
        [SUB_ALERTS] = "健康与告警", [SUB_NET] = "运营商选择", [SUB_SCENE] = "情景", [SUB_APN] = "APN",
        [SUB_ALERT_DETAIL] = "详情",
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

/* Open a subpage fresh: always from its top (2026-09-25: a page reopened
 * half-way down read as a different page). */
static void sub_open(int id)
{
    if (id < 0 || id >= SUB_N) return;
    sub_show(id);
    lv_obj_t *sc = obj_scroller(s_sub_page[id]);
    if (sc) lv_obj_scroll_to_y(sc, 0, LV_ANIM_OFF);
}

static void sub_close(void)
{
    lv_obj_add_flag(s_sub_layer, LV_OBJ_FLAG_HIDDEN);
    s_sub_cur = -1;
    s_sub_parent = -1;
    bench_gate();
    update_tabs();
}

/* Open a page one level below a top-level subpage (短信详情, 告警详情 and
 * similar drill-downs). 返回 from here goes back to `parent`, not all the way out —
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
    if (s_sub_parent >= 0) sub_show(s_sub_parent);
    else                   sub_close();
}
static void sub_back_cb(lv_event_t *e) { LV_UNUSED(e); sub_back(); }

/* Visibility predicates for the pollers (Tailscale/eSIM only talk to
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

/* ---- Home: 信号 / 邻区 / Tailscale ----
 * Mirrors ui/01-signal.html section for section. The headline is the
 * aggregate line ("5G SA · 3 NR 载波 · 240 MHz"): mode, carrier count and
 * TOTAL aggregated bandwidth. An earlier pass showed only the primary
 * carrier ("SA · n78 · 100") and the user immediately flagged the page as
 * having no focal point — the number people actually look for is how much
 * spectrum is currently aggregated, not which band the PCC happens to be. */
static void home_reflow(void);

#define NET_EXIT_ROW_H 50
/* One 40 px row inside the status card; the box carries its labels so the
 * whole row moves with one lv_obj_set_y. cb = tappable with a chevron. */
static void home_row(home_row_t *r, lv_obj_t *c, const char *key, lv_event_cb_t cb, void *user)
{
    r->box = uk_box(c, 0, 0, UK_CARD_W, UK_ROW_H, T->card, 0);
    lv_obj_set_style_bg_opa(r->box, LV_OPA_TRANSP, 0);
    uk_sep(r->box, 0);
    r->key = uk_label(r->box, UF.cj14, T->t2, UK_PAD, 11, key);
    r->val = uk_label_r(r->box, UF.n15, T->t1, UK_CARD_W - UK_PAD - (cb ? 16 : 0), 10, "");
    if (cb) {
        uk_chevron(r->box, 0);
        uk_tappable(r->box, cb, user);
    }
}

/* 磁贴底部的说明：最多两行，放不下末尾「…」（节点名可以很长） */
static lv_obj_t *home_tile_note(lv_obj_t *tile)
{
    lv_obj_t *l = uk_label_w(tile, UF.cj12, T->t2, 12, 52, HOME_TILE_W - 24, 1, "");
    lv_obj_set_height(l, 34);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    return l;
}

/* 「国家 省 市」只留国家和最后一段（城市）；两段以内原样 */
static const char *geo_short(const char *geo, char *out, size_t n)
{
    const char *first_sp = strchr(geo, ' ');
    const char *last_sp = strrchr(geo, ' ');
    if (!first_sp || first_sp == last_sp) { snprintf(out, n, "%s", geo); return out; }
    snprintf(out, n, "%.*s%s", (int)(first_sp - geo), geo, last_sp);
    return out;
}

static int sim_usable_ui(const char *st) { return !st || !*st || strstr(st, "ready") != NULL; }


static void build_home(lv_obj_t *t)
{
    /* Worst case (5 active carriers, 5 Tailscale rows, …) is
     * ~1010; the spacer is moved by home_reflow to the real height. */
    t = s_home_scroll = uk_scroll(t, 0, UI_VIEW_H, 1100);

    /* status card: the conclusion, then Wi-Fi · traffic · carrier summary */
    s_cell_card = uk_card(t, UK_MARGIN, 4, UK_CARD_W, UK_HERO_H + 4 * UK_ROW_H);
    lv_obj_t *c = s_cell_card;
    uk_hero(&s_cc_hero, c, UF.cj24b);
    uk_show(s_cc_hero.unit, 0);
    s_cc_hint = uk_label_w(c, UF.cj13, T->t2, UK_PAD, UK_HERO_H + 10, UK_CARD_W - 2 * UK_PAD, 1, "");
    /* 顶行（运营商 · 制式 · 本地/漫游）名字可能很长：限宽，末尾「…」 */
    lv_obj_set_size(s_cc_hero.st, UK_CARD_W - 26 - UK_PAD, 18);
    lv_label_set_long_mode(s_cc_hero.st, LV_LABEL_LONG_MODE_DOTS);
    /* 摘要行（2026-09-25 按任务分标签）：蜂窝 / Wi-Fi / 出口 各一行，点了跳到
     * 那个标签；流量只读，没有 ›、没有按下态。 */
    home_row(&s_hr_ca, c, "载波", tab_go_cb, (void *)(intptr_t)TAB_CELL);
    home_row(&s_hr_wifi, c, "Wi-Fi", tab_go_cb, (void *)(intptr_t)TAB_WIFI);
    home_row(&s_hr_exit, c, "出口", tab_go_cb, (void *)(intptr_t)TAB_EXIT);
    home_row(&s_hr_traf, c, "流量", NULL, NULL);
    s_hr_ca_sub = uk_label_w(s_hr_ca.box, UF.cj12, T->t3, UK_PAD, 34, UK_CARD_W - 2 * UK_PAD - 16, 0, "");
    s_hr_exit_sub = uk_label_w(s_hr_exit.box, UF.cj12, T->t3, UK_PAD, 34, UK_CARD_W - 2 * UK_PAD - 16, 0, "");
    lv_obj_t *subs[2] = { s_hr_ca_sub, s_hr_exit_sub };
    for (int i = 0; i < 2; i++) {
        lv_obj_set_height(subs[i], 18);
        lv_label_set_long_mode(subs[i], LV_LABEL_LONG_MODE_DOTS);
    }
    lv_obj_set_y(s_hr_ca.box, UK_HERO_H);
    lv_obj_set_y(s_hr_wifi.box, UK_HERO_H + UK_ROW_H);
    lv_obj_set_y(s_hr_exit.box, UK_HERO_H + 2 * UK_ROW_H);
    lv_obj_set_y(s_hr_traf.box, UK_HERO_H + 3 * UK_ROW_H);
    lv_label_set_text(s_hr_wifi.val, "—");
    for (int i = 0; i < 2; i++) {
        lv_obj_t *v = i ? s_hr_ca.val : s_hr_exit.val;
        lv_label_set_text(v, "—");
        lv_obj_set_width(v, 210);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(v, UF.cj14, 0);
        lv_obj_set_height(v, 20);    /* 一行：放不下末尾「…」，不折行 */
        lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -(UK_PAD + 16), 11);
    }
    lv_label_set_text(s_hr_traf.val, "—");

    /* 情景: what the device decided about where it is (opens the 情景 page) */
    s_sc_card = uk_card(t, UK_MARGIN, 0, HOME_TILE_W, HOME_TILE_H);
    uk_label(s_sc_card, UF.cj12, T->t3, 12, 10, "情景");
    s_sc_state = uk_label_w(s_sc_card, UF.cj17b, T->t1, 12, 27, HOME_TILE_W - 24, 0, "");
    s_sc_note = home_tile_note(s_sc_card);
    uk_label_r(s_sc_card, UF.cj15, T->t3, HOME_TILE_W - 12, 6, "›");   /* 开页：有 ›（DESIGN.md §4 导航） */
    uk_tappable(s_sc_card, sc_card_cb, NULL);
    uk_show(s_sc_card, 0);

    /* 出口: the public IP and where it is; Tailscale's one-line summary */
    s_nh_card = uk_card(t, UK_MARGIN, 0, UK_CARD_W, NET_EXIT_ROW_H);
    {
        lv_obj_t *r = uk_box(s_nh_card, 0, 0, UK_CARD_W, NET_EXIT_ROW_H, T->card, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        uk_label(r, UF.cj14, T->t2, UK_PAD, 7, "出口");
        s_nh_ip  = uk_label_r(r, UF.n15, T->t1, UK_CARD_W - UK_PAD - 16, 6, "");
        s_nh_geo = uk_label_w(r, UF.cj12, T->t3, UK_PAD, 29, UK_CARD_W - 2 * UK_PAD - 16, 0, "");
        uk_label_r(r, UF.cj15, T->t3, UK_CARD_W - UK_PAD, 14, "›");
        uk_tappable(r, nh_card_cb, NULL);
        r = s_nh_tsrow = uk_box(s_nh_card, 0, NET_EXIT_ROW_H, UK_CARD_W, UK_ROW_H, T->card, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        uk_sep(r, 0);
        uk_label(r, UF.cj14, T->t2, UK_PAD, 11, "Tailscale");
        s_nh_tsval = uk_label_r(r, UF.n15, T->t1, UK_CARD_W - UK_PAD - 16, 10, "");
        uk_chevron(r, 0);
        uk_tappable(r, tile_click_cb, (void *)(intptr_t)SUB_TS);
        uk_show(r, 0);
    }

    {
        lv_obj_t *gone = lv_obj_create(t);
        lv_obj_remove_style_all(gone);
        lv_obj_add_flag(gone, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_parent(s_nh_card, gone);
    }

    /* 载波明细: every carrier row (moved to the top of 蜂窝 by build_cellular) */
    s_ca_card = uk_card(t, UK_MARGIN, 0, UK_CARD_W, CA_CARD_TOP + 40);
    uk_show(s_ca_card, 0);
    uk_label(s_ca_card, UF.cj12, T->t3, UK_PAD, 10, "载波");
    s_ca_qos = uk_label_r(s_ca_card, UF.n12, T->t2, UK_CARD_W - UK_PAD, 9, "");
    c = s_ca_card;
    for (int i = 0; i < CA_SLOTS; i++) {
        home_ca_t *k = &s_ca[i];
        k->box = uk_box(c, 0, CA_CARD_TOP + i * 40, UK_CARD_W, 40, T->card, 0);
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



    /* Tailscale: hidden entirely when the device has no tailscaled. The card
     * is the entry to the peer list (no 功能 tile for it). */
    s_ts_card = uk_card(t, UK_MARGIN, 500, UK_CARD_W, UK_ROW_H);
    static const char *const ts_keys[TS_HOME_ROWS] = { "Tailscale", "本机", "节点", "子网", "出口" };
    for (int i = 0; i < TS_HOME_ROWS; i++) {
        if (i) s_ts_sep[i] = uk_sep(s_ts_card, i * UK_ROW_H);
        s_ts_key[i] = uk_label(s_ts_card, UF.cj14, i ? T->t2 : T->t1, UK_PAD, i * UK_ROW_H + 11, ts_keys[i]);
        s_ts_val[i] = uk_label_r(s_ts_card, i ? UF.n15 : UF.cj14, T->t1, UK_CARD_W - UK_PAD - (i ? 0 : 16), i * UK_ROW_H + (i ? 10 : 11), "");
    }
    s_ts_dot = uk_dot(s_ts_card, 0, 17, 7, T->green);
    uk_chevron(s_ts_card, 0);   /* 整张卡开 Tailscale 页 */
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
    lv_obj_set_y(s_cell_card, y);
    y += home_visible_h(s_cell_card) + UK_MARGIN;
    /* 情景：独立的一块，整行 */
    if (home_visible_h(s_sc_card) >= 0) {
        lv_obj_set_width(s_sc_card, UK_CARD_W);
        lv_obj_set_y(s_sc_card, y);
        y += HOME_TILE_H + UK_MARGIN;
    }
    /* Tailscale：2026-09-25 用户要回首页（离家时靠它连回来，一眼要看到） */
    if (!lv_obj_has_flag(s_ts_card, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_y(s_ts_card, y);
        y += (int)lv_obj_get_style_height(s_ts_card, 0) + UK_MARGIN;
    }
    if (s_ch_net_card) {
        lv_obj_set_y(s_ch_net_card, y);
        y += (int)lv_obj_get_style_height(s_ch_net_card, 0) + UK_MARGIN;
    }
    uk_scroll_extent(s_home_scroll, y + UK_TAB_PAD);
    /* 载波等卡片在别的标签上，显隐变了那边也要重排 */
    cell_reflow();
    net_relayout();
}

/* No snapshot from the data service. Before the first one: 「正在读取…」.
 * After: the numbers stay where they were, dimmed, and the block says when
 * they stopped — a data-service outage is not "no signal". */
static int  s_ever_valid;
static long s_last_valid_wall;
static int  s_cc_tone = -1;
static void home_signal_down(void)
{
    static char c_st[64];
    uk_hero_tone(&s_cc_hero, 3);
    s_cc_tone = 3;
    if (!s_ever_valid) {
        set_label_fmt(s_cc_hero.st, c_st, sizeof c_st, "%s", "正在读取…");
        lv_label_set_text(s_cc_hero.big, "--");
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
        lv_label_set_text(s_cc_hero.big, "读不到数据");
    }
    for (int i = 0; i < CA_SLOTS; i++) {
        uk_text_color(s_ca[i].band, T->t3);
        uk_text_color(s_ca[i].rsrp, T->t3);
        uk_text_color(s_ca[i].sinr, T->t3);
    }
    /* 数字停住了：下面凡是跟着网络走的都调淡，别让人当成实时 */
    uk_text_color(s_hr_wifi.val, T->t3);
    uk_text_color(s_hr_traf.val, T->t3);
    uk_text_color(s_hr_exit.val, T->t3);
    uk_text_color(s_hr_ca.val, T->t3);
    uk_text_color(s_cc_hero.r1, T->t3);
    uk_text_color(s_nh_ip, T->t3);
    home_reflow();
}

/* ---- auxiliary device state ----
 * Things zwrt-datad's /state doesn't carry: interface up/down, WiFi power
 * save, direct-power-supply, DHCP pool text. Same one-shot shell round-trip
 * htmlmain.c's wifi_aux_refresh() uses, on the same kind of throttle, and
 * only while a page that displays it is actually visible. */
static int  s_aux_w24 = -1, s_aux_w5 = -1, s_aux_psm = -1, s_aux_dps = -1;
/* 蜂窝页的移动数据 / 数据漫游：zwrt_data get_wwaniface 的 enable / roam_enable */
static int  s_aux_data = -1, s_aux_roam = -1;
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
        "echo DPS=$(ubus call zwrt_bsp.charger list 2>/dev/null | grep direct_power_supply_mode | grep -o 'enable\\|disable');"
        "w=$(ubus call zwrt_data get_wwaniface '{\"cid\":1}' 2>/dev/null);"
        "echo WD=$(echo \"$w\" | grep '\"enable\"' | grep -o '[01]');"
        "echo WR=$(echo \"$w\" | grep '\"roam_enable\"' | grep -o '[01]')",
        "r");
    if (!fp) return;
    while (fgets(line, sizeof line, fp)) {
        if      (!strncmp(line, "W0=", 3))   s_aux_w24 = strstr(line, "=up") != NULL;
        else if (!strncmp(line, "W2=", 3))   s_aux_w5  = strstr(line, "=up") != NULL;
        else if (!strncmp(line, "PSM=", 4))  s_aux_psm = atoi(line + 4);
        else if (!strncmp(line, "POOL=", 5)) {
            char *nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
            snprintf(s_aux_pool, sizeof s_aux_pool, "%.*s", (int)sizeof s_aux_pool - 1, line + 5);
        } else if (!strncmp(line, "WD=", 3)) {
            s_aux_data = (line[3] == '0' || line[3] == '1') ? line[3] - '0' : -1;
        } else if (!strncmp(line, "WR=", 3)) {
            s_aux_roam = (line[3] == '0' || line[3] == '1') ? line[3] - '0' : -1;
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

/* A card row with a label, a state word and a switch (e.g. Wi-Fi). */
static lv_obj_t *toggle_row(lv_obj_t *c, int y, const char *name, int first, lv_obj_t **state,
                            lv_event_cb_t cb, void *user)
{
    if (!first) uk_sep(c, y);
    uk_label(c, UF.cj14, T->t1, UK_PAD, y + 11, name);
    if (state) *state = uk_label_r(c, UF.cj12, T->t3, UK_CARD_W - UK_PAD - 52, y + 13, "");
    return uk_toggle(c, UK_CARD_W - UK_PAD, y + 7, cb, user);
}

#define WIFI_CLI_H 50
static void build_wifi(lv_obj_t *t)
{
    int y = 4;
    t = s_w_scroll = uk_scroll(t, 0, UI_VIEW_H, 1000);

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
        s_w_cli_mac[i] = uk_label_w(s_w_cli[i], UF.cj12, T->t3, UK_PAD, 29, UK_CARD_W - 2 * UK_PAD, 0, "");
        s_w_cli_ip[i] = uk_label_r(s_w_cli[i], UF.n15, T->t1, UK_CARD_W - UK_PAD, 7, "");
        s_w_cli_tot[i] = uk_label(s_w_cli[i], UF.cj12, T->t3, UK_PAD, 47, "");
        uk_show(s_w_cli[i], 0);
    }
    y += WIFI_MAX_CLI * WIFI_CLI_H + 10;

    s_w_dhcp_sec = uk_section(t, y, "DHCP"); y += 20;
    s_w_dhcp_card = uk_card(t, UK_MARGIN, y, UK_CARD_W, 3 * UK_ROW_H);
    s_w_gw = uk_row(s_w_dhcp_card, 0, "网关", 1);
    s_w_pool = uk_row(s_w_dhcp_card, UK_ROW_H, "地址池", 0);
    s_w_lease = uk_row(s_w_dhcp_card, 2 * UK_ROW_H, "租期", 0);
    s_nh_scroll[NH_WIFI] = t;
    s_nh_base[NH_WIFI] = y + 3 * UK_ROW_H + 10;   /* 设备流量（build_sub_net）从这里起 */
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* ---- eSIM subpage ---- */
/* 一句几秒后消失的提示。点击的结果（「已经是…」「正忙」「没执行：…」）
 * 在 eSIM 页写在右上状态行（150 宽，约 12 个字），要短
 * 用它说出来，不静默忽略（2026-09-25）。 */
typedef struct { char txt[140]; uint32_t col, until; } net_flash_t;

static void net_flash(net_flash_t *f, uint32_t col, int ms, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->txt, sizeof f->txt, fmt, ap);
    va_end(ap);
    f->col = col;
    f->until = lv_tick_get() + (uint32_t)ms;
    if (!f->until) f->until = 1;
}
static int net_flash_on(const net_flash_t *f) { return f->until && (int32_t)(f->until - lv_tick_get()) > 0; }

static net_flash_t s_es_flash;
static int         s_es_dirty;     /* 点了一下：下一轮一定重画 eSIM 列表 */
static void esim_paint(void);

static void esim_row_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    int r = esim_select(idx);
    switch (r) {
    case ESIM_SEL_CURRENT:
        net_flash(&s_es_flash, T->accT, 3000, "这张就是正在用的");
        break;
    case ESIM_SEL_BUSY:
        net_flash(&s_es_flash, T->t2, 3000, "正在切换，等它做完");
        break;
    case ESIM_SEL_COOLDOWN:
    case ESIM_SEL_FAIL:
        net_flash(&s_es_flash, T->badT, 8000, "%s", esim_state());   /* 状态行本身就写着原因，标红 */
        break;
    default:   /* ARMED / STARTED：行本身会变色、写「再点一次确认」/「切换中…」 */
        s_es_flash.until = 0;
        break;
    }
    s_es_dirty = 1;
    esim_paint();
}

#define ESIM_ROW_H 50
static void build_sub_esim(lv_obj_t *t)
{
    t = uk_scroll(t, 0, UI_SUB_VIEW, 4 + 20 + UK_HERO_H + 10 + 20 + 3 * UK_ROW_H + 10 + 20 + ESIM_MAX_ROWS * ESIM_ROW_H + 16);
    uk_section(t, 4, "当前配置");
    lv_obj_t *cur = uk_card(t, UK_MARGIN, 24, UK_CARD_W, UK_HERO_H);
    uk_hero(&s_es_hero, cur, UF.cj22b);
    uk_hero_tone(&s_es_hero, 4);
    s_es_cur = s_es_hero.big;
    s_es_state = s_es_hero.r2;
    lv_label_set_text(s_es_hero.st, "使用中");

    /* 卡信息：实体 SIM 和 eSIM 都列（2026-09-25：插普通 SIM 时这页原来只有「-」） */
    int y = 24 + UK_HERO_H + 10;
    uk_section(t, y, "卡信息");
    lv_obj_t *info = uk_card(t, UK_MARGIN, y + 20, UK_CARD_W, 3 * UK_ROW_H);
    static const char *const k_info[3] = { "号码", "ICCID", "IMSI" };
    for (int i = 0; i < 3; i++) s_es_info[i] = uk_row(info, i * UK_ROW_H, k_info[i], i == 0);
    y += 20 + 3 * UK_ROW_H + 10;
    s_es_list_sec = uk_section(t, y, "eSIM 配置");
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

static int build_charts(lv_obj_t *t, int y0);

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
    /* 预估：公式见 estimate.c（和管理网页同一份规则） */
    c = uk_card(t, UK_MARGIN, y, UK_CARD_W, 6 * UK_ROW_H);
    static const char *const k_load_cap[6] = { "电池", "预估", "充电器", "CPU", "内存", "运行" };
    lv_obj_t **load_val[6] = { &s_sy_bat, &s_sy_est, &s_sy_chg, &s_sy_cpu, &s_sy_mem, &s_sy_up };
    for (int i = 0; i < 6; i++) *load_val[i] = uk_row(c, i * UK_ROW_H, k_load_cap[i], i == 0);
    lv_obj_set_style_text_font(s_sy_est, UF.cj14, 0);
    y += 6 * UK_ROW_H + 10;

    uk_section(t, y, "近 5 分钟"); y += 20;
    y += build_charts(t, y) + 10;

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

    uk_section(t, y, "调试"); y += 20;
    c = uk_card(t, UK_MARGIN, y, UK_CARD_W, UK_ROW_H);
    s_tile_sub[SUB_PERF] = uk_row_nav(c, 0, "性能测试", 1, tile_click_cb, (void *)(intptr_t)SUB_PERF);
    lv_label_set_text(s_tile_sub[SUB_PERF], "调试页");
    uk_text_color(s_tile_sub[SUB_PERF], T->t3);
    y += UK_ROW_H + 10;

    uk_section(t, y, "系统"); y += 20;
    c = uk_card(t, UK_MARGIN, y, UK_CARD_W, 100);
    s_vendor_btn = uk_button(c, UK_PAD, 12, UK_CARD_W - 2 * UK_PAD, 40, "切换到原厂界面", UK_BTN_PLAIN,
                             act_switch_vendor, NULL, &s_vendor_lbl);
    uk_label_w(c, UF.cj12, T->t3, UK_PAD, 62, UK_CARD_W - 2 * UK_PAD, 1,
               "电源键：短按 亮屏/息屏  长按 电源菜单\n回到这里：长按屏幕右下角 3 秒");
    y += 100;
    uk_scroll_extent(t, y + UK_TAB_PAD);   /* build_system: 屏幕 … 系统 */
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

/* 原「图表」标签（2026-09-25 并进系统，放在电池与负载下面）。y0 = 第一张卡的
 * 位置，返回占的高度。 */
static int build_charts(lv_obj_t *t, int y0)
{
    /* 网速: log scale (bytes/s spans five orders of magnitude here), two
     * lines told apart by colour and dash, legend in text colours. */
    /* 网速这张在首页（2026-09-25），CPU / 内存 / 电池留在系统 */
    lv_obj_t *c = s_ch_net_card = uk_card(s_home_scroll, UK_MARGIN, 0, UK_CARD_W, 116);
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

    lv_obj_t *a = uk_card(t, UK_MARGIN, y0, 145, 110), *m = uk_card(t, UK_MARGIN + 155, y0, 145, 110);
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

    lv_obj_t *b = uk_card(t, UK_MARGIN, y0 + 120, UK_CARD_W, 110);
    uk_label(b, UF.cj12, T->t3, 12, 9, "电池");
    s_ch_bat_s = uk_label_r(b, UF.cj12, T->t2, 288, 9, "");
    s_ch_bat_v = uk_label(b, UF.n20, T->t1, 12, 26, "");
    s_ch_bat = uk_chart(b, 12, 56, 276, 36, CHART_PTS, T->green, 0, &s_cs_bat, NULL);
    uk_label_r(b, UF.cj12, T->t3, 288, 92, "近 5 分钟");
    s_ch_wait[3] = chart_wait(b, 12, 66);
    home_reflow();
    return 120 + 110;
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


/* 情景卡：任何时候都能点，进「情景」页（手动固定情景）。 */
static void sc_card_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    sub_open(SUB_SCENE);
}


/* Tailscale 页（2026-09-25 加详）：本机一块，每台节点三行——名字和怎么连着、
 * IP 和系统、跟本机之间的连接（直连地址或经哪个 DERP、上次握手、收发了多少）。 */
#define TS_PEER_H 70
static void build_sub_ts(lv_obj_t *t)
{
    t = uk_scroll(t, 0, UI_SUB_VIEW, 24 + TS_SELF_ROWS * UK_ROW_H + 10 + 20 + TS_PEER_MAX * TS_PEER_H + 16);
    uk_section(t, 4, "本机");
    lv_obj_t *self = uk_card(t, UK_MARGIN, 24, UK_CARD_W, TS_SELF_ROWS * UK_ROW_H);
    static const char *const k_self_cap[TS_SELF_ROWS] = {
        "主机名", "IP", "IPv6", "DERP 中继", "子网路由", "Tailnet", "版本", "密钥到期" };
    for (int i = 0; i < TS_SELF_ROWS; i++) s_tp_self[i] = uk_row(self, i * UK_ROW_H, k_self_cap[i], i == 0);
    lv_obj_set_style_text_font(s_tp_self[0], UF.cj14, 0);
    lv_obj_set_style_text_font(s_tp_self[5], UF.cj14, 0);
    lv_obj_set_style_text_font(s_tp_self[7], UF.cj14, 0);
    int y = 24 + TS_SELF_ROWS * UK_ROW_H + 10;
    uk_section(t, y, "节点（和本机之间）");
    s_tp_card = uk_card(t, UK_MARGIN, y + 20, UK_CARD_W, TS_PEER_MAX * TS_PEER_H);
    for (int i = 0; i < TS_PEER_MAX; i++) {
        s_tp_row[i] = uk_box(s_tp_card, 0, i * TS_PEER_H, UK_CARD_W, TS_PEER_H, T->card, 0);
        lv_obj_set_style_bg_opa(s_tp_row[i], LV_OPA_TRANSP, 0);
        s_tp_sep[i] = i ? uk_sep(s_tp_row[i], 0) : NULL;
        s_tp_name[i] = uk_label_w(s_tp_row[i], UF.cj14, T->t1, UK_PAD, 8, 170, 0, "");
        lv_label_set_long_mode(s_tp_name[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(s_tp_name[i], lv_font_get_line_height(UF.cj14));
        s_tp_ip[i] = uk_label(s_tp_row[i], UF.n11, T->t3, UK_PAD, 30, "");
        s_tp_tag[i] = uk_label_r(s_tp_row[i], UF.cj13, T->t3, UK_CARD_W - UK_PAD, 9, "");
        s_tp_link[i] = uk_label_w(s_tp_row[i], UF.cj12, T->t2, UK_PAD, 47, UK_CARD_W - 2 * UK_PAD, 0, "");
        lv_label_set_long_mode(s_tp_link[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(s_tp_link[i], lv_font_get_line_height(UF.cj12));
        uk_show(s_tp_row[i], 0);
    }
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* "42 秒前" / "5 分钟前" / "3 小时前" / "2 天前" */
static void fmt_ago(char *out, size_t n, long s)
{
    if (s < 60)         snprintf(out, n, "%ld 秒前", s);
    else if (s < 3600)  snprintf(out, n, "%ld 分钟前", s / 60);
    else if (s < 86400) snprintf(out, n, "%ld 小时前", s / 3600);
    else                snprintf(out, n, "%ld 天前", s / 86400);
}

static void build_sub_cell(lv_obj_t *t)
{
    int y = 4;
    t = uk_scroll(t, 0, UI_SUB_VIEW, 1000);

    s_sg_nr_sec = uk_section(t, y, "5G 服务小区"); y += 20;
    lv_obj_t *nr = uk_card(t, UK_MARGIN, y, UK_CARD_W, 6 * UK_ROW_H);
    static const char *const k_nr_cap[6] = { "频段", "ARFCN", "PCI", "Cell ID", "PLMN", "RSRP / RSRQ / SINR" };
    for (int i = 0; i < 6; i++) s_sg_nr[i] = uk_row(nr, i * UK_ROW_H, k_nr_cap[i], i == 0);
    y += 6 * UK_ROW_H + 10;

    /* LTE 和 5G 一样逐行列（2026-09-25：原来挤成一行看不懂） */
    s_sg_lte_sec = uk_section(t, y, "LTE 服务小区"); y += 20;
    lv_obj_t *lte = uk_card(t, UK_MARGIN, y, UK_CARD_W, 6 * UK_ROW_H);
    static const char *const k_lt_cap[6] = { "频段", "EARFCN", "PCI", "Cell ID", "RSRP / RSRQ / SINR", "RSSI" };
    for (int i = 0; i < 6; i++) s_sg_lt[i] = uk_row(lte, i * UK_ROW_H, k_lt_cap[i], i == 0);
    y += 6 * UK_ROW_H + 10;

    uk_section(t, y, "网络"); y += 20;
    lv_obj_t *net = uk_card(t, UK_MARGIN, y, UK_CARD_W, 4 * UK_ROW_H);
    static const char *const k_net_cap[4] = { "网络模式", "WAN", "制式", "高铁模式" };
    for (int i = 0; i < 4; i++) s_sg_net[i] = uk_row(net, i * UK_ROW_H, k_net_cap[i], i == 0);
    lv_obj_set_style_text_font(s_sg_net[3], UF.cj14, 0);
    y += 4 * UK_ROW_H + 10;

    /* 支持频段不在这页列（2026-09-25）：锁频页的频段按钮就是同一份清单。
     * 标签还建着、挂在隐藏的父对象下，刷新代码不用改。 */
    {
        lv_obj_t *gone = lv_obj_create(t);
        lv_obj_remove_style_all(gone);
        lv_obj_add_flag(gone, LV_OBJ_FLAG_HIDDEN);
        s_sg_nrb = uk_label(gone, UF.n12, T->t1, 0, 0, "");
        s_sg_lteb = uk_label(gone, UF.n12, T->t1, 0, 0, "");
    }

    /* 邻小区一块由 build_sub_net 挂在这下面（net_reflow 定位置和滚动范围） */
    s_nh_scroll[NH_CELL] = t;
    s_nh_base[NH_CELL] = y;
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
    static const char *const k_mode_v[4] = { "WL_AND_5G", "LTE_AND_5G", "Only_5G", "Only_LTE" };
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
    /* 5G SA only: many countries have no SA and roaming SIMs often can't use
     * it; with no 2G to fall back on that is no signal at all. */
    lv_label_set_text(s_lk_mode_lbl, idx == 2 ? "再按一次确认 · 国外和漫游卡常没有 SA"
                                              : "再按一次确认切换");
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
    int y = 4;   /* 网络模式 2026-09-25 搬到蜂窝标签，这页只剩频段 */
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
    for (int i = 0; i < ST_SRV_ROWS; i++)   /* 对勾当场挪过去，不等 1 秒的刷新 */
        if (s_st_srv_ok[i]) uk_show(s_st_srv_ok[i], i == idx);
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
/* ---- 网络 subpage ----
 * 出口 IP 和归属地、原始/注册运营商、漫游、手动选网、邻小区。数据全来自
 * zte-agent 的 /api/netinfo（netinfo.c）；慢的事都在 agent 的后台线程里，
 * 这里只发起动作再轮询。碰模组的动作（搜网、注册、恢复自动）和换情景都是
 * 两步：点一下整行 / 按钮变色并写出后果，4 秒内再点才发。
 *
 * 每一次点击都要当场看得见（2026-09-25 用户：点了毫无反应）：
 * - 按下：行有底色（uk_tappable）；
 * - 第一下：当场重画，不等 1 秒的刷新；
 * - 点了已经生效的选项：一句「已经是…」，不静默忽略；
 * - 发出后：「切换中…」一直显示到真的变过去，或者超时说明原因；
 * - 被拒：原因写在这张卡片里，不只写在页顶。 */
#define NET_EXIT_H   50
#define NET_STAT_H   48
#define NET_OP_H     44
#define NET_BTN_H    52
#define NET_NBR_HDR  40
#define NET_NBR_H    28
#define NET_NOTE_H   64
#define NET_CL_H     50
static lv_obj_t *s_net_cl_state, *s_net_cl_row[NI_MAX_CLIENTS], *s_net_cl_name[NI_MAX_CLIENTS],
                *s_net_cl_tot[NI_MAX_CLIENTS], *s_net_cl_sub[NI_MAX_CLIENTS];
#define NET_SCENE_ROWS (1 + NI_MAX_SCENES + 1)   /* 自动 + 各情景 + 1 行备用 */
#define NET_SC_NOTE_H  44

static lv_obj_t *s_net_sec[6], *s_net_card[6];
static lv_obj_t *s_net_sc_row[NET_SCENE_ROWS], *s_net_sc_name[NET_SCENE_ROWS], *s_net_sc_tag[NET_SCENE_ROWS];
static lv_obj_t *s_net_sc_note;
static uint32_t  s_net_arm_sc;
static int       s_net_arm_sc_idx = -1;        /* 0 = 自动，1.. = scenes[i-1] */
static lv_obj_t *s_net_ex_row[2], *s_net_ex_key[2], *s_net_ex_ip[2], *s_net_ex_sub[2];
static lv_obj_t *s_net_op[4];
static lv_obj_t *s_net_status;
static lv_obj_t *s_net_opr[NI_MAX_OPS], *s_net_opr_name[NI_MAX_OPS], *s_net_opr_det[NI_MAX_OPS], *s_net_opr_tag[NI_MAX_OPS];
static lv_obj_t *s_net_btns, *s_net_scan_btn, *s_net_scan_lbl, *s_net_auto_btn, *s_net_auto_lbl;
static lv_obj_t *s_net_nbr_state, *s_net_nbr_btn, *s_net_nbr_lbl, *s_net_nbr_row[NI_MAX_CELLS], *s_net_nbr_l[NI_MAX_CELLS], *s_net_nbr_r[NI_MAX_CELLS];
static uint32_t  s_net_arm_scan, s_net_arm_auto, s_net_arm_op, s_net_arm_nbr;
static int       s_net_arm_idx = -1;
static int       s_net_painted_arm = -2;   /* 上次画的界面状态（待确认 / 切换中 / 提示），变了就重画 */
static lv_obj_t *s_net_sc_mark[NET_SCENE_ROWS];  /* 单选圈：实心 = 现在生效的那个 */
/* 每个情景下面两行小字：什么时候进入、进入后改什么（2026-09-25 用户：光有名字看不懂） */
static lv_obj_t *s_net_sc_when[NET_SCENE_ROWS], *s_net_sc_does[NET_SCENE_ROWS];
#define NET_SC_ROW3_H 70
/* 情景切换：发出后等到真的切过去（或超时） */
static int       s_sc_pend = -1;               /* 0 = 自动，1.. = scenes[i-1] */
static uint32_t  s_sc_pend_at;
static char      s_sc_pend_id[24], s_sc_pend_name[48];
static int       s_sc_pend_wifi_off;
#define SC_PEND_MS 45000
/* 临时提示：情景卡片一条，手动选网卡片一条（类型在 eSIM 页前面） */
static net_flash_t s_sc_flash, s_ms_flash;

static void net_paint(int changed);

static const char *net_scene_name(const netinfo_t *n, const char *id)
{
    for (int i = 0; i < n->nscenes; i++)
        if (!strcmp(n->scenes[i].id, id)) return n->scenes[i].name[0] ? n->scenes[i].name : id;
    return id;
}

static int net_armed(uint32_t at) { return at && lv_tick_get() - at < 4000; }
static int net_confirm(uint32_t at) { uint32_t d = lv_tick_get() - at; return at && d > 300 && d < 4000; }

static void net_clear_arms(void)
{
    s_net_arm_scan = s_net_arm_auto = s_net_arm_nbr = s_net_arm_op = s_net_arm_sc = 0;
    s_net_arm_idx = s_net_arm_sc_idx = -1;
}

static int net_busy(const netinfo_t *n)
{
    return !strcmp(n->scan_state, "scanning") ||
           !strcmp(n->guard_phase, "registering") || !strcmp(n->guard_phase, "reverting");
}

/* 发一个会碰模组的动作：先把「已发送」画出来再发（发的时候界面会停一下），
 * 被拒就把原因写回这张卡片 */
static void net_send(void (*fn)(void))
{
    net_flash(&s_ms_flash, T->t2, 2500, "已发送，等设备回应…");
    net_paint(1);
    lv_refr_now(NULL);
    fn();
    const char *err = netinfo_action_error();
    if (err[0]) net_flash(&s_ms_flash, T->badT, 8000, "没执行：%s", err);
    else netinfo_hurry(15);
    net_paint(1);
}

static const char *net_busy_what(const netinfo_t *n)
{
    return !strcmp(n->scan_state, "scanning") ? "正在搜索网络" :
           !strcmp(n->guard_phase, "reverting") ? "正在恢复自动选网" :
           !strcmp(n->guard_phase, "registering") ? "正在注册" : "正在忙";
}

static void net_scan_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    const netinfo_t *n = netinfo_get();
    if (net_busy(n)) {
        net_flash(&s_ms_flash, T->t2, 3000, "%s，等它做完再操作", net_busy_what(n));
        net_paint(1);
        return;
    }
    if (net_confirm(s_net_arm_scan)) { net_clear_arms(); net_send(netinfo_scan); return; }
    net_clear_arms();
    s_net_arm_scan = lv_tick_get();
    net_paint(1);
}

static void net_auto_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    const netinfo_t *n = netinfo_get();
    if (net_busy(n)) {
        net_flash(&s_ms_flash, T->t2, 3000, "%s，等它做完再操作", net_busy_what(n));
        net_paint(1);
        return;
    }
    if (!strcmp(n->selection, "auto")) {
        net_clear_arms();
        net_flash(&s_ms_flash, T->t2, 3000, "现在已经是自动选网，不用恢复");
        net_paint(1);
        return;
    }
    if (net_confirm(s_net_arm_auto)) { net_clear_arms(); net_send(netinfo_auto); return; }
    net_clear_arms();
    s_net_arm_auto = lv_tick_get();
    net_paint(1);
}

static int s_net_reg_op;
static void net_register_armed(void) { netinfo_register(s_net_reg_op); }

static void net_op_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    const netinfo_t *n = netinfo_get();
    if (net_busy(n)) {
        net_flash(&s_ms_flash, T->t2, 3000, "%s，等它做完再操作", net_busy_what(n));
        net_paint(1);
        return;
    }
    if (i >= n->nops) return;
    if (!strcmp(n->ops[i].status, "2")) {
        net_clear_arms();
        net_flash(&s_ms_flash, T->t2, 3000, "现在就在 %s 上", n->ops[i].name[0] ? n->ops[i].name : n->ops[i].plmn);
        net_paint(1);
        return;
    }
    if (s_net_arm_idx == i && net_confirm(s_net_arm_op)) {
        net_clear_arms();
        s_net_reg_op = i;
        net_send(net_register_armed);
        return;
    }
    net_clear_arms();
    s_net_arm_idx = i;
    s_net_arm_op = lv_tick_get();
    net_paint(1);
}

static void net_nbr_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!strcmp(netinfo_get()->nbr_state, "scanning")) return;
    if (net_confirm(s_net_arm_nbr)) { net_clear_arms(); net_send(netinfo_nbr_scan); return; }
    net_clear_arms();
    s_net_arm_nbr = lv_tick_get();
    net_paint(1);
}

/* 情景行：0 = 自动，1..n = 固定到 scenes[i-1]，最后一行另有用途。
 * 换情景可能开关 Wi-Fi，所以两步：第一下整行变色、写出后果，4 秒内再点才发；
 * 发出后显示「切换中…」直到真的切过去。 */
static void net_sc_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    const netinfo_t *n = netinfo_get();
    if (i == NET_SCENE_ROWS - 1) return;
    if (i > n->nscenes) return;
    if (s_sc_pend >= 0) {
        net_flash(&s_sc_flash, T->t2, 3000, "正在切换到「%s」，稍等", s_sc_pend_name);
        net_paint(1);
        return;
    }
    int already = i == 0 ? !n->scene_pin[0] : !strcmp(n->scene_pin, n->scenes[i - 1].id);
    if (already) {
        net_clear_arms();
        if (i == 0)
            net_flash(&s_sc_flash, T->t2, 4000, "现在已经是自动 · 按位置和 SIM 判断为「%s」",
                      n->scene_current[0] ? net_scene_name(n, n->scene_current) : "判定中");
        else
            net_flash(&s_sc_flash, T->t2, 4000, "已经固定在「%s」。要恢复自动判断，点「自动」",
                      net_scene_name(n, n->scene_pin));
        net_paint(1);
        return;
    }
    if (s_net_arm_sc_idx == i && net_confirm(s_net_arm_sc)) {
        net_clear_arms();
        s_sc_pend = i;
        s_sc_pend_at = lv_tick_get();
        snprintf(s_sc_pend_id, sizeof s_sc_pend_id, "%s", i ? n->scenes[i - 1].id : "");
        snprintf(s_sc_pend_name, sizeof s_sc_pend_name, "%s", i ? net_scene_name(n, n->scenes[i - 1].id) : "自动");
        s_sc_pend_wifi_off = i ? n->scenes[i - 1].wifi_off : 0;
        s_sc_flash.until = 0;
        net_paint(1);
        lv_refr_now(NULL);
        netinfo_pin(i == 0 ? NULL : n->scenes[i - 1].id);
        const char *err = netinfo_action_error();
        if (err[0]) {
            s_sc_pend = -1;
            net_flash(&s_sc_flash, T->badT, 8000, "没切成：%s", err);
        } else {
            scenario_kick();
            netinfo_hurry(SC_PEND_MS / 1000);
        }
        net_paint(1);
        return;
    }
    net_clear_arms();
    s_net_arm_sc_idx = i;
    s_net_arm_sc = lv_tick_get();
    net_paint(1);
}

static void net_reflow(int err_h, int scene_rows, int exits, int ops, int cells);

/* 原「情景 · 网络」页的六块，2026-09-25 起各回各家（k_net_host）：情景 → 情景页，
 * 出口 IP → 出口标签，运营商 + 手动选网 → 运营商选择页，邻小区 → 小区信息页，
 * 设备流量 → Wi-Fi 标签。数据和画法没变（netinfo + net_paint），只是卡片挂在
 * 不同的页上，net_reflow 按页各自排。 */
static void build_sub_net(lv_obj_t *t)
{
    static const char *const k_sec[6] = { "选择", "出口 IP", "运营商", "手动选网", "邻小区", "已连接设备流量" };
    static const char *const k_op_cap[4] = { "原始运营商", "注册运营商", "漫游", "选网" };
    lv_obj_t *c, *host[6];

    t = s_net_scroll = s_nh_scroll[NH_OPER] = uk_scroll(t, 0, UI_SUB_VIEW, 1400);
    s_nh_scroll[NH_SCENE] = uk_scroll(s_sub_page[SUB_SCENE], 0, UI_SUB_VIEW, 600);
    s_nh_base[NH_SCENE] = s_nh_base[NH_OPER] = 4;
    for (int i = 0; i < 6; i++) host[i] = s_nh_scroll[k_net_host[i]];
    s_net_err = uk_label_w(t, UF.cj13, T->badT, UK_MARGIN + 6, 4, UK_CARD_W - 12, 1, "");
    for (int i = 0; i < 6; i++) s_net_sec[i] = uk_section(host[i], 0, k_sec[i]);

    c = s_net_card[0] = uk_card(host[0], UK_MARGIN, 0, UK_CARD_W, UK_ROW_H + NET_SC_NOTE_H);
    for (int i = 0; i < NET_SCENE_ROWS; i++) {
        lv_obj_t *r = s_net_sc_row[i] = uk_box(c, 0, i * UK_ROW_H, UK_CARD_W, UK_ROW_H, T->card, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        if (i) uk_sep(r, 0);
        s_net_sc_mark[i] = uk_box(r, UK_PAD, (UK_ROW_H - 16) / 2, 16, 16, T->card, 8);
        lv_obj_set_style_border_width(s_net_sc_mark[i], 2, 0);
        s_net_sc_name[i] = uk_label_w(r, UF.cj14, T->t1, UK_PAD + 26, 11, 140, 0, "");
        s_net_sc_tag[i]  = uk_label_r(r, UF.cj13, T->t3, UK_CARD_W - UK_PAD, 12, "");
        s_net_sc_when[i] = uk_label_w(r, UF.cj12, T->t2, UK_PAD + 26, 32, UK_CARD_W - 2 * UK_PAD - 26, 1, "");
        s_net_sc_does[i] = uk_label_w(r, UF.cj12, T->t3, UK_PAD + 26, 50, UK_CARD_W - 2 * UK_PAD - 26, 0, "");
        for (int k = 0; k < 2; k++) {
            lv_obj_t *l = k ? s_net_sc_does[i] : s_net_sc_when[i];
            lv_obj_set_height(l, lv_font_get_line_height(UF.cj12));
            lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        }
        uk_tappable(r, net_sc_cb, (void *)(intptr_t)i);
        uk_show(r, i == 0);
    }
    s_net_sc_note = uk_label_w(c, UF.cj12, T->t3, UK_PAD, UK_ROW_H + 8, UK_CARD_W - 2 * UK_PAD, 1, "");

    c = s_net_card[1] = uk_card(host[1], UK_MARGIN, 0, UK_CARD_W, 2 * NET_EXIT_H);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *r = s_net_ex_row[i] = uk_box(c, 0, i * NET_EXIT_H, UK_CARD_W, NET_EXIT_H, T->card, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        if (i) uk_sep(r, 0);
        s_net_ex_key[i] = uk_label(r, UF.cj14, T->t2, UK_PAD, 7, "");
        s_net_ex_ip[i]  = uk_label_r(r, UF.n15, T->t1, UK_CARD_W - UK_PAD, 6, "");
        s_net_ex_sub[i] = uk_label_w(r, UF.cj12, T->t3, UK_PAD, 29, UK_CARD_W - 2 * UK_PAD, 0, "");
    }

    c = s_net_card[2] = uk_card(host[2], UK_MARGIN, 0, UK_CARD_W, 4 * UK_ROW_H);
    for (int i = 0; i < 4; i++) {
        s_net_op[i] = uk_row(c, i * UK_ROW_H, k_op_cap[i], i == 0);
        lv_obj_set_style_text_font(s_net_op[i], UF.cj14, 0);
    }

    c = s_net_card[3] = uk_card(host[3], UK_MARGIN, 0, UK_CARD_W, NET_STAT_H + NET_BTN_H);
    s_net_status = uk_label_w(c, UF.cj13, T->t2, UK_PAD, 8, UK_CARD_W - 2 * UK_PAD, 1, "");
    for (int i = 0; i < NI_MAX_OPS; i++) {
        lv_obj_t *r = s_net_opr[i] = uk_box(c, 0, NET_STAT_H + i * NET_OP_H, UK_CARD_W, NET_OP_H, T->card, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        uk_sep(r, 0);
        s_net_opr_name[i] = uk_label_w(r, UF.cj14, T->t1, UK_PAD, 5, UK_CARD_W - 2 * UK_PAD - 70, 0, "");
        s_net_opr_det[i]  = uk_label_w(r, UF.cj12, T->t3, UK_PAD, 25, UK_CARD_W - 2 * UK_PAD - 70, 0, "");
        s_net_opr_tag[i]  = uk_label_r(r, UF.cj13, T->t3, UK_CARD_W - UK_PAD, 13, "");
        uk_tappable(r, net_op_cb, (void *)(intptr_t)i);
        uk_show(r, 0);
    }
    s_net_btns = uk_box(c, 0, NET_STAT_H, UK_CARD_W, NET_BTN_H, T->card, 0);
    lv_obj_set_style_bg_opa(s_net_btns, LV_OPA_TRANSP, 0);
    uk_sep(s_net_btns, 0);
    s_net_scan_btn = uk_button(s_net_btns, UK_PAD, 10, (UK_CARD_W - 2 * UK_PAD - 8) / 2, 32, "搜索网络",
                               UK_BTN_PLAIN, net_scan_cb, NULL, &s_net_scan_lbl);
    s_net_auto_btn = uk_button(s_net_btns, UK_PAD + (UK_CARD_W - 2 * UK_PAD - 8) / 2 + 8, 10,
                               (UK_CARD_W - 2 * UK_PAD - 8) / 2, 32, "恢复自动",
                               UK_BTN_PLAIN, net_auto_cb, NULL, &s_net_auto_lbl);

    c = s_net_card[4] = uk_card(host[4], UK_MARGIN, 0, UK_CARD_W, NET_NBR_HDR);
    s_net_nbr_state = uk_label_w(c, UF.cj13, T->t2, UK_PAD, 12, UK_CARD_W - 2 * UK_PAD - 80, 0, "");
    s_net_nbr_btn = uk_button(c, UK_CARD_W - UK_PAD - 72, 6, 72, 28, "扫描", UK_BTN_PLAIN, net_nbr_cb, NULL, &s_net_nbr_lbl);
    for (int i = 0; i < NI_MAX_CELLS; i++) {
        lv_obj_t *r = s_net_nbr_row[i] = uk_box(c, 0, NET_NBR_HDR + i * NET_NBR_H, UK_CARD_W, NET_NBR_H, T->card, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        if (!i) uk_sep(r, 0);
        s_net_nbr_l[i] = uk_label(r, UF.n12, T->t1, UK_PAD, 7, "");
        s_net_nbr_r[i] = uk_label_r(r, UF.n12, T->t2, UK_CARD_W - UK_PAD, 7, "");
        uk_show(r, 0);
    }

    /* 每台 Wi-Fi 设备：名字、连上以来的总流量、此刻的速率和信号 */
    c = s_net_card[5] = uk_card(host[5], UK_MARGIN, 0, UK_CARD_W, NET_NOTE_H);
    s_net_cl_state = uk_label_w(c, UF.cj13, T->t3, UK_PAD, 11, UK_CARD_W - 2 * UK_PAD, 1, "读取中…");
    for (int i = 0; i < NI_MAX_CLIENTS; i++) {
        lv_obj_t *r = s_net_cl_row[i] = uk_box(c, 0, i * NET_CL_H, UK_CARD_W, NET_CL_H, T->card, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        if (i) uk_sep(r, 0);
        s_net_cl_name[i] = uk_label_w(r, UF.cj14, T->t1, UK_PAD, 7, 130, 0, "");
        s_net_cl_tot[i]  = uk_label_r(r, UF.n12, T->t2, UK_CARD_W - UK_PAD, 9, "");
        s_net_cl_sub[i]  = uk_label_w(r, UF.n12, T->t3, UK_PAD, 29, UK_CARD_W - 2 * UK_PAD, 0, "");
        uk_show(r, 0);
    }
    /* 设备流量已并进 Wi-Fi 标签的设备列表（refresh_wifi 按 MAC/IP 对上）：这块不显示 */
    uk_show(s_net_sec[5], 0);
    uk_show(s_net_card[5], 0);
    net_reflow(0, UK_ROW_H, 1, 0, 0);
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* Cards move with the lists above them: lay each page out from its base. */
static int s_net_ncl;   /* 设备流量卡里显示几行（net_paint 定） */
static int s_nr_last[5] = { 0, UK_ROW_H, 1, 0, 0 };
/* scene_px：情景各行加起来的高度（行高不一样，2026-09-25 起按像素传） */
static void net_reflow(int err_h, int scene_rows, int exits, int ops, int cells)
{
    int a[5] = { err_h, scene_rows, exits, ops, cells };
    memcpy(s_nr_last, a, sizeof a);
    int y[NH_N];
    for (int k = 0; k < NH_N; k++) y[k] = s_nh_base[k];
    y[NH_OPER] += err_h;
    int h[6] = {
        scene_rows + NET_SC_NOTE_H,
        exits * NET_EXIT_H,
        4 * UK_ROW_H,
        NET_STAT_H + ops * NET_OP_H + NET_BTN_H,
        NET_NBR_HDR + cells * NET_NBR_H + (cells ? 6 : 0),
        s_net_ncl ? s_net_ncl * NET_CL_H : NET_NOTE_H,
    };
    for (int i = 0; i < 6; i++) {
        if (lv_obj_has_flag(s_net_card[i], LV_OBJ_FLAG_HIDDEN)) continue;
        int *yy = &y[k_net_host[i]];
        lv_obj_set_y(s_net_sec[i], *yy);
        lv_obj_set_y(s_net_card[i], *yy + 20);
        lv_obj_set_height(s_net_card[i], h[i]);
        *yy += 20 + h[i] + 10;
    }
    lv_obj_set_y(s_net_btns, NET_STAT_H + ops * NET_OP_H);
    lv_obj_set_y(s_net_sc_note, scene_rows + 8);
    lv_obj_set_y(s_exit_nav_sec, y[NH_EXIT]);
    lv_obj_set_y(s_exit_nav_card, y[NH_EXIT] + 20);
    y[NH_EXIT] += 20 + (int)lv_obj_get_style_height(s_exit_nav_card, 0) + 10;
    for (int k = 0; k < NH_N; k++)
        uk_scroll_extent(s_nh_scroll[k], y[k] - 10 + (k == NH_EXIT || k == NH_WIFI ? UK_TAB_PAD : 16));
}

/* 某一页上面的内容高度变了（Wi-Fi 设备列表）：按上次的参数重排 */
static void net_relayout(void)
{
    if (!s_net_card[0]) return;   /* 还没建好 */
    net_reflow(s_nr_last[0], s_nr_last[1], s_nr_last[2], s_nr_last[3], s_nr_last[4]);
}

/* 首页出口卡 → 出口标签（出口 IP 在最上面） */
static void nh_card_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    tab_go(TAB_EXIT);
}

static const char *net_rat_name(const char *rat)
{
    if (!strcmp(rat, "12") || !strcmp(rat, "11") || !strcmp(rat, "13")) return "5G";
    if (!strcmp(rat, "7")) return "4G";
    if (!strcmp(rat, "2") || !strcmp(rat, "4") || !strcmp(rat, "5") || !strcmp(rat, "6")) return "3G";
    if (!strcmp(rat, "0") || !strcmp(rat, "1") || !strcmp(rat, "3")) return "2G";
    return rat;
}

/* "中国联通 46001"；国外的带上国家："SoftBank（日本）44020" */
static void net_oper_text(char *out, size_t n, const ni_oper_t *o)
{
    const char *name = o->name[0] ? o->name : "未知";
    if (!o->mcc[0] && !o->name[0]) { snprintf(out, n, "—"); return; }
    if (o->country[0] && strcmp(o->country, "中国"))
        snprintf(out, n, "%s（%s）%s%s", name, o->country, o->mcc, o->mnc);
    else
        snprintf(out, n, "%s %s%s", name, o->mcc, o->mnc);
}

/* 第二行：归属地 · 运营商或节点；查不到时写原因，旧结果刷新失败时标一下 */
static void net_exit_sub(char *out, size_t n, const ni_exit_t *e, const char *extra)
{
    /* 原始错误（"ipapi.co: io: unexpected end of file"）只是最后一家的失败；
     * 几家都失败多半是刚换网/节点不通，半分钟后会再查。 */
    if (!e->ip[0] && e->err[0]) { snprintf(out, n, "暂时查不到，稍后自动重试"); return; }
    snprintf(out, n, "%s%s%s%s", e->geo, e->geo[0] && extra[0] ? " · " : "", extra,
             e->err[0] ? " · 刷新失败" : "");
}

/* ---- APN（蜂窝 → APN，2026-09-25）----
 * 看得到数据连接正在拨哪条 APN；在「自动」和已存的手动 APN 之间切（两下确认，
 * 切换中直到读回来）。新建、修改要打字，在管理网页做。 */
#define APN_ROWS    (1 + NI_MAX_APNS)
#define APN_ROW_H   UK_ROW2_H
#define APN_NOTE_H  44
#define APN_PEND_MS 45000
static lv_obj_t *s_apn_now, *s_apn_now_sub, *s_apn_sec, *s_apn_card, *s_apn_note, *s_apn_foot, *s_apn_scroll;
static lv_obj_t *s_apn_row[APN_ROWS], *s_apn_mark[APN_ROWS], *s_apn_name[APN_ROWS], *s_apn_sub[APN_ROWS], *s_apn_tag[APN_ROWS];
static uint32_t  s_apn_arm;
static int       s_apn_arm_idx = -1;
static int       s_apn_pend = -1;             /* 0 = 自动，1.. = apns[i-1] */
static uint32_t  s_apn_pend_at;
static char      s_apn_pend_id[24], s_apn_pend_name[48];
static net_flash_t s_apn_flash;

static const char *apn_pdp(int pdp) { return pdp == 1 ? "IPv4" : pdp == 2 ? "IPv6" : pdp == 3 ? "IPv4v6" : ""; }

/* 这一行是不是现在生效的选择（自动模式 = 第 0 行；手动 = 手动模式选中的那条） */
static int apn_row_current(const netinfo_t *n, int i)
{
    if (!n->apn_known) return 0;
    if (i == 0) return !n->apn_manual;
    return i <= n->napns && n->apn_manual && n->apns[i - 1].selected;
}

static void apn_paint(int changed);

static void apn_row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    const netinfo_t *n = netinfo_get();
    if (!n->apn_known) {
        net_flash(&s_apn_flash, T->t2, 3000, "还没读到 APN，稍等");
    } else if (s_apn_pend >= 0) {
        net_flash(&s_apn_flash, T->t2, 3000, "正在切换到「%s」，稍等", s_apn_pend_name);
    } else if (apn_row_current(n, i)) {
        s_apn_arm_idx = -1;
        if (i == 0) net_flash(&s_apn_flash, T->t2, 4000, "现在已经是自动选择");
        else        net_flash(&s_apn_flash, T->t2, 4000, "已经在用「%s」", n->apns[i - 1].name);
    } else if (i > n->napns) {
        return;
    } else if (s_apn_arm_idx == i && net_confirm(s_apn_arm)) {
        s_apn_arm_idx = -1;
        s_apn_pend = i;
        s_apn_pend_at = lv_tick_get();
        snprintf(s_apn_pend_id, sizeof s_apn_pend_id, "%s", i ? n->apns[i - 1].id : "auto");
        snprintf(s_apn_pend_name, sizeof s_apn_pend_name, "%s", i ? n->apns[i - 1].name : "自动");
        s_apn_flash.until = 0;
        apn_paint(1);
        lv_refr_now(NULL);
        netinfo_apn_use(s_apn_pend_id);
        const char *err = netinfo_action_error();
        if (err[0]) {
            s_apn_pend = -1;
            net_flash(&s_apn_flash, T->badT, 8000, "没切成：%s", err);
        } else {
            netinfo_hurry(APN_PEND_MS / 1000);
        }
    } else {
        s_apn_arm_idx = i;
        s_apn_arm = lv_tick_get();
        s_apn_flash.until = 0;
    }
    apn_paint(1);
}

static void build_sub_apn(lv_obj_t *t)
{
    t = s_apn_scroll = uk_scroll(t, 0, UI_SUB_VIEW, 800);
    uk_section(t, 4, "正在用");
    lv_obj_t *c = uk_card(t, UK_MARGIN, 24, UK_CARD_W, UK_HERO_H);
    s_apn_now = uk_label_w(c, UF.cj17b, T->t1, UK_PAD, 12, UK_CARD_W - 2 * UK_PAD, 0, "读取中…");
    s_apn_now_sub = uk_label_w(c, UF.cj13, T->t2, UK_PAD, 42, UK_CARD_W - 2 * UK_PAD, 0, "");
    int y = 24 + UK_HERO_H + 10;
    s_apn_sec = uk_section(t, y, "选择");
    c = s_apn_card = uk_card(t, UK_MARGIN, y + 20, UK_CARD_W, APN_ROW_H + APN_NOTE_H);
    for (int i = 0; i < APN_ROWS; i++) {
        lv_obj_t *r = s_apn_row[i] = uk_box(c, 0, i * APN_ROW_H, UK_CARD_W, APN_ROW_H, T->card, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        if (i) uk_sep(r, 0);
        s_apn_mark[i] = uk_box(r, UK_PAD, (APN_ROW_H - 16) / 2, 16, 16, T->card, 8);
        lv_obj_set_style_border_width(s_apn_mark[i], 2, 0);
        s_apn_name[i] = uk_label_w(r, UF.cj14, T->t1, UK_PAD + 26, 6, 170, 0, "");
        s_apn_sub[i]  = uk_label_w(r, UF.cj12, T->t3, UK_PAD + 26, 27, 170, 0, "");
        s_apn_tag[i]  = uk_label_r(r, UF.cj13, T->t3, UK_CARD_W - UK_PAD, 16, "");
        uk_tappable(r, apn_row_cb, (void *)(intptr_t)i);
        uk_show(r, i == 0);
    }
    s_apn_note = uk_label_w(c, UF.cj12, T->t3, UK_PAD, APN_ROW_H + 8, UK_CARD_W - 2 * UK_PAD, 1, "");
    s_apn_foot = uk_label_w(t, UF.cj12, T->t3, UK_MARGIN + 6, 0, UK_CARD_W - 12, 1,
                            "新建或修改 APN 请用管理网页的「APN」页");
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

static void apn_paint(int changed)
{
    static char c_now[64], c_nsub[96], c_n[APN_ROWS][48], c_s[APN_ROWS][64], c_t[APN_ROWS][32], c_note[160], c_row[64];
    static int painted = -2;
    const netinfo_t *n = netinfo_get();
    int arm = (s_apn_arm_idx >= 0 && net_armed(s_apn_arm)) ? s_apn_arm_idx : -1;
    char buf[160];

    if (s_apn_pend >= 0) {
        int done = s_apn_pend == 0 ? (n->apn_known && !n->apn_manual)
                                   : (n->apn_manual && !strcmp(n->apn_in_use.id, s_apn_pend_id));
        if (done) {
            net_flash(&s_apn_flash, T->okT, 6000, "已切换：现在用「%s」",
                      n->apn_in_use.name[0] ? n->apn_in_use.name : s_apn_pend_name);
            s_apn_pend = -1;
        } else if (lv_tick_get() - s_apn_pend_at > APN_PEND_MS) {
            net_flash(&s_apn_flash, T->warnT, 10000, "设备没确认这次切换，看一下上面「正在用」再试");
            s_apn_pend = -1;
        }
    }
    int key = arm + (s_apn_pend >= 0 ? 1000 + s_apn_pend : 0) + (net_flash_on(&s_apn_flash) ? 10000 : 0);
    if (!changed && key == painted) return;
    painted = key;

    /* 蜂窝标签上那一行 */
    if (s_tile_sub[SUB_APN]) {
        if (!n->apn_known) snprintf(buf, sizeof buf, "%s", n->err[0] ? "—" : "");
        else snprintf(buf, sizeof buf, "%s · %s", n->apn_in_use.name[0] ? n->apn_in_use.name : "未拨号",
                      n->apn_manual ? "手动" : "自动");
        set_label_fmt(s_tile_sub[SUB_APN], c_row, sizeof c_row, "%s", buf);
    }

    if (!n->apn_known) snprintf(buf, sizeof buf, "%s", n->err[0] ? "读不到 APN" : "读取中…");
    else snprintf(buf, sizeof buf, "%s", n->apn_in_use.name[0] ? n->apn_in_use.name : "没有拨号");
    set_label_fmt(s_apn_now, c_now, sizeof c_now, "%s", buf);
    if (!n->apn_known) buf[0] = 0;
    else if (n->apn_in_use.id[0])
        snprintf(buf, sizeof buf, "%s%s%s · %s", n->apn_in_use.apn, n->apn_in_use.pdp ? " · " : "",
                 apn_pdp(n->apn_in_use.pdp), n->apn_manual ? "手动指定" : "自动选择");
    else snprintf(buf, sizeof buf, "%s", n->apn_manual ? "手动模式" : "自动模式");
    set_label_fmt(s_apn_now_sub, c_nsub, sizeof c_nsub, "%s", buf);

    int rows = 0;
    for (int i = 0; i < APN_ROWS; i++) {
        int show = i == 0 || (n->apn_known && i <= n->napns);
        uk_show(s_apn_row[i], show);
        if (!show) continue;
        const ni_apn_t *a = i ? &n->apns[i - 1] : NULL;
        int sel = apn_row_current(n, i);
        const char *tag = "";
        uint32_t tag_col = T->t3;
        if (i == 0) {
            set_label_fmt(s_apn_name[i], c_n[i], sizeof c_n[i], "%s", "自动");
            set_label_fmt(s_apn_sub[i], c_s[i], sizeof c_s[i], "%s", "按 SIM 卡自动选");
        } else {
            set_label_fmt(s_apn_name[i], c_n[i], sizeof c_n[i], "%s", a->name[0] ? a->name : a->id);
            snprintf(buf, sizeof buf, "%s%s%s", a->apn, a->pdp ? " · " : "", apn_pdp(a->pdp));
            set_label_fmt(s_apn_sub[i], c_s[i], sizeof c_s[i], "%s", buf);
            if (a->in_use) { tag = "在用"; tag_col = T->okT; }
        }
        if (i == 0 && sel && n->apn_in_use.id[0]) { tag = "在用"; tag_col = T->okT; }
        if (s_apn_pend == i) { tag = "切换中…"; tag_col = T->accT; }
        if (arm == i) { tag = "再点一次确认"; tag_col = T->warnT; }
        uk_bg(s_apn_row[i], T->washW);
        lv_obj_set_style_bg_opa(s_apn_row[i], arm == i ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        uk_bg(s_apn_mark[i], T->fillBlue);
        lv_obj_set_style_bg_opa(s_apn_mark[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(s_apn_mark[i], lv_color_hex(sel ? T->fillBlue : T->t3), 0);
        lv_obj_set_y(s_apn_row[i], rows * APN_ROW_H);
        rows++;
        set_label_fmt(s_apn_tag[i], c_t[i], sizeof c_t[i], "%s", tag);
        uk_text_color(s_apn_tag[i], tag_col);
    }

    uint32_t col = T->t3;
    if (s_apn_pend >= 0)
        { snprintf(buf, sizeof buf, "正在切换到「%s」…数据连接会重拨，断几秒", s_apn_pend_name); col = T->accT; }
    else if (net_flash_on(&s_apn_flash))
        { snprintf(buf, sizeof buf, "%s", s_apn_flash.txt); col = s_apn_flash.col; }
    else if (arm == 0)
        { snprintf(buf, sizeof buf, "回到自动：按 SIM 卡选 APN，会断网几秒"); col = T->warnT; }
    else if (arm > 0 && arm <= n->napns)
        { snprintf(buf, sizeof buf, "改用「%s」：会断网几秒，之后换卡也一直用它，点「自动」才恢复",
                   n->apns[arm - 1].name); col = T->warnT; }
    else if (!n->apn_known)
        snprintf(buf, sizeof buf, "%s", n->err[0] ? n->err : "读取中…");
    else if (!n->napns)
        snprintf(buf, sizeof buf, "还没有手动 APN。要用自定义 APN，先在管理网页里新建");
    else if (n->apn_manual)
        snprintf(buf, sizeof buf, "手动：一直用选中的这条，换卡也不变");
    else
        snprintf(buf, sizeof buf, "点一条手动 APN 可以改用它（两下确认）");
    set_label_fmt(s_apn_note, c_note, sizeof c_note, "%s", buf);
    uk_text_color(s_apn_note, col);

    int ch = rows * APN_ROW_H + APN_NOTE_H;
    lv_obj_set_height(s_apn_card, ch);
    lv_obj_set_y(s_apn_note, rows * APN_ROW_H + 8);
    int y = 24 + UK_HERO_H + 10 + 20 + ch + 10;
    lv_obj_set_y(s_apn_foot, y);
    uk_scroll_extent(s_apn_scroll, y + 40 + 16);
}

static void net_paint(int changed)
{
    static char c_err[100], c_key[2][24], c_ip[2][48], c_sub[2][160], c_op[4][96], c_st[200];
    static char c_on[NI_MAX_OPS][64], c_od[NI_MAX_OPS][64], c_ot[NI_MAX_OPS][24];
    static char c_ns[64], c_nl[NI_MAX_CELLS][48], c_nr[NI_MAX_CELLS][24], c_sb[24], c_ab[24], c_nb[24];
    const netinfo_t *n = netinfo_get();
    const char *aerr = netinfo_action_error();
    int arm = net_armed(s_net_arm_scan) ? 100 : net_armed(s_net_arm_auto) ? 101 :
              net_armed(s_net_arm_nbr) ? 102 :
              (s_net_arm_sc_idx >= 0 && net_armed(s_net_arm_sc)) ? 200 + s_net_arm_sc_idx :
              (s_net_arm_idx >= 0 && net_armed(s_net_arm_op)) ? s_net_arm_idx : -1;
    int busy = net_busy(n);
    char buf[256];

    apn_paint(changed);

    /* 情景切换有没有真的过去 */
    if (s_sc_pend >= 0) {
        int done = s_sc_pend == 0 ? !n->scene_pin[0]
                                  : !strcmp(n->scene_pin, s_sc_pend_id) && !strcmp(n->scene_current, s_sc_pend_id);
        if (done) {
            if (s_sc_pend == 0)
                net_flash(&s_sc_flash, T->okT, 6000, "已恢复自动判断 · 现在是「%s」",
                          n->scene_current[0] ? net_scene_name(n, n->scene_current) : "判定中");
            else
                net_flash(&s_sc_flash, T->okT, 6000, "已切换到「%s」，一直保持到你点「自动」", s_sc_pend_name);
            s_sc_pend = -1;
        } else if (lv_tick_get() - s_sc_pend_at > SC_PEND_MS) {
            if (s_sc_pend && !strcmp(n->scene_pin, s_sc_pend_id))
                net_flash(&s_sc_flash, T->warnT, 10000, "已固定「%s」，但设备还没切过去；Wi-Fi 看门狗可能正在接管，过一会再看",
                          s_sc_pend_name);
            else
                net_flash(&s_sc_flash, T->warnT, 10000, "设备没确认这次切换，再试一次");
            s_sc_pend = -1;
        }
    }
    /* 画的依据不只是数据：待确认、切换中、临时提示变了也要重画 */
    int key = arm + (s_sc_pend >= 0 ? 1000 + s_sc_pend : 0) +
              (net_flash_on(&s_sc_flash) ? 10000 : 0) + (net_flash_on(&s_ms_flash) ? 20000 : 0);
    if (!changed && key == s_net_painted_arm) return;
    s_net_painted_arm = key;
    if (arm < 0) { s_net_arm_idx = -1; s_net_arm_sc_idx = -1; }

    set_label_fmt(s_net_err, c_err, sizeof c_err, "%s", n->err);

    /* 情景：自动 + 各情景（可固定） */
    static char c_scn[NET_SCENE_ROWS][48], c_sct[NET_SCENE_ROWS][32], c_scnote[160];
    static char c_scw[NET_SCENE_ROWS][120], c_scd[NET_SCENE_ROWS][120];
    int scene_px = 0;
    {
        const char *cur_name = "";
        int cur_abroad = 0;
        for (int i = 0; i < n->nscenes; i++)
            if (!strcmp(n->scenes[i].id, n->scene_current)) { cur_name = n->scenes[i].name; cur_abroad = n->scenes[i].abroad; }
        (void)cur_abroad;
        int usable = n->scene_known && n->scene_enabled && n->nscenes > 0;
        for (int i = 0; i < NET_SCENE_ROWS; i++) {
            int show = 0;
            const char *name = "", *tag = "", *when = "", *does = "";
            uint32_t tag_col = T->t3;
            char buf2[48];
            if (i == 0) {
                show = 1;
                name = "自动";
                if (usable) { when = "按下面每个情景的条件自己切换"; does = "条件变了，1–2 分钟内跟着换"; }
                if (!usable) tag = "—";
                else if (!n->scene_pin[0]) {
                    snprintf(buf2, sizeof buf2, "现在：%s", cur_name[0] ? cur_name : "判定中");
                    tag = buf2; tag_col = T->accT;
                }
            } else if (i <= n->nscenes && usable) {
                const ni_scene_t *sc = &n->scenes[i - 1];
                show = 1;
                name = sc->name[0] ? sc->name : sc->id;
                when = sc->when;
                does = sc->does;
                if (!strcmp(n->scene_pin, sc->id)) { tag = "已固定"; tag_col = T->accT; }
                else if (!strcmp(n->scene_current, sc->id)) { tag = "现在"; tag_col = T->accT; }
                else if (sc->wifi_off && !does[0]) tag = "会关 Wi-Fi";
            }
            int is_exit = i == NET_SCENE_ROWS - 1;
            int sel = usable && !is_exit &&
                      (i == 0 ? !n->scene_pin[0] : i <= n->nscenes && !strcmp(n->scene_pin, n->scenes[i - 1].id));
            if (s_sc_pend == i) { tag = "切换中…"; tag_col = T->accT; }
            if (arm == 200 + i) { tag = "再点一次确认"; tag_col = T->warnT; }
            uk_show(s_net_sc_row[i], show);
            if (!show) continue;
            /* 待确认：整行浅黄底（不用彩色左边框，DESIGN.md §6） */
            uk_bg(s_net_sc_row[i], T->washW);
            lv_obj_set_style_bg_opa(s_net_sc_row[i], arm == 200 + i ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            /* 单选圈：实心 = 现在生效的那个（没固定时是「自动」） */
            uk_show(s_net_sc_mark[i], usable && !is_exit);
            uk_bg(s_net_sc_mark[i], T->fillBlue);
            lv_obj_set_style_bg_opa(s_net_sc_mark[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(s_net_sc_mark[i], lv_color_hex(sel ? T->fillBlue : T->t3), 0);
            lv_obj_set_x(s_net_sc_name[i], usable && !is_exit ? UK_PAD + 26 : UK_PAD);
            /* 有说明的行三行高，圈对齐第一行；没说明的（读不到时）一行高 */
            /* 条件那行放不下就折成两行（Wi-Fi 名可以很长），再长末尾「…」 */
            int lh = lv_font_get_line_height(UF.cj12), wl = 1;
            if (when[0]) {
                lv_point_t sz;
                lv_text_get_size(&sz, when, UF.cj12, 0, 0, UK_CARD_W - 2 * UK_PAD - 26, LV_TEXT_FLAG_NONE);
                if (sz.y > lh) wl = 2;
            }
            int rh = !when[0] && !does[0] ? UK_ROW_H : 32 + (when[0] ? wl * lh : 0) + (does[0] ? lh + 2 : 0) + 8;
            lv_obj_set_height(s_net_sc_row[i], rh);
            lv_obj_set_y(s_net_sc_mark[i], rh == UK_ROW_H ? (UK_ROW_H - 16) / 2 : 12);
            lv_obj_set_height(s_net_sc_when[i], wl * lh);
            lv_obj_set_y(s_net_sc_does[i], 32 + wl * lh + 2);
            set_label_fmt(s_net_sc_when[i], c_scw[i], sizeof c_scw[i], "%s", when);
            set_label_fmt(s_net_sc_does[i], c_scd[i], sizeof c_scd[i], "%s", does[0] ? does : "");
            uk_show(s_net_sc_when[i], when[0] != 0);
            uk_show(s_net_sc_does[i], does[0] != 0);
            lv_obj_set_y(s_net_sc_row[i], scene_px);
            scene_px += rh;
            set_label_fmt(s_net_sc_name[i], c_scn[i], sizeof c_scn[i], "%s", name);
            set_label_fmt(s_net_sc_tag[i], c_sct[i], sizeof c_sct[i], "%s", tag);
            uk_text_color(s_net_sc_tag[i], tag_col);
        }
        uint32_t note_col = T->t3;
        if (!n->scene_known)
            snprintf(buf, sizeof buf, "%s", n->err[0] ? "读不到情景" : "读取中…");
        else if (!n->scene_enabled)
            snprintf(buf, sizeof buf, "情景引擎已停用，在管理网页的「情景」页打开");
        else if (!n->nscenes)
            snprintf(buf, sizeof buf, "还没配置情景，在管理网页的「情景」页设置");
        else if (s_sc_pend > 0)
            { snprintf(buf, sizeof buf, "正在切换到「%s」…%s", s_sc_pend_name,
                       s_sc_pend_wifi_off ? "会关掉 Wi-Fi" : "开关 Wi-Fi 要 10–30 秒"); note_col = T->accT; }
        else if (s_sc_pend == 0)
            { snprintf(buf, sizeof buf, "正在恢复自动判断…"); note_col = T->accT; }
        else if (net_flash_on(&s_sc_flash))
            { snprintf(buf, sizeof buf, "%s", s_sc_flash.txt); note_col = s_sc_flash.col; }
        else if (n->scene_takeover && n->scene_pin[0])
            { snprintf(buf, sizeof buf, "Wi-Fi 看门狗接管中，固定暂时不生效，先按「外出」"); note_col = T->warnT; }
        else if (arm == 200)
            { snprintf(buf, sizeof buf, "恢复自动：按位置和 SIM 重新判断\n可能会开关 Wi-Fi"); note_col = T->warnT; }
        else if (arm > 200 && arm - 200 <= n->nscenes) {
            const ni_scene_t *sc = &n->scenes[arm - 201];
            snprintf(buf, sizeof buf, "切到「%s」：%s\n之后一直固定，点「自动」才恢复",
                     sc->name[0] ? sc->name : sc->id,
                     sc->wifi_off ? "会关掉 Wi-Fi" : sc->abroad ? "按国外设置" : "Wi-Fi 开着");
            note_col = T->warnT;
        }
        else if (n->scene_pin[0])
            snprintf(buf, sizeof buf, "手动固定后一直保持（重启也是），点「自动」才恢复自动判断");
        else
            snprintf(buf, sizeof buf, "情景只管 Wi-Fi 这些，不碰蜂窝网络。点一个情景就固定在它，点「自动」恢复");
        set_label_fmt(s_net_sc_note, c_scnote, sizeof c_scnote, "%s", buf);
        uk_text_color(s_net_sc_note, note_col);
    }

    /* 出口：蜂窝直连出去的公网 IP，一行（第二行备用，不显示） */
    int two = 0;
    for (int i = 0; i < 1; i++) {
        const ni_exit_t *e = &n->direct;
        const char *key = "出口 IP";
        char extra[80] = "";
        snprintf(extra, sizeof extra, "%s", e->isp);
        net_exit_sub(buf, sizeof buf, e, extra);
        set_label_fmt(s_net_ex_key[i], c_key[i], sizeof c_key[i], "%s", key);
        set_label_fmt(s_net_ex_ip[i], c_ip[i], sizeof c_ip[i], "%s",
                      e->ip[0] ? e->ip : (e->present || n->err[0]) ? "—" : "查询中…");
        set_label_fmt(s_net_ex_sub[i], c_sub[i], sizeof c_sub[i], "%s", buf);
    }
    uk_show(s_net_ex_row[1], two);

    /* 运营商 */
    net_oper_text(buf, sizeof buf, &n->home);
    set_label_fmt(s_net_op[0], c_op[0], sizeof c_op[0], "%s", buf);
    net_oper_text(buf, sizeof buf, &n->serving);
    set_label_fmt(s_net_op[1], c_op[1], sizeof c_op[1], "%s", buf);
    set_label_fmt(s_net_op[2], c_op[2], sizeof c_op[2], "%s", n->roaming > 0 ? "漫游中" : n->roaming == 0 ? "本地" : "—");
    uk_text_color(s_net_op[2], n->roaming > 0 ? T->warnT : T->t1);
    if (!strcmp(n->guard_phase, "registering"))
        snprintf(buf, sizeof buf, "正在注册 %s…", n->guard_target);
    else if (!strcmp(n->guard_phase, "reverting"))
        snprintf(buf, sizeof buf, "正在恢复自动…");
    else
        snprintf(buf, sizeof buf, "%s", !strcmp(n->selection, "auto") ? "自动" : !strcmp(n->selection, "manual") ? "手动" : "—");
    set_label_fmt(s_net_op[3], c_op[3], sizeof c_op[3], "%s", buf);

    /* 手动选网：状态一句话，按「正在发生的事」优先 */
    uint32_t st_col = T->t2;
    if (net_flash_on(&s_ms_flash))
        { snprintf(buf, sizeof buf, "%s", s_ms_flash.txt); st_col = s_ms_flash.col; }
    else if (!strcmp(n->guard_phase, "registering"))
        snprintf(buf, sizeof buf, "正在注册到 %s。没注册上会自动回到自动选网。", n->guard_target);
    else if (!strcmp(n->guard_phase, "reverting"))
        snprintf(buf, sizeof buf, "正在回到自动选网…");
    else if (!strcmp(n->scan_state, "scanning"))
        snprintf(buf, sizeof buf, "正在搜索，数据连接会断 1–3 分钟…");
    else if (arm == 100)
        { snprintf(buf, sizeof buf, "搜索时会断网 1–3 分钟，再点一次开始。"); st_col = T->warnT; }
    else if (arm == 101)
        { snprintf(buf, sizeof buf, "回到自动选网，可能短暂断网，再点一次确认。"); st_col = T->warnT; }
    else if (arm >= 0 && arm < n->nops)   /* 只有运营商行；情景（200+）、按钮（100+）不是 */
        { snprintf(buf, sizeof buf, "再点一次注册到 %s。没注册上会自动回到自动选网。",
                   n->ops[arm].name[0] ? n->ops[arm].name : n->ops[arm].plmn); st_col = T->warnT; }
    else if (aerr[0])
        { snprintf(buf, sizeof buf, "%s", aerr); st_col = T->badT; }
    else if (!strcmp(n->guard_phase, "revert_failed"))
        { snprintf(buf, sizeof buf, "%s。重启设备也会回到自动选网。", n->guard_reason); st_col = T->badT; }
    else if (!strcmp(n->guard_phase, "reverted"))
        snprintf(buf, sizeof buf, !strcmp(n->guard_reason, "手动恢复自动") ? "已回到自动选网。" : "%s，已回到自动选网。",
                 n->guard_reason);
    else if (!strcmp(n->guard_phase, "ok"))
        { snprintf(buf, sizeof buf, "已注册到 %s。要回自动选网点「恢复自动」。", n->guard_target); st_col = T->okT; }
    else if (!strcmp(n->scan_state, "error"))
        { snprintf(buf, sizeof buf, "搜索失败：%s", n->scan_err); st_col = T->badT; }
    else if (!strcmp(n->scan_state, "done"))
        snprintf(buf, sizeof buf, "点一个网络，再点一次确认注册。");
    else
        snprintf(buf, sizeof buf, "手动指定注册的网络。搜索会断网 1–3 分钟。");
    /* 一句话放不下时，句号会单独掉到第二行：不要句号 */
    size_t bl = strlen(buf);
    if (bl >= 3 && !strcmp(buf + bl - 3, "。")) buf[bl - 3] = 0;
    set_label_fmt(s_net_status, c_st, sizeof c_st, "%s", buf);
    uk_text_color(s_net_status, st_col);

    int ops = (!strcmp(n->scan_state, "done") && !busy) ? n->nops : 0;
    for (int i = 0; i < NI_MAX_OPS; i++) {
        const ni_scan_op_t *o = &n->ops[i];
        if (i >= ops) { uk_show(s_net_opr[i], 0); continue; }
        uk_show(s_net_opr[i], 1);
        set_label_fmt(s_net_opr_name[i], c_on[i], sizeof c_on[i], "%s", o->name[0] ? o->name : o->plmn);
        snprintf(buf, sizeof buf, "%s · %s%s%s", o->plmn, net_rat_name(o->rat),
                 o->country[0] ? " · " : "", o->country);
        set_label_fmt(s_net_opr_det[i], c_od[i], sizeof c_od[i], "%s", buf);
        const char *tag = arm == i ? "再点一次确认" : !strcmp(o->status, "2") ? "当前" : !strcmp(o->status, "3") ? "禁止" : "";
        uk_bg(s_net_opr[i], T->washW);
        lv_obj_set_style_bg_opa(s_net_opr[i], arm == i ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        set_label_fmt(s_net_opr_tag[i], c_ot[i], sizeof c_ot[i], "%s", tag);
        uk_text_color(s_net_opr_tag[i], arm == i ? T->warnT : T->t3);
        uk_text_color(s_net_opr_name[i], !strcmp(o->status, "3") ? T->t3 : T->t1);
    }
    set_label_fmt(s_net_scan_lbl, c_sb, sizeof c_sb, "%s", arm == 100 ? "再点开始" : "搜索网络");
    uk_button_kind(s_net_scan_btn, s_net_scan_lbl, arm == 100 ? UK_BTN_ARMED : UK_BTN_PLAIN);
    set_label_fmt(s_net_auto_lbl, c_ab, sizeof c_ab, "%s", arm == 101 ? "再点确认" : "恢复自动");
    uk_button_kind(s_net_auto_btn, s_net_auto_lbl, arm == 101 ? UK_BTN_ARMED : UK_BTN_PLAIN);

    /* 邻小区。原厂扫描会断网且拿不到数据（2026-09-25 实测），agent 报
     * unsupported：不给按钮，写明原因 */
    int nbr_off = !strcmp(n->nbr_state, "unsupported");
    uk_show(s_net_nbr_btn, !nbr_off);
    lv_obj_set_width(s_net_nbr_state, UK_CARD_W - 2 * UK_PAD - (nbr_off ? 0 : 80));
    if (nbr_off)
        snprintf(buf, sizeof buf, "%s", n->nbr_err[0] ? n->nbr_err : "本机暂时读不到邻区");
    else if (!strcmp(n->nbr_state, "scanning"))
        snprintf(buf, sizeof buf, "扫描中…");
    else if (arm == 102)
        snprintf(buf, sizeof buf, "可能短暂影响网速");
    else if (!strcmp(n->nbr_state, "error"))
        snprintf(buf, sizeof buf, "扫描失败");
    else if (n->nbr_at > 0) {
        time_t tt = (time_t)n->nbr_at;   /* 设备时钟 = 当地时间标成 UTC，localtime 给出对的数字 */
        struct tm tm;
        char hm[8];
        localtime_r(&tt, &tm);
        strftime(hm, sizeof hm, "%H:%M", &tm);
        snprintf(buf, sizeof buf, "%s 扫描 · %d 个", hm, n->ncells);
    } else
        snprintf(buf, sizeof buf, "还没扫过，点「扫描」读一次");
    set_label_fmt(s_net_nbr_state, c_ns, sizeof c_ns, "%s", buf);
    uk_text_color(s_net_nbr_state, arm == 102 ? T->warnT : T->t2);
    set_label_fmt(s_net_nbr_lbl, c_nb, sizeof c_nb, "%s", arm == 102 ? "再点开始" : "扫描");
    uk_button_kind(s_net_nbr_btn, s_net_nbr_lbl, arm == 102 ? UK_BTN_ARMED : UK_BTN_PLAIN);
    for (int i = 0; i < NI_MAX_CELLS; i++) {
        const ni_cell_t *cl = &n->cells[i];
        if (i >= n->ncells) { uk_show(s_net_nbr_row[i], 0); continue; }
        uk_show(s_net_nbr_row[i], 1);
        set_label_fmt(s_net_nbr_l[i], c_nl[i], sizeof c_nl[i], "%-3s PCI %s  %s", cl->rat, cl->pci, cl->arfcn);
        set_label_fmt(s_net_nbr_r[i], c_nr[i], sizeof c_nr[i], "%s%s", cl->rsrp[0] ? cl->rsrp : "-", cl->rsrp[0] ? " dBm" : "");
    }

    /* 已连接设备流量 */
    {
        static char c_cls[96], c_cn[NI_MAX_CLIENTS][48], c_ct[NI_MAX_CLIENTS][40], c_cs[NI_MAX_CLIENTS][96];
        int k = n->nclients;
        s_net_ncl = k;
        uk_show(s_net_cl_state, k == 0);
        set_label_fmt(s_net_cl_state, c_cls, sizeof c_cls, "%s",
                      !n->clients_known ? "读取中…" : "现在没有 Wi-Fi 设备连着（网线和 USB 连的设备不在这里）");
        for (int i = 0; i < NI_MAX_CLIENTS; i++) {
            const ni_client_t *cl = &n->clients[i];
            char a[16], b[16], ra[16], rb[16];
            if (i >= k) { uk_show(s_net_cl_row[i], 0); continue; }
            uk_show(s_net_cl_row[i], 1);
            set_label_fmt(s_net_cl_name[i], c_cn[i], sizeof c_cn[i], "%s", cl->name[0] ? cl->name : cl->ip[0] ? cl->ip : cl->mac);
            fmt_bytes_total(a, sizeof a, (long)cl->down);
            fmt_bytes_total(b, sizeof b, (long)cl->up);
            set_label_fmt(s_net_cl_tot[i], c_ct[i], sizeof c_ct[i], "\xE2\x86\x93%s \xE2\x86\x91%s", a, b);
            if (cl->down_rate >= 0) fmt_rate_top(ra, sizeof ra, cl->down_rate, s_cf_speed_bits, 0); else snprintf(ra, sizeof ra, "-");
            if (cl->up_rate >= 0)   fmt_rate_top(rb, sizeof rb, cl->up_rate, s_cf_speed_bits, 0);   else snprintf(rb, sizeof rb, "-");
            /* 「5 GHz · ↓12K/s ↑3K/s · 信号很好」：频段在前（2.4 还是 5，2026-09-25 用户要的），
             * 信号说成话；Wi-Fi 几代和协商速率放不下，管理网页的已连设备页有 */
            char band[16] = "", sig[24] = "";
            if (cl->band[0]) snprintf(band, sizeof band, "%s · ", cl->band);
            if (cl->signal)
                snprintf(sig, sizeof sig, " · %s", cl->signal >= -55 ? "信号很好" : cl->signal >= -67 ? "信号好" :
                                                   cl->signal >= -75 ? "信号一般" : "信号弱");
            if (cl->down_rate < 0 && cl->up_rate < 0)
                set_label_fmt(s_net_cl_sub[i], c_cs[i], sizeof c_cs[i], "%s速率稍后显示%s", band, sig);
            else
                set_label_fmt(s_net_cl_sub[i], c_cs[i], sizeof c_cs[i], "%s\xE2\x86\x93%s/s \xE2\x86\x91%s/s%s", band, ra, rb, sig);
        }
    }

    net_reflow(n->err[0] ? 22 : 0, scene_px, two ? 2 : 1, ops, n->ncells);
}

static void tile_click_cb(lv_event_t *e)
{
    sub_open((int)(intptr_t)lv_event_get_user_data(e));
}


/* 标签页上的一张「›」行卡片：每行开一个二级页，右边是 refresh_cb 写的状态字。 */
static lv_obj_t *s_nav_sec, *s_nav_card;   /* 最近一张（出口标签要跟着出口 IP 挪） */
static int nav_card(lv_obj_t *t, int y, const char *section, const int *ids, const char *const *names, int n)
{
    lv_obj_t *c;
    s_nav_sec = uk_section(t, y, section);
    s_nav_card = c = uk_card(t, UK_MARGIN, y + 20, UK_CARD_W, n * UK_ROW_H);
    for (int k = 0; k < n; k++)
        s_tile_sub[ids[k]] = uk_row_nav(c, k * UK_ROW_H, names[k], k == 0, tile_click_cb, (void *)(intptr_t)ids[k]);
    return y + 20 + n * UK_ROW_H + 10;
}

/* 蜂窝：跟这张卡、这个运营商有关的都在这里。网络模式直接在标签上切（两下确认）。 */
/* ---- 移动数据 / 数据漫游（蜂窝页） ----
 * 两个开关都会断网或花钱：按一次只是「待确认」（整行说要做什么），5 秒内再按
 * 同一个才下发，和网络模式一样。下发后 8 秒内不拿读数覆盖开关（固件要重拨）。 */
enum { MD_DATA, MD_ROAM };
static lv_obj_t *s_md_sw[2], *s_md_st[2], *s_md_note;
static uint32_t s_md_arm, s_md_hold;
static int s_md_pending = -1, s_md_want;

static void md_note(const char *t, uint32_t col)
{
    static char c[96];
    snprintf(c, sizeof c, "%s", t);
    lv_label_set_text_static(s_md_note, c);
    uk_text_color(s_md_note, col);
}

static void md_sw_cb(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    int on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    uint32_t now = lv_tick_get();

    if (s_md_pending == id && s_md_want == on && s_md_arm && now - s_md_arm < 5000) {
        int data = id == MD_DATA ? on : s_aux_data != 0;
        int roam = id == MD_ROAM ? on : s_aux_roam == 1;
        char cmd[220];
        snprintf(cmd, sizeof cmd,
                 "ubus call zwrt_data set_wwaniface '{\"cid\":1,\"connect_mode\":1,\"enable\":%d,\"roam_enable\":%d}' >/dev/null 2>&1 &",
                 data, roam);
        system(cmd);
        if (id == MD_DATA) s_aux_data = on; else s_aux_roam = on;
        s_md_arm = 0;
        s_md_pending = -1;
        s_md_hold = now ? now : 1;
        md_note(on ? "已下发，正在拨号…" : "已下发，正在断开…", T->t2);
        return;
    }
    /* 第一下：开关先回原位，整行说清按第二下会怎样 */
    sw_apply(sw, !on);
    s_md_arm = now ? now : 1;
    s_md_pending = id;
    s_md_want = on;
    md_note(id == MD_DATA ? (on ? "再按一次：打开移动数据" : "再按一次：关掉移动数据，所有设备断网")
                          : (on ? "再按一次：打开数据漫游，按漫游计费" : "再按一次：关掉数据漫游，漫游时会断网"),
            T->warnT);
}

static void md_refresh(void)
{
    static char c_st[2][16];
    uint32_t now = lv_tick_get();
    if (!s_md_note) return;
    if (s_md_pending >= 0 && now - s_md_arm >= 5000) {   /* 没按第二下：作罢 */
        s_md_pending = -1;
        s_md_arm = 0;
        md_note("会断网或按漫游计费，切换要按两次确认", T->t3);
    }
    if (s_md_hold && now - s_md_hold >= 8000) {
        s_md_hold = 0;
        md_note("会断网或按漫游计费，切换要按两次确认", T->t3);
    }
    int v[2] = { s_aux_data, s_aux_roam };
    for (int i = 0; i < 2; i++) {
        if (s_md_pending != i && !s_md_hold) sw_apply(s_md_sw[i], v[i] == 1);
        set_label_fmt(s_md_st[i], c_st[i], sizeof c_st[i], "%s", v[i] < 0 ? "—" : v[i] ? "已开启" : "已关闭");
    }
}

static void build_cellular(lv_obj_t *t)
{
    static const int ids1[] = { SUB_ESIM, SUB_APN };
    static const char *const names1[] = { "SIM 与 eSIM", "APN" };
    static const int ids2[] = { SUB_NET, SUB_LOCK, SUB_CELL };
    static const char *const names2[] = { "运营商选择", "锁频", "小区信息" };
    static const int ids3[] = { SUB_SMS };
    static const char *const names3[] = { "短信" };
    t = s_cell_scroll = uk_scroll(t, 0, UI_VIEW_H, 1000);
    lv_obj_set_parent(s_ca_card, t);      /* 当前连接：每个载波（原首页载波明细） */
    lv_obj_set_pos(s_ca_card, UK_MARGIN, 4);
    t = s_cell_rest = lv_obj_create(t);
    lv_obj_remove_style_all(t);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(t, UK_W);
    int y = nav_card(t, 4, "SIM 卡", ids1, names1, 2);

    uk_section(t, y, "移动数据");
    lv_obj_t *mc = uk_card(t, UK_MARGIN, y + 20, UK_CARD_W, 2 * UK_ROW_H + 30);
    s_md_sw[MD_DATA] = toggle_row(mc, 0, "移动数据", 1, &s_md_st[MD_DATA], md_sw_cb, (void *)(intptr_t)MD_DATA);
    s_md_sw[MD_ROAM] = toggle_row(mc, UK_ROW_H, "数据漫游", 0, &s_md_st[MD_ROAM], md_sw_cb, (void *)(intptr_t)MD_ROAM);
    s_md_note = uk_label_w(mc, UF.cj12, T->t3, UK_PAD, 2 * UK_ROW_H + 6, UK_CARD_W - 2 * UK_PAD, 0,
                           "会断网或按漫游计费，切换要按两次确认");
    y += 20 + 2 * UK_ROW_H + 30 + 10;

    uk_section(t, y, "网络模式");
    lv_obj_t *md = uk_card(t, UK_MARGIN, y + 20, UK_CARD_W, 84);
    /* 顺序和取值照原厂网页（config.js AUTO_MODES）：5G NSA = LTE_AND_5G */
    static const char *const k_mode_lab[4] = { "自动", "5G NSA", "5G SA", "4G" };
    uk_seg(&s_lk_seg, md, UK_PAD, 12, UK_CARD_W - 2 * UK_PAD, k_mode_lab, 4, lk_mode_cb);
    for (int i = 0; i < 4; i++) s_lk_mode_btn[i] = s_lk_seg.item[i];
    s_lk_mode_lbl = uk_label_w(md, UF.cj12, T->t3, UK_PAD, 54, UK_CARD_W - 2 * UK_PAD, 0, "切换会短暂断网，需要按两次确认");
    y += 20 + 84 + 10;

    y = nav_card(t, y, "网络", ids2, names2, 3);
    y = nav_card(t, y, "消息", ids3, names3, 1);
    lv_obj_set_height(t, y);
    cell_reflow();
}

static void cell_reflow(void)
{
    if (!s_cell_rest) return;
    int y = 0, h = (int)lv_obj_get_style_height(s_ca_card, 0);
    if (!lv_obj_has_flag(s_ca_card, LV_OBJ_FLAG_HIDDEN)) y = 4 + h + UK_MARGIN - 4;
    lv_obj_set_y(s_cell_rest, y);
    uk_scroll_extent(s_cell_scroll, y + (int)lv_obj_get_style_height(s_cell_rest, 0) - 10 + UK_TAB_PAD);
}

/* 出口：流量从哪出去（Tailscale）、出去有多快。 */
static void build_exit(lv_obj_t *t)
{
    static const int ids[] = { SUB_TS, SUB_SPEED };
    static const char *const names[] = { "Tailscale", "测速" };
    t = s_nh_scroll[NH_EXIT] = uk_scroll(t, 0, UI_VIEW_H, 1000);
    s_nh_base[NH_EXIT] = 4;   /* 出口 IP（build_sub_net）在最上面，› 行跟在后面（net_reflow） */
    nav_card(t, 4, "连接与测速", ids, names, 2);
    s_exit_nav_sec = s_nav_sec;
    s_exit_nav_card = s_nav_card;
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
    lv_tileview_set_tile_by_index(s_tv, TAB_SYS, 0, LV_ANIM_OFF);
    sub_open(SUB_PERF);
    lv_timer_resume(s_bench_timer);
}
#endif

/* netinfo 里这台设备的 Wi-Fi 读数（按 MAC，没有 MAC 时按 IP） */
static const ni_client_t *wifi_station_for(const char *mac, const char *ip)
{
    const netinfo_t *n = netinfo_get();
    for (int k = 0; k < n->nclients; k++)
        if (mac[0] && n->clients[k].mac[0] && !strcasecmp(mac, n->clients[k].mac)) return &n->clients[k];
    for (int k = 0; k < n->nclients; k++)
        if (ip[0] && !strcmp(ip, n->clients[k].ip)) return &n->clients[k];
    return NULL;
}

/* 「5 GHz · ↓12K/s ↑3K/s · 信号很好」：频段在前（2.4 还是 5，2026-09-25 用户要的），
 * 信号说成话；Wi-Fi 几代和协商速率放不下，管理网页的已连设备页有 */
static void ni_client_line(char *out, size_t n, const ni_client_t *cl)
{
    char ra[16], rb[16], band[16] = "", sig[24] = "";
    if (cl->down_rate >= 0) fmt_rate_top(ra, sizeof ra, cl->down_rate, s_cf_speed_bits, 0); else snprintf(ra, sizeof ra, "-");
    if (cl->up_rate >= 0)   fmt_rate_top(rb, sizeof rb, cl->up_rate, s_cf_speed_bits, 0);   else snprintf(rb, sizeof rb, "-");
    if (cl->band[0]) snprintf(band, sizeof band, "%s · ", cl->band);
    if (cl->signal)
        snprintf(sig, sizeof sig, " · %s", cl->signal >= -55 ? "信号很好" : cl->signal >= -67 ? "信号好" :
                                           cl->signal >= -75 ? "信号一般" : "信号弱");
    if (cl->down_rate < 0 && cl->up_rate < 0) snprintf(out, n, "%s速率稍后显示%s", band, sig);
    else snprintf(out, n, "%s\xE2\x86\x93%s/s \xE2\x86\x91%s/s%s", band, ra, rb, sig);
}

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
    int cli_y = 0;
    for (int i = 0; i < WIFI_MAX_CLI; i++) {
        if (i >= n) { uk_show(s_w_cli[i], 0); continue; }
        lv_obj_remove_flag(s_w_cli[i], LV_OBJ_FLAG_HIDDEN);
        set_label_fmt(s_w_cli_name[i], c_cli_name[i], sizeof c_cli_name[i], "%s",
                      d->client[i].name[0] ? d->client[i].name : "?");
        set_label_fmt(s_w_cli_ip[i], c_cli_ip[i], sizeof c_cli_ip[i], "%s", d->client[i].ip);
        /* 这台是 Wi-Fi 设备：第二行换成「5 GHz · ↓… ↑… · 信号很好」，右下是总流量
         * （原情景·网络页的设备流量，2026-09-25 并到这里）；网线 / USB 设备照旧写 MAC */
        const ni_client_t *w = wifi_station_for(d->client[i].mac, d->client[i].ip);
        static char c_tot[WIFI_MAX_CLI][40];
        if (w) {
            char line[96], a[16], b[16];
            ni_client_line(line, sizeof line, w);
            set_label_fmt(s_w_cli_mac[i], c_cli_mac[i], sizeof c_cli_mac[i], "%s", line);
            fmt_bytes_total(a, sizeof a, (long)w->down);
            fmt_bytes_total(b, sizeof b, (long)w->up);
            set_label_fmt(s_w_cli_tot[i], c_tot[i], sizeof c_tot[i], "连上以来 \xE2\x86\x93%s \xE2\x86\x91%s", a, b);
        } else {
            set_label_fmt(s_w_cli_mac[i], c_cli_mac[i], sizeof c_cli_mac[i], "%s", d->client[i].mac);
            set_label_fmt(s_w_cli_tot[i], c_tot[i], sizeof c_tot[i], "%s", "");
        }
        /* Wi-Fi 设备多一行总流量：行高跟着变 */
        int rh = w ? WIFI_CLI_H + 18 : WIFI_CLI_H;
        lv_obj_set_y(s_w_cli[i], cli_y);
        lv_obj_set_height(s_w_cli[i], rh);
        cli_y += rh;
    }
    /* Reflow: the card shrinks to the rows shown, DHCP follows. */
    uk_show(s_w_cli_empty, n == 0);
    int cli_h = n ? cli_y : UK_ROW_H;
    lv_obj_set_height(s_w_cli_card, cli_h);
    int dy = lv_obj_get_style_y(s_w_cli_card, 0) + cli_h + 10;
    lv_obj_set_y(s_w_dhcp_sec, dy);
    lv_obj_set_y(s_w_dhcp_card, dy + 20);
    int base = dy + 20 + 3 * UK_ROW_H + 10;
    if (base != s_nh_base[NH_WIFI]) { s_nh_base[NH_WIFI] = base; net_relayout(); }

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
/* eSIM 列表。定时刷新在数据变了时画，点一下也当场画（esim_row_cb）。 */
static void esim_paint(void)
{
    s_es_dirty = 0;
    int n = esim_profile_count();

    int plain = s_sim.kind == UI_SIM_PLAIN, none = s_sim.kind == UI_SIM_NONE;
    const char *cur = esim_current();
    char tail[16] = "";
    {
        size_t l = strlen(s_sim.iccid);
        while (l && (s_sim.iccid[l - 1] == 'F' || s_sim.iccid[l - 1] == 'f')) l--;
        if (l >= 4) snprintf(tail, sizeof tail, "尾号 %.4s", s_sim.iccid + l - 4);
    }
    /* 顶上一块写的是「现在用的这张卡」：实体 SIM 写运营商，eSIM 写配置名 */
    lv_label_set_text(s_es_hero.st, none ? "没有卡" : plain ? "使用中 · 实体 SIM 卡" : "使用中 · eSIM");
    lv_label_set_text(s_es_cur, none ? "没插卡" : plain ? (s_sim.oper[0] ? s_sim.oper : "SIM 卡")
                                                        : (cur[0] && strcmp(cur, "-") ? cur : "—"));
    if (net_flash_on(&s_es_flash)) {
        lv_label_set_text(s_es_state, s_es_flash.txt);
        uk_text_color(s_es_state, s_es_flash.col);
    } else {
        lv_label_set_text(s_es_state, none ? "插上 SIM 卡或 eSIM 卡后这里显示卡信息"
                                     : plain ? (s_sim.msisdn[0] ? s_sim.msisdn : "号码没写在卡里") : esim_state());
        uk_text_color(s_es_state, T->t2);
    }
    lv_label_set_text(s_es_hero.rtop, plain ? tail : "");
    lv_label_set_text(s_es_info[0], s_sim.msisdn[0] ? s_sim.msisdn : "—");
    lv_label_set_text(s_es_info[1], s_sim.iccid[0] ? s_sim.iccid : "—");
    lv_label_set_text(s_es_info[2], s_sim.imsi[0] ? s_sim.imsi : "—");
    lv_label_set_text(s_es_list_sec, plain && n ? "eSIM 配置（现在没在用）" : "eSIM 配置");
    uk_show(s_es_empty, n == 0);
    if (n == 0) {
        const char *st = esim_state();
        lv_label_set_text(s_es_empty, plain ? "现在插的是普通 SIM 卡，没有 eSIM 配置。换成 eSIM 卡后可以在这里切换。"
                                     : !esim_loaded() && strcmp(st, "就绪") == 0 ? "读取中…"
                                     : strcmp(st, "就绪") ? st : "还没有 eSIM 配置 · 用管理网页添加");
    }
    for (int i = 0; i < ESIM_MAX_ROWS; i++) {
        if (i >= n) { uk_show(s_es_row[i], 0); continue; }
        esim_profile_t p;
        esim_get_profile(i, &p);
        uk_show(s_es_row[i], 1);
        lv_label_set_text(s_es_row_name[i], p.name);
        lv_label_set_text(s_es_row_sub[i], p.sub);
        if (p.enabled && !plain) lv_label_set_text(s_es_hero.rtop, p.sub);
        lv_label_set_text(s_es_row_tag[i],
            p.going ? "切换中…" : p.armed ? "再点一次确认切换" : p.enabled ? (plain ? "已启用" : "使用中") : "");
        /* 正在用的、切换中的也能点：esim_row_cb 会说一句为什么不动 */
        uk_bg(s_es_row[i], p.armed ? T->fillOrange : p.enabled ? T->accS : T->card);
        uint32_t fg = p.armed ? 0xffffff : T->t1;
        uk_text_color(s_es_row_name[i], fg);
        uk_text_color(s_es_row_sub[i], p.armed ? T->onFill : T->t3);
        uk_text_color(s_es_row_tag[i], p.armed ? 0xffffff : p.enabled ? T->accT : T->t2);
    }
    lv_obj_set_height(s_es_list_card, n ? n * ESIM_ROW_H : UK_ROW_H);
}

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
    if (s_vendor_arm && lv_tick_get() - s_vendor_arm >= 5000) {
        s_vendor_arm = 0;
        uk_button_kind(s_vendor_btn, s_vendor_lbl, UK_BTN_PLAIN);
        lv_label_set_text(s_vendor_lbl, "切换到原厂界面");
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

    /* 网络页只靠 zte-agent，不靠 datad：放在 datad 的提前返回之前。首页的
     * 网络卡也读它（30 秒一次、精简读取），画在后面首页那段。 */
    {
        /* 只有运营商选择页做完整读取（agent 会读 AT+COPS?）；别的页只要它显示的那部分 */
        int ni_mode = sub_visible(SUB_NET) ? NI_PAGE :
                      (sub_visible(SUB_APN) || tab_visible(TAB_CELL)) ? NI_APN :
                      tab_visible(TAB_WIFI) ? NI_CLIENTS :
                      (sub_visible(SUB_SCENE) || sub_visible(SUB_CELL) || tab_visible(TAB_EXIT)) ? NI_LITE :
                      tab_visible(TAB_HOME) ? NI_HOME : NI_OFF;
        int paint = ni_mode != NI_OFF && ni_mode != NI_HOME;
        static int ni_last;
        int ni_changed = netinfo_poll(ni_mode) || (paint && ni_last != ni_mode);
        ni_last = ni_mode;
        if (paint) net_paint(ni_changed);
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
        /* 和首页右边的「信号强/中/弱」同一套：4–5 格绿、3 格橙、1–2 格红 */
        int tier = ui_bars_tier(d.bars);
        uint32_t sig_col = tier == 2 ? T->green : tier == 1 ? T->orange : T->red;
        static uint32_t c_sig[5];
        for (int i = 0; i < 5; i++) {
            uint32_t col = i < d.bars ? sig_col : T->track;
            if (col != c_sig[i]) { c_sig[i] = col; uk_bg(s_top_sig[i], col); }
        }
        /* the phone-style label (5G-A / 5G+ / 4G+ / 3G …) needs the carrier
         * counts, so the signal card below writes it (s_top_label) */
        set_label_fmt(s_top_net, c_net, sizeof c_net, "%s", s_top_label[0] ? s_top_label : ui_rat_short(d.net_type));
        /* 「无服务」这类中文要中文字体，数字字体里没有 */
        lv_obj_set_style_text_font(s_top_net, (unsigned char)c_net[0] >= 0x80 ? UF.cj12 : UF.n12, 0);
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
        ui_rat_t rat = ui_rat(d.net_type);
        if (ca_n < CA_SLOTS) {
            int lte_n = parse_ca(d.lteca, ca + ca_n, CA_SLOTS - ca_n);
            for (int i = 0; i < lte_n; i++) pfx[ca_n + i] = 'B';
            ca_n += lte_n;
            /* 4G without carrier aggregation (and an NSA anchor the modem does
             * not list in lteca): the LTE serving cell is only in lte_*. */
            if (!lte_n && d.lte_rsrp != 0 && (rat == UI_RAT_4G || rat == UI_RAT_5G_NSA)) {
                char bs[16];
                ui_band_short(d.band, 0, bs, sizeof bs);
                ca[ca_n].band = atoi(bs + 1);
                ca[ca_n].pci = d.lte_pci;
                ca[ca_n].arfcn = d.channel;
                ca[ca_n].bw = atoi(d.bandwidth);
                ca[ca_n].rsrp = d.lte_rsrp;
                ca[ca_n].rsrq = d.lte_rsrq;
                ca[ca_n].sinr = atof(d.lte_snr[0] ? d.lte_snr : "0");
                ca[ca_n].active = 1;
                pfx[ca_n++] = 'B';
            }
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
        int act_n = 0, act_bw = 0, act_nr = 0, act_lte = 0;
        for (int i = 0; i < ca_n; i++)
            if (ca[i].active) {
                act_n++; act_bw += ca[i].bw;
                if (pfx[i] == 'n') act_nr++; else act_lte++;
            }
        /* 状态栏叫法要主 NR 载波的频段和 NR 总带宽（ui_net_badge） */
        int nr_band0 = 0, nr_mhz = 0;
        for (int i = 0; i < ca_n; i++)
            if (ca[i].active && pfx[i] == 'n') {
                if (!nr_band0) {
                    if (ca[i].band) nr_band0 = ca[i].band;
                    else { char bs[16]; ui_band_short(d.nr_band, 1, bs, sizeof bs); nr_band0 = atoi(bs + 1); }
                }
                nr_mhz += ca[i].bw;
            }
        /* 漫游：datad 的 net.roaming（Home / Roaming / …） */
        const char *rm = d.roaming;
        int roam = rm[0] && strcmp(rm, "Home") && strcmp(rm, "home") && strcmp(rm, "0");
        /* 结论、解读、下一步：全部在 ui_net_story 里按优先级决定（有逐条单测）；
         * 这里只负责把它画出来，参数只作为小字佐证。 */
        const char *ws = d.wan_status;
        ui_net_in_t nin = {
            .ever_valid = 1, .valid = 1, .sim_state = d.sim_state,
            .airplane = strstr(d.operate_mode, "LPM") || strstr(d.operate_mode, "OFFLINE"),
            .net_type = d.net_type, .bars = d.bars,
            .data_up = !ws[0] || (strstr(ws, "connected") && !strstr(ws, "disconnect")),
            .roaming = !rm[0] ? -1 : roam, .n_active = act_n, .nr_active = act_nr, .lte_active = act_lte,
            .mhz = act_bw,
            .sinr_valid = ca_n > 0, .sinr = sinr0, .rsrp_valid = ca_n > 0, .rsrp = ca_n ? (int)ca[0].rsrp : 0,
            .rsrq_valid = ca_n > 0 && ca[0].rsrq != 0, .rsrq = ca_n ? (int)ca[0].rsrq : 0,
            .rx_bps = d.rx_speed, .ambr_dl = d.ambr_dl,
            .mcc = d.mcc, .mnc = d.mnc, .nr_band = nr_band0, .nr_mhz = nr_mhz,
            .net_select = d.net_select,
        };
        ui_net_story_t story;
        ui_net_story(&nin, &story);
        /* 大字换结论要先稳 15 秒（阈值附近别来回闪）；没服务、没卡这类马上显示。
         * 下面五行一直是实时的。 */
        static ui_net_story_t shown;
        static ui_net_hold_t hold;
        {
            unsigned key = 5381;
            for (const char *p = story.headline; *p; p++) key = key * 33u + (unsigned char)*p;
            if (story.tone >= UI_NET_BAD || shown.tone >= UI_NET_BAD) hold.have = 0;
            if (ui_net_hold(&hold, key, lv_tick_get())) {
                shown.tone = story.tone; shown.cause = story.cause;
                memcpy(shown.headline, story.headline, sizeof shown.headline);
                memcpy(shown.hint, story.hint, sizeof shown.hint);
            }
        }
        snprintf(s_top_label, sizeof s_top_label, "%s", story.rat);
        static uint32_t nosig_since;
        int nosvc = !strcmp(story.headline, "无服务") || !strcmp(story.headline, "只能紧急呼叫");
        if (nosvc) { if (!nosig_since) nosig_since = lv_tick_get() ? lv_tick_get() : 1; }
        else nosig_since = 0;
        int tone = shown.tone == UI_NET_OK ? 0 : shown.tone == UI_NET_WARN ? 1 : shown.tone == UI_NET_BAD ? 2 : 3;
        if (tone != s_cc_tone) { s_cc_tone = tone; uk_hero_tone(&s_cc_hero, tone); }
        const char *hint = shown.hint;
        /* 顶行：谁的网 · 什么网 · 本地/漫游 */
        if (!sim_usable_ui(d.sim_state))
            set_label_fmt(s_cc_hero.st, c_st, sizeof c_st, "%s", "没有 SIM 卡");
        else
        {
            /* 顶栏已经是简写（5G-A / 4G+ …），这里写更细的：怎么组网、哪种技术 */
            char fine[32];
            ui_rat_long(d.net_type, act_lte, fine, sizeof fine);
            set_label_fmt(s_cc_hero.st, c_st, sizeof c_st, "%s%s%s%s",
                          d.operator_name[0] ? d.operator_name : "未注册",
                          fine[0] ? " · " : "", fine,
                          !rm[0] || nosvc ? "" : roam ? " · 漫游" : " · 本地");
        }
        set_label_fmt(s_cc_hero.big, c_big, sizeof c_big, "%s", shown.headline);
        if (nosvc) {
            uint32_t mins = (lv_tick_get() - nosig_since) / 60000;
            if (mins) set_label_fmt(s_cc_hero.rtop, c_rtop, sizeof c_rtop, "已 %u 分钟", (unsigned)mins);
            else      set_label_fmt(s_cc_hero.rtop, c_rtop, sizeof c_rtop, "%s", "刚刚");
        } else
            set_label_fmt(s_cc_hero.rtop, c_rtop, sizeof c_rtop, "%s", "");
        /* 右边：三件互不决定的事，位置固定。
         *   上：信号强/中/弱（就是状态栏的格数，颜色也一样）
         *   下：干扰小/中/大 · 负载正常/高（没在下载时不写负载）
         * 数字只在大字下面那行提示里，跟着「为什么慢」走。 */
        {
            char a[40] = "", b[64] = "";
            ui_net_tone_t at = story.sig_tone;
            if (nosvc) snprintf(a, sizeof a, "正在搜网");
            else if (story.sig[0]) {
                snprintf(a, sizeof a, "信号%s", story.sig);
                size_t bo = 0;
                if (story.noise[0]) bo += (size_t)snprintf(b + bo, sizeof b - bo, "干扰%s", story.noise);
                if (story.load[0])  bo += (size_t)snprintf(b + bo, sizeof b - bo, "%s负载%s", bo ? " · " : "", story.load);
                if (!bo && d.rssi) snprintf(b, sizeof b, "RSSI %d dBm", d.rssi);
            }
            set_label_fmt(s_cc_hero.r1, c_r1, sizeof c_r1, "%s", a);
            set_label_fmt(s_cc_hero.r2, c_r2, sizeof c_r2, "%s", b);
            uk_text_color(s_cc_hero.r1, nosvc ? T->t1 : at == UI_NET_OK ? T->okT : at == UI_NET_WARN ? T->warnT : T->badT);
        }
        uk_hero_layout(&s_cc_hero);

        int y = UK_HERO_H;
        uk_show(s_cc_hint, hint[0] != 0);
        if (hint[0]) {
            static char c_hint[96];
            set_label_fmt(s_cc_hint, c_hint, sizeof c_hint, "%s", hint);
            lv_obj_set_y(s_cc_hint, y + 8);
            lv_obj_update_layout(s_cc_hint);
            y += 8 + lv_obj_get_height(s_cc_hint) + 8;
        }
        /* 载波：基站配了几条、在用几条；小字是下行用哪几条、上行用哪条。
         * 配了没激活的 = RSRP 在 -140 底值的那几条；服务小区在 nrca 里会以
         * 未激活的样子再出现一次，按 PCI + 频段去重。上行只写主载波：
         * 上行聚合的数据还没对上（home-net-card.md「待确认」）。 */
        {
            static char c_cas[48], c_sub[160];
            char list[120] = "", first[20] = "", val[48], sub[160];
            size_t lo = 0;
            int cfg = 0;
            for (int i = 0; i < ca_n; i++) {
                int dup = 0;
                for (int j = 0; j < i; j++)
                    if (pfx[j] == pfx[i] && ca[j].pci == ca[i].pci &&
                        (ca[j].band == ca[i].band || !ca[j].band || !ca[i].band)) dup = 1;
                if (!dup) cfg++;
            }
            for (int i = 0; i < ca_n && lo + 16 < sizeof list; i++) {
                char b[20];
                if (!ca[i].active) continue;
                if (ca[i].band) snprintf(b, sizeof b, "%c%d", pfx[i], ca[i].band);
                else if (pfx[i] == 'n') ui_band_short(d.nr_band, 1, b, sizeof b);
                else ui_band_short(d.band, 0, b, sizeof b);
                if (!first[0]) snprintf(first, sizeof first, "%s", b);
                if (ca[i].bw) lo += (size_t)snprintf(list + lo, sizeof list - lo, "%s%s %dM", lo ? " + " : "", b, ca[i].bw);
                else          lo += (size_t)snprintf(list + lo, sizeof list - lo, "%s%s", lo ? " + " : "", b);
            }
            ui_rat_t r2 = ui_rat(d.net_type);
            if (!act_n && (r2 == UI_RAT_3G || r2 == UI_RAT_2G)) {
                char bs[16] = "";
                if (d.band[0]) ui_band_short(d.band, 0, bs, sizeof bs);
                snprintf(val, sizeof val, "无聚合");
                snprintf(sub, sizeof sub, "%s", bs);
            } else if (!act_n) {
                snprintf(val, sizeof val, "%s", nosvc ? "没连上基站" : "—");
                sub[0] = 0;
            } else {
                /* 3GPP 的说法：基站配置（configured）几条、激活（activated）几条 */
                if (cfg > act_n) snprintf(val, sizeof val, "激活 %d/%d", act_n, cfg);
                else if (act_n > 1) snprintf(val, sizeof val, "%d 载波聚合", act_n);
                else snprintf(val, sizeof val, "单载波");
                snprintf(sub, sizeof sub, "↓ %s   ↑ %s", list, first);
            }
            set_label_fmt(s_hr_ca.val, c_cas, sizeof c_cas, "%s", val);
            uk_text_color(s_hr_ca.val, shown.cause == UI_CAUSE_NARROW ? T->warnT : act_n ? T->t1 : T->t3);
            set_label_fmt(s_hr_ca_sub, c_sub, sizeof c_sub, "%s", sub);
            uk_show(s_hr_ca_sub, sub[0] != 0);
            int ch = sub[0] ? 60 : UK_ROW_H;
            lv_obj_set_height(s_hr_ca.box, ch);
            lv_obj_set_y(s_hr_ca.box, y);
            y += ch;
            static char c_qos[48];
            set_label_fmt(s_ca_qos, c_qos, sizeof c_qos, "QCI %d · AMBR %d/%d", d.qci, (int)d.ambr_dl, (int)d.ambr_ul);
        }
        /* Wi-Fi · 设备数（点进 Wi-Fi 页） */
        {
            static char c_w[64];
            if (!d.wifi_enabled)
                set_label_fmt(s_hr_wifi.val, c_w, sizeof c_w, "%s", "已关闭");
            else
                set_label_fmt(s_hr_wifi.val, c_w, sizeof c_w, "%.16s · %d 台", d.wifi_ssid[0] ? d.wifi_ssid : "-",
                              d.clients_total);
            uk_text_color(s_hr_wifi.val, d.wifi_enabled ? T->t1 : T->t3);
            lv_obj_set_y(s_hr_wifi.box, y);
            y += UK_ROW_H;
        }
        /* 出口：一行；有另一条路时两行（小字写另一条路），和下面写字的条件一致 */
        {
            int eh = UK_ROW_H;
            lv_obj_set_height(s_hr_exit.box, eh);
            lv_obj_set_y(s_hr_exit.box, y);
            y += eh;
        }
        /* 今日/本月：固件（zwrt_data）按日历日/月累计的计数器，不是本次开机的 rx/tx */
        {
            char c_day[32], c_month[32];
            static char c_traf[80] = "";
            fmt_bytes_total(c_day, sizeof c_day, d.day_rx_bytes + d.day_tx_bytes);
            fmt_bytes_total(c_month, sizeof c_month, d.month_rx_bytes + d.month_tx_bytes);
            set_label_fmt(s_hr_traf.val, c_traf, sizeof c_traf, "今日 %s · 本月 %s", c_day, c_month);
            uk_text_color(s_hr_traf.val, T->t1);
            lv_obj_set_y(s_hr_traf.box, y);
            y += UK_ROW_H;
        }
        lv_obj_set_height(s_cell_card, y);
        y = CA_CARD_TOP;
        for (int i = 0; i < CA_SLOTS; i++) {
            home_ca_t *k = &s_ca[i];
            if (i >= ca_n) { uk_show(k->box, 0); continue; }
            uk_show(k->box, 1);
            uk_show(k->sep, 1);
            int act = ca[i].active;
            char band_s[16];
            if (ca[i].band)         snprintf(band_s, sizeof band_s, "%c%d", pfx[i], ca[i].band);
            else if (pfx[i] == 'n') snprintf(band_s, sizeof band_s, "%s", d.nr_band[0] ? d.nr_band : "-");
            else                    ui_band_short(d.band, 0, band_s, sizeof band_s);
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
        lv_obj_set_height(s_ca_card, y + (ca_n ? 2 : 0));
        uk_show(s_ca_card, ca_n > 0);
        cell_reflow();
    }

    /* ---- 出口 (Home) ----
     * IP 和归属地来自 zte-agent 的缓存（netinfo_poll 在首页 30 秒读一次）；
     * 运营商和漫游已经在状态卡顶行。 */
    if (tab_visible(TAB_HOME)) {
        static char c_nip[48], c_ngeo[320];
        const netinfo_t *n = netinfo_get();
        int dead = s_cc_tone >= 2;      /* 没信号 / 没卡：出口是旧的 */
        char g[320];
        set_label_fmt(s_nh_ip, c_nip, sizeof c_nip, "%s", n->direct.ip[0] ? n->direct.ip : "—");
        uk_text_color(s_nh_ip, dead ? T->t3 : T->t1);
        if (n->err[0]) snprintf(g, sizeof g, "%s", n->err);
        else if (!n->direct.present) snprintf(g, sizeof g, "归属地查询中…");
        else if (!n->direct.ip[0]) snprintf(g, sizeof g, "查不到归属地");
        else snprintf(g, sizeof g, "%s%s%s", n->direct.geo[0] ? n->direct.geo : n->direct.ip,
                      n->direct.isp[0] ? " · " : "", n->direct.isp);
        set_label_fmt(s_nh_geo, c_ngeo, sizeof c_ngeo, "%s", g);
        /* 首页出口：直连 · 国家 城市；完整归属地在出口标签。 */
        static char c_hx[96], c_hx2[128];
        char sg[96], sub[128] = "";
        if (n->direct.ip[0])
            snprintf(g, sizeof g, "直连 · %s", n->direct.geo[0] ? geo_short(n->direct.geo, sg, sizeof sg) : n->direct.ip);
        else
            snprintf(g, sizeof g, "%s", n->err[0] ? "—" : "查询中…");
        set_label_fmt(s_hr_exit.val, c_hx, sizeof c_hx, "%s", g);
        uk_text_color(s_hr_exit.val, dead ? T->t3 : T->t1);
        set_label_fmt(s_hr_exit_sub, c_hx2, sizeof c_hx2, "%s", sub);
        uk_show(s_hr_exit_sub, sub[0] != 0);
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
                    set_label_fmt(s_sc_state, c_ss, sizeof c_ss, "%s", sc.name[0] ? sc.name : "判定中");
                uk_text_color(s_sc_state, sc.enabled ? T->t1 : T->t3);
                if (sc.last_switch > 0) {
                    time_t tt = (time_t)sc.last_switch;
                    struct tm tm;
                    /* 磁贴只有 121 px 宽：当天只写时间，更早的只写日期 */
                    time_t nw = time(NULL);
                    struct tm tn;
                    localtime_r(&tt, &tm);
                    localtime_r(&nw, &tn);
                    int today = tm.tm_year == tn.tm_year && tm.tm_yday == tn.tm_yday;
                    strftime(when, sizeof when, today ? "%H:%M" : "%m-%d", &tm);
                }
                /* 在家：解释 Wi-Fi 为什么没了；判定中：为什么还没结论；
                 * 其他：上次什么时候切过来的。手动固定的，先说「已固定」 */
                const char *pinned = sc.pin[0] ? "已固定 · " : "";
                uint32_t note_col = T->t2;
                if (sc.enabled && !sc.name[0])
                    set_label_fmt(s_sc_note, c_sn, sizeof c_sn, "开机后要连续两次扫描确认位置");
                else
                    set_label_fmt(s_sc_note, c_sn, sizeof c_sn, "%s%s%s%s", pinned,
                                  sc.wifi_off ? "Wi-Fi 已关 · " : (when[0] ? "切换于 " : ""),
                                  when, sc.wifi_off && !when[0] ? "手机走家里网络" : "");
                uk_text_color(s_sc_note, note_col);
            }
        }
    }

    /* ---- Tailscale (Home) ---- */
    {
        if (tailscale_poll(tab_visible(TAB_HOME) || tab_visible(TAB_EXIT) || sub_visible(SUB_TS))) {
            tailscale_status_t ts;
            tailscale_get_status(&ts);
            if (!ts.available) {
                lv_obj_add_flag(s_ts_card, LV_OBJ_FLAG_HIDDEN);
                uk_show(s_nh_tsrow, 0);
                lv_obj_set_height(s_nh_card, NET_EXIT_ROW_H);
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
                {
                    /* 出口卡里的一行摘要；完整的节点、子网在下面的 Tailscale 卡 */
                    static char c_nts[80];
                    if (running && ts.ip[0])
                        set_label_fmt(s_nh_tsval, c_nts, sizeof c_nts, "%s · %s", state_txt, ts.ip);
                    else
                        set_label_fmt(s_nh_tsval, c_nts, sizeof c_nts, "%s", state_txt);
                    uk_text_color(s_nh_tsval, running && ts.self_online ? T->okT : state_col);
                    if (lv_obj_has_flag(s_nh_tsrow, LV_OBJ_FLAG_HIDDEN)) {
                        uk_show(s_nh_tsrow, 1);
                        lv_obj_set_height(s_nh_card, NET_EXIT_ROW_H + UK_ROW_H);
                    }
                }
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
                {
                    /* 出口标签「Tailscale ›」行的小字 */
                    static char c_tsn[64];
                    if (running && ts.peers)
                        set_label_fmt(s_tile_sub[SUB_TS], c_tsn, sizeof c_tsn, "%s · 在线 %d/%d", state_txt, ts.peers_online, ts.peers);
                    else
                        set_label_fmt(s_tile_sub[SUB_TS], c_tsn, sizeof c_tsn, "%s", state_txt);
                    uk_text_color(s_tile_sub[SUB_TS], running && ts.self_online ? T->okT : T->t2);
                }
                /* Subpage: own identity + the peer list. */
                lv_label_set_text(s_tp_self[0], ts.name[0] ? ts.name : "-");
                lv_label_set_text(s_tp_self[1], ts.ip[0] ? ts.ip : "-");
                lv_label_set_text(s_tp_self[2], ts.ip6[0] ? ts.ip6 : "-");
                lv_label_set_text(s_tp_self[3], ts.relay[0] ? ts.relay : "-");
                lv_label_set_text(s_tp_self[4], ts.routes[0] ? ts.routes : "-");
                lv_label_set_text(s_tp_self[5], ts.tailnet[0] ? ts.tailnet : "-");
                lv_label_set_text_fmt(s_tp_self[6], "%s%s%s", ts.version[0] ? ts.version : "-",
                                      ts.os[0] ? " · " : "", ts.os);
                lv_label_set_text(s_tp_self[7], ts.key_expiry[0] ? ts.key_expiry : "不过期");
                int pn = tailscale_peer_count();
                if (pn > TS_PEER_MAX) pn = TS_PEER_MAX;
                for (int i = 0; i < TS_PEER_MAX; i++) {
                    if (i >= pn) { uk_show(s_tp_row[i], 0); continue; }
                    tailscale_peer_t pe;
                    tailscale_get_peer(i, &pe);
                    lv_obj_remove_flag(s_tp_row[i], LV_OBJ_FLAG_HIDDEN);
                    lv_label_set_text(s_tp_name[i], pe.name[0] ? pe.name : "-");
                    {
                        char rx[16], tx[16], tr[48] = "";
                        fmt_bytes_total(rx, sizeof rx, (long)pe.rx);
                        fmt_bytes_total(tx, sizeof tx, (long)pe.tx);
                        if (pe.rx || pe.tx) snprintf(tr, sizeof tr, " · ↓%s ↑%s", rx, tx);
                        lv_label_set_text_fmt(s_tp_ip[i], "%s%s%s%s%s", pe.ip, pe.os[0] ? " · " : "", pe.os,
                                              pe.exit_node ? " · 出口节点" : "", tr);
                    }
                    {
                        /* 第三行：跟本机之间怎么连、多久前握手、收发了多少 */
                        char how[80], ago[24] = "", line[160];
                        if (pe.active && pe.direct) snprintf(how, sizeof how, "直连 %s", pe.cur_addr);
                        else if (pe.active)         snprintf(how, sizeof how, "经 DERP %s 中继", pe.relay[0] ? pe.relay : "?");
                        else if (pe.online)         snprintf(how, sizeof how, "%s", "在线，现在没在传数据");
                        /* 不写「最后在线多久」：LastSeen 是控制服务器给的真 UTC，设备时钟是
                         * 当地时间标成 UTC，一减就差一个时区 */
                        else                        snprintf(how, sizeof how, "%s", "离线");
                        if (pe.hs_ago >= 0 && (pe.active || pe.online)) fmt_ago(ago, sizeof ago, pe.hs_ago);
                        snprintf(line, sizeof line, "%s%s%s", how, ago[0] ? " · 握手 " : "", ago);
                        lv_label_set_text(s_tp_link[i], line);
                        uk_text_color(s_tp_link[i], pe.online ? T->t2 : T->t3);
                    }
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


    home_reflow();

    /* ---- eSIM subpage ---- */
    {
        static int es_flash_shown;
        int es_changed = esim_poll(sub_visible(SUB_ESIM));
        /* 蜂窝标签开着、手没在动时读一次 eSIM 列表（每张卡一次），才分得清
         * 插的是普通 SIM 还是 eSIM 卡；读列表要跑 lpac，会卡一下，所以挑空闲时 */
        if (tab_visible(TAB_CELL) && !sub_visible(SUB_ESIM) && lv_display_get_inactive_time(NULL) > 1500
            && esim_prefetch(d.sim_iccid))
            es_changed = 1;
        {
            const netinfo_t *ni = netinfo_get();
            ui_sim_kind_t k = ui_sim_kind(d.sim_state, d.sim_iccid, esim_enabled_iccid());
            const char *op = ni->home.name[0] ? ni->home.name : d.operator_name;
            if (k != s_sim.kind || strcmp(op, s_sim.oper) || strcmp(d.sim_iccid, s_sim.iccid)
                || strcmp(d.sim_imsi, s_sim.imsi) || strcmp(d.sim_msisdn, s_sim.msisdn)) {
                s_sim.kind = k;
                snprintf(s_sim.oper, sizeof s_sim.oper, "%s", op);
                snprintf(s_sim.iccid, sizeof s_sim.iccid, "%s", d.sim_iccid);
                snprintf(s_sim.imsi, sizeof s_sim.imsi, "%s", d.sim_imsi);
                snprintf(s_sim.msisdn, sizeof s_sim.msisdn, "%s", d.sim_msisdn);
                es_changed = 1;
            }
        }
        if (es_flash_shown && !net_flash_on(&s_es_flash)) s_es_dirty = 1;   /* 提示到时间了：换回状态行 */
        if (es_changed || s_es_dirty) {
            es_flash_shown = net_flash_on(&s_es_flash);
            esim_paint();
        }
    }

    /* ---- Speedtest subpage ---- */
    {
        /* Also polled while just the 功能 tile wall is up (not only the
         * subpage itself) — the tile's own subtitle (功能 tile subtitles,
         * below) needs live data to replace the old hardcoded "插件未安装"
         * text, same as WiFi/SMS/eSIM/锁频 already do. */
        if (speedtest_poll(sub_visible(SUB_SPEED) || tab_visible(TAB_EXIT))) {
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

    /* 采样不看在哪个标签：离开系统页也要攒着，回来时就有数 */
    battery_est_feed(&d);

    /* ---- System page ---- */
    {
        static char c_sest[96] = "";
        set_label_fmt(s_sy_est, c_sest, sizeof c_sest, "%s", battery_est_text());
    }
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
        {
            static char c_hl[HEALTH_MAX][48], c_hd[HEALTH_MAX][320], c_hn[64];
            int hn = health_count();
            for (int i = 0; i < HEALTH_MAX; i++) {
                health_item_t h;
                if (i >= hn) { uk_show(s_hc_row[i], 0); continue; }
                health_get(i, &h);
                uk_show(s_hc_row[i], 1);
                lv_label_set_text(s_hc_mark[i], h.bad ? "■" : "▲");
                uk_text_color(s_hc_mark[i], h.bad ? T->badT : T->warnT);
                set_label_fmt(s_hc_label[i], c_hl[i], sizeof c_hl[i], "%s", h.label);
                set_label_fmt(s_hc_detail[i], c_hd[i], sizeof c_hd[i], "%s", h.detail);
            }
            if (hn == -2) set_label_fmt(s_hc_none, c_hn, sizeof c_hn, "%s", "读不到体检结果（管理后台没响应）");
            else if (hn < 0) set_label_fmt(s_hc_none, c_hn, sizeof c_hn, "%s", "体检结果读取中…");
            else set_label_fmt(s_hc_none, c_hn, sizeof c_hn, "● %d 项检查都正常", health_checked());
            uk_show(s_hc_none, hn <= 0);
            int hh = hn > 0 ? hn * HC_ROW_H : UK_ROW_H;
            lv_obj_set_height(s_hc_card, hh);
            lv_obj_set_y(s_al_body, HC_TOP + hh + 10);
        }
        if (err[0]) set_label_fmt(s_al_count, c_ac, sizeof c_ac, "%s", err);
        else if (!n) set_label_fmt(s_al_count, c_ac, sizeof c_ac, "%s", "告警记录");
        else if (un) set_label_fmt(s_al_count, c_ac, sizeof c_ac, "告警记录 · %d 条未读", un);
        else set_label_fmt(s_al_count, c_ac, sizeof c_ac, "%s", "告警记录 · 都已读");
        if (un && !err[0]) lv_obj_remove_flag(s_al_allread_btn, LV_OBJ_FLAG_HIDDEN);
        else               lv_obj_add_flag(s_al_allread_btn, LV_OBJ_FLAG_HIDDEN);
        if (!n && !err[0]) {
            lv_label_set_text(s_al_empty, "没有告警记录。程序崩溃、Wi-Fi 被看门狗打开这类事会记在这里。");
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
        static char c_sg[6][64], c_sgn[4][48], c_nrb[160], c_lteb[200];
        set_label_fmt(s_sg_nr[0], c_sg[0], sizeof c_sg[0], "%s  %s MHz",
                      d.nr_band[0] ? d.nr_band : "-", d.nr_bw[0] ? d.nr_bw : "-");
        set_label_fmt(s_sg_nr[1], c_sg[1], sizeof c_sg[1], "%ld", d.nr_channel);
        set_label_fmt(s_sg_nr[2], c_sg[2], sizeof c_sg[2], "%d", d.nr_pci);
        set_label_fmt(s_sg_nr[3], c_sg[3], sizeof c_sg[3], "%ld", d.nr_cell_id);
        set_label_fmt(s_sg_nr[4], c_sg[4], sizeof c_sg[4], "%d-%02d %s",
                      d.mcc, d.mnc, d.operator_name);
        set_label_fmt(s_sg_nr[5], c_sg[5], sizeof c_sg[5], "%d / %d / %s",
                      d.nr_rsrp, d.nr_rsrq, d.nr_snr[0] ? d.nr_snr : "-");
        {
            /* 服务小区：有 lteca 就取第一条（主载波），否则取 net.* 的 lte_* */
            static char c_lt[6][48];
            ca_carrier_t pc[1];
            int have = parse_ca(d.lteca, pc, 1) > 0;
            char bs[16] = "";
            int bw = have ? pc[0].bw : atoi(d.bandwidth);
            long earfcn = have ? pc[0].arfcn : d.channel;
            int pci = have ? pc[0].pci : d.lte_pci;
            if (have) snprintf(bs, sizeof bs, "B%d", pc[0].band);
            else if (d.band[0] && strstr(d.band, "LTE")) ui_band_short(d.band, 0, bs, sizeof bs);
            ui_rat_t rat = ui_rat(d.net_type);
            int in_use = rat == UI_RAT_4G || rat == UI_RAT_5G_NSA;
            lv_label_set_text(s_sg_lte_sec, in_use ? "LTE 服务小区" : "LTE（现在不用，下面是测量值）");
            lv_label_set_text(s_sg_nr_sec, rat == UI_RAT_4G ? "5G（现在不用，下面是测量值）" : "5G 服务小区");
            if (bs[0] && bw > 0) set_label_fmt(s_sg_lt[0], c_lt[0], sizeof c_lt[0], "%s  %d MHz", bs, bw);
            else set_label_fmt(s_sg_lt[0], c_lt[0], sizeof c_lt[0], "%s", bs[0] ? bs : "-");
            if (earfcn > 0) set_label_fmt(s_sg_lt[1], c_lt[1], sizeof c_lt[1], "%ld", earfcn);
            else set_label_fmt(s_sg_lt[1], c_lt[1], sizeof c_lt[1], "%s", "-");
            if (pci > 0) set_label_fmt(s_sg_lt[2], c_lt[2], sizeof c_lt[2], "%d", pci);
            else set_label_fmt(s_sg_lt[2], c_lt[2], sizeof c_lt[2], "%s", "-");
            if (in_use && d.lte_cell_id > 0) set_label_fmt(s_sg_lt[3], c_lt[3], sizeof c_lt[3], "%ld", d.lte_cell_id);
            else set_label_fmt(s_sg_lt[3], c_lt[3], sizeof c_lt[3], "%s", "-");
            if (d.lte_rsrp != 0)
                set_label_fmt(s_sg_lt[4], c_lt[4], sizeof c_lt[4], "%d / %d / %s",
                              d.lte_rsrp, d.lte_rsrq, d.lte_snr[0] ? d.lte_snr : "-");
            else set_label_fmt(s_sg_lt[4], c_lt[4], sizeof c_lt[4], "%s", "-");
            if (d.lte_rssi != 0) set_label_fmt(s_sg_lt[5], c_lt[5], sizeof c_lt[5], "%d", d.lte_rssi);
            else set_label_fmt(s_sg_lt[5], c_lt[5], sizeof c_lt[5], "%s", "-");
        }
        set_label_fmt(s_sg_net[0], c_sgn[0], sizeof c_sgn[0], "%s", ui_net_select_word(d.net_select));
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
    /* Highlight whichever 网络模式 the modem is actually on. Skipped while a
     * tap is armed so the orange "confirm?" state isn't repainted away by
     * the next refresh tick. */
    if (s_lk_mode_pending < 0) {
        static const char *const k_mode_v[4] = { "WL_AND_5G", "LTE_AND_5G", "Only_5G", "Only_LTE" };
        int sel = -1;
        for (int i = 0; i < 4; i++) if (!strcmp(d.net_select, k_mode_v[i])) sel = i;
        if (ui_net_select_is_auto(d.net_select)) sel = 0;   /* TCHGWL_5G is automatic too */
        if (sel != s_lk_seg.sel) uk_seg_set(&s_lk_seg, sel);
    } else if (lv_tick_get() - s_lk_mode_arm >= 5000) {
        s_lk_mode_pending = -1;          /* confirm window lapsed */
        s_lk_seg.sel = -2;               /* repaint the real selection next tick */
        lv_label_set_text(s_lk_mode_lbl, "切换会短暂断网，需要按两次确认");
    }

    /* ---- › 行右边的状态字（蜂窝 / 出口标签） ---- */
    {
        static char c_t1[40] = "", c_t6[112] = "";
        /* unread SMS = accent (the old tile's badge) */
        {
            static int c_on[2] = { -1, -1 };
            int unread = d.sms_unread > 0;
            if (unread != c_on[1]) { c_on[1] = unread; uk_text_color(s_tile_sub[SUB_SMS], unread ? T->accT : T->t2); }
        }
        if (d.sms_unread)
            set_label_fmt(s_tile_sub[SUB_SMS], c_t1, sizeof c_t1,
                          "%d \xE6\x9D\xA1 \xC2\xB7 %d \xE6\x9C\xAA\xE8\xAF\xBB", d.sms_n, d.sms_unread);
        else
            set_label_fmt(s_tile_sub[SUB_SMS], c_t1, sizeof c_t1, "%d \xE6\x9D\xA1", d.sms_n);
        /* 实体 SIM 写「SIM 卡 · 运营商」，eSIM 写「eSIM · 配置名」 */
        if (s_sim.kind == UI_SIM_ESIM)
            set_label_fmt(s_tile_sub[SUB_ESIM], c_t6, sizeof c_t6, "eSIM · %s", esim_current());
        else if (s_sim.kind == UI_SIM_PLAIN)
            set_label_fmt(s_tile_sub[SUB_ESIM], c_t6, sizeof c_t6, "SIM 卡 · %s", s_sim.oper[0] ? s_sim.oper : "已插入");
        else
            set_label_fmt(s_tile_sub[SUB_ESIM], c_t6, sizeof c_t6, "%s", "没插卡");
        static char c_t3[40] = "";
        int locked = 0, known = 0;
        for (int gi = 0; gi < 3; gi++)
            for (int i = 0; i < s_bg[gi].n; i++) { known = 1; if (!s_bg[gi].sel[i]) locked = 1; }
        set_label_fmt(s_tile_sub[SUB_LOCK], c_t3, sizeof c_t3, "%s", !known ? "" : locked ? "已锁定" : "未锁定");
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
    aux_refresh(tab_visible(TAB_WIFI) || tab_visible(TAB_SYS) || tab_visible(TAB_CELL));
    md_refresh();
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
    return power_menu_visible();
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
 * Five top-level pages, text only (the icon row read as guesses). The
 * capsule floats over the page (glass, rim, soft shadow; no real blur: it is
 * in every frame). It hides while a subpage or a sheet is open: subpages have
 * their own ‹ back button, top-left, and sheets cover the bottom. */
static const char *k_tab_names[UI_TABS] = { "首页", "蜂窝", "Wi-Fi", "出口", "系统" };
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

/* Jump to a tab from somewhere else (Home's summary rows): from its top. */
static void tab_go(int idx)
{
    sub_close();
    lv_tileview_set_tile_by_index(s_tv, idx, 0, LV_ANIM_OFF);
    lv_obj_t *sc = tab_scroller(idx);
    if (sc) lv_obj_scroll_to_y(sc, 0, LV_ANIM_OFF);
    update_tabs();
}
static void tab_go_cb(lv_event_t *e) { tab_go((int)(intptr_t)lv_event_get_user_data(e)); }

/* Tab bar: another tab keeps where it was; the tab you are on goes to its top. */
static void tab_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_sub_cur < 0 && cur_tab() == idx) { tab_go(idx); return; }
    sub_close();
    lv_tileview_set_tile_by_index(s_tv, idx, 0, LV_ANIM_OFF);
    update_tabs();
}

/* 二级页：从左边缘（x < 24）往右滑 = 返回（2026-09-25）。挂在输入设备上，
 * 松手时判断；成立就把这次按下作废，手指下面那一行不会再收到「点击」
 * （不然滑一下返回的同时点中了会断网的选项）。 */
#define EDGE_X   24
#define EDGE_DX  60
#define EDGE_DY  40
static lv_point_t s_edge_p0;
static int s_edge_armed;

static void edge_back_cb(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (!in || lv_indev_get_type(in) != LV_INDEV_TYPE_POINTER) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        s_edge_p0 = p;
        s_edge_armed = s_sub_cur >= 0 && p.x < EDGE_X && !any_sheet_open();
        return;
    }
    if (!s_edge_armed) return;
    s_edge_armed = 0;
    int dx = p.x - s_edge_p0.x, dy = p.y - s_edge_p0.y;
    if (dx >= EDGE_DX && dy < EDGE_DY && dy > -EDGE_DY) {
        lv_indev_reset(in, NULL);
        sub_back();
    }
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
#define TOP_LEFT_END 150   /* time + dots + RAT end here; the rate never crosses it.
                            * The RAT slot fits "5G UC" / "5G UW" / "无服务" (the widest). */

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
    build_cellular(s_tiles[TAB_CELL]);
    build_wifi(s_tiles[TAB_WIFI]);
    build_exit(s_tiles[TAB_EXIT]);
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
    build_sub_sms(s_sub_page[SUB_SMS]);
    build_sub_cell(s_sub_page[SUB_CELL]);
    build_sub_lock(s_sub_page[SUB_LOCK]);
    build_sub_speed(s_sub_page[SUB_SPEED]);
    build_sub_ts(s_sub_page[SUB_TS]);
    build_sub_esim(s_sub_page[SUB_ESIM]);
    build_sub_perf(s_sub_page[SUB_PERF]);
    build_sub_sms_detail(s_sub_page[SUB_SMS_DETAIL]);
    build_sub_alerts(s_sub_page[SUB_ALERTS]);
    build_sub_alert_detail(s_sub_page[SUB_ALERT_DETAIL]);
    build_sub_apn(s_sub_page[SUB_APN]);
    build_sub_net(s_sub_page[SUB_NET]);   /* also fills SUB_SCENE and hangs cards on 小区信息 / 出口 / Wi-Fi */
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
    for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in)) {
        lv_indev_add_event_cb(in, edge_back_cb, LV_EVENT_PRESSED, NULL);
        lv_indev_add_event_cb(in, edge_back_cb, LV_EVENT_RELEASED, NULL);
    }

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
    key_input_init(&s_key);
    s_key_timer = lv_timer_create(key_poll_cb, 50, NULL);
}
