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
/* Home page */
static lv_obj_t *s_operator;
static lv_obj_t *s_cc_qci, *s_cc_ambr;
/* 5 carrier slots: 3 NR + 2 LTE covers EN-DC on this modem with headroom. */
#define CA_SLOTS 5
static lv_obj_t *s_ca_card[CA_SLOTS], *s_ca_title[CA_SLOTS], *s_ca_rsrp[CA_SLOTS],
                *s_ca_sinr[CA_SLOTS], *s_ca_freq[CA_SLOTS], *s_ca_tag[CA_SLOTS];
static lv_obj_t *s_ca_cap_rsrp[CA_SLOTS], *s_ca_cap_sinr[CA_SLOTS];
static lv_obj_t *s_cell_card, *s_cc_sum;
static lv_obj_t *s_cc_traffic;   /* 今日/本月流量，见 fmt_bytes_total() */
/* 情景 — zte-agent 情景引擎的当前判定，只读。 */
#define SC_CARD_H 56
static lv_obj_t *s_sc_card, *s_sc_state, *s_sc_note;
static int s_sc_force;          /* 情景卡片要按新状态重画 */
/* 国外时点情景卡片弹出的「CHILL 出口」面板（见 build_exit_menu） */
enum { XM_PROXY, XM_GLOBAL, XM_KEEP_AI, XM_ALL, XM_OFF, XM_N };
static lv_obj_t *s_xm, *s_xm_btn[XM_N], *s_xm_lbl[XM_N], *s_xm_foot;
static uint32_t s_xm_off_arm;   /* 「关闭」点了第一下的时刻，0 = 没准备 */
#define XM_ARM_MS 4000
static lv_obj_t *s_cp_sw;       /* CHILL 页的总开关 */
static lv_obj_t *s_cp_exit_note;  /* CHILL 页「出口」行右边的说明 */
static lv_obj_t *s_ts_card, *s_ts_state, *s_ts_addr, *s_ts_routes, *s_ts_peers, *s_ts_note;
/* Charts page */
#define CHART_PTS 40
static lv_obj_t *s_ch_cpu, *s_ch_mem, *s_ch_net, *s_ch_bat;
static lv_chart_series_t *s_cs_cpu, *s_cs_mem, *s_cs_rx, *s_cs_tx, *s_cs_bat;
static lv_obj_t *s_ch_cpu_r, *s_ch_mem_r, *s_ch_net_r, *s_ch_bat_r;
/* Function tile wall */
static lv_obj_t *s_tile_sub[SUB_N];      /* per-tile status subtitle */
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
/* Each pair gets its own nested sub-card (DESIGN.md §4: "一组同形状的多字段
 * 条目" — name can run long ("漏网之鱼 → TW 台湾 01｜1x TW"), so it gets the
 * full sub-card width on its own line instead of sharing one line with the
 * rate and clipping, which is what the flat-list version did. */
#define CHILL_PAIR_ROW_H 40
#define CHILL_PAIR_GAP    6
static lv_obj_t *s_chill_card, *s_chill_state,
                *s_chill_split, *s_chill_total,
                *s_chill_pair_box[CHILL_HOME_ROWS],
                *s_chill_pair_name[CHILL_HOME_ROWS], *s_chill_pair_val[CHILL_HOME_ROWS],
                *s_chill_rate;
/* 2026-09-22：新增的手动选节点组一个就有 168 个节点，8 太小——家宽/NX 节点
 * 排在后面，直接被截没，界面上看起来像是"消失了"。卡片本来就在可滚动的
 * 容器里（build_sub_chill 的 mk_scroll_h），提高上限只是多建几个隐藏行，
 * 没有别的副作用；配 chill.c 的 SC_MAX_NODE=200。 */
#define CHILL_MAX_NODES 200
#define CHILL_MAX_GROUPS 12
#define CHILL_GRP_COLS 3
static lv_obj_t *s_cp_core, *s_cp_conns, *s_cp_traffic,
                *s_cp_mode_btn[4], *s_cp_node_card, *s_cp_node_row[CHILL_MAX_NODES],
                *s_cp_node_name[CHILL_MAX_NODES], *s_cp_node_dl[CHILL_MAX_NODES],
                *s_cp_grp_card, *s_cp_grp_btn[CHILL_MAX_GROUPS], *s_cp_grp_lbl[CHILL_MAX_GROUPS],
                *s_cp_delay_lbl;
/* 流量分布：一张"规则 -> 节点"卡，跟上面的"节点"卡不是一回事——那张卡是
 * 节点选择器（点了会切换节点），这张是只读统计，数字来自 chill_top_pair()，
 * 跟 chill.h 里的大注释对应：反映规则模式下流量实际去哪了，不是"配置了哪个
 * 节点"。曾经拆成按节点、按分流组两张独立卡，各自的 top N 排名互不相干，
 * 摆在一起容易被误读成一一对应（2026-09-22 反馈：具体哪条规则走了哪个
 * 节点，没有放出来）——改成一张卡，一行就是一对真实关系。行数跟 chill.c 的
 * SC_TOP_SHOW 对齐，改一边要记得改另一边。 */
#define CHILL_TRAF_ROWS 6
static lv_obj_t *s_cp_pair_card, *s_cp_pair_name[CHILL_TRAF_ROWS], *s_cp_pair_val[CHILL_TRAF_ROWS];
/* CHILL page nav rows — 策略组/节点/规则→节点 used to be built inline on the
 * CHILL page itself (12 group buttons + up to 200 node rows + 6 pair rows,
 * all pre-built whether you ever scroll to them or not). One screen showing
 * everything at once was the 2026-09-22 "会卡" complaint, so the heavy
 * content moved to drill-down subpages and the CHILL page itself only keeps
 * a one-line summary + chevron per section. 策略组+节点 share ONE row/page
 * (SUB_CHILL_NODES) — they're the same picker, not two unrelated lists. */
#define CHILL_NAV_ROWS 2
enum { CHILL_NAV_NODES, CHILL_NAV_PAIRS };
static lv_obj_t *s_cp_nav_row[CHILL_NAV_ROWS], *s_cp_nav_val[CHILL_NAV_ROWS];
/* WiFi page */
#define WIFI_MAX_CLI 5    /* fixed sub-card slots; backend reports up to 16 */
static lv_obj_t *s_w_ssid, *s_w_pass, *s_w_enc, *s_w_state;
static lv_obj_t *s_w_cli_card, *s_w_cli_n, *s_w_dhcp_card, *s_w_gw, *s_w_pool, *s_w_lease;
static lv_obj_t *s_w_cli[WIFI_MAX_CLI], *s_w_cli_name[WIFI_MAX_CLI],
                *s_w_cli_ip[WIFI_MAX_CLI], *s_w_cli_mac[WIFI_MAX_CLI];
static lv_obj_t *s_w_sw[5], *s_w_sw_st[5];
/* eSIM page */
#define ESIM_MAX_ROWS 5   /* fits one screen without scrolling; backend caps at 16 */
static lv_obj_t *s_es_cur, *s_es_state, *s_es_list_card;
static lv_obj_t *s_es_row[ESIM_MAX_ROWS], *s_es_row_name[ESIM_MAX_ROWS],
                *s_es_row_sub[ESIM_MAX_ROWS], *s_es_row_tag[ESIM_MAX_ROWS];
/* System page */
static lv_obj_t *s_set_bright, *s_off_btn[3], *s_vendor_btn, *s_vendor_lbl;
static lv_obj_t *s_set_ver, *s_set_imei, *s_set_usb, *s_set_fw, *s_set_health;
static lv_obj_t *s_sy_bat, *s_sy_chg, *s_sy_cpu, *s_sy_mem, *s_sy_up;
static lv_obj_t *s_sy_dps_sw, *s_sy_dps_st;
static lv_obj_t *s_sy_speedunit_sw, *s_sy_speedunit_st;
/* Tailscale subpage */
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
    lv_obj_t *card, *summary, *apply_lbl;
    lv_obj_t *chip[BAND_MAX], *chip_lbl[BAND_MAX];
    int   band_no[BAND_MAX];
    char  sel[BAND_MAX];
    int   n;
    char  prefix;
    char  last_csv[256];      /* the supported-band CSV the chips were built from */
    uint32_t arm;             /* two-stage confirm timestamp */
} band_group_t;
static band_group_t s_bg[3];
static lv_obj_t *s_lk_mode_btn[4], *s_lk_mode_lbl, *s_lk_reset_lbl;
static uint32_t  s_lk_mode_arm, s_lk_reset_arm;
static int       s_lk_mode_pending = -1;
/* SMS subpage: a toolbar (unread count + 全部已读), then one card per
 * message showing two lines; tapping a card opens SUB_SMS_DETAIL. */
#define SMS_MAX_ROWS 12
#define SMS_TOOLBAR_H 44
#define SMS_ROW_PITCH 86
static lv_obj_t *s_sms_card, *s_sms_row[SMS_MAX_ROWS], *s_sms_num[SMS_MAX_ROWS],
                *s_sms_date[SMS_MAX_ROWS], *s_sms_body[SMS_MAX_ROWS], *s_sms_dot[SMS_MAX_ROWS];
static lv_obj_t *s_sms_count, *s_sms_allread_btn, *s_sms_empty;
static long      s_sms_row_id[SMS_MAX_ROWS];
/* SMS detail subpage */
static long      s_smsd_id = -1;
static lv_obj_t *s_smsd_scroll, *s_smsd_num, *s_smsd_date, *s_smsd_body, *s_smsd_del_btn, *s_smsd_del_lbl;
static uint32_t  s_smsd_del_arm;
static uint32_t  s_autooff_ms = 0;   /* 0 = never */
static int       s_auto_slept = 0;
/* CJK fonts loaded from the device at runtime (NULL if unavailable).
 * FCN/FCN_S/FCN_L resolve to the CJK font, or Montserrat as a fallback. */
static lv_font_t *s_cjk, *s_cjk16, *s_cjk28;
static const lv_font_t *FCN, *FCN_S, *FCN_L;
#ifndef DEVUI_CJK_FONT
#define DEVUI_CJK_FONT "/usr/ui/fonts/ZTEZhengYuan.ttf"
#endif
/* Test page */
static lv_obj_t *s_t_fps, *s_t_touch, *s_t_box;
static lv_timer_t *s_bench_timer;
/* Speedtest page */
static lv_obj_t *s_st_card, *s_st_phase, *s_st_live, *s_st_detail,
                *s_st_result, *s_st_server, *s_st_btn, *s_st_btn_lbl,
                *s_st_offline;
#define ST_SRV_ROWS (ST_MAX_SERVERS + 1)   /* +1 = "自动" 固定占第 0 行 */
static lv_obj_t *s_st_srv_card;
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
static lv_obj_t   *s_power_menu;
/* Global status banner (backend down) — device-wide state, so it lives in the
 * shared chrome on lv_layer_top() rather than in any one page. */
static lv_obj_t   *s_banner, *s_banner_txt;
/* Fixed top status bar — time/signal/throughput/battery, visible on every
 * page (not just Home), same "shared chrome" pattern as the tab bar. */
static lv_obj_t   *s_top_time, *s_top_net, *s_top_updown, *s_top_bat, *s_top_sig[5];
static lv_obj_t   *s_top_bat_icon, *s_top_bat_fill, *s_top_bat2;
static lv_obj_t   *s_top_alert;   /* red: admin backend lost; amber: unread alerts */
static lv_obj_t   *s_top_alert_hit;  /* the dot's tap target (a 7px dot is not tappable) */
/* Alerts subpage */
static lv_obj_t   *s_al_count, *s_al_allread_btn, *s_al_empty;
static lv_obj_t   *s_al_row[ALERTS_MAX], *s_al_label[ALERTS_MAX], *s_al_time[ALERTS_MAX],
                  *s_al_text[ALERTS_MAX], *s_al_mark[ALERTS_MAX];

/* ================= design system =================
 * Every layout number and semantic colour lives here. Pages compose cards and
 * never invent their own radius/colour/spacing — that is what keeps a newly
 * added page (CHILL, eSIM, SMS, …) visually identical to the existing ones. */
#define UI_W            320
#define UI_H            480
#define UI_INSET        8                        /* page side margin */
#define UI_CARD_W       (UI_W - 2 * UI_INSET)    /* 304 — content width, sections and cards alike */
#define UI_CARD_RADIUS  18                        /* only used by the interactive-card exception (eSIM rows, buttons) */
#define UI_SECTION_BODY_Y 18                      /* content y-offset below a section title, DESIGN.md §4 spacing */
#define UI_PAD          12                       /* card inner padding */
#define UI_NAV_H        36                       /* bottom icon tab bar */
#define UI_TOPBAR_H     24                       /* fixed top status bar */
/* Actually visible page area: the tile (screen minus the top bar) minus the
 * bottom nav bar, which floats over the tile on lv_layer_top() and therefore
 * covers content instead of displacing it. 480-24-36 = 420. */
#define UI_VIEW_H       (UI_H - UI_TOPBAR_H - UI_NAV_H)
#define UI_SUB_HDR      28                       /* subpage title row */
#define UI_SUB_VIEW     (UI_VIEW_H - UI_SUB_HDR)

#define UI_C_BG       0x0c0f13
#define UI_C_CARD     0x1c2733
#define UI_C_TRACK    0x28323d
#define UI_C_OK       0x2bd67b
#define UI_C_BUSY     0x3ddcff
#define UI_C_WARN     0xffa040
#define UI_C_BAD      0xff5040
#define UI_C_ACCENT   0x4ea1ff
#define UI_C_TEXT     0xe6ecf2
#define UI_C_TEXT_2   0xc0c8d0
#define UI_C_TEXT_3   0x7a8694
#define UI_C_IDLE     0x3a4048

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
/* No unit-space padding — for the status bar, where "↓11.9 MB/s ↑501.7 KB/s"
 * (fmt_rate's verbosity, fine inside a detail card) ran into the battery
 * percentage next to it. The B/b suffix on every branch (not just the
 * sub-1024 one) is deliberate: 2026-09-21 user feedback on the Mbps<->MB/s
 * tap-toggle (topbar_speed_unit_cb) was "I can't tell it switched" — with
 * only digits changing, nothing marks which mode you're in. Folding the
 * unit letter into the number itself (MB/KB vs Mb/Kb) makes the switch
 * visible without a separate label eating more topbar width. */
static void fmt_rate_compact(char *out, size_t n, long bps)
{
    if (bps >= 1024 * 1024) snprintf(out, n, "%.1fMB", bps / 1048576.0);
    else if (bps >= 1024)   snprintf(out, n, "%.0fKB", bps / 1024.0);
    else                    snprintf(out, n, "%ldB", bps);
}
/* Same compact style, bit-rate reading (bytes/s * 8) — Mbps instead of MB/s.
 * Matches htmlmain.c's speed_bits=1 meaning. */
static void fmt_rate_compact_bits(char *out, size_t n, long Bps)
{
    long bps = Bps * 8;
    if (bps >= 1000000) snprintf(out, n, "%.1fMb", bps / 1000000.0);
    else if (bps >= 1000) snprintf(out, n, "%.0fKb", bps / 1000.0);
    else                  snprintf(out, n, "%ldb", bps);
}

/* ---- persisted UI settings, shared file with htmlmain.c's load_conf()/
 * save_conf() (src/htmlmain.c:815-844) — same devui.conf, same key set, so
 * whichever binary runs doesn't clobber the other's settings. LVGL only
 * acts on speed_bits (2026-09-21: tap-to-toggle Mbps/MB/s on the status
 * bar); the rest just round-trip verbatim through load/save. */
#define DEVUI_CONF_FILE "/data/plugins/u60pro-devui/devui.conf"
static int  s_cf_theme = 0, s_cf_speed_bits = 1, s_cf_show_batpct = 1,
            s_cf_autooff_ms = 60000, s_cf_refresh_ms = 5000,
            s_cf_sig_read = 0, s_cf_sig_parse = 0, s_cf_bright = 232, s_cf_st_dur = 15;
static char s_cf_st_src[16] = "auto", s_cf_st_dir[16] = "both";

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
    }
    fclose(fp);
}

static void save_devui_conf(void)
{
    FILE *fp = fopen(DEVUI_CONF_FILE, "w");
    if (!fp) return;
    fprintf(fp,
            "theme=%d\nspeed_bits=%d\nshow_batpct=%d\nautooff=%d\nrefresh_ms=%d\nsig_read=%d\nsig_parse=%d\nbright=%d\nst_src=%s\nst_dir=%s\nst_dur=%d\n",
            s_cf_theme, s_cf_speed_bits, s_cf_show_batpct, s_cf_autooff_ms, s_cf_refresh_ms,
            s_cf_sig_read, s_cf_sig_parse, s_cf_bright, s_cf_st_src, s_cf_st_dir, s_cf_st_dur);
    fclose(fp);
}

static void topbar_speed_unit_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    s_cf_speed_bits = !s_cf_speed_bits;
    save_devui_conf();
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

