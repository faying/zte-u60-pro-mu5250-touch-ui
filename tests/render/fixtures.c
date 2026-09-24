/*
 * fixtures.c - scene data behind every device-facing interface ui.c uses
 * (data / chill / tailscale / esim / speedtest / scenario / alerts /
 * backlight / key / touch / exec). Nothing here touches the system.
 *
 * SPDX-License-Identifier: MIT
 */
#include "fixtures.h"

#include "alerts.h"
#include "backlight.h"
#include "chill.h"
#include "data.h"
#include "esim.h"
#include "key_input.h"
#include "scenario.h"
#include "speedtest.h"
#include "tailscale.h"
#include "touch_input.h"
#include "ui_exec.h"

#include <stdio.h>
#include <string.h>

int  rt_scene;
long rt_now = 1790250112L;            /* 2026-09-24 10:21:52 device-local */
int  rt_refreshes;
int  rt_exec_calls;
ui_launch_t rt_exec_last;
int  rt_system_calls;
char rt_system_last[256];

/* CHILL names are what chill.c hands out on the device: flags turned into
 * country codes, other emoji dropped (sanitize_name). */
static const char *const k_names[RT_SCENES] = {
    "good", "weak", "nosignal", "datad-down", "loading", "nosim",
    "abroad", "lowbat", "full-charging", "long-names", "empty",
};
const char *rt_scene_name(int s) { return s >= 0 && s < RT_SCENES ? k_names[s] : "?"; }

#define IS(s) (rt_scene == (s))
#define LONG_NAMES IS(RT_LONG_NAMES)
#define EMPTY IS(RT_EMPTY)

static void cp(char *dst, size_t n, const char *s) { snprintf(dst, n, "%s", s); }

