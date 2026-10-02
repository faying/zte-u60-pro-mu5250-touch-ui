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
#include "net_view.h"
#include "screen_feed.h"
#include "ui_theme.h"
#include "ui_exec.h"
#include "battery_est.h"
#include "ui_kit.h"
#include "lang.h"
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
static lv_obj_t *s_cc_logo;            /* operator logo, top right of the hero */
static const char *s_cc_logo_slug;     /* what s_cc_logo shows, NULL = hidden   */
static char s_cc_logo_buf[24];         /* s_cc_logo_slug points here            */
static int s_cc_logo_w, s_cc_logo_h;   /* its pixel size, 0 = hidden            */
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
#define SC_CARD_H   60          /* 情景卡：一行 40 + 小字一行 */
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
static uk_seg_t  s_off_seg, s_ap_seg, s_lang_seg;
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
    char  last_csv[520];      /* "supported|locked" the chips were built from */
    uint32_t arm;             /* two-stage confirm timestamp */
    uint32_t sent;            /* 已下发… since (0 = not waiting) */
} band_group_t;
static band_group_t s_bg[3];
/* 蜂窝页「网络模式」：取值和顺序照原厂网页（config.js AUTO_MODES：5G/4G/3G、5G NSA = LTE_AND_5G、
 * 5G SA = Only_5G、4G/3G、4G Only）。5G SA 要在下标 2（按下时的国外提醒看它）。
 * 名字和 datad 的制式文字一致（WCDMA_AND_LTE 是「4G + 3G」，这里放不下空格）。 */
#define LK_MODES 5
static const char *const k_lk_mode_v[LK_MODES] = { "WL_AND_5G", "LTE_AND_5G", "Only_5G", "WCDMA_AND_LTE", "Only_LTE" };
static const char *const k_lk_mode_n[LK_MODES] = { N_("自动"), "5G NSA", "5G SA", "4G+3G", "4G" };   /* TR() where shown */
static lv_obj_t *s_lk_mode_btn[LK_MODES], *s_lk_mode_lbl, *s_lk_reset_lbl, *s_lk_reset_btn, *s_lk_reset_card,
                *s_lk_reset_sec, *s_lk_scroll;
static uk_seg_t  s_lk_seg;
static uint32_t  s_lk_mode_arm, s_lk_reset_arm;
static int       s_lk_mode_pending = -1;
/* 网络模式已发出、等读回：目标项和发出的时间（-1 = 没有在等） */
static int       s_lk_mode_want = -1;
static uint32_t  s_lk_mode_sent;
/* 最近一次读回的设备模式（下标，-1 = 不在这一排或从没读到）：datad 连不上时高亮留在它上面 */
static int       s_lk_mode_real = -1;
#define LK_MODE_WAIT_MS 20000
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
/* lang=en → English (include/lang.h); u60-guard reads the same line for the
 * alert SMS. Chosen once per process, like the theme. */
static int s_cf_lang_en = 0;

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
        else if (!strncmp(line, "lang=", 5)) s_cf_lang_en = lang_parse(line + 5);
    }
    fclose(fp);
}

static void save_devui_conf(void)
{
    FILE *fp = fopen(DEVUI_CONF_FILE, "w");
    if (!fp) return;
    fprintf(fp,
            "theme=%d\nspeed_bits=%d\nshow_batpct=%d\nautooff=%d\nrefresh_ms=%d\nsig_read=%d\nsig_parse=%d\nbright=%d\nst_src=%s\nst_dir=%s\nst_dur=%d\n"
            "appearance=%s\nappearance_dark_from=%s\nappearance_dark_to=%s\nlang=%s\n",
            s_cf_theme, s_cf_speed_bits, s_cf_show_batpct, s_cf_autooff_ms, s_cf_refresh_ms,
            s_cf_sig_read, s_cf_sig_parse, s_cf_bright, s_cf_st_src, s_cf_st_dir, s_cf_st_dur,
            ui_appear_name(s_cf_appear), s_cf_dark_from, s_cf_dark_to, s_cf_lang_en ? "en" : "zh");
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
static void lang_ui_note(const char *msg, int warn);   /* the 语言 row's small line, ditto */
static int ui_busy(void);

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
    if (dark != s_dark && ui_busy()) {   /* the exec would cut a speed test or an eSIM switch short (L2 R12) */
        appearance_ui_sync();
        lang_ui_note(TR("测速或切卡进行中，完成后再换外观"), 1);
        return;
    }
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

/* 系统 → 语言 (L2, manager docs/designs/ui-english.md R1/R9/R10): the same
 * exec as a theme switch, back on this tab and scroll position. First a full
 * screen "switching" frame in both languages, held long enough to be seen —
 * the exec drops the DRM fd and the panel goes dark until the new process
 * draws. Busy (speed test, eSIM switch): no switch, say why. Exec failed:
 * the setting goes back, the frame goes, the row says so. */
#ifndef UI_LANG_HOLD_MS
#define UI_LANG_HOLD_MS 400
#endif
static lv_obj_t *s_lang_frame;

static void lang_frame_show(int to_en)
{
    if (!s_lang_frame) {
        s_lang_frame = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_lang_frame);
        lv_obj_set_size(s_lang_frame, UI_W, UI_H);
        lv_obj_set_style_bg_color(s_lang_frame, lv_color_hex(T->bg), 0);
        lv_obj_set_style_bg_opa(s_lang_frame, LV_OPA_COVER, 0);
        lv_obj_add_flag(s_lang_frame, LV_OBJ_FLAG_CLICKABLE);   /* swallows taps while it is up */
        for (int i = 0; i < 2; i++) {
            lv_obj_t *l = lv_label_create(s_lang_frame);
            lv_obj_set_style_text_font(l, UF.cj20b, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(T->t1), 0);
            lv_obj_align(l, LV_ALIGN_CENTER, 0, i ? 16 : -16);
        }
    }
    /* both languages whichever way: whoever reads one of them knows what is happening */
    lv_label_set_text(lv_obj_get_child(s_lang_frame, 0), to_en ? "正在切换到 English…" : "正在切换到中文…");
    lv_label_set_text(lv_obj_get_child(s_lang_frame, 1), to_en ? "Switching to English…" : "Switching to 中文…");
    lv_obj_remove_flag(s_lang_frame, LV_OBJ_FLAG_HIDDEN);
}