static lv_obj_t *mklabel(lv_obj_t *parent, int x, int y, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
    lv_label_set_text(l, "");
    return l;
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
static lv_obj_t *mk_card(lv_obj_t *parent, int y, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(c, UI_CARD_W, h);
    lv_obj_align(c, LV_ALIGN_TOP_LEFT, UI_INSET, y);
    lv_obj_set_style_radius(c, UI_CARD_RADIUS, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(UI_C_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    return c;
}

/* Nested sub-card: a thin bordered box inside a regular card, for a list of
 * same-shape multi-field items (e.g. one component carrier's full detail)
 * where each item is a meaningful group, not a single metric — matches the
 * litehtml reference's per-carrier boxes exactly. Border only, no separate
 * fill color from its parent: differentiation comes from the outline, same
 * as the reference. */
static lv_obj_t *mk_subcard(lv_obj_t *parent, int y, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(c, UI_CARD_W - 2 * UI_PAD, h);
    lv_obj_align(c, LV_ALIGN_TOP_LEFT, UI_PAD, y);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_white(), 0);
    lv_obj_set_style_border_opa(c, LV_OPA_20, 0);
    return c;
}

/* mk_section_title()/mk_divider() lived here until eSIM, WiFi and Settings
 * were moved onto cards (2026-09-21) — with every page on the card system
 * there is no caller left for a bare section title or a hairline divider,
 * and DESIGN.md §4 doesn't want them back. Removed rather than kept as
 * dead code someone re-reaches for. */

/* Invisible grouping container — for the one case (Tailscale) that needs to
 * hide/show a whole section as a unit depending on runtime availability. No
 * bg, no border: it carries no visual weight of its own, only grouping. */
static lv_obj_t *mk_group(lv_obj_t *parent, int y, int w, int h)
{
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g, w, h);
    lv_obj_align(g, LV_ALIGN_TOP_LEFT, UI_INSET, y);
    return g;
}

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
static lv_obj_t *mk_scroll_h(lv_obj_t *t, int view_h, int content_h)
{
    lv_obj_t *sc = lv_obj_create(t);
    lv_obj_remove_style_all(sc);
    lv_obj_set_size(sc, UI_W, view_h);
    lv_obj_align(sc, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_scroll_dir(sc, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sc, LV_SCROLLBAR_MODE_ACTIVE);

    lv_obj_t *sp = lv_obj_create(sc);
    lv_obj_remove_style_all(sp);
    lv_obj_set_size(sp, 1, 1);
    lv_obj_align(sp, LV_ALIGN_TOP_LEFT, 0, content_h);
    return sc;
}

static lv_obj_t *mk_scroll(lv_obj_t *t, int content_h)
{
    return mk_scroll_h(t, UI_VIEW_H, content_h);
}

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
static void build_home(lv_obj_t *t)
{
    /* 1050 covers the worst case: 5 carrier slots (3 NR + 2 LTE in EN-DC)
     * push the cellular card to ~490, +10 gap, Tailscale card 126, +10 gap,
     * then the CHILL card — grown again 2026-09-22 (each rule->node pair is
     * now its own sub-card instead of a flat text row, plus a new summary
     * line) to a worst case (5 pairs) of ~336. 490+10+126+10+336 == 972;
     * 1050 leaves slack without measuring it to the pixel every time either
     * card's worst case grows again. */
    t = mk_scroll(t, 1050 + SC_CARD_H + 10);

    s_cell_card = mk_card(t, 10, 182);
    lv_obj_t *cell = s_cell_card;
    s_operator = mklabel(cell, UI_PAD, 8, FCN, UI_C_TEXT);
    lv_label_set_text(s_operator, "…");
    s_cc_qci = mklabel(cell, UI_CARD_W - UI_PAD - 90, 12, &lv_font_montserrat_14, UI_C_TEXT_3);
    lv_obj_set_width(s_cc_qci, 90 - UI_PAD);
    lv_obj_set_style_text_align(s_cc_qci, LV_TEXT_ALIGN_RIGHT, 0);

    s_cc_sum = mklabel(cell, UI_PAD, 34, FCN_S, UI_C_TEXT_2);
    s_cc_ambr = mklabel(cell, UI_CARD_W - UI_PAD - 140, 35, &lv_font_montserrat_12, UI_C_TEXT_3);
    lv_obj_set_width(s_cc_ambr, 140);
    lv_obj_set_style_text_align(s_cc_ambr, LV_TEXT_ALIGN_RIGHT, 0);

    /* 今日/本月流量（2026-09-22 用户要求加到首页）——y 由刷新时的载波堆叠
     * 结果决定，跟着 s_ca_card[] 一起重排，位置在 build 阶段先随便摆，
     * 每次刷新都会重新 align，见下面 refresh_cb 里 "y" 累加的那段。 */
    s_cc_traffic = mklabel(cell, UI_PAD, 58, FCN_S, UI_C_TEXT_2);
    lv_obj_set_width(s_cc_traffic, UI_CARD_W - 2 * UI_PAD);

    /* Carrier slots: fixed count, built once, hidden/shown and re-laid-out
     * per refresh — never created per tick (see [[lvgl-heap-instability]]).
     * An active carrier gets the full two-row treatment; a
     * configured-but-inactive one (RSRP at the -140 floor) collapses to a
     * single line, because its RSRP/SINR are floor sentinels and carry no
     * information worth two rows of screen. */
    for (int i = 0; i < CA_SLOTS; i++) {
        lv_obj_t *c = mk_subcard(cell, 58 + i * 86, 78);
        int sw = UI_CARD_W - 2 * UI_PAD;
        s_ca_card[i]  = c;
        s_ca_title[i] = mklabel(c, 10, 6, &lv_font_montserrat_16, UI_C_TEXT);
        s_ca_tag[i]   = mklabel(c, 96, 8, FCN_S, UI_C_TEXT_3);
        s_ca_freq[i]  = mklabel(c, sw - 10 - 140, 6, FCN_S, UI_C_TEXT_3);
        lv_obj_set_width(s_ca_freq[i], 140);
        lv_obj_set_style_text_align(s_ca_freq[i], LV_TEXT_ALIGN_RIGHT, 0);
        /* Both RSRP and SINR carry quality colour, matching car_row() in
         * htmlmain.c. The previous pass coloured SINR only. */
        s_ca_cap_rsrp[i] = mklabel(c, 10, 34, FCN_S, UI_C_TEXT_3);
        lv_label_set_text(s_ca_cap_rsrp[i], "RSRP");
        s_ca_rsrp[i]     = mklabel(c, 50, 33, &lv_font_montserrat_14, UI_C_TEXT);
        s_ca_cap_sinr[i] = mklabel(c, 104, 34, FCN_S, UI_C_TEXT_3);
        lv_label_set_text(s_ca_cap_sinr[i], "SINR");
        s_ca_sinr[i]     = mklabel(c, 144, 33, &lv_font_montserrat_14, UI_C_OK);
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    }

    /* 情景 — between signal and Tailscale: it answers "why is Wi-Fi off?",
     * which is the first thing to wonder when the U-Chill SSID vanishes.
     * Hidden until the engine is configured and the agent answers. One row
     * of state, one row of context. Abroad, a tap opens the CHILL exit menu
     * (sc_card_cb → build_exit_menu); otherwise the settings live in the
     * admin web page. */
    s_sc_card = mk_group(t, 202, UI_CARD_W, SC_CARD_H);
    lv_obj_set_style_radius(s_sc_card, UI_CARD_RADIUS, 0);
    lv_obj_set_style_bg_color(s_sc_card, lv_color_hex(UI_C_CARD), 0);
    lv_obj_set_style_bg_opa(s_sc_card, LV_OPA_COVER, 0);
    lv_label_set_text(mklabel(s_sc_card, UI_PAD, 8, FCN_S, UI_C_TEXT_3), "情景");
    s_sc_state = mklabel(s_sc_card, UI_CARD_W - UI_PAD - 160, 8, FCN_S, UI_C_TEXT);
    lv_obj_set_width(s_sc_state, 160);
    lv_obj_set_style_text_align(s_sc_state, LV_TEXT_ALIGN_RIGHT, 0);
    s_sc_note = mklabel(s_sc_card, UI_PAD, 30, FCN_S, UI_C_TEXT_2);
    lv_label_set_long_mode(s_sc_note, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_sc_note, UI_CARD_W - 2 * UI_PAD);
    lv_obj_add_flag(s_sc_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_sc_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_sc_card, sc_card_cb, LV_EVENT_CLICKED, NULL);

    /* Tailscale — hidden entirely when the device has no tailscaled, matching
     * the old UI's "card doesn't exist" behaviour rather than an empty box. */
    s_ts_card = mk_group(t, 202, UI_CARD_W, 116);
    lv_obj_set_style_radius(s_ts_card, UI_CARD_RADIUS, 0);
    lv_obj_set_style_bg_color(s_ts_card, lv_color_hex(UI_C_CARD), 0);
    lv_obj_set_style_bg_opa(s_ts_card, LV_OPA_COVER, 0);
    lv_label_set_text(mklabel(s_ts_card, UI_PAD, 8, FCN_S, UI_C_TEXT_3), "Tailscale");
    /* The card is the entry point to the peer list — there is no 功能 tile
     * for Tailscale, same as the litehtml UI where it only exists on the
     * signal page. */
    lv_obj_add_flag(s_ts_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ts_card, tile_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)SUB_TS);
    s_ts_state  = mklabel(s_ts_card, UI_CARD_W - UI_PAD - 90, 8, FCN_S, UI_C_OK);
    lv_obj_set_width(s_ts_state, 90);
    lv_obj_set_style_text_align(s_ts_state, LV_TEXT_ALIGN_RIGHT, 0);
    /* FCN_S, not montserrat: this line joins IP and DERP with a '·'
     * (U+00B7), which montserrat renders as tofu. Rule of thumb for
     * this file: LVGL's LV_SYMBOL_* glyphs only ever go in montserrat
     * labels, and CJK plus typographic punctuation (·, ↓, ↑) only in
     * FCN/FCN_S ones. */
    s_ts_addr   = mklabel(s_ts_card, UI_PAD, 28, FCN_S, UI_C_TEXT_2);
    s_ts_peers  = mklabel(s_ts_card, UI_PAD, 50, FCN_S, UI_C_TEXT_2);
    s_ts_routes = mklabel(s_ts_card, UI_PAD, 70, FCN_S, UI_C_TEXT_2);
    s_ts_note   = mklabel(s_ts_card, UI_PAD, 90, FCN_S, UI_C_TEXT_3);
    lv_label_set_long_mode(s_ts_note, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_ts_note, UI_CARD_W - 2 * UI_PAD);

    /* CHILL (mihomo) — same treatment: hidden when the API isn't reachable.
     * Shows the real per-node / per-policy-group traffic breakdown (top 2
     * each) directly here, not a one-line "X 等 N 个" summary with the
     * actual numbers a screen away — that was the first attempt at this and
     * the user pointed out plainly that the fix belongs on the card people
     * actually look at, not the detail page. chill_top_node()/
     * chill_top_group() already degrade correctly outside 规则 mode (all
     * connections share one chain there, so "top 2" naturally becomes "top
     * 1"), so there is no mode branch here — same code path always. */
    s_chill_card = mk_group(t, 328, UI_CARD_W, 340);
    lv_obj_set_style_radius(s_chill_card, UI_CARD_RADIUS, 0);
    lv_obj_set_style_bg_color(s_chill_card, lv_color_hex(UI_C_CARD), 0);
    lv_obj_set_style_bg_opa(s_chill_card, LV_OPA_COVER, 0);
    lv_label_set_text(mklabel(s_chill_card, UI_PAD, 8, FCN_S, UI_C_TEXT_3), "CHILL");
    s_chill_state = mklabel(s_chill_card, UI_CARD_W - UI_PAD - 120, 8, FCN_S, UI_C_OK);
    lv_obj_set_width(s_chill_state, 120);
    lv_obj_set_style_text_align(s_chill_state, LV_TEXT_ALIGN_RIGHT, 0);

    /* Summary row — connections/proxy-direct split and cumulative traffic.
     * Neither was on this card before (2026-09-22: "信息可以完整一些");
     * both were already tracked by chill.c (chill_conn_split()/
     * chill_traffic()), just never surfaced here. Real-time rate keeps its
     * own line below the pair list — it ticks every refresh, mixing it into
     * this static-ish row would make the row's height/position jitter. */
    s_chill_split = mklabel(s_chill_card, UI_PAD, 30, FCN_S, UI_C_TEXT_2);
    s_chill_total = mklabel(s_chill_card, UI_CARD_W - UI_PAD - 140, 30, FCN_S, UI_C_TEXT_2);
    lv_obj_set_width(s_chill_total, 140);
    lv_obj_set_style_text_align(s_chill_total, LV_TEXT_ALIGN_RIGHT, 0);

    lv_label_set_text(mklabel(s_chill_card, UI_PAD, 52, FCN_S, UI_C_TEXT_3),
                      "\xE8\xA7\x84\xE5\x88\x99 \xE2\x86\x92 \xE8\x8A\x82\xE7\x82\xB9" /* 规则 → 节点 */);
    for (int i = 0; i < CHILL_HOME_ROWS; i++) {
        int ry = 68 + i * (CHILL_PAIR_ROW_H + CHILL_PAIR_GAP);
        lv_obj_t *box = mk_subcard(s_chill_card, ry, CHILL_PAIR_ROW_H);
        s_chill_pair_box[i] = box;
        /* Name on its own line, full sub-card width — this is the field
         * that runs long ("规则 → 节点丨倍率 地区"); sharing a line with the
         * rate (the old layout) left it ~110px to work with and clipped
         * mid-word. Wrap instead of clip as a safety net for the rare
         * longer-than-usual name; two lines still fits the row height. */
        s_chill_pair_name[i] = mklabel(box, 10, 4, FCN_S, UI_C_TEXT);
        lv_obj_set_width(s_chill_pair_name[i], UI_CARD_W - 2 * UI_PAD - 20);
        lv_label_set_long_mode(s_chill_pair_name[i], LV_LABEL_LONG_WRAP);
        s_chill_pair_val[i] = mklabel(box, 10, 22, FCN_S, UI_C_TEXT_3);
        lv_obj_set_width(s_chill_pair_val[i], UI_CARD_W - 2 * UI_PAD - 20);
    }

    /* Bigger + accent-colored (2026-09-22, "好看一点"): this is the one
     * number on the card that's alive every second, so it earns the
     * "emphasized value" treatment DESIGN.md §4 calls for (montserrat/FCN
     * 16-20px for things like this vs. 13px body text) — FCN not montserrat
     * because the ↓↑ glyphs only exist in the CJK font (font-pairing rule,
     * same reason the topbar rate uses FCN_S not montserrat). */
    s_chill_rate = mklabel(s_chill_card, UI_PAD,
                           68 + CHILL_HOME_ROWS * (CHILL_PAIR_ROW_H + CHILL_PAIR_GAP) + 4,
                           FCN, UI_C_ACCENT);
    lv_obj_add_flag(s_chill_card, LV_OBJ_FLAG_HIDDEN);
    /* Tap to jump straight to the CHILL subpage (mode switch, node list,
     * delay test) — same pattern as the Tailscale card above. This got
     * missed the first time: the Tailscale card was built, then the CHILL
     * card was added right after by copy-paste, but the click wiring
     * (added separately, after both cards existed) only got attached to
     * s_ts_card. */
    lv_obj_add_flag(s_chill_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_chill_card, tile_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)SUB_CHILL);

    /* LVGL's lv_slider/lv_button/lv_switch all set LV_OBJ_FLAG_SCROLL_ON_FOCUS
     * by default (see lv_slider.c/lv_button.c/lv_switch.c), and this project
     * never disables it (touch-only, no keypad/encoder group where focus-
     * driven scrolling would matter). Something focuses a widget the first
     * time a page with one of those is built, so the very first visit can
     * land scrolled a card or two down instead of at the top (caught on
     * the 系统 tab: 电池与负载 was the first thing visible, not 屏幕/亮度).
     * Pin it back to 0 once, right after building — this runs exactly once
     * per page (inside its build_* function), so it doesn't fight the user's
     * own scrolling on later visits. */
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
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
#define SW_ROW_H 30
static lv_obj_t *mk_switch_row(lv_obj_t *card, int y, const char *name,
                               lv_obj_t **state_out, lv_event_cb_t cb, int id)
{
    lv_label_set_text(mklabel(card, UI_PAD, y + 5, FCN_S, UI_C_TEXT), name);
    lv_obj_t *st = mklabel(card, UI_CARD_W - UI_PAD - 56 - 100, y + 6, FCN_S, UI_C_TEXT_3);
    lv_obj_set_width(st, 100);
    lv_obj_set_style_text_align(st, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(st, LV_LABEL_LONG_CLIP);
    if (state_out) *state_out = st;
    lv_obj_t *sw = lv_switch_create(card);
    lv_obj_set_size(sw, 44, 24);
    lv_obj_align(sw, LV_ALIGN_TOP_LEFT, UI_CARD_W - 2 * UI_PAD - 44 + UI_PAD, y + 2);
    if (cb) lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)id);
    return sw;
}

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