/* ------------------------------------------------------------------ data */
static void fill_data(devui_data_t *d)
{
    memset(d, 0, sizeof *d);
    d->valid = 1;
    cp(d->net_type, sizeof d->net_type, "5G");
    d->bars = 5;
    cp(d->operator_name, sizeof d->operator_name, LONG_NAMES ? "中华电信 Chunghwa Telecom Co., Ltd. 4G/5G" : "中国电信");
    cp(d->band, sizeof d->band, "n78");
    cp(d->nr_band, sizeof d->nr_band, "n78");
    d->nr_rsrp = -87; d->nr_rsrq = -11; d->nr_rssi = -80;
    cp(d->nr_snr, sizeof d->nr_snr, "17.7");
    d->lte_rsrp = -106; d->lte_rsrq = -17; d->lte_rssi = -71;
    cp(d->lte_snr, sizeof d->lte_snr, "-4.0");
    d->mcc = 460; d->mnc = 11; d->nr_pci = 603;
    d->nr_cell_id = 23960174596L; d->nr_channel = 627264;
    cp(d->nr_bw, sizeof d->nr_bw, "100");
    /* idx,PCI,?,band,arfcn,bw,?,rsrp,rsrq,sinr,rssi. As on the device: the
     * serving cell (PCI 603) is also in nrca, at the -140 floor (= inactive),
     * and not first; its live signal is only in nr_*. The UI lists the serving
     * cell first from nr_*, then every nrca entry as-is. */
    cp(d->nrca, sizeof d->nrca,
       "0,615,1,1,426030,20,0,-95.0,-12.0,9.5,-85.0;"
       "1,603,1,78,633984,100,0,-140.0,-43.0,-23.0,-120.0;"
       "2,120,1,41,504990,100,0,-140.0,-43.0,-23.0,-120.0;");
    cp(d->wan_status, sizeof d->wan_status, "ipv4_ipv6_connected");
    cp(d->net_select, sizeof d->net_select, "WL_AND_5G");
    cp(d->sa_bands, sizeof d->sa_bands, "1,28,41,78,79");
    cp(d->nsa_bands, sizeof d->nsa_bands, "1,3,28,41,78");
    cp(d->lte_bands, sizeof d->lte_bands, "1,3,5,8,34,38,39,40,41");

    d->bat_percent = 78; d->bat_temp = 33; d->charging = 0; d->charger_connect = 0;
    d->bat_uv = 4055664; d->bat_ua = -349242; d->chg_uv = 30000; d->chg_ua = 0;

    static const char *const cn[] = { "iPhone", "MacBook-Pro", "iPad", "Nintendo-Switch", "Kindle", "Pixel-9" };
    int nc = EMPTY ? 0 : 6;
    for (int i = 0; i < nc; i++) {
        if (LONG_NAMES) cp(d->client[i].name, sizeof d->client[i].name, "a-very-long-hostname-for-a-laptop-0123456");
        else            cp(d->client[i].name, sizeof d->client[i].name, cn[i]);
        snprintf(d->client[i].ip, sizeof d->client[i].ip, "10.0.66.%d", 20 + i);
        snprintf(d->client[i].mac, sizeof d->client[i].mac, "02:00:00:00:00:%02x", i);
    }
    d->client_n = nc; d->clients_total = nc; d->clients_wifi = nc; d->clients_lan = 0;

    static const struct { const char *num, *date, *text; int unread; } sm[] = {
        { "10000", "09-24 09:12", "【中国电信】您好，您本月套餐内通用流量已使用 80%，剩余 20.4GB。如需帮助请回复 CX。", 1 },
        { "+8613800000000", "09-23 21:40", "明天下午三点在老地方见，别忘了带上次说的那本书。", 1 },
        { "106900000000", "09-22 08:05", "您的验证码是 482913，5 分钟内有效，请勿泄露给他人。", 0 },
    };
    int ns = EMPTY ? 0 : 3;
    for (int i = 0; i < ns; i++) {
        d->sms[i].id = 100 + i;
        cp(d->sms[i].num, sizeof d->sms[i].num, LONG_NAMES && i == 0 ? "+886912345678901234567890123456789" : sm[i].num);
        cp(d->sms[i].date, sizeof d->sms[i].date, sm[i].date);
        cp(d->sms[i].text, sizeof d->sms[i].text, sm[i].text);
        d->sms[i].unread = sm[i].unread;
        d->sms_unread += sm[i].unread;
    }
    d->sms_n = ns;

    cp(d->wifi_ssid, sizeof d->wifi_ssid, LONG_NAMES ? "My-Very-Long-Home-Network-Name-5G-Extended-0123456789" : "U60-Test");
    cp(d->wifi_key, sizeof d->wifi_key, "correct-horse-battery");
    cp(d->wifi_enc, sizeof d->wifi_enc, "sae");
    d->wifi_enabled = 1;
    d->nfc_switch = 1;
    cp(d->dhcp_ip, sizeof d->dhcp_ip, "10.0.66.1");
    cp(d->dhcp_start, sizeof d->dhcp_start, "2");
    cp(d->dhcp_limit, sizeof d->dhcp_limit, "252");
    cp(d->dhcp_leasetime, sizeof d->dhcp_leasetime, "86400");

    d->rx_speed = 1532000; d->tx_speed = 188000;
    d->rx_bytes = 1377107391; d->tx_bytes = 399834190;
    d->day_rx_bytes = 2352570100L; d->day_tx_bytes = 190568990;
    d->month_rx_bytes = 48589732137L; d->month_tx_bytes = 15347738524L;
    d->qci = 9; d->ambr_dl = 1668.64; d->ambr_ul = 1008.64;
    cp(d->usb_mode, sizeof d->usb_mode, "user");

    d->uptime = 32455; d->cpu_temp = 42; d->cpu_usage = 15; d->mem_used_pct = 74;
    d->mem_total = 1667600384L; d->mem_avail = 425123840L;
    cp(d->model, sizeof d->model, "Qualcomm Technologies, Inc. SDXPINN IDP MBB");
    cp(d->fw, sizeof d->fw, "OpenWrt 23.05.4 r24012-d8dd03c46f");
    cp(d->sw_version, sizeof d->sw_version, "BD_CNMU5250V1.0.0B27");
    cp(d->imei, sizeof d->imei, "490154203237518");
    cp(d->sim_state, sizeof d->sim_state, "sim ready");

    switch (rt_scene) {
    case RT_WEAK:
        d->bars = 2; d->nr_rsrp = -112; d->nr_rsrq = -16; cp(d->nr_snr, sizeof d->nr_snr, "-2.5");
        cp(d->nrca, sizeof d->nrca, "0,603,1,78,633984,100,0,-140.0,-43.0,-23.0,-120.0;");
        d->rx_speed = 42000; d->tx_speed = 9000;
        break;
    case RT_NOSIGNAL:
        d->bars = 0; cp(d->net_type, sizeof d->net_type, "LIMITED_SERVICE");
        d->nr_rsrp = 0; d->nr_rsrq = 0; cp(d->nr_snr, sizeof d->nr_snr, "");
        cp(d->nr_band, sizeof d->nr_band, ""); cp(d->band, sizeof d->band, "");
        d->nrca[0] = 0; d->nr_pci = 0; d->nr_channel = 0; d->nr_cell_id = 0;
        cp(d->wan_status, sizeof d->wan_status, "disconnected");
        d->rx_speed = 0; d->tx_speed = 0; d->qci = 0; d->ambr_dl = 0; d->ambr_ul = 0;
        break;
    case RT_NOSIM:
        d->bars = 0; cp(d->net_type, sizeof d->net_type, "");
        cp(d->operator_name, sizeof d->operator_name, "");
        d->nrca[0] = 0; cp(d->nr_band, sizeof d->nr_band, ""); d->nr_rsrp = 0; cp(d->nr_snr, sizeof d->nr_snr, "");
        cp(d->sim_state, sizeof d->sim_state, "sim absent");
        d->rx_speed = 0; d->tx_speed = 0; d->qci = 0;
        break;
    case RT_ABROAD:
        cp(d->operator_name, sizeof d->operator_name, "中華電信");
        d->mcc = 466; d->mnc = 92;
        cp(d->nr_band, sizeof d->nr_band, "n78"); cp(d->nr_bw, sizeof d->nr_bw, "60");
        cp(d->nrca, sizeof d->nrca, "0,211,1,78,636666,60,0,-140.0,-43.0,-23.0,-120.0;");
        break;
    case RT_LOW_BAT:
        d->bat_percent = 8; d->bat_temp = 31;
        break;
    case RT_FULL_CHARGING:
        d->bat_percent = 100; d->charging = 1; d->charger_connect = 1;
        d->chg_uv = 4674000; d->chg_ua = 1203000; d->bat_ua = 949242;
        d->rx_speed = 118400000; d->tx_speed = 88100000;
        break;
    default:
        break;
    }
}