static void lang_frame_hide(void)
{
    if (s_lang_frame) lv_obj_add_flag(s_lang_frame, LV_OBJ_FLAG_HIDDEN);
}

static void lang_set(int en)
{
    en = !!en;
    if (en == s_cf_lang_en) return;
    if (ui_busy()) {
        uk_seg_set(&s_lang_seg, s_cf_lang_en);
        lang_ui_note("测速或切卡中，稍后再切 · Busy, try later", 1);   /* one 12 px line */
        return;
    }
    s_vendor_arm = 0;
    uk_seg_set(&s_lang_seg, en);
    s_cf_lang_en = en;
    save_devui_conf();                   /* u60-guard's alert SMS follows from the next one */
    lang_frame_show(en);
    lv_refr_now(NULL);
    if (UI_LANG_HOLD_MS > 0) usleep(UI_LANG_HOLD_MS * 1000);
    if (theme_exec(0) != 0) {
        s_cf_lang_en = !en;
        save_devui_conf();
        lang_frame_hide();
        uk_seg_set(&s_lang_seg, s_cf_lang_en);
        lang_ui_note("没切成 · Couldn't switch", 1);
    }
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
    char buf[256];
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
        [SUB_SMS] = N_("短信"), [SUB_CELL] = N_("小区信息"), [SUB_LOCK] = N_("锁频"),
        [SUB_SPEED] = N_("测速"), [SUB_ESIM] = N_("SIM 与 eSIM"),
        [SUB_PERF] = N_("性能测试"), [SUB_TS] = "Tailscale", [SUB_SMS_DETAIL] = N_("短信详情"),
        [SUB_ALERTS] = N_("健康与告警"), [SUB_NET] = N_("运营商选择"), [SUB_SCENE] = N_("情景"), [SUB_APN] = "APN",
        [SUB_ALERT_DETAIL] = N_("详情"),
    };
    if (id < 0 || id >= SUB_N) return;
    for (int i = 0; i < SUB_N; i++)
        if (s_sub_page[i]) {
            if (i == id) lv_obj_remove_flag(s_sub_page[i], LV_OBJ_FLAG_HIDDEN);
            else         lv_obj_add_flag(s_sub_page[i], LV_OBJ_FLAG_HIDDEN);
        }
    lv_label_set_text(s_sub_title, TR(k_sub_title[id]));
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

/* ---- the pages, one file each (src/ui_parts/, see the note at the top of each) ---- */
#include "ui_parts/home.c"
#include "ui_parts/wifi.c"
#include "ui_parts/esim.c"
#include "ui_parts/system.c"
#include "ui_parts/sms.c"
#include "ui_parts/exit.c"
#include "ui_parts/lock.c"
#include "ui_parts/speedtest.c"
#include "ui_parts/network.c"
#include "ui_parts/apn.c"
#include "ui_parts/cellular.c"
#include "ui_parts/refresh.c"
#include "ui_parts/power.c"

/* ---- shared chrome: floating tab capsule ----
 * Five top-level pages, text only (the icon row read as guesses). The
 * capsule floats over the page (glass, rim, soft shadow; no real blur: it is
 * in every frame). It hides while a subpage or a sheet is open: subpages have
 * their own ‹ back button, top-left, and sheets cover the bottom. */
static const char *k_tab_names[UI_TABS] = { N_("首页"), N_("蜂窝"), "Wi-Fi", N_("出口"), N_("系统") };   /* Home / Cellular / Wi-Fi / Route / System */
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
        s_tabs[i] = uk_label(cell, UF.cj13, T->t2, 0, 0, TR(k_tab_names[i]));
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
int ui_key_held(void) { return s_key.fd >= 0 && s_key.pressed; }

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
    lang_set_en(s_cf_lang_en);          /* before the fonts and the first TR() */
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
    fprintf(stderr, "ui: appearance=%s dark=%d lang=%s tab=%d screen_off=%d exec_n=%d\n",
            ui_appear_name(s_cf_appear), s_dark, s_cf_lang_en ? "en" : "zh", s_launch.tab, s_launch.screen_off,
            s_launch.hist.count);

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