static void build_sub_wifi(lv_obj_t *t)
{
    t = mk_scroll_h(t, UI_SUB_VIEW, 700);

    lv_obj_t *ap = mk_card(t, 8, 96);
    lv_label_set_text(mklabel(ap, UI_PAD, 8, FCN_S, UI_C_TEXT_3), "\xE7\x83\xAD\xE7\x82\xB9" /* 热点 */);
    s_w_state = mklabel(ap, UI_CARD_W - UI_PAD - 90, 8, FCN_S, UI_C_OK);
    lv_obj_set_width(s_w_state, 90);
    lv_obj_set_style_text_align(s_w_state, LV_TEXT_ALIGN_RIGHT, 0);
    s_w_ssid = mklabel(ap, UI_PAD, 28, FCN, UI_C_TEXT);
    lv_label_set_text(mklabel(ap, UI_PAD, 54, FCN_S, UI_C_TEXT_3), "\xE5\x8A\xA0\xE5\xAF\x86" /* 加密 */);
    s_w_enc = mklabel(ap, UI_CARD_W - UI_PAD - 170, 54, FCN_S, UI_C_TEXT_2);
    lv_obj_set_width(s_w_enc, 170);
    lv_obj_set_style_text_align(s_w_enc, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(mklabel(ap, UI_PAD, 74, FCN_S, UI_C_TEXT_3), "\xE5\xAF\x86\xE7\xA0\x81" /* 密码 */);
    s_w_pass = mklabel(ap, UI_CARD_W - UI_PAD - 170, 74, FCN_S, UI_C_TEXT_2);
    lv_obj_set_width(s_w_pass, 170);
    lv_obj_set_style_text_align(s_w_pass, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *sws = mk_card(t, 112, 30 + 5 * SW_ROW_H + 8);
    lv_label_set_text(mklabel(sws, UI_PAD, 8, FCN_S, UI_C_TEXT_3), "\xE5\xBC\x80\xE5\x85\xB3" /* 开关 */);
    static const char *const k_sw_name[5] = {
        "WiFi \xE6\x80\xBB\xE5\xBC\x80\xE5\x85\xB3",          /* WiFi 总开关 */
        "2.4G \xE9\xA2\x91\xE6\xAE\xB5",                      /* 2.4G 频段 */
        "5G \xE9\xA2\x91\xE6\xAE\xB5",                        /* 5G 频段 */
        "\xE8\x8A\x82\xE8\x83\xBD\xE6\xA8\xA1\xE5\xBC\x8F",   /* 节能模式 */
        "NFC \xE7\xA2\xB0\xE4\xB8\x80\xE7\xA2\xB0",           /* NFC 碰一碰 */
    };
    for (int i = 0; i < 5; i++)
        s_w_sw[i] = mk_switch_row(sws, 30 + i * SW_ROW_H, k_sw_name[i], &s_w_sw_st[i], wifi_sw_cb, i);

    s_w_cli_card = mk_card(t, 310, 30 + WIFI_MAX_CLI * 48 + 8);
    lv_label_set_text(mklabel(s_w_cli_card, UI_PAD, 8, FCN_S, UI_C_TEXT_3),
                      "\xE5\xB7\xB2\xE8\xBF\x9E\xE6\x8E\xA5\xE8\xAE\xBE\xE5\xA4\x87" /* 已连接设备 */);
    s_w_cli_n = mklabel(s_w_cli_card, UI_CARD_W - UI_PAD - 90, 8, FCN_S, UI_C_TEXT_3);
    lv_obj_set_width(s_w_cli_n, 90);
    lv_obj_set_style_text_align(s_w_cli_n, LV_TEXT_ALIGN_RIGHT, 0);
    for (int i = 0; i < WIFI_MAX_CLI; i++) {
        int sub_w = UI_CARD_W - 2 * UI_PAD;
        s_w_cli[i] = mk_subcard(s_w_cli_card, 30 + i * 48, 44);
        s_w_cli_name[i] = mklabel(s_w_cli[i], 10, 4, FCN_S, UI_C_TEXT);
        lv_obj_set_width(s_w_cli_name[i], sub_w - 20 - 100);
        lv_label_set_long_mode(s_w_cli_name[i], LV_LABEL_LONG_CLIP);
        s_w_cli_ip[i] = mklabel(s_w_cli[i], sub_w - 10 - 110, 4, &lv_font_montserrat_14, UI_C_TEXT_2);
        lv_obj_set_width(s_w_cli_ip[i], 110);
        lv_obj_set_style_text_align(s_w_cli_ip[i], LV_TEXT_ALIGN_RIGHT, 0);
        s_w_cli_mac[i] = mklabel(s_w_cli[i], 10, 24, &lv_font_montserrat_14, UI_C_TEXT_3);
        lv_obj_add_flag(s_w_cli[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_w_dhcp_card = mk_card(t, 572, 100);
    lv_label_set_text(mklabel(s_w_dhcp_card, UI_PAD, 8, FCN_S, UI_C_TEXT_3), "DHCP");
    static const char *const dhcp_cap[3] = {
        "\xE7\xBD\x91\xE5\x85\xB3",                          /* 网关 */
        "\xE5\x9C\xB0\xE5\x9D\x80\xE6\xB1\xA0",              /* 地址池 */
        "\xE7\xA7\x9F\xE6\x9C\x9F",                          /* 租期 */
    };
    lv_obj_t **dhcp_val[3] = { &s_w_gw, &s_w_pool, &s_w_lease };
    for (int i = 0; i < 3; i++) {
        lv_label_set_text(mklabel(s_w_dhcp_card, UI_PAD, 30 + i * 22, FCN_S, UI_C_TEXT_3), dhcp_cap[i]);
        *dhcp_val[i] = mklabel(s_w_dhcp_card, UI_CARD_W - UI_PAD - 180, 30 + i * 22,
                               FCN_S, UI_C_TEXT_2);
        lv_obj_set_width(*dhcp_val[i], 180);
        lv_obj_set_style_text_align(*dhcp_val[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    /* LVGL's lv_slider/lv_button/lv_switch all set LV_OBJ_FLAG_SCROLL_ON_FOCUS
     * by default (see lv_slider.c/lv_button.c/lv_switch.c), and this project
     * never disables it (touch-only, no keypad/encoder group where focus-
     * driven scrolling would matter). Something focuses a widget the first
     * time a page with one of those is built, so the very first visit can
     * land scrolled a card or two down instead of at the top (caught on
     * the 系统 tab: 电池与负载 was the first thing visible, not 屏幕/亮度).
     * Pin it back to 0 once, right after building — this runs exactly once
     * per page (inside its build_* function), so it doesn't fight the user's
     * own scrolling on later visits. */
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* ---- eSIM subpage ---- */
static void esim_row_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    esim_select(idx);
}

static void build_sub_esim(lv_obj_t *t)
{
    lv_obj_t *cur = mk_card(t, 8, 68);
    lv_label_set_text(mklabel(cur, UI_PAD, 8, FCN_S, UI_C_TEXT_3),
                      "\xE5\xBD\x93\xE5\x89\x8D\xE9\x85\x8D\xE7\xBD\xAE" /* 当前配置 */);
    s_es_cur   = mklabel(cur, UI_PAD, 26, FCN,   UI_C_TEXT);
    s_es_state = mklabel(cur, UI_PAD, 48, FCN_S, UI_C_TEXT_3);

    s_es_list_card = mk_card(t, 84, 30 + ESIM_MAX_ROWS * 52 + 8);
    lv_label_set_text(mklabel(s_es_list_card, UI_PAD, 8, FCN_S, UI_C_TEXT_3),
                      "\xE9\x85\x8D\xE7\xBD\xAE\xE5\x88\x97\xE8\xA1\xA8" /* 配置列表 */);
    for (int i = 0; i < ESIM_MAX_ROWS; i++) {
        lv_obj_t *row = lv_obj_create(s_es_list_card);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, UI_CARD_W - 2 * UI_PAD, 48);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, UI_PAD, 30 + i * 52);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_C_TRACK), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, esim_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_es_row[i] = row;
        s_es_row_name[i] = mklabel(row, UI_PAD, 4,  FCN_S, UI_C_TEXT);
        s_es_row_sub[i]  = mklabel(row, UI_PAD, 26, FCN_S, UI_C_TEXT_3);
        s_es_row_tag[i]  = mklabel(row, 0, 4, FCN_S, UI_C_ACCENT);
        lv_obj_set_width(s_es_row_tag[i], 96);
        lv_obj_align(s_es_row_tag[i], LV_ALIGN_TOP_RIGHT, -UI_PAD, 4);
        lv_obj_set_style_text_align(s_es_row_tag[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
}

/* ---- System page ---- */
static const uint32_t k_off_ms[3] = { 0, 30000, 120000 };  /* Never / 30s / 2m */

static void bright_cb(lv_event_t *e)
{
    backlight_set(lv_slider_get_value((lv_obj_t *)lv_event_get_target(e)));
}

static void highlight_off_btns(int sel)
{
    for (int i = 0; i < 3; i++)
        lv_obj_set_style_bg_color(s_off_btn[i],
            lv_color_hex(i == sel ? UI_C_ACCENT : 0x394049), 0);
}

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
    if (arm && now - arm < 5000) {
        lv_label_set_text(s_vendor_lbl, "切换中…");
        if (request_vendor_via_uid()) return;
        system("( sleep 2; /etc/init.d/zte_topsw_devui start ) >/dev/null 2>&1 &");
        raise(SIGTERM);
        return;
    }
    arm = now;
    lv_obj_set_style_bg_color(s_vendor_btn, lv_color_hex(UI_C_WARN), 0);
    lv_label_set_text(s_vendor_lbl, "再按一次确认切换");
}

static void build_system(lv_obj_t *t)
{
    t = mk_scroll(t, 720);

    lv_obj_t *disp = mk_card(t, 10, 134);
    lv_label_set_text(mklabel(disp, UI_PAD, 8, FCN_S, UI_C_TEXT_3), "\xE5\xB1\x8F\xE5\xB9\x95" /* 屏幕 */);
    lv_label_set_text(mklabel(disp, UI_PAD, 30, FCN_S, UI_C_TEXT_2), "\xE4\xBA\xAE\xE5\xBA\xA6" /* 亮度 */);
    s_set_bright = lv_slider_create(disp);
    lv_obj_set_size(s_set_bright, UI_CARD_W - 2 * UI_PAD, 10);
    lv_obj_align(s_set_bright, LV_ALIGN_TOP_LEFT, UI_PAD, 52);
    lv_slider_set_range(s_set_bright, 10, backlight_max());
    lv_slider_set_value(s_set_bright, backlight_get() > 0 ? backlight_get() : backlight_max(), LV_ANIM_OFF);
    lv_obj_add_event_cb(s_set_bright, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_label_set_text(mklabel(disp, UI_PAD, 70, FCN_S, UI_C_TEXT_2),
                      "\xE8\x87\xAA\xE5\x8A\xA8\xE6\x81\xAF\xE5\xB1\x8F" /* 自动息屏 */);
    static const char *labels[3] = { "常亮", "30秒", "2分钟" };
    int bw = (UI_CARD_W - 2 * UI_PAD - 2 * 6) / 3;
    for (int i = 0; i < 3; i++) {
        s_off_btn[i] = lv_button_create(disp);
        lv_obj_set_size(s_off_btn[i], bw, 32);
        lv_obj_set_style_radius(s_off_btn[i], 8, 0);
        lv_obj_align(s_off_btn[i], LV_ALIGN_TOP_LEFT, UI_PAD + i * (bw + 6), 92);
        lv_obj_add_event_cb(s_off_btn[i], offsel_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(s_off_btn[i]);
        lv_obj_set_style_text_font(l, FCN_S, 0);
        lv_label_set_text(l, labels[i]);
        lv_obj_center(l);
    }
    highlight_off_btns(0);

    /* 电池与负载 — moved off Home (2026-09-21): Home is the signal page in
     * the reference UI, and these five rows belong with the other system
     * readouts, next to the charts that plot them. */
    lv_obj_t *load = mk_card(t, 154, 142);
    lv_label_set_text(mklabel(load, UI_PAD, 8, FCN_S, UI_C_TEXT_3),
                      "\xE7\x94\xB5\xE6\xB1\xA0\xE4\xB8\x8E\xE8\xB4\x9F\xE8\xBD\xBD" /* 电池与负载 */);
    static const char *const k_load_cap[5] = {
        "\xE7\x94\xB5\xE6\xB1\xA0",                          /* 电池 */
        "\xE5\x85\x85\xE7\x94\xB5\xE5\x99\xA8",              /* 充电器 */
        "CPU", "\xE5\x86\x85\xE5\xAD\x98" /* 内存 */,
        "\xE8\xBF\x90\xE8\xA1\x8C",                          /* 运行 */
    };
    lv_obj_t **load_val[5] = { &s_sy_bat, &s_sy_chg, &s_sy_cpu, &s_sy_mem, &s_sy_up };
    for (int i = 0; i < 5; i++) {
        lv_label_set_text(mklabel(load, UI_PAD, 30 + i * 22, FCN_S, UI_C_TEXT_3), k_load_cap[i]);
        *load_val[i] = mklabel(load, UI_CARD_W - UI_PAD - 210, 30 + i * 22, FCN_S, UI_C_TEXT_2);
        lv_obj_set_width(*load_val[i], 210);
        lv_obj_set_style_text_align(*load_val[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    lv_obj_t *dev = mk_card(t, 306, 152);
    lv_label_set_text(mklabel(dev, UI_PAD, 8, FCN_S, UI_C_TEXT_3), "\xE8\xAE\xBE\xE5\xA4\x87" /* 设备 */);
    /* 健康: the device check's summary (doctor.sh via the agent). Only counts
     * here — what failed is on the admin web's health page. */
    static const char *const dev_cap[4] = { "\xE7\x89\x88\xE6\x9C\xAC" /* 版本 */, "IMEI", "USB", "健康" };
    lv_obj_t **dev_val[4] = { &s_set_ver, &s_set_imei, &s_set_usb, &s_set_health };
    for (int i = 0; i < 4; i++) {
        lv_label_set_text(mklabel(dev, UI_PAD, 30 + i * 22, FCN_S, UI_C_TEXT_3), dev_cap[i]);
        *dev_val[i] = mklabel(dev, UI_CARD_W - UI_PAD - 200, 30 + i * 22, FCN_S, UI_C_TEXT_2);
        lv_obj_set_width(*dev_val[i], 200);
        lv_obj_set_style_text_align(*dev_val[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
    /* 健康 row → the alerts page (the whole row, not just the words). */
    {
        lv_obj_t *hit = lv_obj_create(dev);
        lv_obj_remove_style_all(hit);
        lv_obj_set_size(hit, UI_CARD_W - 2 * UI_PAD, 22);
        lv_obj_align(hit, LV_ALIGN_TOP_LEFT, UI_PAD, 30 + 3 * 22);
        lv_obj_set_ext_click_area(hit, 10);
        lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(hit, open_alerts_cb, LV_EVENT_CLICKED, NULL);
    }
    s_set_fw = mklabel(dev, UI_PAD, 120, &lv_font_montserrat_14, UI_C_TEXT_3);
    lv_obj_set_width(s_set_fw, UI_CARD_W - 2 * UI_PAD);
    lv_label_set_long_mode(s_set_fw, LV_LABEL_LONG_CLIP);

    lv_obj_t *swc = mk_card(t, 468, 30 + 2 * SW_ROW_H + 8);
    lv_label_set_text(mklabel(swc, UI_PAD, 8, FCN_S, UI_C_TEXT_3), "\xE5\xBC\x80\xE5\x85\xB3" /* 开关 */);
    s_sy_dps_sw = mk_switch_row(swc, 30,
        "\xE7\x94\xB5\xE6\xBA\x90\xE7\x9B\xB4\xE4\xBE\x9B\xE7\x94\xB5" /* 电源直供电 */,
        &s_sy_dps_st, dps_cb, 0);
    s_sy_speedunit_sw = mk_switch_row(swc, 30 + SW_ROW_H,
        "\xE7\x8A\xB6\xE6\x80\x81\xE6\xA0\x8F\xE7\xBD\x91\xE9\x80\x9F\xE7\x94\xA8 Mbps" /* 状态栏网速用 Mbps */,
        &s_sy_speedunit_st, speedunit_cb, 0);

    lv_obj_t *sys = mk_card(t, 468 + 30 + 2 * SW_ROW_H + 8 + 10, 104);
    lv_label_set_text(mklabel(sys, UI_PAD, 8, FCN_S, UI_C_TEXT_3), "\xE7\xB3\xBB\xE7\xBB\x9F" /* 系统 */);
    s_vendor_btn = lv_button_create(sys);
    lv_obj_set_size(s_vendor_btn, UI_CARD_W - 2 * UI_PAD, 36);
    lv_obj_set_style_radius(s_vendor_btn, 10, 0);
    lv_obj_align(s_vendor_btn, LV_ALIGN_TOP_LEFT, UI_PAD, 28);
    lv_obj_add_event_cb(s_vendor_btn, act_switch_vendor, LV_EVENT_CLICKED, NULL);
    s_vendor_lbl = lv_label_create(s_vendor_btn);
    lv_obj_set_style_text_font(s_vendor_lbl, FCN_S, 0);
    lv_label_set_text(s_vendor_lbl, "切换到原厂界面");
    lv_obj_center(s_vendor_lbl);
    lv_obj_t *hint = mklabel(sys, UI_PAD, 72, FCN_S, UI_C_TEXT_3);
    lv_obj_set_width(hint, UI_CARD_W - 2 * UI_PAD);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint, "电源键：短按 亮屏/息屏  长按 电源菜单");

    /* LVGL's lv_slider/lv_button/lv_switch all set LV_OBJ_FLAG_SCROLL_ON_FOCUS
     * by default (see lv_slider.c/lv_button.c/lv_switch.c), and this project
     * never disables it (touch-only, no keypad/encoder group where focus-
     * driven scrolling would matter). Something focuses a widget the first
     * time a page with one of those is built, so the very first visit can
     * land scrolled a card or two down instead of at the top (caught on
     * the 系统 tab: 电池与负载 was the first thing visible, not 屏幕/亮度).
     * Pin it back to 0 once, right after building — this runs exactly once
     * per page (inside its build_* function), so it doesn't fight the user's
     * own scrolling on later visits. */
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* ---- Charts page ----
 * The four series ui/05-charts.html plots. lv_chart with a fixed point count
 * and lv_chart_set_next_value(): the series buffer is allocated once, values
 * shift in place, so a page that updates every second allocates nothing. */
static lv_obj_t *mk_chart(lv_obj_t *t, int y, const char *title,
                          lv_obj_t **right_out, int range_max)
{
    lv_obj_t *card = mk_card(t, y, 108);
    lv_label_set_text(mklabel(card, UI_PAD, 8, FCN_S, UI_C_TEXT), title);
    *right_out = mklabel(card, UI_CARD_W - UI_PAD - 200, 9, FCN_S, UI_C_TEXT_3);
    lv_obj_set_width(*right_out, 200);
    lv_obj_set_style_text_align(*right_out, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *ch = lv_chart_create(card);
    lv_obj_set_size(ch, UI_CARD_W - 2 * UI_PAD, 68);
    lv_obj_align(ch, LV_ALIGN_TOP_LEFT, UI_PAD, 30);
    lv_chart_set_type(ch, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ch, CHART_PTS);
    lv_chart_set_range(ch, LV_CHART_AXIS_PRIMARY_Y, 0, range_max);
    lv_chart_set_div_line_count(ch, 3, 0);
    lv_chart_set_update_mode(ch, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ch, 0, 0);
    lv_obj_set_style_line_color(ch, lv_color_hex(0x252f3a), LV_PART_MAIN);
    lv_obj_set_style_size(ch, 0, 0, LV_PART_INDICATOR);   /* line only, no dots */
    lv_obj_set_style_line_width(ch, 2, LV_PART_ITEMS);
    return ch;
}

static void build_charts(lv_obj_t *t)
{
    t = mk_scroll(t, 484);
    s_ch_cpu = mk_chart(t, 10,  "CPU", &s_ch_cpu_r, 100);
    s_cs_cpu = lv_chart_add_series(s_ch_cpu, lv_color_hex(UI_C_ACCENT), LV_CHART_AXIS_PRIMARY_Y);
    s_ch_mem = mk_chart(t, 128, "\xE5\x86\x85\xE5\xAD\x98" /* 内存 */, &s_ch_mem_r, 100);
    s_cs_mem = lv_chart_add_series(s_ch_mem, lv_color_hex(UI_C_BUSY), LV_CHART_AXIS_PRIMARY_Y);
    /* Throughput is plotted on a log-ish 0..100 scale filled in by refresh_cb
     * (bytes/s spans five orders of magnitude; a linear axis would flatten
     * everything below the session peak into the baseline). */
    s_ch_net = mk_chart(t, 246, "\xE7\xBD\x91\xE9\x80\x9F" /* 网速 */, &s_ch_net_r, 100);
    s_cs_rx  = lv_chart_add_series(s_ch_net, lv_color_hex(UI_C_OK), LV_CHART_AXIS_PRIMARY_Y);
    s_cs_tx  = lv_chart_add_series(s_ch_net, lv_color_hex(UI_C_WARN), LV_CHART_AXIS_PRIMARY_Y);
    s_ch_bat = mk_chart(t, 364, "\xE7\x94\xB5\xE6\xB1\xA0" /* 电池 */, &s_ch_bat_r, 100);
    s_cs_bat = lv_chart_add_series(s_ch_bat, lv_color_hex(UI_C_WARN), LV_CHART_AXIS_PRIMARY_Y);

    /* LVGL's lv_slider/lv_button/lv_switch all set LV_OBJ_FLAG_SCROLL_ON_FOCUS
     * by default (see lv_slider.c/lv_button.c/lv_switch.c), and this project
     * never disables it (touch-only, no keypad/encoder group where focus-
     * driven scrolling would matter). Something focuses a widget the first
     * time a page with one of those is built, so the very first visit can
     * land scrolled a card or two down instead of at the top (caught on
     * the 系统 tab: 电池与负载 was the first thing visible, not 屏幕/亮度).
     * Pin it back to 0 once, right after building — this runs exactly once
     * per page (inside its build_* function), so it doesn't fight the user's
     * own scrolling on later visits. */
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* ---- perf test subpage ---- */
static void build_sub_perf(lv_obj_t *t)
{
    lv_obj_t *c = mk_card(t, 8, 120);
    lv_label_set_text(mklabel(c, UI_PAD, 8, FCN_S, UI_C_TEXT_3),
                      "\xE6\xB8\xB2\xE6\x9F\x93\xE5\x88\xB7\xE6\x96\xB0\xE7\x8E\x87" /* 渲染刷新率 */);
    s_t_fps = mklabel(c, UI_PAD, 28, &lv_font_montserrat_20, UI_C_OK);
    lv_label_set_text(s_t_fps, "-- FPS");
    lv_label_set_text(mklabel(c, UI_PAD, 62, FCN_S, UI_C_TEXT_3),
                      "\xE8\xA7\xA6\xE6\x8E\xA7\xE4\xB8\x8A\xE6\x8A\xA5\xE7\x8E\x87" /* 触控上报率 */);
    s_t_touch = mklabel(c, UI_PAD, 82, &lv_font_montserrat_20, UI_C_WARN);
    lv_label_set_text(s_t_touch, "-- Hz");

    s_t_box = lv_obj_create(t);
    lv_obj_remove_style_all(s_t_box);
    lv_obj_set_size(s_t_box, UI_CARD_W, 76);
    lv_obj_set_pos(s_t_box, UI_INSET, 150);
    lv_obj_set_style_radius(s_t_box, UI_CARD_RADIUS, 0);
    lv_obj_set_style_bg_color(s_t_box, lv_color_hex(UI_C_OK), 0);
    lv_obj_set_style_bg_opa(s_t_box, LV_OPA_COVER, 0);
}

/*
 * 点一下 = 已读，长按 = 举手删除（防抖窗口过后再点一下同一行才真删）。
 * 长按松手时 LVGL 在同一次手势上还会补发一次 CLICKED——sms_delete_tap()
 * 里的防抖窗口就是用来吃掉那次，不然长按一放手就等于自动确认删除了。
 */
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
static lv_obj_t *mk_small_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb, lv_obj_t **lbl_out)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 32);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_pad_hor(b, 14, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, FCN_S, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    if (lbl_out) *lbl_out = l;
    return b;
}

static void build_sub_sms(lv_obj_t *t)
{
    t = mk_scroll_h(t, UI_SUB_VIEW, 8 + SMS_TOOLBAR_H + SMS_MAX_ROWS * SMS_ROW_PITCH + 8);
    s_sms_card = t;
    s_sms_count = mklabel(t, UI_INSET + 4, 16, FCN_S, UI_C_TEXT_2);
    s_sms_allread_btn = mk_small_btn(t, "全部已读", sms_allread_cb, NULL);
    lv_obj_align(s_sms_allread_btn, LV_ALIGN_TOP_RIGHT, -UI_INSET, 8);
    s_sms_empty = mklabel(t, UI_INSET + 4, 8 + SMS_TOOLBAR_H, FCN_S, UI_C_TEXT_3);
    lv_label_set_text(s_sms_empty, "没有短信。新短信会显示在这里，点开可看全文。");
    lv_obj_set_width(s_sms_empty, UI_CARD_W - 8);
    lv_label_set_long_mode(s_sms_empty, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(s_sms_empty, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < SMS_MAX_ROWS; i++) {
        s_sms_row_id[i] = -1;
        lv_obj_t *c = mk_card(t, 8 + SMS_TOOLBAR_H + i * SMS_ROW_PITCH, 76);
        s_sms_row[i]  = c;
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, sms_row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(c, sms_row_longpress_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        s_sms_dot[i]  = lv_obj_create(c);
        lv_obj_remove_style_all(s_sms_dot[i]);
        lv_obj_set_size(s_sms_dot[i], 6, 6);
        lv_obj_align(s_sms_dot[i], LV_ALIGN_TOP_LEFT, 6, 14);
        lv_obj_set_style_radius(s_sms_dot[i], 3, 0);
        lv_obj_set_style_bg_color(s_sms_dot[i], lv_color_hex(UI_C_ACCENT), 0);
        lv_obj_set_style_bg_opa(s_sms_dot[i], LV_OPA_COVER, 0);
        s_sms_num[i]  = mklabel(c, UI_PAD + 6, 8, FCN_S, UI_C_TEXT);
        s_sms_date[i] = mklabel(c, UI_CARD_W - UI_PAD - 110, 9, &lv_font_montserrat_12, UI_C_TEXT_3);
        lv_obj_set_width(s_sms_date[i], 110);
        lv_obj_set_style_text_align(s_sms_date[i], LV_TEXT_ALIGN_RIGHT, 0);
        s_sms_body[i] = mklabel(c, UI_PAD, 30, FCN_S, UI_C_TEXT_2);
        /* Two lines, then "…": the card is a preview, the detail page has it all. */
        lv_obj_set_size(s_sms_body[i], UI_CARD_W - 2 * UI_PAD, 40);
        lv_label_set_long_mode(s_sms_body[i], LV_LABEL_LONG_DOT);
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    }

    /* LVGL's lv_slider/lv_button/lv_switch all set LV_OBJ_FLAG_SCROLL_ON_FOCUS
     * by default (see lv_slider.c/lv_button.c/lv_switch.c), and this project
     * never disables it (touch-only, no keypad/encoder group where focus-
     * driven scrolling would matter). Something focuses a widget the first
     * time a page with one of those is built, so the very first visit can
     * land scrolled a card or two down instead of at the top (caught on
     * the 系统 tab: 电池与负载 was the first thing visible, not 屏幕/亮度).
     * Pin it back to 0 once, right after building — this runs exactly once
     * per page (inside its build_* function), so it doesn't fight the user's
     * own scrolling on later visits. */
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* ---- Alerts subpage ----
 * What went wrong while nobody was looking (crashes, Wi-Fi rescues, SMS
 * failures), newest first: one plain Chinese line per alert, its time, and
 * the technical text small underneath. Same data as the admin web's
 * 系统 → 告警 page; details need a login, so alerts.c signs in like esim.c. */
#define AL_ROW_PITCH 76
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
    t = mk_scroll_h(t, UI_SUB_VIEW, 8 + SMS_TOOLBAR_H + ALERTS_MAX * AL_ROW_PITCH + 8);
    s_al_count = mklabel(t, UI_INSET + 4, 16, FCN_S, UI_C_TEXT_2);
    s_al_allread_btn = mk_small_btn(t, "全部已读", al_allread_cb, NULL);
    lv_obj_align(s_al_allread_btn, LV_ALIGN_TOP_RIGHT, -UI_INSET, 8);
    s_al_empty = mklabel(t, UI_INSET + 4, 8 + SMS_TOOLBAR_H, FCN_S, UI_C_TEXT_3);
    lv_obj_set_width(s_al_empty, UI_CARD_W - 8);
    lv_label_set_long_mode(s_al_empty, LV_LABEL_LONG_WRAP);
    for (int i = 0; i < ALERTS_MAX; i++) {
        lv_obj_t *c = mk_card(t, 8 + SMS_TOOLBAR_H + i * AL_ROW_PITCH, 66);
        s_al_row[i] = c;
        s_al_mark[i] = mklabel(c, UI_PAD, 8, FCN_S, UI_C_WARN);
        lv_label_set_text(s_al_mark[i], "▲");
        s_al_label[i] = mklabel(c, UI_PAD + 18, 8, FCN_S, UI_C_TEXT);
        lv_obj_set_width(s_al_label[i], UI_CARD_W - 2 * UI_PAD - 18 - 84);
        lv_label_set_long_mode(s_al_label[i], LV_LABEL_LONG_DOT);
        s_al_time[i] = mklabel(c, UI_CARD_W - UI_PAD - 84, 10, &lv_font_montserrat_12, UI_C_TEXT_3);
        lv_obj_set_width(s_al_time[i], 84);
        lv_obj_set_style_text_align(s_al_time[i], LV_TEXT_ALIGN_RIGHT, 0);
        s_al_text[i] = mklabel(c, UI_PAD, 36, &lv_font_montserrat_12, UI_C_TEXT_3);
        lv_obj_set_width(s_al_text[i], UI_CARD_W - 2 * UI_PAD);
        lv_label_set_long_mode(s_al_text[i], LV_LABEL_LONG_DOT);
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* ---- SMS detail subpage ----
 * One message in full: sender, time, the whole text (scrolls), delete.
 * The column grows with the text, so it is a flex layout rather than the
 * fixed-y cards the other pages use. */
static void build_sub_sms_detail(lv_obj_t *t)
{
    lv_obj_t *sc = lv_obj_create(t);
    lv_obj_remove_style_all(sc);
    lv_obj_set_size(sc, UI_W, UI_SUB_VIEW);
    lv_obj_set_scroll_dir(sc, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sc, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sc, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(sc, 8, 0);
    lv_obj_set_style_pad_bottom(sc, 24, 0);
    lv_obj_set_style_pad_row(sc, 10, 0);
    s_smsd_scroll = sc;

    lv_obj_t *c = lv_obj_create(sc);
    lv_obj_remove_style_all(c);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(c, UI_CARD_W);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(c, UI_CARD_RADIUS, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(UI_C_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(c, UI_PAD, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 6, 0);

    s_smsd_num = lv_label_create(c);
    lv_obj_set_style_text_font(s_smsd_num, FCN, 0);
    lv_obj_set_style_text_color(s_smsd_num, lv_color_hex(UI_C_TEXT), 0);
    s_smsd_date = lv_label_create(c);
    lv_obj_set_style_text_font(s_smsd_date, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_smsd_date, lv_color_hex(UI_C_TEXT_3), 0);
    s_smsd_body = lv_label_create(c);
    lv_obj_set_style_text_font(s_smsd_body, FCN_S, 0);
    lv_obj_set_style_text_color(s_smsd_body, lv_color_hex(UI_C_TEXT), 0);
    lv_obj_set_width(s_smsd_body, lv_pct(100));
    lv_label_set_long_mode(s_smsd_body, LV_LABEL_LONG_WRAP);

    s_smsd_del_btn = mk_small_btn(sc, "删除这条", smsd_delete_cb, &s_smsd_del_lbl);
}

/* ---- CHILL subpage ---- */
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

#define CHILL_GRP_ROWS ((CHILL_MAX_GROUPS + CHILL_GRP_COLS - 1) / CHILL_GRP_COLS)
/* 策略组卡永远按 CHILL_MAX_GROUPS 的满槽位留高度，不跟着实际组数收缩（这张
 * 卡本来就是这么建的，不是我这次改的）——所以它的高度是个编译期常量，
 * 一个宏就够，refresh_cb 给流量卡定位时要用同一个数，不能各算各的。 */
#define CHILL_GRP_H (30 + CHILL_GRP_ROWS * 36 - 6 + 8)

/* 一张"按 X 统计流量"卡：标题 + 最多 CHILL_TRAF_ROWS 行（名字左，流量右），
 * 固定槽位建好、按实际条目数隐藏/显示——跟节点卡、策略组卡同一个规矩，不
 * 跟着数据量动态建对象。返回卡片高度，调用方拿去算下一张卡的 y。 */
static lv_obj_t *build_traffic_card(lv_obj_t *t, int y, const char *title,
                                    lv_obj_t **card_out,
                                    lv_obj_t **name_out, lv_obj_t **val_out)
{
    int h = 30 + CHILL_TRAF_ROWS * 22 + 8;
    lv_obj_t *c = mk_card(t, y, h);
    *card_out = c;
    lv_label_set_text(mklabel(c, UI_PAD, 8, FCN_S, UI_C_TEXT_3), title);
    for (int i = 0; i < CHILL_TRAF_ROWS; i++) {
        name_out[i] = mklabel(c, UI_PAD, 30 + i * 22, FCN_S, UI_C_TEXT_2);
        lv_obj_set_width(name_out[i], UI_CARD_W - 2 * UI_PAD - 110);
        lv_label_set_long_mode(name_out[i], LV_LABEL_LONG_CLIP);
        val_out[i] = mklabel(c, UI_CARD_W - UI_PAD - 100, 30 + i * 22, FCN_S, UI_C_TEXT_2);
        lv_obj_set_width(val_out[i], 100);
        lv_obj_set_style_text_align(val_out[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_add_flag(name_out[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(val_out[i], LV_OBJ_FLAG_HIDDEN);
    }
    return c;
}

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
        uint32_t col = cur ? UI_C_ACCENT : 0x394049;
        const char *t = k_lbl[i];
        if (i == XM_OFF) {
            if (!on) { t = "打开 CHILL"; col = 0x2b6e4a; }
            else if (s_xm_off_arm) { t = "再点一下：关闭（AI、VoWiFi 也直连）"; col = 0xb23b3b; }
            else col = 0x6b2f2f;
        } else if (!on) {
            col = 0x262c33;             /* CHILL 关着时这三项没意义 */
        }
        lv_label_set_text(s_xm_lbl[i], t);
        lv_obj_set_style_bg_color(s_xm_btn[i], lv_color_hex(col), 0);
    }
    lv_label_set_text(s_xm_foot, sc.auto_direct
        ? "换国外卡时已自动切到「直连 · AI 不动」\n换回国内卡后自动回到「代理」"
        : "换回国内卡后自动回到「代理」");
    lv_obj_set_style_text_color(s_xm_foot, lv_color_hex(0x9aa4ae), 0);
}

static void exit_menu_set(int v)
{
    s_xm_off_arm = 0;
    if (v) { exit_menu_refresh(); lv_obj_remove_flag(s_xm, LV_OBJ_FLAG_HIDDEN); }
    else   lv_obj_add_flag(s_xm, LV_OBJ_FLAG_HIDDEN);
}

static void exit_menu_fail(void)
{
    lv_label_set_text(s_xm_foot, "没成功（后台没回应或 CHILL 没在运行），稍后再试");
    lv_obj_set_style_text_color(s_xm_foot, lv_color_hex(UI_C_WARN), 0);
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

static void build_exit_menu(void)
{
    s_xm = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_xm, 296, 452);
    lv_obj_center(s_xm);
    lv_obj_set_style_bg_color(s_xm, lv_color_hex(0x161b21), 0);
    lv_obj_set_style_border_color(s_xm, lv_color_hex(0x4ea1ff), 0);
    lv_obj_set_style_border_width(s_xm, 2, 0);
    lv_obj_set_style_radius(s_xm, 12, 0);
    lv_obj_set_flex_flow(s_xm, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_xm, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_xm, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_xm);
    lv_label_set_text(title, "在国外 · CHILL 怎么走");
    lv_obj_set_style_text_font(title, FCN, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x9aa4ae), 0);

    for (int i = 0; i < XM_N; i++) {
        s_xm_btn[i] = lv_button_create(s_xm);
        lv_obj_set_width(s_xm_btn[i], lv_pct(94));
        lv_obj_set_height(s_xm_btn[i], 44);
        lv_obj_add_event_cb(s_xm_btn[i], exit_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_xm_lbl[i] = lv_label_create(s_xm_btn[i]);
        lv_obj_set_style_text_font(s_xm_lbl[i], FCN_S, 0);
        lv_obj_center(s_xm_lbl[i]);
    }
    s_xm_foot = lv_label_create(s_xm);
    lv_obj_set_style_text_font(s_xm_foot, FCN_S, 0);
    lv_obj_set_style_text_align(s_xm_foot, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_xm_foot, lv_pct(94));
    lv_label_set_long_mode(s_xm_foot, LV_LABEL_LONG_WRAP);

    lv_obj_t *c = lv_button_create(s_xm);
    lv_obj_set_width(c, lv_pct(94));
    lv_obj_set_height(c, 40);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x262c33), 0);
    lv_obj_add_event_cb(c, exit_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(c);
    lv_label_set_text(cl, "取消");
    lv_obj_set_style_text_font(cl, FCN_S, 0);
    lv_obj_center(cl);
    lv_obj_add_flag(s_xm, LV_OBJ_FLAG_HIDDEN);
}

/* 首页情景卡片：在国外时点开出口面板，其他时候点了什么都不做。 */
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
    t = mk_scroll_h(t, UI_SUB_VIEW, 8 + 82 + 10 + 106 + 10 + (30 + CHILL_NAV_ROWS * 44 + 8) + 16);

    /* 曾经这里还有"节点"/"链路"两行，取自 chill_node()/chill_chain()——
     * 只描述 chill.conf 里配置的那一个组，规则模式下大部分流量根本不走它，
     * 跟按节点/规则的真实流量数据对不上（2026-09-22 用户反馈：进详情页看到
     * 的节点信息不一致）。首页卡片已经改用真实数据，这里漏改了；直接删掉
     * 这两行，不留误导性的旧字段。 */
    lv_obj_t *st = mk_card(t, 8, 82);
    lv_label_set_text(mklabel(st, UI_PAD, 8, FCN_S, UI_C_TEXT_3), "\xE7\x8A\xB6\xE6\x80\x81" /* 状态 */);
    /* 总开关在「状态」行最右，核心状态文字挪到它左边。 */
    s_cp_sw = lv_switch_create(st);
    lv_obj_set_size(s_cp_sw, 44, 24);
    lv_obj_align(s_cp_sw, LV_ALIGN_TOP_LEFT, UI_CARD_W - UI_PAD - 44, 5);
    lv_obj_add_event_cb(s_cp_sw, chill_master_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_flag(s_cp_sw, LV_OBJ_FLAG_HIDDEN);   /* 知道开关状态后才显示 */
    s_sc_force = 1;                                  /* 下一轮按已知状态同步 */
    s_cp_core = mklabel(st, UI_CARD_W - UI_PAD - 44 - 8 - 110, 8, FCN_S, UI_C_OK);
    lv_obj_set_width(s_cp_core, 110);
    lv_obj_set_style_text_align(s_cp_core, LV_TEXT_ALIGN_RIGHT, 0);
    static const char *const k_cp_cap[2] = {
        "\xE8\xBF\x9E\xE6\x8E\xA5" /* 连接 */, "\xE6\xB5\x81\xE9\x87\x8F" /* 流量 */,
    };
    lv_obj_t **cp_val[2] = { &s_cp_conns, &s_cp_traffic };
    for (int i = 0; i < 2; i++) {
        lv_label_set_text(mklabel(st, UI_PAD, 30 + i * 22, FCN_S, UI_C_TEXT_3), k_cp_cap[i]);
        *cp_val[i] = mklabel(st, UI_CARD_W - UI_PAD - 200, 30 + i * 22, FCN_S, UI_C_TEXT_2);
        lv_obj_set_width(*cp_val[i], 200);
        lv_obj_set_style_text_align(*cp_val[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    lv_obj_t *md = mk_card(t, 100, 106);
    /* 出口：用户关心的是「流量怎么走」，不是 mihomo 的模式名。「代理」= 规则模式；
     * 「全局」= 全局模式（跟原来的全局按钮一样，只切模式）；「直连·AI 不动」=
     * 规则模式 + 🚀 节点选择切 DIRECT（AI、VoWiFi 是独立的组，照旧走节点）；
     * 「全部直连」= 直连模式，AI 也直连。四个一行放不下，两行两列。
     * 全局曾被收进网页又加了回来：屏幕上已有的操作不要拿掉。 */
    lv_label_set_text(mklabel(md, UI_PAD, 8, FCN_S, UI_C_TEXT_3), "出口");
    s_cp_exit_note = mklabel(md, UI_CARD_W - UI_PAD - 200, 8, FCN_S, UI_C_TEXT_3);
    lv_obj_set_width(s_cp_exit_note, 200);
    lv_obj_set_style_text_align(s_cp_exit_note, LV_TEXT_ALIGN_RIGHT, 0);
    static const char *labels[4] = { "代理", "全局", "直连·AI 不动", "全部直连" };
    int bw = (UI_CARD_W - 2 * UI_PAD - 6) / 2;
    for (int i = 0; i < 4; i++) {
        s_cp_mode_btn[i] = lv_button_create(md);
        lv_obj_set_size(s_cp_mode_btn[i], bw, 30);
        lv_obj_set_style_radius(s_cp_mode_btn[i], 8, 0);
        lv_obj_align(s_cp_mode_btn[i], LV_ALIGN_TOP_LEFT, UI_PAD + (i % 2) * (bw + 6), 28 + (i / 2) * 36);
        lv_obj_add_event_cb(s_cp_mode_btn[i], chill_mode_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(s_cp_mode_btn[i]);
        lv_obj_set_style_text_font(l, FCN_S, 0);
        lv_label_set_text(l, labels[i]);
        lv_obj_center(l);
    }

    /* 策略组+节点/规则→节点流量——曾经是这个页面上一直展开的大块
     * （12 个组按钮 + 最多 200 行节点列表 + 6 行流量表，无论翻不翻到都已经
     * 建好），一屏塞满，2026-09-22 反馈说这样会卡。现在只留两行摘要+箭头，
     * 点进去才是那些重的内容所在的独立二级页（跟"专线节点 >"这类设置列表
     * 一个思路）。策略组和节点共用一行/一个页面（点组切节点，两者是同一个
     *选择器，不是两件事——2026-09-22 反馈："策略组下面展开节点啊，这两个
     * 是紧密关联的"）。 */
    lv_obj_t *nav = mk_card(t, 216, 30 + CHILL_NAV_ROWS * 44 + 8);
    static const char *const k_nav_cap[CHILL_NAV_ROWS] = {
        "\xE8\x8A\x82\xE7\x82\xB9" /* 节点 */,
        "\xE8\xA7\x84\xE5\x88\x99 \xE2\x86\x92 \xE8\x8A\x82\xE7\x82\xB9" /* 规则 → 节点 */,
    };
    static const int k_nav_child[CHILL_NAV_ROWS] = { SUB_CHILL_NODES, SUB_CHILL_PAIRS };
    for (int i = 0; i < CHILL_NAV_ROWS; i++) {
        lv_obj_t *row = lv_obj_create(nav);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, UI_CARD_W - 2 * UI_PAD, 40);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, UI_PAD, 8 + i * 44);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_C_TRACK), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, chill_nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k_nav_child[i]);
        s_cp_nav_row[i] = row;
        lv_label_set_text(mklabel(row, 10, 11, FCN_S, UI_C_TEXT), k_nav_cap[i]);
        s_cp_nav_val[i] = mklabel(row, UI_CARD_W - 2 * UI_PAD - 136, 11, FCN_S, UI_C_TEXT_3);
        lv_obj_set_width(s_cp_nav_val[i], 110);
        lv_obj_set_style_text_align(s_cp_nav_val[i], LV_TEXT_ALIGN_RIGHT, 0);
        /* 纯 ASCII，不用 LV_SYMBOL_RIGHT——那是私有区码位，只能配 montserrat，
         * 这一行其余文字都在 FCN_S（见 DESIGN.md 字体配对的坑）。 */
        lv_label_set_text(mklabel(row, UI_CARD_W - 2 * UI_PAD - 20, 11, FCN_S, UI_C_TEXT_3), ">");
    }

    /* LVGL's lv_slider/lv_button/lv_switch all set LV_OBJ_FLAG_SCROLL_ON_FOCUS
     * by default (see lv_slider.c/lv_button.c/lv_switch.c), and this project
     * never disables it (touch-only, no keypad/encoder group where focus-
     * driven scrolling would matter). Something focuses a widget the first
     * time a page with one of those is built, so the very first visit can
     * land scrolled a card or two down instead of at the top (caught on
     * the 系统 tab: 电池与负载 was the first thing visible, not 屏幕/亮度).
     * Pin it back to 0 once, right after building — this runs exactly once
     * per page (inside its build_* function), so it doesn't fight the user's
     * own scrolling on later visits. */
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* ---- CHILL / 节点 (drill-down from the CHILL page's nav row) ----
 * 策略组网格在上，选中组的成员节点列表紧跟在下面，一个页面——mihomo 自己
 * 的组成员本来就可能是别的策略组（比如按国家分的 🇹🇼台湾/🇯🇵日本，
 * 2026-09-22 反馈举的 Surge 配置例子同一个思路：上层组把这些地区组当成员
 * 选），选中一个之后 chill_select_group()/chill_select_node() 直接把成员
 * 名字发给 mihomo 的 select API，不关心那个名字背后是不是又是一个组——
 * 所以点组切换下面的节点列表、点节点（哪怕节点本身是个地区组）直接选中，
 * 都是已经支持的行为，不需要额外的树形递归。 */
static void build_sub_chill_nodes(lv_obj_t *t)
{
    int node_h = 30 + CHILL_MAX_NODES * 44 + 8;
    t = mk_scroll_h(t, UI_SUB_VIEW, CHILL_GRP_H + 10 + node_h + 16);

    s_cp_grp_card = mk_card(t, 8, CHILL_GRP_H);
    lv_label_set_text(mklabel(s_cp_grp_card, UI_PAD, 8, FCN_S, UI_C_TEXT_3),
                      "\xE7\xAD\x96\xE7\x95\xA5\xE7\xBB\x84" /* 策略组 */);
    int gbw = (UI_CARD_W - 2 * UI_PAD - (CHILL_GRP_COLS - 1) * 6) / CHILL_GRP_COLS;
    for (int i = 0; i < CHILL_MAX_GROUPS; i++) {
        int row = i / CHILL_GRP_COLS, col = i % CHILL_GRP_COLS;
        s_cp_grp_btn[i] = lv_button_create(s_cp_grp_card);
        lv_obj_set_size(s_cp_grp_btn[i], gbw, 30);
        lv_obj_set_style_radius(s_cp_grp_btn[i], 8, 0);
        lv_obj_align(s_cp_grp_btn[i], LV_ALIGN_TOP_LEFT,
                    UI_PAD + col * (gbw + 6), 30 + row * 36);
        lv_obj_add_event_cb(s_cp_grp_btn[i], chill_group_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        s_cp_grp_lbl[i] = lv_label_create(s_cp_grp_btn[i]);
        lv_obj_set_style_text_font(s_cp_grp_lbl[i], FCN_S, 0);
        lv_obj_set_width(s_cp_grp_lbl[i], gbw - 8);
        lv_label_set_long_mode(s_cp_grp_lbl[i], LV_LABEL_LONG_CLIP);
        lv_obj_center(s_cp_grp_lbl[i]);
        lv_obj_add_flag(s_cp_grp_btn[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_cp_node_card = mk_card(t, CHILL_GRP_H + 10, node_h);
    lv_label_set_text(mklabel(s_cp_node_card, UI_PAD, 8, FCN_S, UI_C_TEXT_3),
                      "\xE8\x8A\x82\xE7\x82\xB9" /* 节点 */);
    s_cp_delay_lbl = mklabel(s_cp_node_card, UI_CARD_W - UI_PAD - 90, 8, FCN_S, UI_C_ACCENT);
    lv_obj_set_width(s_cp_delay_lbl, 90);
    lv_obj_set_style_text_align(s_cp_delay_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_cp_delay_lbl, "\xE6\xB5\x8B\xE5\xBB\xB6\xE8\xBF\x9F" /* 测延迟 */);
    lv_obj_add_flag(s_cp_delay_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_cp_delay_lbl, chill_delay_cb, LV_EVENT_CLICKED, NULL);
    for (int i = 0; i < CHILL_MAX_NODES; i++) {
        lv_obj_t *row = lv_obj_create(s_cp_node_card);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, UI_CARD_W - 2 * UI_PAD, 40);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, UI_PAD, 30 + i * 44);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_C_TRACK), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, chill_node_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_cp_node_row[i]  = row;
        s_cp_node_name[i] = mklabel(row, 10, 10, FCN_S, UI_C_TEXT);
        lv_obj_set_width(s_cp_node_name[i], UI_CARD_W - 2 * UI_PAD - 100);
        lv_label_set_long_mode(s_cp_node_name[i], LV_LABEL_LONG_CLIP);
        s_cp_node_dl[i] = mklabel(row, UI_CARD_W - 2 * UI_PAD - 90, 11, FCN_S, UI_C_TEXT_3);
        lv_obj_set_width(s_cp_node_dl[i], 80);
        lv_obj_set_style_text_align(s_cp_node_dl[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* ---- CHILL / 规则 → 节点流量 (drill-down) ---- */
static void build_sub_chill_pairs(lv_obj_t *t)
{
    int traf_h = 30 + CHILL_TRAF_ROWS * 22 + 8;
    t = mk_scroll_h(t, UI_SUB_VIEW, traf_h + 16);
    build_traffic_card(t, 8,
                       "\xE8\xA7\x84\xE5\x88\x99 \xE2\x86\x92 \xE8\x8A\x82\xE7\x82\xB9\xE6\xB5\x81\xE9\x87\x8F" /* 规则 → 节点流量 */,
                       &s_cp_pair_card, s_cp_pair_name, s_cp_pair_val);
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}


/* ---- Tailscale subpage ----
 * The Home card answers "is the tunnel up"; this answers "who is on it".
 * Peer rows are fixed slots filled from tailscale.c's own parse of the
 * LocalAPI status — no extra request, and no objects created per refresh. */
static void build_sub_ts(lv_obj_t *t)
{
    t = mk_scroll_h(t, UI_SUB_VIEW, 40 + 118 + 10 + 30 + TS_PEER_MAX * 44 + 16);

    lv_obj_t *self = mk_card(t, 8, 118);
    lv_label_set_text(mklabel(self, UI_PAD, 8, FCN_S, UI_C_TEXT_3),
                      "\xE6\x9C\xAC\xE6\x9C\xBA" /* 本机 */);
    static const char *const k_self_cap[4] = {
        "\xE4\xB8\xBB\xE6\x9C\xBA\xE5\x90\x8D" /* 主机名 */, "IP",
        "DERP", "\xE5\xAD\x90\xE7\xBD\x91\xE8\xB7\xAF\xE7\x94\xB1" /* 子网路由 */,
    };
    for (int i = 0; i < 4; i++) {
        lv_label_set_text(mklabel(self, UI_PAD, 30 + i * 21, FCN_S, UI_C_TEXT_3), k_self_cap[i]);
        s_tp_self[i] = mklabel(self, UI_CARD_W - UI_PAD - 190, 30 + i * 21, FCN_S, UI_C_TEXT_2);
        lv_obj_set_width(s_tp_self[i], 190);
        lv_obj_set_style_text_align(s_tp_self[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    s_tp_card = mk_card(t, 136, 30 + TS_PEER_MAX * 44 + 8);
    lv_label_set_text(mklabel(s_tp_card, UI_PAD, 8, FCN_S, UI_C_TEXT_3),
                      "\xE8\x8A\x82\xE7\x82\xB9" /* 节点 */);
    for (int i = 0; i < TS_PEER_MAX; i++) {
        int sw = UI_CARD_W - 2 * UI_PAD;
        s_tp_row[i]  = mk_subcard(s_tp_card, 30 + i * 44, 40);
        s_tp_name[i] = mklabel(s_tp_row[i], 10, 4, FCN_S, UI_C_TEXT);
        lv_obj_set_width(s_tp_name[i], sw - 20 - 90);
        lv_label_set_long_mode(s_tp_name[i], LV_LABEL_LONG_CLIP);
        s_tp_ip[i]   = mklabel(s_tp_row[i], 10, 22, &lv_font_montserrat_12, UI_C_TEXT_3);
        s_tp_tag[i]  = mklabel(s_tp_row[i], sw - 10 - 90, 12, FCN_S, UI_C_TEXT_3);
        lv_obj_set_width(s_tp_tag[i], 90);
        lv_obj_set_style_text_align(s_tp_tag[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_add_flag(s_tp_row[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* LVGL's lv_slider/lv_button/lv_switch all set LV_OBJ_FLAG_SCROLL_ON_FOCUS
     * by default (see lv_slider.c/lv_button.c/lv_switch.c), and this project
     * never disables it (touch-only, no keypad/encoder group where focus-
     * driven scrolling would matter). Something focuses a widget the first
     * time a page with one of those is built, so the very first visit can
     * land scrolled a card or two down instead of at the top (caught on
     * the 系统 tab: 电池与负载 was the first thing visible, not 屏幕/亮度).
     * Pin it back to 0 once, right after building — this runs exactly once
     * per page (inside its build_* function), so it doesn't fight the user's
     * own scrolling on later visits. */
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* ---- 信令读取 subpage ----
 * Everything here comes from zwrt-datad's /state. The per-carrier MIMO/RB/
 * BLER numbers and the neighbour-cell list that the litehtml UI can show
 * come from a different endpoint (`/modem/latest-signals`), which the datad
 * build on this device does not implement at all — its route table is
 * /state, /events, /capabilities, /ubus, /ubus/call, /control, /healthz.
 * So this page shows what the device actually has and says plainly what it
 * doesn't, instead of rendering empty rows. */
static void build_sub_cell(lv_obj_t *t)
{
    t = mk_scroll_h(t, UI_SUB_VIEW, 660);

    lv_obj_t *nr = mk_card(t, 8, 156);
    lv_label_set_text(mklabel(nr, UI_PAD, 8, FCN_S, UI_C_TEXT_3),
                      "5G \xE6\x9C\x8D\xE5\x8A\xA1\xE5\xB0\x8F\xE5\x8C\xBA" /* 5G 服务小区 */);
    static const char *const k_nr_cap[6] = {
        "\xE9\xA2\x91\xE6\xAE\xB5" /* 频段 */, "ARFCN", "PCI",
        "Cell ID", "PLMN", "RSRP / RSRQ / SINR",
    };
    for (int i = 0; i < 6; i++) {
        lv_label_set_text(mklabel(nr, UI_PAD, 30 + i * 21, FCN_S, UI_C_TEXT_3), k_nr_cap[i]);
        s_sg_nr[i] = mklabel(nr, UI_CARD_W - UI_PAD - 200, 30 + i * 21, FCN_S, UI_C_TEXT_2);
        lv_obj_set_width(s_sg_nr[i], 200);
        lv_obj_set_style_text_align(s_sg_nr[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    lv_obj_t *lte = mk_card(t, 172, 56);
    lv_label_set_text(mklabel(lte, UI_PAD, 8, FCN_S, UI_C_TEXT_3), "LTE");
    s_sg_lte = mklabel(lte, UI_PAD, 30, FCN_S, UI_C_TEXT_2);
    lv_obj_set_width(s_sg_lte, UI_CARD_W - 2 * UI_PAD);
    lv_label_set_long_mode(s_sg_lte, LV_LABEL_LONG_CLIP);

    lv_obj_t *net = mk_card(t, 238, 118);
    lv_label_set_text(mklabel(net, UI_PAD, 8, FCN_S, UI_C_TEXT_3), "\xE7\xBD\x91\xE7\xBB\x9C" /* 网络 */);
    static const char *const k_net_cap[4] = {
        "\xE9\x80\x89\xE7\xBD\x91\xE6\x96\xB9\xE5\xBC\x8F" /* 选网方式 */,
        "WAN", "\xE5\x88\xB6\xE5\xBC\x8F" /* 制式 */,
        "\xE9\xAB\x98\xE9\x93\x81\xE6\xA8\xA1\xE5\xBC\x8F" /* 高铁模式 */,
    };
    for (int i = 0; i < 4; i++) {
        lv_label_set_text(mklabel(net, UI_PAD, 30 + i * 21, FCN_S, UI_C_TEXT_3), k_net_cap[i]);
        s_sg_net[i] = mklabel(net, UI_CARD_W - UI_PAD - 190, 30 + i * 21, FCN_S, UI_C_TEXT_2);
        lv_obj_set_width(s_sg_net[i], 190);
        lv_obj_set_style_text_align(s_sg_net[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    lv_obj_t *cap = mk_card(t, 366, 148);
    lv_label_set_text(mklabel(cap, UI_PAD, 8, FCN_S, UI_C_TEXT_3),
                      "\xE6\x94\xAF\xE6\x8C\x81\xE9\xA2\x91\xE6\xAE\xB5" /* 支持频段 */);
    lv_label_set_text(mklabel(cap, UI_PAD, 30, FCN_S, UI_C_TEXT_3), "5G");
    s_sg_nrb = mklabel(cap, UI_PAD + 30, 30, FCN_S, UI_C_TEXT_2);
    lv_obj_set_width(s_sg_nrb, UI_CARD_W - 2 * UI_PAD - 30);
    lv_label_set_long_mode(s_sg_nrb, LV_LABEL_LONG_WRAP);
    lv_label_set_text(mklabel(cap, UI_PAD, 84, FCN_S, UI_C_TEXT_3), "LTE");
    s_sg_lteb = mklabel(cap, UI_PAD + 30, 84, FCN_S, UI_C_TEXT_2);
    lv_obj_set_width(s_sg_lteb, UI_CARD_W - 2 * UI_PAD - 30);
    lv_label_set_long_mode(s_sg_lteb, LV_LABEL_LONG_WRAP);

    lv_obj_t *note = mk_card(t, 524, 96);
    lv_label_set_text(mklabel(note, UI_PAD, 8, FCN_S, UI_C_TEXT_3),
                      "\xE9\x82\xBB\xE5\xB0\x8F\xE5\x8C\xBA / \xE8\xB0\x83\xE5\xBA\xA6\xE6\x98\x8E\xE7\xBB\x86" /* 邻小区 / 调度明细 */);
    lv_obj_t *nl = mklabel(note, UI_PAD, 30, FCN_S, UI_C_TEXT_3);
    lv_obj_set_width(nl, UI_CARD_W - 2 * UI_PAD);
    lv_label_set_long_mode(nl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(nl, "本机的 zwrt-datad 没有 /modem/latest-signals 接口，"
                          "MIMO/层数/RB/BLER 和邻小区列表拿不到数据。");

    /* LVGL's lv_slider/lv_button/lv_switch all set LV_OBJ_FLAG_SCROLL_ON_FOCUS
     * by default (see lv_slider.c/lv_button.c/lv_switch.c), and this project
     * never disables it (touch-only, no keypad/encoder group where focus-
     * driven scrolling would matter). Something focuses a widget the first
     * time a page with one of those is built, so the very first visit can
     * land scrolled a card or two down instead of at the top (caught on
     * the 系统 tab: 电池与负载 was the first thing visible, not 屏幕/亮度).
     * Pin it back to 0 once, right after building — this runs exactly once
     * per page (inside its build_* function), so it doesn't fight the user's
     * own scrolling on later visits. */
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
    /* Muted blue for "included", not full accent: the default state is every
     * band selected, and 21 saturated chips read as 21 alarms. */
    lv_obj_set_style_bg_color(g->chip[i], lv_color_hex(g->sel[i] ? 0x28415e : UI_C_TRACK), 0);
    lv_obj_set_style_text_color(g->chip_lbl[i],
                                lv_color_hex(g->sel[i] ? UI_C_TEXT : UI_C_TEXT_3), 0);
}

static void band_chip_cb(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    int gi = code / 64, i = code % 64;
    if (gi < 0 || gi > 2 || i >= s_bg[gi].n) return;
    s_bg[gi].sel[i] = !s_bg[gi].sel[i];
    band_chip_paint(gi, i);
    band_summary_set(gi);
}

static void band_apply_cb(lv_event_t *e)
{
    int gi = (int)(intptr_t)lv_event_get_user_data(e);
    band_group_t *g = &s_bg[gi];
    uint32_t now = lv_tick_get();

    if (g->arm && now - g->arm < 5000) {
        g->arm = 0;
        lv_label_set_text(g->apply_lbl, "\xE5\xB7\xB2\xE4\xB8\x8B\xE5\x8F\x91" /* 已下发 */);
        band_group_apply(gi);
        return;
    }
    g->arm = now;
    lv_label_set_text(g->apply_lbl, "\xE5\x86\x8D\xE6\x8C\x89\xE4\xB8\x80\xE6\xAC\xA1\xE7\xA1\xAE\xE8\xAE\xA4" /* 再按一次确认 */);
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
        lv_label_set_text(s_lk_mode_lbl, "\xE5\xB7\xB2\xE4\xB8\x8B\xE5\x8F\x91" /* 已下发 */);
        return;
    }
    s_lk_mode_arm = now;
    s_lk_mode_pending = idx;
    lv_label_set_text(s_lk_mode_lbl, "\xE5\x86\x8D\xE6\x8C\x89\xE4\xB8\x80\xE6\xAC\xA1\xE7\xA1\xAE\xE8\xAE\xA4\xE5\x88\x87\xE6\x8D\xA2" /* 再按一次确认切换 */);
    for (int i = 0; i < 4; i++)
        lv_obj_set_style_bg_color(s_lk_mode_btn[i],
            lv_color_hex(i == idx ? UI_C_WARN : 0x394049), 0);
}

static void lk_reset_cb(lv_event_t *e)
{
    uint32_t now = lv_tick_get();
    LV_UNUSED(e);
    if (s_lk_reset_arm && now - s_lk_reset_arm < 5000) {
        s_lk_reset_arm = 0;
        system("ubus call zte_nwinfo_api nwinfo_reset_band_cell_setting '{}' >/dev/null 2>&1 &");
        lv_label_set_text(s_lk_reset_lbl, "\xE5\xB7\xB2\xE6\x81\xA2\xE5\xA4\x8D\xE9\xBB\x98\xE8\xAE\xA4" /* 已恢复默认 */);
        return;
    }
    s_lk_reset_arm = now;
    lv_label_set_text(s_lk_reset_lbl, "\xE5\x86\x8D\xE6\x8C\x89\xE4\xB8\x80\xE6\xAC\xA1\xE7\xA1\xAE\xE8\xAE\xA4" /* 再按一次确认 */);
}

static int band_group_build(lv_obj_t *t, int gi, int y, const char *title, char prefix)
{
    band_group_t *g = &s_bg[gi];
    int cw = (UI_CARD_W - 2 * UI_PAD - 5 * 4) / 6;   /* 6 chips per row */
    int rows = (BAND_MAX + 5) / 6;
    int h = 52 + rows * 30 + 40;

    g->prefix = prefix;
    g->card = mk_card(t, y, h);
    lv_label_set_text(mklabel(g->card, UI_PAD, 8, FCN_S, UI_C_TEXT_3), title);
    g->summary = mklabel(g->card, UI_PAD, 28, FCN_S, UI_C_TEXT_2);
    lv_obj_set_width(g->summary, UI_CARD_W - 2 * UI_PAD);
    lv_label_set_long_mode(g->summary, LV_LABEL_LONG_CLIP);
    lv_label_set_text(g->summary, "—");

    for (int i = 0; i < BAND_MAX; i++) {
        lv_obj_t *c = lv_obj_create(g->card);
        lv_obj_remove_style_all(c);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(c, cw, 26);
        lv_obj_align(c, LV_ALIGN_TOP_LEFT, UI_PAD + (i % 6) * (cw + 4), 52 + (i / 6) * 30);
        lv_obj_set_style_radius(c, 6, 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(UI_C_TRACK), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, band_chip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(gi * 64 + i));
        lv_obj_t *l = lv_label_create(c);
        lv_obj_set_style_text_font(l, FCN_S, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(UI_C_TEXT_3), 0);
        lv_label_set_text(l, "");
        lv_obj_center(l);
        g->chip[i] = c;
        g->chip_lbl[i] = l;
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *btn = lv_button_create(g->card);
    lv_obj_set_size(btn, UI_CARD_W - 2 * UI_PAD, 32);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, UI_PAD, 52 + rows * 30 + 4);
    lv_obj_add_event_cb(btn, band_apply_cb, LV_EVENT_CLICKED, (void *)(intptr_t)gi);
    g->apply_lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(g->apply_lbl, FCN_S, 0);
    lv_label_set_text(g->apply_lbl, "\xE5\xBA\x94\xE7\x94\xA8\xE9\x94\x81\xE9\xA2\x91" /* 应用锁频 */);
    lv_obj_center(g->apply_lbl);
    return y + h + 10;
}

static void build_sub_lock(lv_obj_t *t)
{
    t = mk_scroll_h(t, UI_SUB_VIEW, 1120);

    lv_obj_t *md = mk_card(t, 8, 106);
    lv_label_set_text(mklabel(md, UI_PAD, 8, FCN_S, UI_C_TEXT_3),
                      "\xE9\x80\x89\xE7\xBD\x91\xE6\x96\xB9\xE5\xBC\x8F" /* 选网方式 */);
    static const char *const k_mode_lab[4] = { "自动", "5G SA", "5G NSA", "4G" };
    int bw = (UI_CARD_W - 2 * UI_PAD - 3 * 5) / 4;
    for (int i = 0; i < 4; i++) {
        s_lk_mode_btn[i] = lv_button_create(md);
        lv_obj_set_size(s_lk_mode_btn[i], bw, 32);
        lv_obj_set_style_radius(s_lk_mode_btn[i], 8, 0);
        lv_obj_align(s_lk_mode_btn[i], LV_ALIGN_TOP_LEFT, UI_PAD + i * (bw + 5), 30);
        lv_obj_add_event_cb(s_lk_mode_btn[i], lk_mode_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(s_lk_mode_btn[i]);
        lv_obj_set_style_text_font(l, FCN_S, 0);
        lv_label_set_text(l, k_mode_lab[i]);
        lv_obj_center(l);
    }
    s_lk_mode_lbl = mklabel(md, UI_PAD, 72, FCN_S, UI_C_TEXT_3);
    lv_obj_set_width(s_lk_mode_lbl, UI_CARD_W - 2 * UI_PAD);
    lv_label_set_long_mode(s_lk_mode_lbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_lk_mode_lbl, "切换会短暂断网，需要按两次确认");

    int y = 124;
    y = band_group_build(t, BG_SA,  y, "5G SA \xE9\xA2\x91\xE6\xAE\xB5" /* 频段 */, 'n');
    y = band_group_build(t, BG_NSA, y, "5G NSA \xE9\xA2\x91\xE6\xAE\xB5", 'n');
    y = band_group_build(t, BG_LTE, y, "4G \xE9\xA2\x91\xE6\xAE\xB5", 'B');

    lv_obj_t *rst = mk_card(t, y, 78);
    lv_obj_t *rbtn = lv_button_create(rst);
    lv_obj_set_size(rbtn, UI_CARD_W - 2 * UI_PAD, 32);
    lv_obj_set_style_radius(rbtn, 8, 0);
    lv_obj_set_style_bg_color(rbtn, lv_color_hex(0x394049), 0);
    lv_obj_align(rbtn, LV_ALIGN_TOP_LEFT, UI_PAD, 10);
    lv_obj_add_event_cb(rbtn, lk_reset_cb, LV_EVENT_CLICKED, NULL);
    s_lk_reset_lbl = lv_label_create(rbtn);
    lv_obj_set_style_text_font(s_lk_reset_lbl, FCN_S, 0);
    lv_label_set_text(s_lk_reset_lbl, "\xE6\x81\xA2\xE5\xA4\x8D\xE9\xBB\x98\xE8\xAE\xA4\xE9\x85\x8D\xE7\xBD\xAE" /* 恢复默认配置 */);
    lv_obj_center(s_lk_reset_lbl);
    lv_obj_t *rh = mklabel(rst, UI_PAD, 50, FCN_S, UI_C_TEXT_3);
    lv_obj_set_width(rh, UI_CARD_W - 2 * UI_PAD);
    lv_label_set_long_mode(rh, LV_LABEL_LONG_WRAP);
    lv_label_set_text(rh, "清掉全部锁频和锁小区设置，回到自动选网");

    /* LVGL's lv_slider/lv_button/lv_switch all set LV_OBJ_FLAG_SCROLL_ON_FOCUS
     * by default (see lv_slider.c/lv_button.c/lv_switch.c), and this project
     * never disables it (touch-only, no keypad/encoder group where focus-
     * driven scrolling would matter). Something focuses a widget the first
     * time a page with one of those is built, so the very first visit can
     * land scrolled a card or two down instead of at the top (caught on
     * the 系统 tab: 电池与负载 was the first thing visible, not 屏幕/亮度).
     * Pin it back to 0 once, right after building — this runs exactly once
     * per page (inside its build_* function), so it doesn't fight the user's
     * own scrolling on later visits. */
    lv_obj_scroll_to_y(t, 0, LV_ANIM_OFF);
}

/* Fill a band group's chips from the modem's supported-band CSV. Only runs
 * when that CSV changes (it is a capability list, so effectively once), and
 * never creates objects — the chips are pre-built and hidden. */
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
    t = mk_scroll_h(t, UI_SUB_VIEW,
                    8 + 210 + 10 + 30 + ST_SRV_ROWS * 40 + 16);

    s_st_card = mk_card(t, 8, 210);
    lv_label_set_text(mklabel(s_st_card, UI_PAD, 8, FCN_S, UI_C_TEXT_3),
                      "\xE7\xBD\x91\xE7\xBB\x9C\xE6\xB5\x8B\xE9\x80\x9F" /* 网络测速 */);
    s_st_phase = mklabel(s_st_card, UI_CARD_W - UI_PAD - 120, 8, FCN_S, UI_C_TEXT_2);
    lv_obj_set_width(s_st_phase, 120);
    lv_obj_set_style_text_align(s_st_phase, LV_TEXT_ALIGN_RIGHT, 0);

    /* Headline number — this page's one job, so it gets the biggest type on
     * screen (matches the 性能测试 page's FPS/touch-rate treatment, same
     * montserrat_20+ scale for "the number you opened this page to see").
     * Digits + "Mbps" are all ASCII, montserrat is fine here (no ↓↑ glyphs
     * in this label, so the CJK-only font-pairing rule doesn't apply). */
    s_st_live = mklabel(s_st_card, UI_PAD, 32, &lv_font_montserrat_28, UI_C_ACCENT);
    lv_label_set_text(s_st_live, "--");

    s_st_detail = mklabel(s_st_card, UI_PAD, 74, FCN_S, UI_C_TEXT_2);
    lv_obj_set_width(s_st_detail, UI_CARD_W - 2 * UI_PAD);

    s_st_result = mklabel(s_st_card, UI_PAD, 96, FCN_S, UI_C_TEXT_2);
    lv_obj_set_width(s_st_result, UI_CARD_W - 2 * UI_PAD);

    s_st_server = mklabel(s_st_card, UI_PAD, 118, FCN_S, UI_C_TEXT_3);
    lv_obj_set_width(s_st_server, UI_CARD_W - 2 * UI_PAD);
    lv_label_set_long_mode(s_st_server, LV_LABEL_LONG_CLIP);

    s_st_btn = lv_button_create(s_st_card);
    lv_obj_set_size(s_st_btn, UI_CARD_W - 2 * UI_PAD, 40);
    lv_obj_set_style_radius(s_st_btn, 10, 0);
    lv_obj_align(s_st_btn, LV_ALIGN_TOP_LEFT, UI_PAD, 146);
    lv_obj_add_event_cb(s_st_btn, speedtest_btn_cb, LV_EVENT_CLICKED, NULL);
    s_st_btn_lbl = lv_label_create(s_st_btn);
    lv_obj_set_style_text_font(s_st_btn_lbl, FCN_S, 0);
    lv_label_set_text(s_st_btn_lbl, "\xE5\xBC\x80\xE5\xA7\x8B\xE6\xB5\x8B\xE9\x80\x9F" /* 开始测速 */);
    lv_obj_center(s_st_btn_lbl);

    /* zte-agent unreachable / no password on file (agent not running, or
     * ZTE_AGENT_PASSWORD missing from start_zte_agent.sh) — say so instead
     * of a card that just sits on "--" forever with a dead button. Same
     * pattern as eSIM's offline message. */
    s_st_offline = mklabel(s_st_card, UI_PAD, 190, FCN_S, UI_C_BAD);
    lv_obj_set_width(s_st_offline, UI_CARD_W - 2 * UI_PAD);
    lv_label_set_long_mode(s_st_offline, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(s_st_offline, LV_OBJ_FLAG_HIDDEN);

    /* Server list — 管理网页那边有下拉选服务器，这版一开始漏了，一律自动
     * 选最佳（2026-09-22 用户指出）。固定槽位、按实际拿到的数量隐藏/显示，
     * 同 CHILL 节点列表、eSIM profile 列表一个模式。"自动"固定占第 0 行，
     * 不算进 speedtest_servers_count()。 */
    s_st_srv_card = mk_card(t, 8 + 210 + 10, 30 + ST_SRV_ROWS * 40 + 8);
    lv_label_set_text(mklabel(s_st_srv_card, UI_PAD, 8, FCN_S, UI_C_TEXT_3),
                      "\xE6\x9C\x8D\xE5\x8A\xA1\xE5\x99\xA8" /* 服务器 */);
    for (int i = 0; i < ST_SRV_ROWS; i++) {
        lv_obj_t *row = lv_obj_create(s_st_srv_card);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, UI_CARD_W - 2 * UI_PAD, 34);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, UI_PAD, 30 + i * 40);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_C_TRACK), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, speedtest_srv_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_st_srv_row[i] = row;
        s_st_srv_name[i] = mklabel(row, 10, 9, FCN_S, UI_C_TEXT);
        lv_obj_set_width(s_st_srv_name[i], UI_CARD_W - 2 * UI_PAD - 20);
        lv_label_set_long_mode(s_st_srv_name[i], LV_LABEL_LONG_CLIP);
        if (i > 0) lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);   /* 等列表拉回来再显示 */
    }
    lv_label_set_text(s_st_srv_name[0],
                      "\xE8\x87\xAA\xE5\x8A\xA8\xEF\xBC\x88\xE6\x9C\x80\xE4\xBD\xB3\xE6\x9C\x8D\xE5\x8A\xA1\xE5\x99\xA8\xEF\xBC\x89" /* 自动（最佳服务器） */);

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
    int ntile = (int)(sizeof k_tiles / sizeof k_tiles[0]);
    int tw = (UI_CARD_W - 10) / 2;
    for (int k = 0; k < ntile; k++) {
        int i = k_tiles[k];
        lv_obj_t *tile = lv_obj_create(t);
        lv_obj_remove_style_all(tile);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(tile, tw, 74);
        lv_obj_align(tile, LV_ALIGN_TOP_LEFT, UI_INSET + (k % 2) * (tw + 10),
                     10 + (k / 2) * 84);
        lv_obj_set_style_radius(tile, 14, 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(UI_C_CARD), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile, tile_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *nm = mklabel(tile, 12, 14, FCN, i == SUB_PERF ? UI_C_TEXT_3 : UI_C_TEXT);
        lv_label_set_text(nm, k_tile_name[i]);
        s_tile_sub[i] = mklabel(tile, 12, 44, FCN_S, UI_C_TEXT_3);
        lv_obj_set_width(s_tile_sub[i], tw - 24);
        lv_label_set_long_mode(s_tile_sub[i], LV_LABEL_LONG_CLIP);
        /* Tiles whose subtitle isn't live data get a static one here, so no
         * tile ships with an empty second line. */
        if (k_tile_static[i]) lv_label_set_text(s_tile_sub[i], k_tile_static[i]);
    }
}

static void bench_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    s_box_x += s_box_dir * 4;
    if (s_box_x >= 16) { s_box_x = 16; s_box_dir = -1; }
    if (s_box_x <= 0)  { s_box_x = 0;  s_box_dir = 1; }
    lv_obj_set_pos(s_t_box, UI_INSET + s_box_x, 150);
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
    lv_obj_set_style_text_color(s_w_state,
        lv_color_hex(d->wifi_enabled ? UI_C_OK : UI_C_TEXT_3), 0);

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
        if (i >= n) { lv_obj_add_flag(s_w_cli[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(s_w_cli[i], LV_OBJ_FLAG_HIDDEN);
        set_label_fmt(s_w_cli_name[i], c_cli_name[i], sizeof c_cli_name[i], "%s",
                      d->client[i].name[0] ? d->client[i].name : "?");
        set_label_fmt(s_w_cli_ip[i], c_cli_ip[i], sizeof c_cli_ip[i], "%s", d->client[i].ip);
        set_label_fmt(s_w_cli_mac[i], c_cli_mac[i], sizeof c_cli_mac[i], "%s", d->client[i].mac);
    }
    /* Reflow: shrink the card to the rows actually shown and pull DHCP up
     * behind it, same rule as Home's carrier card — property updates on
     * existing objects, no allocation. */
    int cli_h = 30 + (n ? n : 1) * 48 + 8;
    lv_obj_set_height(s_w_cli_card, cli_h);
    lv_obj_align(s_w_dhcp_card, LV_ALIGN_TOP_LEFT, UI_INSET, 310 + cli_h + 10);

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
        static int c_alert = -1;
        int st = ah.lost_secs ? 2 : ah.unread > 0 ? 1 : 0;
        if (st != c_alert) {
            c_alert = st;
            if (st == 0) {
                lv_obj_add_flag(s_top_alert, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_top_alert_hit, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_set_style_bg_color(s_top_alert, lv_color_hex(st == 2 ? UI_C_BAD : UI_C_WARN), 0);
                lv_obj_remove_flag(s_top_alert, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_top_alert_hit, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    {
        static char c_h[40] = "";
        static uint32_t c_hcol;
        uint32_t col = UI_C_TEXT_3;
        if (!ah.checked) {
            set_label_fmt(s_set_health, c_h, sizeof c_h, "%s", "—");
        } else if (ah.bad) {
            col = UI_C_BAD;
            set_label_fmt(s_set_health, c_h, sizeof c_h, "■ %d 项异常 ›", ah.bad);
        } else if (ah.warn) {
            col = UI_C_WARN;
            set_label_fmt(s_set_health, c_h, sizeof c_h, "▲ %d 项注意 ›", ah.warn);
        } else {
            col = UI_C_OK;
            set_label_fmt(s_set_health, c_h, sizeof c_h, "%s", "● 一切正常 ›");
        }
        if (col != c_hcol) {
            c_hcol = col;
            lv_obj_set_style_text_color(s_set_health, lv_color_hex(col), 0);
        }
    }

    if (!data_refresh(&d)) {
        /* Backend down is a device-wide condition, so it is reported once in
         * the shared banner instead of overwriting home-page content. */
        static char c_op_down[8] = "", c_sum_down[4] = "";
        banner_set("后台数据服务不可用");
        set_label_fmt(s_operator, c_op_down, sizeof c_op_down, "%s", "—");
        set_label_fmt(s_cc_sum, c_sum_down, sizeof c_sum_down, "%s", "");
        return;
    }
    if (ah.lost_secs) {
        /* Same banner, lower priority than "data service down" above. */
        char msg[64];
        snprintf(msg, sizeof msg, "管理后台失联 %ld 分钟", ah.lost_secs / 60);
        banner_set(msg);
    } else {
        banner_set(NULL);
    }

    /* Top status bar — same signal-strength color tiers as the Home card's
     * dots, reused here for the always-visible summary. */
    {
        static char c_net[16] = "", c_updown[32] = "", c_bat[8] = "";
        uint32_t top_sig_col = d.bars <= 1 ? UI_C_BAD : d.bars <= 2 ? UI_C_WARN : UI_C_OK;
        for (int i = 0; i < 5; i++)
            lv_obj_set_style_bg_color(s_top_sig[i],
                lv_color_hex(i < d.bars ? top_sig_col : UI_C_TRACK), 0);
        set_label_fmt(s_top_net, c_net, sizeof c_net, "%s", d.net_type);
        char dn[24], up[24];
        if (s_cf_speed_bits) {
            fmt_rate_compact_bits(dn, sizeof dn, d.rx_speed);
            fmt_rate_compact_bits(up, sizeof up, d.tx_speed);
        } else {
            fmt_rate_compact(dn, sizeof dn, d.rx_speed);
            fmt_rate_compact(up, sizeof up, d.tx_speed);
        }
        set_label_fmt(s_top_updown, c_updown, sizeof c_updown,
                      "\xE2\x86\x93%s \xE2\x86\x91%s", dn, up);   /* ↓ ↑ */
        set_label_fmt(s_top_bat, c_bat, sizeof c_bat, "%d", d.bat_percent);
        lv_label_set_text(s_top_bat2, c_bat);   /* fake-bold second pass, kept in lockstep */
        int fill_w = 31 * d.bat_percent / 100;
        if (fill_w < 1 && d.bat_percent > 0) fill_w = 1;   /* stay visible at very low % */
        lv_obj_set_width(s_top_bat_fill, fill_w);
        /* Charging shown as fill color, not an overlaid bolt glyph (see the
         * build-time comment on why the glyph approach was dropped).
         * d.charging is a multi-value status CODE (seen "2" == Discharging
         * on real hardware, /data/plugins/zwrt-datad's own "charging"
         * field), not a plain bool — OR-ing it in (2026-09-21's mistake)
         * made is_charging true almost always, since any nonzero status
         * code is truthy in C. That's why the fill color never changed:
         * it was stuck reading "charging" permanently. htmlmain.c's own
         * status-bar draw (draw_native_statusbar(), which this LVGL icon
         * is modeled on) uses ONLY charger_connect for exactly this reason
         * (src/htmlmain.c:3943, g_charging = d.charger_connect) — match
         * that, not the other g_charging-adjacent expression at
         * htmlmain.c:2194 that mixes both fields for a different purpose
         * (a text label, not this color). */
        int is_charging = d.charger_connect;
        uint32_t fill_col = is_charging ? UI_C_ACCENT
                           : d.bat_percent <= 15 ? UI_C_BAD : UI_C_OK;
        lv_obj_set_style_bg_color(s_top_bat_fill, lv_color_hex(fill_col), 0);
    }

    /* ---- Home: cellular card ----
     * Headline first (mode · carrier count · TOTAL aggregated MHz), then one
     * row per carrier. NR carriers come from `nrca`, LTE from `lteca`; both
     * use the documented 11-field layout. The total bandwidth counts every
     * carrier the modem reports, active or not, exactly like
     * htmlmain.c's own summary (`total_show_bw`). */
    static char c_operator[48] = "", c_qci[16] = "", c_sum[64] = "", c_ambr[24] = "";
    set_label_fmt(s_operator, c_operator, sizeof c_operator, "%s",
                  d.operator_name[0] ? d.operator_name : "—");
    set_label_fmt(s_cc_qci, c_qci, sizeof c_qci, "QCI %d", d.qci);
    set_label_fmt(s_cc_ambr, c_ambr, sizeof c_ambr, "AMBR %d/%d", (int)d.ambr_dl, (int)d.ambr_ul);
    {
        static char c_ca_title[CA_SLOTS][32], c_ca_tag[CA_SLOTS][16],
                    c_ca_rsrp[CA_SLOTS][16], c_ca_sinr[CA_SLOTS][16], c_ca_freq[CA_SLOTS][48];
        ca_carrier_t ca[CA_SLOTS];
        char pfx[CA_SLOTS];
        int ca_n = 0;
        /* The serving cell is NOT in `nrca` — or rather, it may appear there
         * as a floor-RSRP entry, but its live signal only exists in the
         * primary nr_* fields. htmlmain.c builds the list the same way:
         * serving cell first from nr_band/nr_bw/nr_rsrp/nr_pci/nr_channel,
         * then every `nrca` group appended as-is (duplicated band included,
         * because a configured-but-inactive carrier on the serving band is
         * a real thing the modem reports). An earlier pass only used `nrca`
         * and so showed "2 载波" with both of them 未激活 while the device
         * was plainly connected. */
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
        if (lte_cc && nr_cc) snprintf(cnt, sizeof cnt, "%d LTE + %d NR \xE8\xBD\xBD\xE6\xB3\xA2", lte_cc, nr_cc);
        else if (nr_cc)      snprintf(cnt, sizeof cnt, "%d NR \xE8\xBD\xBD\xE6\xB3\xA2", nr_cc);
        else if (lte_cc)     snprintf(cnt, sizeof cnt, "%d LTE \xE8\xBD\xBD\xE6\xB3\xA2", lte_cc);
        else                 snprintf(cnt, sizeof cnt, "\xE6\x97\xA0\xE8\xBD\xBD\xE6\xB3\xA2" /* 无载波 */);
        set_label_fmt(s_cc_sum, c_sum, sizeof c_sum, "%s \xC2\xB7 %s \xC2\xB7 %d MHz",
                      d.net_type[0] ? d.net_type : "-", cnt, total_bw);

        int y = 58;
        for (int i = 0; i < CA_SLOTS; i++) {
            if (i >= ca_n) { lv_obj_add_flag(s_ca_card[i], LV_OBJ_FLAG_HIDDEN); continue; }
            lv_obj_remove_flag(s_ca_card[i], LV_OBJ_FLAG_HIDDEN);
            int act = ca[i].active;
            char band_s[16];
            if (ca[i].band)        snprintf(band_s, sizeof band_s, "%c%d", pfx[i], ca[i].band);
            else if (pfx[i] == 'n') snprintf(band_s, sizeof band_s, "%s", d.nr_band[0] ? d.nr_band : "-");
            else                    snprintf(band_s, sizeof band_s, "%s", d.band[0] ? d.band : "-");
            /* Floats go through real snprintf into a buffer, never through a
             * %f in set_label_fmt — see [[lvgl-sprintf-no-float]]. */
            char rsrp_s[12], sinr_s[12];
            snprintf(rsrp_s, sizeof rsrp_s, "%.0f", ca[i].rsrp);
            snprintf(sinr_s, sizeof sinr_s, "%.1f", ca[i].sinr);

            set_label_fmt(s_ca_title[i], c_ca_title[i], sizeof c_ca_title[i],
                          "%s %dM", band_s, ca[i].bw);
            set_label_fmt(s_ca_tag[i], c_ca_tag[i], sizeof c_ca_tag[i], "%s",
                          act ? "" : "\xE6\x9C\xAA\xE6\xBF\x80\xE6\xB4\xBB" /* 未激活 */);

            /* An inactive carrier's RSRP/SINR are floor sentinels (-140 /
             * -23), so its row collapses to one line: band, the 未激活 tag
             * and where it sits. Only active carriers earn the two-row
             * treatment with signal numbers. */
            if (act) {
                set_label_fmt(s_ca_rsrp[i], c_ca_rsrp[i], sizeof c_ca_rsrp[i], "%s", rsrp_s);
                set_label_fmt(s_ca_sinr[i], c_ca_sinr[i], sizeof c_ca_sinr[i], "%s", sinr_s);
                set_label_fmt(s_ca_freq[i], c_ca_freq[i], sizeof c_ca_freq[i],
                              "%s %ld\nPCI %d", pfx[i] == 'n' ? "ARFCN" : "EARFCN",
                              ca[i].arfcn, ca[i].pci);
                lv_obj_remove_flag(s_ca_rsrp[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_ca_sinr[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_ca_cap_rsrp[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_ca_cap_sinr[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_width(s_ca_freq[i], 140);
                lv_obj_align(s_ca_freq[i], LV_ALIGN_TOP_LEFT,
                             UI_CARD_W - 2 * UI_PAD - 10 - 140, 6);
                lv_obj_set_style_text_align(s_ca_freq[i], LV_TEXT_ALIGN_RIGHT, 0);
                lv_obj_set_height(s_ca_card[i], 78);
                /* Both numbers carry quality colour, same tiers as
                 * htmlmain.c's car_row(): RSRP -85/-105, SINR 13/0. */
                uint32_t rc = ca[i].rsrp >= -85 ? UI_C_OK : ca[i].rsrp >= -105 ? UI_C_WARN : UI_C_BAD;
                uint32_t sc = ca[i].sinr >= 13  ? UI_C_OK : ca[i].sinr >= 0    ? UI_C_WARN : UI_C_BAD;
                lv_obj_set_style_text_color(s_ca_rsrp[i], lv_color_hex(rc), 0);
                lv_obj_set_style_text_color(s_ca_sinr[i], lv_color_hex(sc), 0);
            } else {
                set_label_fmt(s_ca_freq[i], c_ca_freq[i], sizeof c_ca_freq[i],
                              "%s %ld \xC2\xB7 PCI %d", pfx[i] == 'n' ? "ARFCN" : "EARFCN",
                              ca[i].arfcn, ca[i].pci);
                lv_obj_add_flag(s_ca_rsrp[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_ca_sinr[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_ca_cap_rsrp[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_ca_cap_sinr[i], LV_OBJ_FLAG_HIDDEN);
                /* Full inner width on the second line: at 140px the
                 * "ARFCN 627264 · PCI 425" string wrapped and the tail was
                 * clipped by the card. */
                lv_obj_set_width(s_ca_freq[i], UI_CARD_W - 2 * UI_PAD - 20);
                lv_obj_align(s_ca_freq[i], LV_ALIGN_TOP_LEFT, 10, 28);
                lv_obj_set_style_text_align(s_ca_freq[i], LV_TEXT_ALIGN_LEFT, 0);
                lv_obj_set_height(s_ca_card[i], 50);
            }
            lv_obj_set_style_text_color(s_ca_title[i],
                                        lv_color_hex(act ? UI_C_TEXT : UI_C_TEXT_3), 0);
            lv_obj_set_style_border_opa(s_ca_card[i], act ? LV_OPA_20 : LV_OPA_10, 0);
            lv_obj_align(s_ca_card[i], LV_ALIGN_TOP_LEFT, UI_PAD, y);
            y += (act ? 78 : 50) + 8;
        }

        /* 今日/本月流量：firmware（zwrt_data ubus）自己按日历日/月累计好的
         * 计数器，不是拿 rx_bytes/tx_bytes（本次开机会话流量）自己再按
         * 时间窗口加总——那两个字段开机就清零，answer 不了"这个月用了多少"。 */
        {
            char c_day[32], c_month[32];
            fmt_bytes_total(c_day, sizeof c_day, d.day_rx_bytes + d.day_tx_bytes);
            fmt_bytes_total(c_month, sizeof c_month, d.month_rx_bytes + d.month_tx_bytes);
            static char c_traf[80] = "";
            set_label_fmt(s_cc_traffic, c_traf, sizeof c_traf,
                          "\xE4\xBB\x8A\xE6\x97\xA5 %s \xC2\xB7 \xE6\x9C\xAC\xE6\x9C\x88 %s" /* 今日 X · 本月 Y */,
                          c_day, c_month);
            lv_obj_align(s_cc_traffic, LV_ALIGN_TOP_LEFT, UI_PAD, y);
            y += 22;
        }

        /* Reflow the card stack: the cellular card shrinks to the rows it
         * actually shows, and the cards under it follow. Property updates on
         * existing objects only — nothing is allocated here. */
        int cell_h = y + 4;
        lv_obj_set_height(s_cell_card, cell_h);
        int sc_y = 10 + cell_h + 10;
        lv_obj_align(s_sc_card, LV_ALIGN_TOP_LEFT, UI_INSET, sc_y);
        int ts_y = sc_y + (lv_obj_has_flag(s_sc_card, LV_OBJ_FLAG_HIDDEN) ? 0 : SC_CARD_H + 10);
        lv_obj_align(s_ts_card, LV_ALIGN_TOP_LEFT, UI_INSET, ts_y);
        lv_obj_align(s_chill_card, LV_ALIGN_TOP_LEFT, UI_INSET,
                     ts_y + (lv_obj_has_flag(s_ts_card, LV_OBJ_FLAG_HIDDEN) ? 0 : 126));
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
                lv_obj_set_style_text_color(s_sc_state,
                    lv_color_hex(sc.enabled ? UI_C_TEXT : UI_C_TEXT_3), 0);
                if (sc.last_switch > 0) {
                    time_t tt = (time_t)sc.last_switch;
                    struct tm tm;
                    localtime_r(&tt, &tm);
                    strftime(when, sizeof when, "%m-%d %H:%M", &tm);
                }
                /* 在家：解释 Wi-Fi 为什么没了；判定中：为什么还没结论；
                 * 其他：上次什么时候切过来的 */
                uint32_t note_col = UI_C_TEXT_2;
                if (sc.enabled && !sc.name[0])
                    set_label_fmt(s_sc_note, c_sn, sizeof c_sn, "开机后要连续两次扫描确认位置");
                else if (sc.abroad && sc.chill_on == 1) {
                    /* 在国外：直接写 CHILL 现在怎么走，点卡片改 */
                    note_col = UI_C_ACCENT;
                    set_label_fmt(s_sc_note, c_sn, sizeof c_sn, "CHILL：%s · 点这里改", chill_mode());
                } else if (sc.abroad && sc.chill_on == 0) {
                    note_col = UI_C_ACCENT;
                    set_label_fmt(s_sc_note, c_sn, sizeof c_sn, "%s",
                                  sc.chill_back ? "CHILL 已关 · 回国自动打开 · 点这里改" : "CHILL 已关 · 点这里改");
                } else
                    set_label_fmt(s_sc_note, c_sn, sizeof c_sn, "%s%s%s",
                                  sc.wifi_off ? "Wi-Fi 已关 · " : (when[0] ? "上次切换 " : ""),
                                  when, sc.wifi_off && !when[0] ? "手机走家里网络" : "");
                lv_obj_set_style_text_color(s_sc_note, lv_color_hex(note_col), 0);
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
                const char *state_txt;
                uint32_t state_col;
                int running = ts.ok && !strcmp(ts.state, "Running");

                lv_obj_remove_flag(s_ts_card, LV_OBJ_FLAG_HIDDEN);
                if (!ts.ok)                                     { state_txt = "未运行";   state_col = UI_C_TEXT_3; }
                else if (running)                               { state_txt = ts.self_online ? "已连接" : "离线";
                                                                    state_col = ts.self_online ? UI_C_OK : UI_C_BAD; }
                else if (!strcmp(ts.state, "Starting"))          { state_txt = "连接中";   state_col = UI_C_WARN; }
                else if (!strcmp(ts.state, "NeedsLogin"))        { state_txt = "需要登录"; state_col = UI_C_WARN; }
                else if (!strcmp(ts.state, "NeedsMachineAuth"))  { state_txt = "等待批准"; state_col = UI_C_WARN; }
                else if (!strcmp(ts.state, "Stopped"))           { state_txt = "已停止";   state_col = UI_C_TEXT_3; }
                else                                             { state_txt = ts.state[0] ? ts.state : "-"; state_col = UI_C_TEXT_3; }
                lv_label_set_text(s_ts_state, state_txt);
                lv_obj_set_style_text_color(s_ts_state, lv_color_hex(state_col), 0);

                if (running) {
                    lv_label_set_text_fmt(s_ts_addr, "%s%s%s%s", ts.ip,
                        ts.ip[0] && ts.relay[0] ? " \xC2\xB7 " : "",
                        ts.relay[0] ? "DERP " : "", ts.relay);
                    lv_label_set_text_fmt(s_ts_peers,
                        "\xE5\x9C\xA8\xE7\xBA\xBF %d/%d \xC2\xB7 \xE7\x9B\xB4\xE8\xBF\x9E %d \xC2\xB7 \xE4\xB8\xAD\xE7\xBB\xA7 %d",
                        ts.peers_online, ts.peers, ts.direct, ts.active - ts.direct);
                    lv_label_set_text_fmt(s_ts_routes, "\xE5\xAD\x90\xE7\xBD\x91 %s",
                                          ts.routes[0] ? ts.routes : "-");
                } else {
                    lv_label_set_text(s_ts_addr, "");
                    lv_label_set_text(s_ts_peers, "");
                    lv_label_set_text(s_ts_routes, "");
                }
                /* Subpage: own identity + the peer list. */
                lv_label_set_text(s_tp_self[0], ts.name[0] ? ts.name : "-");
                lv_label_set_text(s_tp_self[1], ts.ip[0] ? ts.ip : "-");
                lv_label_set_text(s_tp_self[2], ts.relay[0] ? ts.relay : "-");
                lv_label_set_text(s_tp_self[3], ts.routes[0] ? ts.routes : "-");
                int pn = tailscale_peer_count();
                if (pn > TS_PEER_MAX) pn = TS_PEER_MAX;
                for (int i = 0; i < TS_PEER_MAX; i++) {
                    if (i >= pn) { lv_obj_add_flag(s_tp_row[i], LV_OBJ_FLAG_HIDDEN); continue; }
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
                    lv_obj_set_style_text_color(s_tp_tag[i],
                        lv_color_hex(pe.active ? UI_C_OK : pe.online ? UI_C_TEXT_2 : UI_C_TEXT_3), 0);
                    lv_obj_set_style_border_opa(s_tp_row[i],
                        pe.online ? LV_OPA_20 : LV_OPA_10, 0);
                }
                lv_obj_set_height(s_tp_card, 30 + (pn ? pn : 1) * 44 + 8);
                /* One trailing line carries either the exit node or a health
                 * warning; health wins when both are present because it is
                 * the actionable one. */
                if (ts.health[0]) {
                    lv_label_set_text(s_ts_note, ts.health);
                    lv_obj_set_style_text_color(s_ts_note, lv_color_hex(UI_C_WARN), 0);
                } else if (ts.exit_node[0]) {
                    lv_label_set_text_fmt(s_ts_note, "\xE5\x87\xBA\xE5\x8F\xA3\xE8\x8A\x82\xE7\x82\xB9 %s%s",
                                          ts.exit_node, ts.exit_online ? "" : " \xC2\xB7 \xE7\xA6\xBB\xE7\xBA\xBF");
                    lv_obj_set_style_text_color(s_ts_note, lv_color_hex(UI_C_TEXT_3), 0);
                } else {
                    lv_label_set_text(s_ts_note, "");
                }
            }
        }
    }

    /* ---- CHILL (Home card + subpage) ---- */
    {
        int on_home = tab_visible(TAB_HOME);
        int on_page = sub_visible(SUB_CHILL) || sub_visible(SUB_CHILL_NODES) ||
                      sub_visible(SUB_CHILL_PAIRS);
        if (chill_poll(on_home || on_page)) {
            static char c_cs[32] = "", c_cr[48] = "", c_csp[32] = "", c_ctt[40] = "";
            int online = chill_online();
            if (!online) {
                lv_obj_add_flag(s_chill_card, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(s_chill_card, LV_OBJ_FLAG_HIDDEN);
                set_label_fmt(s_chill_state, c_cs, sizeof c_cs, "%s \xC2\xB7 %s",
                              chill_core(), chill_mode());
                /* 2026-09-22: connections/direct-vs-proxy split and
                 * cumulative traffic were only ever shown on the detail
                 * page, a swipe away — chill.c already tracked both
                 * (chill_conn_split()/chill_traffic()), this just surfaces
                 * them on the card people actually look at. */
                set_label_fmt(s_chill_split, c_csp, sizeof c_csp, "%s", chill_conn_split());
                set_label_fmt(s_chill_total, c_ctt, sizeof c_ctt, "%s", chill_traffic());
                /*
                 * chill_group()/chill_node()/chill_chain() only ever describe
                 * ONE configured group (chill.conf's "节点选择") — that read
                 * as "current node: X" even in 规则 mode, where most traffic
                 * never goes through that group at all (it's split across
                 * whichever group each connection's domain/rule matched:
                 * Apple/VoWiFi/漏网之鱼/...). Flagged as actively wrong
                 * (2026-09-22). Show the real top (rule -> node) pairs
                 * instead, straight from chill_top_pair() (built off live
                 * /connections data) — directly on this card, not as a
                 * one-line summary pointing at the detail page (that was the
                 * first attempt; the numbers need to be where people
                 * actually look). Node and group used to be two independent
                 * top-N lists side by side, which look paired but aren't —
                 * this is one list keyed by (group, node) so each row is a
                 * real answer to "this rule's traffic went through this
                 * node" (2026-09-22 follow-up feedback). No mode branch
                 * needed: outside 规则 mode every connection shares one
                 * chain, so "top N" naturally collapses to "top 1".
                 *
                 * Each pair is its own sub-card now (2026-09-22, "太丑" +
                 * name clipping fix) instead of a flat name/value text row,
                 * so the card also has to reflow: hidden rows collapse away
                 * — same pattern as the Home signal card's carrier slots
                 * (mk_subcard boxes, accumulate y, resize the parent). */
                static char c_tpn[CHILL_HOME_ROWS][96], c_tpv[CHILL_HOME_ROWS][40];
                int tpn = chill_top_pair_count();
                int y = 68;
                for (int i = 0; i < CHILL_HOME_ROWS; i++) {
                    if (i > 0 && i >= tpn) {
                        lv_obj_add_flag(s_chill_pair_box[i], LV_OBJ_FLAG_HIDDEN);
                        continue;
                    }
                    lv_obj_remove_flag(s_chill_pair_box[i], LV_OBJ_FLAG_HIDDEN);
                    if (i >= tpn) {
                        /* tpn == 0: keep slot 0 visible with a placeholder
                         * instead of hiding every row, so the card doesn't
                         * collapse to just the header when CHILL is up but
                         * idle. */
                        set_label_fmt(s_chill_pair_name[i], c_tpn[i], sizeof c_tpn[i], "%s",
                                      "\xE6\x9A\x82\xE6\x97\xA0\xE6\xB4\xBB\xE8\xB7\x83\xE8\xBF\x9E\xE6\x8E\xA5" /* 暂无活跃连接 */);
                        set_label_fmt(s_chill_pair_val[i], c_tpv[i], sizeof c_tpv[i], "%s", "");
                    } else {
                        chill_traffic_item_t it;
                        chill_get_top_pair(i, &it);
                        set_label_fmt(s_chill_pair_name[i], c_tpn[i], sizeof c_tpn[i], "%s", it.name);
                        set_label_fmt(s_chill_pair_val[i], c_tpv[i], sizeof c_tpv[i], "%s", it.traffic);
                    }
                    lv_obj_align(s_chill_pair_box[i], LV_ALIGN_TOP_LEFT, UI_PAD, y);
                    y += CHILL_PAIR_ROW_H + CHILL_PAIR_GAP;
                }
                lv_obj_align(s_chill_rate, LV_ALIGN_TOP_LEFT, UI_PAD, y + 4);
                /* +22 not +18: s_chill_rate moved from FCN_S (13px) to FCN
                 * (16px) for "bigger" (2026-09-22), needs a bit more row
                 * height so its descenders don't sit on the card's bottom
                 * edge. */
                lv_obj_set_height(s_chill_card, y + 4 + 22 + 10);
                set_label_fmt(s_chill_rate, c_cr, sizeof c_cr, "%s", chill_speed());
            }
            /* Subpage mirrors the same values plus the node list. */
            static char c_pc[24] = "", c_pv[48] = "", c_pt[48] = "";
            set_label_fmt(s_cp_core, c_pc, sizeof c_pc, "%s", chill_core());
            lv_obj_set_style_text_color(s_cp_core,
                lv_color_hex(online ? UI_C_OK : UI_C_TEXT_3), 0);
            set_label_fmt(s_cp_conns, c_pv, sizeof c_pv, "%s", chill_conn_split());
            set_label_fmt(s_cp_traffic, c_pt, sizeof c_pt, "%s", chill_traffic());
            const char *xraw = chill_exit_raw();
            for (int i = 0; i < 4; i++)
                lv_obj_set_style_bg_color(s_cp_mode_btn[i],
                    lv_color_hex(!strcmp(xraw, k_exit_state[i]) ? UI_C_ACCENT : 0x394049), 0);
            {
                static char c_xn[48] = "";
                set_label_fmt(s_cp_exit_note, c_xn, sizeof c_xn, "%s",
                              !strcmp(xraw, "direct_all") ? "AI、VoWiFi 也直连" : "");
            }
            int ng = chill_group_count();
            if (ng > CHILL_MAX_GROUPS) ng = CHILL_MAX_GROUPS;
            static char c_gname[CHILL_MAX_GROUPS][48];
            for (int i = 0; i < CHILL_MAX_GROUPS; i++) {
                if (i >= ng) { lv_obj_add_flag(s_cp_grp_btn[i], LV_OBJ_FLAG_HIDDEN); continue; }
                chill_group_info_t gi;
                chill_get_group(i, &gi);
                lv_obj_remove_flag(s_cp_grp_btn[i], LV_OBJ_FLAG_HIDDEN);
                set_label_fmt(s_cp_grp_lbl[i], c_gname[i], sizeof c_gname[i], "%s", gi.name);
                lv_obj_set_style_bg_color(s_cp_grp_btn[i],
                    lv_color_hex(gi.selected ? UI_C_ACCENT : 0x394049), 0);
                lv_obj_set_style_text_color(s_cp_grp_lbl[i],
                    lv_color_hex(gi.auto_pick && !gi.selected ? UI_C_TEXT_3 : UI_C_TEXT), 0);
            }

            int pending = chill_delay_pending();
            lv_label_set_text(s_cp_delay_lbl, pending
                ? "\xE6\xB5\x8B\xE8\xAF\x95\xE4\xB8\xAD\xE2\x80\xA6" /* 测试中… */
                : "\xE6\xB5\x8B\xE5\xBB\xB6\xE8\xBF\x9F" /* 测延迟 */);
            lv_obj_set_style_text_color(s_cp_delay_lbl,
                lv_color_hex(pending ? UI_C_TEXT_3 : UI_C_ACCENT), 0);

            int nn = chill_node_count();
            if (nn > CHILL_MAX_NODES) nn = CHILL_MAX_NODES;
            static char c_nname[CHILL_MAX_NODES][48], c_ndl[CHILL_MAX_NODES][16];
            for (int i = 0; i < CHILL_MAX_NODES; i++) {
                if (i >= nn) { lv_obj_add_flag(s_cp_node_row[i], LV_OBJ_FLAG_HIDDEN); continue; }
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
                lv_obj_set_style_text_color(s_cp_node_dl[i],
                    lv_color_hex(ni.delay == 0 ? UI_C_BAD : ni.delay < 0 ? UI_C_TEXT_3
                                 : ni.delay < 150 ? UI_C_OK : ni.delay < 400 ? UI_C_WARN : UI_C_BAD), 0);
                lv_obj_set_style_bg_color(s_cp_node_row[i],
                    lv_color_hex(ni.selected ? 0x1e3a2e : UI_C_TRACK), 0);
            }
            lv_obj_set_height(s_cp_node_card, 30 + (nn ? nn : 1) * 44 + 8);

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
            lv_obj_set_height(s_cp_pair_card, 30 + (tpn ? tpn : 1) * 22 + 8);

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

    /* ---- eSIM subpage ---- */
    {
        if (esim_poll(sub_visible(SUB_ESIM))) {
            int n = esim_profile_count();
            int locked = esim_locked();

            lv_label_set_text(s_es_cur, esim_current());
            lv_label_set_text(s_es_state, esim_state());
            for (int i = 0; i < ESIM_MAX_ROWS; i++) {
                if (i >= n) { lv_obj_add_flag(s_es_row[i], LV_OBJ_FLAG_HIDDEN); continue; }
                esim_profile_t p;
                esim_get_profile(i, &p);
                lv_obj_remove_flag(s_es_row[i], LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(s_es_row_name[i], p.name);
                lv_label_set_text(s_es_row_sub[i], p.sub);
                lv_label_set_text(s_es_row_tag[i],
                    p.going ? "切换中…" : p.armed ? "再点一次" : p.enabled ? "使用中" : "");
                if (locked || p.enabled)
                    lv_obj_remove_flag(s_es_row[i], LV_OBJ_FLAG_CLICKABLE);
                else
                    lv_obj_add_flag(s_es_row[i], LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_style_bg_color(s_es_row[i],
                    lv_color_hex(p.going ? UI_C_BUSY : p.armed ? UI_C_WARN :
                                 p.enabled ? UI_C_OK : UI_C_TRACK), 0);
            }
            lv_obj_set_height(s_es_list_card, 30 + (n ? n : 1) * 52 + 8);
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
                lv_obj_set_pos(s_st_offline, UI_PAD, 32);
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
                    set_label_fmt(s_st_detail, c_detail, sizeof c_detail, "%s", "");

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
                lv_obj_set_style_bg_color(s_st_btn,
                    lv_color_hex(running ? UI_C_BAD : UI_C_ACCENT), 0);

                if (ph == ST_ERROR && speedtest_error()[0]) {
                    lv_obj_remove_flag(s_st_offline, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_pos(s_st_offline, UI_PAD, 190);
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
            lv_obj_set_height(s_st_srv_card, 30 + ((n > 0 ? n : 0) + 1) * 40 + 8);
        }
        {
            int sel = speedtest_selected_index();
            for (int i = 0; i < ST_SRV_ROWS; i++) {
                if (!s_st_srv_row[i]) continue;
                int is_sel = (sel < 0 && i == 0) || (sel >= 0 && i == sel + 1);
                lv_obj_set_style_bg_color(s_st_srv_row[i],
                    lv_color_hex(is_sel ? 0x1e3a2e : UI_C_TRACK), 0);
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
     * Fed every tick regardless of which page is showing, so switching to
     * 图表 shows real history instead of a blank axis. Throughput uses a
     * log10 scale (bytes/s spans five orders of magnitude on this device;
     * a linear axis buries everything under the session peak). */
    {
        static char c_rcpu[48] = "", c_rmem[48] = "", c_rnet[48] = "", c_rbat[48] = "";
        lv_chart_set_next_value(s_ch_cpu, s_cs_cpu, (int32_t)d.cpu_usage);
        lv_chart_set_next_value(s_ch_mem, s_cs_mem, (int32_t)d.mem_used_pct);
        lv_chart_set_next_value(s_ch_net, s_cs_rx, rate_scale(d.rx_speed));
        lv_chart_set_next_value(s_ch_net, s_cs_tx, rate_scale(d.tx_speed));
        lv_chart_set_next_value(s_ch_bat, s_cs_bat, (int32_t)d.bat_percent);

        set_label_fmt(s_ch_cpu_r, c_rcpu, sizeof c_rcpu,
                      "\xE5\x8D\xA0\xE7\x94\xA8 %ld%%  \xE6\xB8\xA9\xE5\xBA\xA6 %ld\xC2\xB0""C",
                      d.cpu_usage, d.cpu_temp);
        set_label_fmt(s_ch_mem_r, c_rmem, sizeof c_rmem, "%ld%% \xC2\xB7 %ldM / %ldM",
                      d.mem_used_pct, (d.mem_total - d.mem_avail) / 1048576, d.mem_total / 1048576);
        char dn2[32], up2[32];
        fmt_rate(dn2, sizeof dn2, d.rx_speed);
        fmt_rate(up2, sizeof up2, d.tx_speed);
        set_label_fmt(s_ch_net_r, c_rnet, sizeof c_rnet,
                      "\xE2\x86\x93 %s  \xE2\x86\x91 %s", dn2, up2);   /* ↓ ↑ */
        set_label_fmt(s_ch_bat_r, c_rbat, sizeof c_rbat,
                      "%d%% \xC2\xB7 %d\xC2\xB0""C", d.bat_percent, d.bat_temp);
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
        if (d.sms_n) lv_obj_add_flag(s_sms_empty, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_remove_flag(s_sms_empty, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < SMS_MAX_ROWS; i++) {
            if (i >= n) {
                s_sms_row_id[i] = -1;
                lv_obj_add_flag(s_sms_row[i], LV_OBJ_FLAG_HIDDEN);
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
            lv_obj_set_style_bg_color(s_sms_row[i],
                lv_color_hex(sms_delete_armed(i) ? UI_C_BAD : UI_C_CARD), 0);
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
            lv_obj_set_style_bg_color(s_smsd_del_btn, lv_color_hex(armed ? UI_C_BAD : UI_C_ACCENT), 0);
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
        for (int i = 0; i < ALERTS_MAX; i++) {
            alert_item_t a;
            char when[40];
            if (i >= n) { lv_obj_add_flag(s_al_row[i], LV_OBJ_FLAG_HIDDEN); continue; }
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
            lv_obj_set_style_text_color(s_al_label[i], lv_color_hex(a.unread ? UI_C_TEXT : UI_C_TEXT_2), 0);
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
        for (int i = 0; i < 4; i++)
            lv_obj_set_style_bg_color(s_lk_mode_btn[i],
                lv_color_hex(!strcmp(d.net_select, k_mode_v[i]) ? UI_C_ACCENT : 0x394049), 0);
    } else if (lv_tick_get() - s_lk_mode_arm >= 5000) {
        s_lk_mode_pending = -1;          /* confirm window lapsed */
        lv_label_set_text(s_lk_mode_lbl, "切换会短暂断网，需要按两次确认");
    }

    /* ---- 功能 tile subtitles ---- */
    {
        static char c_t0[40] = "", c_t1[40] = "", c_t5[40] = "", c_t6[40] = "";
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

/* ---- power menu ---- */
static void power_menu_set(int v)
{
    if (v) lv_obj_remove_flag(s_power_menu, LV_OBJ_FLAG_HIDDEN);
    else   lv_obj_add_flag(s_power_menu, LV_OBJ_FLAG_HIDDEN);
}
static int power_menu_visible(void) { return !lv_obj_has_flag(s_power_menu, LV_OBJ_FLAG_HIDDEN); }

static void act_poweroff(lv_event_t *e) { LV_UNUSED(e); system("poweroff"); }
static void act_reboot(lv_event_t *e)   { LV_UNUSED(e); system("reboot"); }
static void act_cancel(lv_event_t *e)   { LV_UNUSED(e); power_menu_set(0); }

static void menu_button(lv_obj_t *parent, const char *txt, uint32_t color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_width(btn, lv_pct(90));
    lv_obj_set_height(btn, 52);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, FCN, 0);
    lv_obj_center(l);
}

static void build_power_menu(void)
{
    s_power_menu = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_power_menu, 280, 320);
    lv_obj_center(s_power_menu);
    lv_obj_set_style_bg_color(s_power_menu, lv_color_hex(0x161b21), 0);
    lv_obj_set_style_border_color(s_power_menu, lv_color_hex(0x4ea1ff), 0);
    lv_obj_set_style_border_width(s_power_menu, 2, 0);
    lv_obj_set_style_radius(s_power_menu, 12, 0);
    lv_obj_set_flex_flow(s_power_menu, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_power_menu, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(s_power_menu);
    lv_label_set_text(title, "电源");
    lv_obj_set_style_text_font(title, FCN, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x9aa4ae), 0);

    menu_button(s_power_menu, "关机", 0xb23b3b, act_poweroff);
    menu_button(s_power_menu, "重启", 0xb2742b, act_reboot);
    menu_button(s_power_menu, "取消", 0x394049, act_cancel);
    power_menu_set(0);
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
        else                    backlight_on();
        s_auto_slept = 0;
    } else if (ev == KEY_EV_LONG) {
        backlight_on();
        s_auto_slept = 0;
        power_menu_set(!power_menu_visible());
    }

    /* Auto screen-off after inactivity; any touch/key wakes it. */
    if (s_autooff_ms) {
        uint32_t idle = lv_display_get_inactive_time(NULL);
        if (idle > s_autooff_ms) {
            if (backlight_is_on()) { backlight_off(); s_auto_slept = 1; }
        } else if (s_auto_slept) {
            backlight_on();
            s_auto_slept = 0;
        }
    }
}

/* ---- shared chrome: bottom icon tab bar ----
 * Replaces the old dot indicator. Dots work up to ~4 pages; with CHILL, eSIM,
 * SMS, lock-freq and charts planned the page count roughly doubles, and dots
 * stop telling you both "where am I" and "how do I get there in one step".
 * The table below is the only place page identity lives, so regrouping pages
 * later is a table edit rather than a navigation rewrite. */
static const char *k_tab_syms[UI_TABS] = {
    LV_SYMBOL_HOME,      /* 首页 */
    LV_SYMBOL_IMAGE,     /* 图表 — LVGL has no chart glyph; this is the closest */
    LV_SYMBOL_LIST,      /* 功能 */
    LV_SYMBOL_SETTINGS,  /* 系统 */
};
static const char *k_tab_names[UI_TABS] = {
    "\xE9\xA6\x96\xE9\xA1\xB5", "\xE5\x9B\xBE\xE8\xA1\xA8",
    "\xE5\x8A\x9F\xE8\x83\xBD", "\xE7\xB3\xBB\xE7\xBB\x9F",
};
static lv_obj_t *s_tab_names[UI_TABS];

static void update_tabs(void)
{
    lv_obj_t *act;
    /* sub_close() runs once during ui_create() to put the subpage layer in
     * its initial hidden state — that happens BEFORE build_tabbar(), so the
     * tab labels don't exist yet. Without this guard that call dereferenced
     * a NULL lv_obj_t* and the process died on startup; with the live UI
     * killed for the side-by-side test, the vendor watchdog then rebooted
     * the whole device (2026-09-21, exactly the failure mode the bypass-name
     * test procedure exists to contain). */
    if (!s_tabs[0]) return;
    act = lv_tileview_get_tile_active(s_tv);
    for (int i = 0; i < UI_TABS; i++) {
        uint32_t col = (s_tiles[i] == act && s_sub_cur < 0) ? UI_C_ACCENT : UI_C_IDLE;
        lv_obj_set_style_text_color(s_tabs[i], lv_color_hex(col), 0);
        lv_obj_set_style_text_color(s_tab_names[i], lv_color_hex(col), 0);
    }
    /* Slot 0 doubles as the subpage back button. Tapping any other slot
     * already leaves the subpage, so the 首页 shortcut is only unavailable
     * for the one tap it takes to come back out. */
    if (s_sub_cur >= 0) {
        lv_label_set_text(s_tabs[0], LV_SYMBOL_LEFT);
        lv_label_set_text(s_tab_names[0], "\xE8\xBF\x94\xE5\x9B\x9E" /* 返回 */);
        lv_obj_set_style_text_color(s_tabs[0], lv_color_hex(UI_C_ACCENT), 0);
        lv_obj_set_style_text_color(s_tab_names[0], lv_color_hex(UI_C_ACCENT), 0);
    } else {
        lv_label_set_text(s_tabs[0], k_tab_syms[0]);
        lv_label_set_text(s_tab_names[0], k_tab_names[0]);
    }
}

static void tab_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    /* Slot 0 is 返回 while a subpage is open: close it and stay where the
     * user was, rather than also jumping to 首页. */
    if (idx == 0 && s_sub_cur >= 0) { sub_back(); return; }
    /* Any other tab tap always lands on that top-level page, even from
     * inside a subpage — otherwise the tab bar would look broken there. */
    sub_close();
    lv_tileview_set_tile_by_index(s_tv, idx, 0, LV_ANIM_OFF);
    update_tabs();
}

/* ---- shared chrome: fixed top status bar ---- */
static void build_statusbar(void)
{
    lv_obj_t *bar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(bar);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bar, UI_W, UI_TOPBAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x121820), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_90, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_white(), 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_10, 0);

    s_top_time = mklabel(bar, 8, 5, &lv_font_montserrat_14, UI_C_TEXT);
    lv_label_set_text(s_top_time, "--:--");

    for (int i = 0; i < 5; i++) {
        s_top_sig[i] = lv_obj_create(bar);
        lv_obj_remove_style_all(s_top_sig[i]);
        lv_obj_set_size(s_top_sig[i], 5, 5);
        lv_obj_set_style_radius(s_top_sig[i], 3, 0);
        lv_obj_set_style_bg_opa(s_top_sig[i], LV_OPA_COVER, 0);
        lv_obj_align(s_top_sig[i], LV_ALIGN_LEFT_MID, 64 + i * 9, 0);
    }
    /* Width-capped + clipped: d.net_type (include/data.h, char[16]) isn't
     * always a short RAT code like "5G" — registration states such as
     * "LIMITED_SERVICE" fill the whole 15-char buffer. Unbounded, that ran
     * straight into s_top_updown with no gap (2026-09-21, user photo of the
     * two overlapping into unreadable text on real hardware). 40px fits
     * the common short codes fully and clips the rare long ones instead of
     * overlapping the neighbour — same trade LV_LABEL_LONG_CLIP already
     * makes for s_top_updown itself. */
    s_top_net = mklabel(bar, 116, 5, FCN_S, UI_C_TEXT_2);
    lv_obj_set_width(s_top_net, 40);
    lv_label_set_long_mode(s_top_net, LV_LABEL_LONG_CLIP);

    /* FCN_S with real ↓↑ (U+2193/2191), not montserrat + LV_SYMBOL_DOWN/UP:
     * LVGL's symbols are chevrons (∨ ∧), which read as "expand/collapse",
     * not as throughput. The litehtml UI's status bar uses the same two
     * arrow characters out of the same device font, so the two renderers
     * now match each other and the approved mockup.
     *
     * x=165, width 95: battery icon (below) sits at x=279 w=35, net_type
     * (above) ends at 116+40=156. 180 (2026-09-21, pushed right off
     * net_type) turned out too close to the battery on real hardware
     * (2026-09-22 user report) — 165 splits the gap, 9px clear of net_type,
     * 19px clear of the battery instead of 4px. */
    s_top_updown = mklabel(bar, 165, 6, FCN_S, UI_C_TEXT_2);
    lv_obj_set_width(s_top_updown, 95);
    lv_label_set_long_mode(s_top_updown, LV_LABEL_LONG_CLIP);
    /* Tap to flip Mbps <-> MB/s (htmlmain.c's "spunit" action, no separate
     * settings page in LVGL yet) — persists via save_devui_conf(). Third
     * pass on the affordance: a grey button chip (second pass) was the only
     * backgrounded element in an otherwise all-plain-text/dots status bar
     * and looked bolted-on. This app already has a convention for "tappable
     * label, no button chrome" — the CHILL page's "测延迟" (chill_delay_cb)
     * is just accent-colored text — so match that instead of inventing a
     * new style. Color itself is the only affordance; that's consistent
     * with how the rest of this app signals "you can tap this text". */
    lv_obj_add_flag(s_top_updown, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_top_updown, topbar_speed_unit_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_text_color(s_top_updown, lv_color_hex(UI_C_ACCENT), 0);

    /* Fourth pass on the battery — this time matching htmlmain.c's own
     * proven native status bar (src/htmlmain.c:2094 draw_native_statusbar())
     * pixel-for-pixel instead of guessing: x=279, w=35, h=16. Earlier
     * LVGL attempts used a 20-28px-wide icon, which is why the percentage
     * never had comfortable room whether it sat inside or outside. At
     * htmlmain's width there's real space for "100%" centered inside
     * without crowding — the original ask ("number inside the battery")
     * was never wrong, the icon was just too small to do it in. */
    /* Alert dot in the 19px gap between the rates (ends x=260) and the
     * battery (x=279). A dot, not text: there is no room for words here, and
     * the words go in the bottom banner (lost) or the admin web (alerts). */
    s_top_alert = lv_obj_create(bar);
    lv_obj_remove_style_all(s_top_alert);
    lv_obj_set_size(s_top_alert, 7, 7);
    lv_obj_set_style_radius(s_top_alert, 4, 0);
    lv_obj_set_style_bg_opa(s_top_alert, LV_OPA_COVER, 0);
    lv_obj_align(s_top_alert, LV_ALIGN_LEFT_MID, 266, 0);
    lv_obj_add_flag(s_top_alert, LV_OBJ_FLAG_HIDDEN);
    /* Tap target around the dot: opens the alerts page. Transparent and as
     * tall as the bar; the gap is only 19px wide, so it reaches a little
     * further with ext_click_area instead of growing over its neighbours. */
    s_top_alert_hit = lv_obj_create(bar);
    lv_obj_remove_style_all(s_top_alert_hit);
    lv_obj_set_size(s_top_alert_hit, 19, UI_TOPBAR_H);
    lv_obj_align(s_top_alert_hit, LV_ALIGN_LEFT_MID, 260, 0);
    lv_obj_set_ext_click_area(s_top_alert_hit, 8);
    lv_obj_add_flag(s_top_alert_hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_top_alert_hit, open_alerts_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_top_alert_hit, LV_OBJ_FLAG_HIDDEN);

    s_top_bat_icon = lv_obj_create(bar);
    lv_obj_remove_style_all(s_top_bat_icon);
    lv_obj_set_size(s_top_bat_icon, 35, 16);
    lv_obj_align(s_top_bat_icon, LV_ALIGN_LEFT_MID, 279, 0);
    lv_obj_set_style_radius(s_top_bat_icon, 3, 0);
    lv_obj_set_style_bg_opa(s_top_bat_icon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_top_bat_icon, 1, 0);
    lv_obj_set_style_border_color(s_top_bat_icon, lv_color_hex(UI_C_TEXT_2), 0);
    lv_obj_set_style_border_opa(s_top_bat_icon, LV_OPA_COVER, 0);
    /* Clip the fill/digit (below) to this box — belt-and-suspenders on top
     * of getting their sizes actually right, so a future glyph/scale
     * change degrades to "clipped" instead of "overflowing into the
     * neighbours". */
    lv_obj_set_style_clip_corner(s_top_bat_icon, true, 0);

    lv_obj_t *nub = lv_obj_create(bar);
    lv_obj_remove_style_all(nub);
    lv_obj_set_size(nub, 2, 6);
    lv_obj_align_to(nub, s_top_bat_icon, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(nub, 1, 0);
    lv_obj_set_style_bg_color(nub, lv_color_hex(UI_C_TEXT_2), 0);
    lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);

    s_top_bat_fill = lv_obj_create(s_top_bat_icon);
    lv_obj_remove_style_all(s_top_bat_fill);
    lv_obj_set_size(s_top_bat_fill, 31, 12);  /* full width; refresh_cb shrinks it to match % */
    lv_obj_align(s_top_bat_fill, LV_ALIGN_LEFT_MID, 1, 0);
    lv_obj_set_style_radius(s_top_bat_fill, 2, 0);
    lv_obj_set_style_bg_color(s_top_bat_fill, lv_color_hex(UI_C_OK), 0);
    lv_obj_set_style_bg_opa(s_top_bat_fill, LV_OPA_COVER, 0);

    /* Digits INSIDE the shell, no "%" (user: not needed — the icon shape
     * already says "this is battery"). Plain white per user request
     * (2026-09-21) — simpler than the earlier dark/light contrast-switch
     * attempt, and white reads fine against all three fill colors
     * (UI_C_OK green / UI_C_BAD red / UI_C_ACCENT blue, refresh_cb) since
     * none of them are pale enough to wash it out.
     *
     * "Slightly bolder": LVGL only ships montserrat_12/14 at regular
     * weight here (lv_conf.h), no bold variant, so there's no font-weight
     * knob to turn. Faking it the way html_view_draw_text_px() does for
     * its own bold pass (src/html_view.cpp:596: render the same glyphs
     * twice, second pass shifted 1px right) — s_top_bat2 is that second
     * copy, created identical and updated in lockstep in refresh_cb. */
    s_top_bat = lv_label_create(s_top_bat_icon);
    lv_obj_set_style_text_font(s_top_bat, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_top_bat, lv_color_white(), 0);
    lv_obj_center(s_top_bat);
    s_top_bat2 = lv_label_create(s_top_bat_icon);
    lv_obj_set_style_text_font(s_top_bat2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_top_bat2, lv_color_white(), 0);
    lv_obj_align(s_top_bat2, LV_ALIGN_CENTER, 1, 0);   /* 1px right of s_top_bat, same center */

    /* No overlaid LV_SYMBOL_CHARGE glyph here — three rounds of shrinking it
     * (font is fixed at 14px minimum per lv_conf.h; tried 62.5% then 47%
     * scale, tried enlarging the icon from 10px to 13px tall) still left a
     * sliver poking past the top border on real hardware every time, because
     * the glyph's own design bearing doesn't shrink linearly with the
     * transform. Charging is now shown as a fill-color change instead
     * (refresh_cb) — a property that by construction can't overflow its
     * own box, since it's just a color on a rect already sized correctly.
     * s_top_bat_bolt is unused; keeping the field would just invite a
     * revert to the broken approach. */
}

static void build_tabbar(void)
{
    lv_obj_t *bar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(bar);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bar, UI_W, UI_NAV_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x121820), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_90, 0);
    /* Deliberately no backdrop blur on the bar: it is on screen in every
     * frame, so blurring it would add its cost to every redraw. Blur is
     * reserved for cards, which only repaint when their data changes. */
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_white(), 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_10, 0);

    /* Four tabs now, so there is room for a text label under each icon —
     * the six-icon bar was icon-only and the icons for 网络/eSIM/性能测试
     * were guesses from LVGL's symbol set that nobody could read. */
    const int w = UI_W / UI_TABS;
    for (int i = 0; i < UI_TABS; i++) {
        lv_obj_t *cell = lv_obj_create(bar);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, w, UI_NAV_H);
        lv_obj_align(cell, LV_ALIGN_LEFT_MID, i * w, 0);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cell, tab_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_tabs[i] = lv_label_create(cell);
        lv_obj_set_style_text_font(s_tabs[i], &lv_font_montserrat_14, 0);
        lv_label_set_text(s_tabs[i], k_tab_syms[i]);
        lv_obj_align(s_tabs[i], LV_ALIGN_TOP_MID, 0, 4);
        s_tab_names[i] = lv_label_create(cell);
        lv_obj_set_style_text_font(s_tab_names[i], FCN_S, 0);
        lv_label_set_text(s_tab_names[i], k_tab_names[i]);
        lv_obj_align(s_tab_names[i], LV_ALIGN_BOTTOM_MID, 0, -2);
    }
    update_tabs();
}

/* ---- shared chrome: global status banner ---- */
static void banner_set(const char *txt)
{
    /* Called every loop; only touch the label when the words change, or the
     * banner is re-laid-out and redrawn on every frame. */
    static char shown[96] = "";
    if (!s_banner) return;
    if (txt) {
        if (strcmp(shown, txt)) {
            snprintf(shown, sizeof shown, "%s", txt);
            lv_label_set_text(s_banner_txt, txt);
        }
        lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_banner(void)
{
    s_banner = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_banner);
    lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_banner, UI_CARD_W, 30);
    lv_obj_align(s_banner, LV_ALIGN_BOTTOM_MID, 0, -(UI_NAV_H + 6));
    lv_obj_set_style_radius(s_banner, 15, 0);
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(0x2a1a1a), 0);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_banner, 1, 0);
    lv_obj_set_style_border_color(s_banner, lv_color_hex(UI_C_BAD), 0);
    lv_obj_set_style_border_opa(s_banner, LV_OPA_60, 0);
    lv_obj_set_style_shadow_width(s_banner, 18, 0);
    lv_obj_set_style_shadow_color(s_banner, lv_color_hex(UI_C_BAD), 0);
    lv_obj_set_style_shadow_opa(s_banner, LV_OPA_40, 0);

    s_banner_txt = lv_label_create(s_banner);
    lv_obj_set_style_text_font(s_banner_txt, FCN_S, 0);
    lv_obj_set_style_text_color(s_banner_txt, lv_color_hex(0xffb4ab), 0);
    lv_obj_center(s_banner_txt);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
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

void ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0c0f13), 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(0xe0e0e0), 0);

    /* CJK fonts (loaded from the device; repo ships no proprietary font).
     * Sizes trimmed down one step (was 16/22/28): the original scale read
     * oversized on a 320px-wide panel, especially the operator name. */
    lv_freetype_init(LV_FREETYPE_CACHE_FT_GLYPH_CNT);
    s_cjk   = lv_freetype_font_create(DEVUI_CJK_FONT,
                                      LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 16,
                                      LV_FREETYPE_FONT_STYLE_NORMAL);
    s_cjk16 = lv_freetype_font_create(DEVUI_CJK_FONT,
                                      LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 13,
                                      LV_FREETYPE_FONT_STYLE_NORMAL);
    s_cjk28 = lv_freetype_font_create(DEVUI_CJK_FONT,
                                      LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 20,
                                      LV_FREETYPE_FONT_STYLE_NORMAL);
    FCN   = s_cjk   ? s_cjk   : &lv_font_montserrat_16;
    FCN_S = s_cjk16 ? s_cjk16 : &lv_font_montserrat_14;
    FCN_L = s_cjk28 ? s_cjk28 : &lv_font_montserrat_20;

    fprintf(stderr, "ui: fonts loaded (cjk=%p)\n", (void *)s_cjk);
    fflush(stderr);

    s_tv = lv_tileview_create(scr);
    /* Leave room at the top for the fixed status bar (built later, on
     * lv_layer_top()) — lv_tileview_create() defaults to 100% of the
     * screen, which would otherwise draw every page's content starting at
     * y=0, right under the status bar. */
    lv_obj_set_size(s_tv, UI_W, UI_H - UI_TOPBAR_H);
    lv_obj_align(s_tv, LV_ALIGN_TOP_MID, 0, UI_TOPBAR_H);
    lv_obj_set_style_bg_color(s_tv, lv_color_hex(0x0c0f13), 0);
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
        lv_obj_set_style_bg_color(s_tiles[i], lv_color_hex(UI_C_BG), 0);
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
    lv_obj_set_style_bg_color(s_sub_layer, lv_color_hex(UI_C_BG), 0);
    lv_obj_set_style_bg_opa(s_sub_layer, LV_OPA_COVER, 0);
    /* Subpage header is the title only. The back control lives in the tab
     * bar's leftmost slot instead (see update_tabs): on a 320x480 panel held
     * in one hand the bottom strip is where the thumb already is, and the
     * top-left corner is the furthest point on the screen from it. */
    {
        s_sub_title = lv_label_create(s_sub_layer);
        lv_obj_set_style_text_font(s_sub_title, FCN, 0);
        lv_obj_set_style_text_color(s_sub_title, lv_color_hex(UI_C_TEXT), 0);
        lv_label_set_text(s_sub_title, "");
        lv_obj_align(s_sub_title, LV_ALIGN_TOP_MID, 0, 4);
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
    lv_tileview_set_tile_by_index(s_tv, 0, 0, LV_ANIM_OFF);

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
    load_devui_conf();
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
    backlight_init();
    key_input_init(&s_key);
    lv_timer_create(key_poll_cb, 50, NULL);
}