int data_refresh(devui_data_t *d)
{
    rt_refreshes++;
    int up = !IS(RT_LOADING) && !(IS(RT_DATAD_DOWN) && rt_refreshes > 3);
    if (!up) {
        memset(d, 0, sizeof *d);
        d->cpu_usage = -1;
        return 0;
    }
    fill_data(d);
    return 1;
}
int  data_refresh_live(devui_data_t *d) { return data_refresh(d); }
void data_set_pace(int panel_lit) { (void)panel_lit; }
int  data_backend_fd(void) { return -1; }
int  data_backend_init(void) { return 0; }
int  data_backend_poll(uint32_t now_ms) { (void)now_ms; return 0; }
int  data_backend_commit_latest(void) { return 0; }

static int s_sms_armed = -1;
int sms_mark_read(int i) { (void)i; return 0; }
int sms_delete_arm(int i) { s_sms_armed = i; return 1; }
int sms_delete_armed(int i) { return i == s_sms_armed; }
int sms_delete_tap(int i) { (void)i; return 0; }
int sms_mark_read_id(long id) { (void)id; return 0; }
int sms_mark_all_read(void) { return 0; }
int sms_delete_id(long id) { (void)id; return 0; }

/* ------------------------------------------------------------------ chill */
static int chill_up(void) { return !EMPTY; }
int chill_poll(int active) { (void)active; return 1; }
const char *chill_card_html(int locked) { (void)locked; return ""; }
const char *chill_core(void) { return chill_up() ? "运行中" : "已停止"; }
const char *chill_mode(void) { return !chill_up() ? "-" : IS(RT_ABROAD) ? "直连·AI 不动" : "代理"; }
const char *chill_exit_raw(void) { return !chill_up() ? "" : IS(RT_ABROAD) ? "direct_keep_ai" : "proxy"; }
int chill_set_exit(const char *state) { (void)state; return 1; }
const char *chill_mode_raw(void) { return chill_up() ? "rule" : ""; }
const char *chill_group(void) { return chill_up() ? "节点选择" : ""; }
const char *chill_node(void)
{
    if (!chill_up()) return "";
    return LONG_NAMES ? "TW 台湾 Taiwan 01 | IEPL 专线 x2.0 | Netflix Disney+ HBO" : "TW 台湾 01";
}
const char *chill_traffic(void) { return chill_up() ? "↓12.4G ↑1.1G" : ""; }
const char *chill_speed(void) { return !chill_up() ? "" : IS(RT_FULL_CHARGING) ? "↓88.2M/s ↑9.1M/s" : "↓1.2M/s ↑45K/s"; }
const char *chill_chain(void) { return chill_up() ? "节点选择 → TW 台湾 01" : ""; }
int chill_restart_armed(void) { return 0; }
const char *chill_conns(void) { return chill_up() ? "37" : "0"; }
int chill_online(void) { return chill_up(); }
const char *chill_conn_split(void) { return chill_up() ? "代理 21 · 直连 16" : ""; }
const char *chill_grouplist_html(void) { return ""; }
int chill_select_group(int i) { (void)i; return 1; }
int chill_group_selectable(void) { return 1; }

static const char *const k_groups[] = { "节点选择", "AI", "流媒体", "苹果服务" };
int chill_group_count(void) { return chill_up() ? 4 : 0; }
void chill_get_group(int i, chill_group_info_t *o)
{
    memset(o, 0, sizeof *o);
    if (i < 0 || i >= 4) return;
    cp(o->name, sizeof o->name, k_groups[i]);
    o->selected = i == 0;
    o->auto_pick = i == 3;
}
const char *chill_nodelist_html(void) { return ""; }
static const struct { const char *n; int delay; } k_nodes[] = {
    { "TW 台湾 01", 42 }, { "TW 台湾 02", 55 }, { "JP 日本 01", 71 }, { "US 美国 01", 168 },
    { "SG 新加坡 01", 0 }, { "KR 韩国 01", 93 }, { "HK 香港 01", -1 }, { "US 美国 02", 201 },
    { "JP 日本 02", 66 }, { "TW 台湾 03", 48 },
};
int chill_node_count(void) { return chill_up() ? 10 : 0; }
void chill_get_node(int i, chill_node_info_t *o)
{
    memset(o, 0, sizeof *o);
    if (i < 0 || i >= 10) return;
    if (LONG_NAMES && i == 0) cp(o->name, sizeof o->name, "TW 台湾 Taiwan 01 | IEPL 专线 x2.0 | Netflix Disney+ HBO Max");
    else cp(o->name, sizeof o->name, k_nodes[i].n);
    o->delay = k_nodes[i].delay;
    o->selected = i == 0;
}
static const struct { const char *n, *t; long b; } k_pairs[] = {
    { "AI → US 美国 01", "↓820M ↑95M", 915000000 },
    { "节点选择 → TW 台湾 01", "↓640M ↑41M", 681000000 },
    { "流媒体 → JP 日本 01", "↓2.1G ↑12M", 2112000000L },
    { "苹果服务 → DIRECT", "↓120M ↑8M", 128000000 },
    { "漏网之鱼 → TW 台湾 02", "↓12M ↑2M", 14000000 },
    { "Google → TW 台湾 01", "↓3M ↑1M", 4000000 },
};
int chill_top_pair_count(void) { return chill_up() ? 6 : 0; }
void chill_get_top_pair(int i, chill_traffic_item_t *o)
{
    memset(o, 0, sizeof *o);
    if (i < 0 || i >= 6) return;
    cp(o->name, sizeof o->name, k_pairs[i].n);
    cp(o->traffic, sizeof o->traffic, k_pairs[i].t);
    o->bytes = k_pairs[i].b;
}
int chill_set_mode(const char *m) { (void)m; return 1; }
int chill_select_node(int i) { (void)i; return 1; }
int chill_restart_core(void) { return 1; }
const char *chill_profile_raw(void) { return "standard"; }
const char *chill_profile_effective_raw(void) { return "standard"; }
int chill_thermal_eco(void) { return 0; }
int chill_set_profile(const char *p) { (void)p; return 1; }
int chill_test_delay(void) { return 1; }
int chill_delay_pending(void) { return 0; }
int devui_restore_stock(void) { return 0; }
int devui_rotate180(void) { return 0; }

/* -------------------------------------------------------------- tailscale */
int tailscale_poll(int active) { (void)active; return 1; }
const char *tailscale_card_html(int locked) { (void)locked; return ""; }
void tailscale_get_status(tailscale_status_t *o)
{
    memset(o, 0, sizeof *o);
    o->available = 1;
    o->ok = 1;
    if (EMPTY) { cp(o->state, sizeof o->state, "NeedsLogin"); return; }
    cp(o->state, sizeof o->state, "Running");
    o->self_online = 1;
    cp(o->name, sizeof o->name, LONG_NAMES ? "u60-pro-living-room-router-with-a-long-name" : "u60-pro");
    cp(o->ip, sizeof o->ip, "100.64.0.7");
    cp(o->relay, sizeof o->relay, "hkg");
    cp(o->routes, sizeof o->routes, "10.0.66.0/24");
    o->peers = 5; o->peers_online = 3; o->active = 2; o->direct = 1;
    if (IS(RT_ABROAD)) { cp(o->exit_node, sizeof o->exit_node, "home-nas"); o->exit_online = 1; }
}
static const struct { const char *n, *ip; int on, act, dir, ex; } k_peers[] = {
    { "macbook-pro", "100.64.0.2", 1, 1, 1, 0 }, { "iphone", "100.64.0.3", 1, 1, 0, 0 },
    { "home-nas", "100.64.0.4", 1, 0, 0, 1 }, { "office-pc", "100.64.0.5", 0, 0, 0, 0 },
    { "old-ipad", "100.64.0.6", 0, 0, 0, 0 },
};
int tailscale_peer_count(void) { return EMPTY ? 0 : 5; }
void tailscale_get_peer(int i, tailscale_peer_t *o)
{
    memset(o, 0, sizeof *o);
    if (i < 0 || i >= 5) return;
    cp(o->name, sizeof o->name, LONG_NAMES && i == 0 ? "macbook-pro-16-inch-2024-work-laptop" : k_peers[i].n);
    cp(o->ip, sizeof o->ip, k_peers[i].ip);
    o->online = k_peers[i].on; o->active = k_peers[i].act; o->direct = k_peers[i].dir; o->exit_node = k_peers[i].ex;
}

/* ------------------------------------------------------------------- esim */
static int s_esim_armed = -1;
int esim_poll(int active) { (void)active; return 1; }
const char *esim_current(void) { return EMPTY ? "" : "中国联通 · 主号"; }
const char *esim_state(void) { return IS(RT_LOADING) ? "" : "就绪"; }
const char *esim_list_html(void) { return ""; }
int esim_select(int i) { s_esim_armed = i; return ESIM_SEL_ARMED; }
int agent_post(const char *p) { (void)p; return 200; }
int agent_request(const char *m, const char *p, const char *j) { (void)m; (void)p; (void)j; return 200; }
int esim_profile_count(void) { return EMPTY ? 0 : 3; }
void esim_get_profile(int i, esim_profile_t *o)
{
    static const char *const nm[] = { "中国联通 · 主号", "Chunghwa Telecom", "giffgaff" };
    static const char *const sub[] = { "ICCID 8986 0000 0000 0000 0001", "ICCID 8988 0000 0000 0000 0002", "ICCID 8944 0000 0000 0000 0003" };
    memset(o, 0, sizeof *o);
    if (i < 0 || i >= 3) return;
    cp(o->name, sizeof o->name, nm[i]);
    cp(o->sub, sizeof o->sub, sub[i]);
    o->enabled = i == 0;
    o->armed = i == s_esim_armed;
}
int esim_locked(void) { return 0; }
int esim_loaded(void) { return !IS(RT_LOADING); }

/* -------------------------------------------------------------- speedtest */
static int st_up(void) { return !EMPTY; }
int speedtest_poll(int active) { (void)active; return 1; }
speedtest_phase_t speedtest_phase(void) { return st_up() ? ST_COMPLETE : ST_IDLE; }
const char *speedtest_phase_label(void) { return st_up() ? "完成" : ""; }
int    speedtest_progress_pct(void) { return st_up() ? 100 : 0; }
double speedtest_live_mbps(void) { return 0; }
double speedtest_ping_ms(void) { return st_up() ? 23 : -1; }
double speedtest_jitter_ms(void) { return st_up() ? 4 : -1; }
double speedtest_download_mbps(void) { return st_up() ? 812.4 : -1; }
double speedtest_upload_mbps(void) { return st_up() ? 96.2 : -1; }
const char *speedtest_server(void) { return st_up() ? "China Telecom (Shanghai)" : ""; }
const char *speedtest_error(void) { return ""; }
int speedtest_running(void) { return 0; }
int speedtest_agent_reachable(void) { return st_up(); }
int speedtest_start(void) { return 1; }
int speedtest_stop(void) { return 1; }
int speedtest_servers_poll(int active) { (void)active; return 1; }
int speedtest_servers_count(void) { return st_up() ? 3 : 0; }
void speedtest_get_server(int i, speedtest_server_t *o)
{
    static const struct { long id; const char *n, *s, *c; } sv[] = {
        { 3633, "Shanghai", "China Telecom", "China" }, { 5083, "Shanghai", "China Unicom", "China" },
        { 4575, "Hangzhou", "China Mobile", "China" },
    };
    memset(o, 0, sizeof *o);
    if (i < 0 || i >= 3) return;
    o->id = sv[i].id;
    cp(o->name, sizeof o->name, sv[i].n);
    cp(o->sponsor, sizeof o->sponsor, sv[i].s);
    cp(o->country, sizeof o->country, sv[i].c);
}
int  speedtest_selected_index(void) { return -1; }
void speedtest_select_server(int i) { (void)i; }

/* --------------------------------------------------------------- scenario */
int scenario_poll(int active) { (void)active; return 1; }
void agent_health(agent_health_t *o)
{
    memset(o, 0, sizeof *o);
    o->checked = 1;
    o->unread = IS(RT_FULL_CHARGING) ? 2 : 0;
    o->warn = IS(RT_FULL_CHARGING) ? 1 : 0;
}
void scenario_get_status(scenario_status_t *o)
{
    memset(o, 0, sizeof *o);
    o->available = 1;
    o->enabled = 1;
    o->chill_on = EMPTY ? 0 : 1;
    if (IS(RT_LOADING)) return;                    /* 判定中 */
    if (IS(RT_ABROAD)) {
        cp(o->name, sizeof o->name, "国外");
        o->abroad = 1;
        o->auto_direct = 1;
    } else {
        cp(o->name, sizeof o->name, "在家");
        o->wifi_off = 1;
    }
    o->last_switch = rt_now - 3 * 3600;
}
int scenario_chill_set(int on) { (void)on; return 1; }

/* ----------------------------------------------------------------- alerts */
int alerts_poll(int active) { (void)active; return 1; }
static int al_n(void) { return EMPTY ? 0 : 2; }
int  alerts_count(void) { return al_n(); }
int  alerts_unread(void) { return IS(RT_FULL_CHARGING) ? 2 : 0; }
void alerts_get(int i, alert_item_t *o)
{
    memset(o, 0, sizeof *o);
    if (i < 0 || i >= al_n()) return;
    o->seq = 40 + i;
    o->time = i == 0 ? rt_now - 1800 : 0;
    o->uptime = 95;
    cp(o->label, sizeof o->label, i == 0 ? "Wi-Fi 看门狗重新打开了 Wi-Fi" : "触屏界面闪退，已自动重新打开");
    cp(o->text, sizeof o->text, i == 0 ? "agent gone (no answer 3 min), Wi-Fi was off; turning it on" : "exit 139 (SIGSEGV)");
    o->unread = IS(RT_FULL_CHARGING);
}
const char *alerts_error(void) { return ""; }
void alerts_mark_all_read(void) {}

/* ---------------------------------------------------- backlight / key / touch */
static int s_bl_on = 1, s_bl_level = 232;
void backlight_init(void) {}
void backlight_on(void) { s_bl_on = 1; }
void backlight_off(void) { s_bl_on = 0; }
int  backlight_panel_lit(void) { return s_bl_on; }
void backlight_toggle(void) { s_bl_on = !s_bl_on; }
int  backlight_is_on(void) { return s_bl_on; }
int  backlight_is_lit(void) { return s_bl_on; }
void backlight_fade_off(void) { s_bl_on = 0; }
void backlight_fade_on(void) { s_bl_on = 1; }
void backlight_predim(void) {}
void backlight_set(int level) { s_bl_level = level; s_bl_on = level > 0; }
int  backlight_get(void) { return s_bl_level; }
int  backlight_max(void) { return 255; }
void backlight_remember(int level) { if (level > 0) s_bl_level = level; }

int  key_input_init(key_input_t *k) { memset(k, 0, sizeof *k); k->fd = -1; return -1; }
int  key_input_poll(key_input_t *k, uint32_t now_ms) { (void)k; (void)now_ms; return KEY_EV_NONE; }
void key_input_close(key_input_t *k) { (void)k; }
unsigned long touch_input_report_count(void) { return 0; }

/* ------------------------------------------------------------------- exec */
long ui_boot_s(void) { return 1000; }
int ui_exec_self(const char *argv0, const ui_launch_t *l)
{
    (void)argv0;
    rt_exec_calls++;
    rt_exec_last = *l;
    return -1;   /* the harness stays in this process */
}
unsigned long g_frame_count;
