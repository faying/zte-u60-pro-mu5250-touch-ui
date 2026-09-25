/*
 * fixtures.c - scene data behind every device-facing interface ui.c uses
 * (data / tailscale / esim / speedtest / scenario / alerts / netinfo /
 * backlight / key / touch / exec). Nothing here touches the system.
 *
 * SPDX-License-Identifier: MIT
 */
#include "fixtures.h"

#include "alerts.h"
#include "backlight.h"
#include "data.h"
#include "esim.h"
#include "key_input.h"
#include "netinfo.h"
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

static const char *const k_names[RT_SCENES] = {
    "good", "weak", "nosignal", "datad-down", "loading", "nosim",
    "abroad", "lowbat", "full-charging", "long-names", "empty",
    "nsa", "lte", "3g", "nodata", "5ga", "edge", "crowd", "today", "us", "jp", "nosvc",
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
    cp(d->net_type, sizeof d->net_type, "SA");   /* the modem's raw network_type */
    d->bars = 5;
    cp(d->operator_name, sizeof d->operator_name, LONG_NAMES ? "中华电信 Chunghwa Telecom Co., Ltd. 4G/5G" : "中国电信");
    cp(d->roaming, sizeof d->roaming, "Home");
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
    /* band lists as the owner's B27 reports them (nwinfo_get_netinfo, 2026-09-25) */
    cp(d->sa_bands, sizeof d->sa_bands, "1,2,3,5,7,8,18,20,26,28,29,38,40,41,48,66,71,75,77,78,79");
    cp(d->nsa_bands, sizeof d->nsa_bands, "1,2,3,5,7,8,18,20,26,28,29,38,40,41,48,66,71,75,77,78,79");
    cp(d->lte_bands, sizeof d->lte_bands, "1,2,3,4,5,7,8,18,19,20,26,28,29,32,34,38,39,40,41,42,43,48,66,71");

    d->bat_percent = 78; d->bat_temp = 33; d->charging = 0; d->charger_connect = 0;
    d->bat_uv = 4055664; d->bat_ua = -349242; d->chg_uv = 30000; d->chg_ua = 0;

    static const char *const cn[] = { "iPhone", "MacBook-Pro", "iPad", "Nintendo-Switch", "Kindle", "Pixel-9" };
    int nc = EMPTY ? 0 : 6;
    for (int i = 0; i < nc; i++) {
        if (LONG_NAMES) cp(d->client[i].name, sizeof d->client[i].name, "a-very-long-hostname-for-a-laptop-0123456");
        else            cp(d->client[i].name, sizeof d->client[i].name, cn[i]);
        snprintf(d->client[i].ip, sizeof d->client[i].ip, "192.168.0.%d", 20 + i);
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
    cp(d->dhcp_ip, sizeof d->dhcp_ip, "192.168.0.1");
    cp(d->dhcp_start, sizeof d->dhcp_start, "2");
    cp(d->dhcp_limit, sizeof d->dhcp_limit, "252");
    cp(d->dhcp_leasetime, sizeof d->dhcp_leasetime, "86400");
    d->dps_mode  = 0;                    /* direct power supply off */
    d->cell_data = 1;                    /* mobile data on */
    d->cell_roam = IS(RT_ABROAD) ? 1 : 0;

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
    /* the eSIM card's enabled profile (esim_enabled_iccid below) is in use */
    cp(d->sim_iccid, sizeof d->sim_iccid, "89860000000000000001");
    cp(d->sim_imsi, sizeof d->sim_imsi, "460010123456789");
    cp(d->sim_msisdn, sizeof d->sim_msisdn, "+8613800001234");

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
        d->sim_iccid[0] = d->sim_imsi[0] = d->sim_msisdn[0] = 0;
        d->rx_speed = 0; d->tx_speed = 0; d->qci = 0;
        break;
    case RT_ABROAD:
        cp(d->operator_name, sizeof d->operator_name, "中華電信");
        cp(d->roaming, sizeof d->roaming, "Roaming");
        d->mcc = 466; d->mnc = 92;
        /* a plain SIM from home, roaming (no eSIM profile matches it) */
        cp(d->sim_iccid, sizeof d->sim_iccid, "8986110000000000077F");
        cp(d->sim_imsi, sizeof d->sim_imsi, "460110000000077");
        d->sim_msisdn[0] = 0;
        d->mcc = 466; d->mnc = 92;
        cp(d->nr_band, sizeof d->nr_band, "n78"); cp(d->nr_bw, sizeof d->nr_bw, "60");
        cp(d->nrca, sizeof d->nrca, "0,211,1,78,636666,60,0,-140.0,-43.0,-23.0,-120.0;");
        break;
    case RT_LOW_BAT:
        d->bat_percent = 8; d->bat_temp = 31;
        break;
    case RT_NSA:
        cp(d->net_type, sizeof d->net_type, "NSA");
        cp(d->nr_bw, sizeof d->nr_bw, "100");
        d->nrca[0] = 0;
        cp(d->lteca, sizeof d->lteca,
           "0,120,1,3,1850,20,0,-95.0,-10.0,12.0,-70.0;1,121,1,1,100,20,0,-99.0,-11.0,9.0,-72.0;");
        d->lte_rsrp = -95; cp(d->lte_snr, sizeof d->lte_snr, "12.0");
        break;
    case RT_LTE:
        cp(d->net_type, sizeof d->net_type, "LTE");
        d->nr_rsrp = 0; cp(d->nr_snr, sizeof d->nr_snr, ""); cp(d->nr_band, sizeof d->nr_band, "");
        d->nrca[0] = 0; d->lteca[0] = 0; d->nr_pci = 0;
        cp(d->band, sizeof d->band, "LTE BAND 3"); cp(d->bandwidth, sizeof d->bandwidth, "20");
        d->lte_rsrp = -98; d->lte_rsrq = -10; cp(d->lte_snr, sizeof d->lte_snr, "14.5");
        d->lte_pci = 120; d->channel = 1850; d->lte_cell_id = 45678901;
        cp(d->net_select, sizeof d->net_select, "Only_LTE");
        break;
    case RT_3G:
        cp(d->net_type, sizeof d->net_type, "WCDMA"); d->bars = 3;
        d->nr_rsrp = 0; cp(d->nr_snr, sizeof d->nr_snr, ""); cp(d->nr_band, sizeof d->nr_band, "");
        d->nrca[0] = 0; d->lteca[0] = 0; d->nr_pci = 0; d->lte_rsrp = 0;
        cp(d->band, sizeof d->band, "WCDMA BAND 1"); d->rssi = -75;
        break;
    case RT_NODATA:
        cp(d->wan_status, sizeof d->wan_status, "disconnected");
        break;
    case RT_5GA:
        cp(d->nr_bw, sizeof d->nr_bw, "160");
        cp(d->nrca, sizeof d->nrca,
           "1,615,1,41,504990,100,0,-92.0,-10.0,15.0,-70.0;2,620,1,78,633984,100,0,-95.0,-11.0,13.0,-72.0;");
        break;
    case RT_CROWD:
        d->bars = 4; d->nr_rsrp = -88; d->nr_rsrq = -18; cp(d->nr_snr, sizeof d->nr_snr, "8.0");
        d->rx_speed = 150000; d->tx_speed = 20000;
        break;
    case RT_TODAY:
        d->bars = 3; cp(d->band, sizeof d->band, "n5"); cp(d->nr_band, sizeof d->nr_band, "n5");
        cp(d->nr_bw, sizeof d->nr_bw, "15");
        d->nr_rsrp = -102; d->nr_rsrq = -17; cp(d->nr_snr, sizeof d->nr_snr, "-1.9");
        d->nrca[0] = 0; d->rx_speed = 3000; d->tx_speed = 1000;
        break;
    case RT_NOSVC:
        d->bars = 0; cp(d->net_type, sizeof d->net_type, "");
        d->nr_rsrp = 0; d->nr_rsrq = 0; cp(d->nr_snr, sizeof d->nr_snr, "");
        cp(d->nr_band, sizeof d->nr_band, ""); cp(d->band, sizeof d->band, "");
        d->nrca[0] = 0; d->nr_pci = 0; d->nr_channel = 0; d->nr_cell_id = 0;
        cp(d->wan_status, sizeof d->wan_status, "disconnected");
        d->rx_speed = 0; d->tx_speed = 0;
        break;
    case RT_US:
        cp(d->operator_name, sizeof d->operator_name, "T-Mobile");
        cp(d->roaming, sizeof d->roaming, "Roaming"); d->mcc = 310; d->mnc = 260;
        cp(d->band, sizeof d->band, "n41"); cp(d->nr_band, sizeof d->nr_band, "n41");
        cp(d->nr_bw, sizeof d->nr_bw, "100"); d->nrca[0] = 0;
        break;
    case RT_JP:
        cp(d->operator_name, sizeof d->operator_name, "SoftBank");
        cp(d->roaming, sizeof d->roaming, "Roaming"); d->mcc = 440; d->mnc = 20;
        cp(d->net_type, sizeof d->net_type, "LTE");
        d->nr_rsrp = 0; cp(d->nr_snr, sizeof d->nr_snr, ""); cp(d->nr_band, sizeof d->nr_band, "");
        d->nrca[0] = 0; d->nr_pci = 0;
        cp(d->band, sizeof d->band, "LTE BAND 1"); cp(d->bandwidth, sizeof d->bandwidth, "20");
        d->lte_rsrp = -92; d->lte_rsrq = -9; cp(d->lte_snr, sizeof d->lte_snr, "16.0");
        cp(d->lteca, sizeof d->lteca,
           "0,120,1,1,300,20,0,-92.0,-9.0,16.0,-60.0;1,121,1,3,1850,20,0,-95.0,-10.0,12.0,-63.0;");
        break;
    case RT_EDGE:
        cp(d->net_type, sizeof d->net_type, "EDGE"); d->bars = 3;
        d->nr_rsrp = 0; cp(d->nr_snr, sizeof d->nr_snr, ""); cp(d->nr_band, sizeof d->nr_band, "");
        d->nrca[0] = 0; d->lteca[0] = 0; d->nr_pci = 0; d->lte_rsrp = 0;
        cp(d->band, sizeof d->band, "GSM 900"); d->rssi = -81;
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
        d->dps_mode = d->cell_data = d->cell_roam = -1;
        return 0;
    }
    fill_data(d);
    return 1;
}
int  data_refresh_live(devui_data_t *d) { return data_refresh(d); }
void data_set_pace(int panel_lit) { (void)panel_lit; }
int  data_control(const char *a, const char *p, const char *fb) { (void)a; (void)p; (void)fb; return 1; }
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
    cp(o->routes, sizeof o->routes, "192.168.0.0/24");
    o->peers = 5; o->peers_online = 3; o->active = 2; o->direct = 1;
    cp(o->ip6, sizeof o->ip6, "fd7a:115c:a1e0::7");
    cp(o->os, sizeof o->os, "linux");
    cp(o->version, sizeof o->version, "1.102.4");
    cp(o->tailnet, sizeof o->tailnet, "me@example.com");
    cp(o->key_expiry, sizeof o->key_expiry, "2027-03-01");
    if (IS(RT_ABROAD)) { cp(o->exit_node, sizeof o->exit_node, "home-nas"); o->exit_online = 1; }
}
static const struct {
    const char *n, *ip; int on, act, dir, ex; const char *os, *addr, *relay; long long rx, tx; long hs, seen;
} k_peers[] = {
    { "macbook-pro", "100.64.0.2", 1, 1, 1, 0, "macOS", "203.0.113.5:41641", "tok", 1288490188LL, 52428800LL, 42, -1 },
    { "iphone", "100.64.0.3", 1, 1, 0, 0, "iOS", "", "hkg", 3145728LL, 1048576LL, 95, -1 },
    { "home-nas", "100.64.0.4", 1, 0, 0, 1, "linux", "", "hkg", 0, 0, 1800, -1 },
    { "office-pc", "100.64.0.5", 0, 0, 0, 0, "windows", "", "sfo", 0, 0, -1, 3 * 3600 },
    { "old-ipad", "100.64.0.6", 0, 0, 0, 0, "iOS", "", "", 0, 0, -1, -1 },
};
int tailscale_peer_count(void) { return EMPTY ? 0 : 5; }
void tailscale_get_peer(int i, tailscale_peer_t *o)
{
    memset(o, 0, sizeof *o);
    if (i < 0 || i >= 5) return;
    cp(o->name, sizeof o->name, LONG_NAMES && i == 0 ? "macbook-pro-16-inch-2024-work-laptop" : k_peers[i].n);
    cp(o->ip, sizeof o->ip, k_peers[i].ip);
    o->online = k_peers[i].on; o->active = k_peers[i].act; o->direct = k_peers[i].dir; o->exit_node = k_peers[i].ex;
    cp(o->os, sizeof o->os, k_peers[i].os);
    cp(o->cur_addr, sizeof o->cur_addr, k_peers[i].addr);
    cp(o->relay, sizeof o->relay, k_peers[i].relay);
    o->rx = k_peers[i].rx; o->tx = k_peers[i].tx;
    o->hs_ago = k_peers[i].hs; o->seen_ago = k_peers[i].seen;
}

/* ------------------------------------------------------------------- esim */
static int s_esim_armed = -1;
int esim_poll(int active) { (void)active; return 1; }
const char *esim_current(void) { return EMPTY ? "" : "中国联通 · 主号"; }
const char *esim_state(void) { return IS(RT_LOADING) ? "" : "就绪"; }
const char *esim_list_html(void) { return ""; }
int esim_select(int i) { if (i == 0) return ESIM_SEL_CURRENT; s_esim_armed = i; return ESIM_SEL_ARMED; }   /* profile 0 is the enabled one */
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
const char *esim_enabled_iccid(void) { return EMPTY ? "" : "89860000000000000001"; }
int esim_prefetch(const char *key) { (void)key; return 0; }

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
    if (IS(RT_LOADING)) return;                    /* 判定中 */
    if (IS(RT_ABROAD)) {
        cp(o->name, sizeof o->name, "国外");
        o->abroad = 1;
    } else {
        cp(o->name, sizeof o->name, "在家");
        o->wifi_off = 1;
    }
    o->last_switch = rt_now - 3 * 3600;
}
void scenario_kick(void) {}

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
/* 体检：good = 一项注意（短信告警没配），full-charging = 一项异常，其余都正常 */
int  health_count(void) { return IS(RT_GOOD) ? 1 : IS(RT_FULL_CHARGING) ? 1 : 0; }
int  health_checked(void) { return 27; }
void health_get(int i, health_item_t *o)
{
    memset(o, 0, sizeof *o);
    if (i != 0) return;
    if (IS(RT_FULL_CHARGING)) {
        o->bad = 1;
        snprintf(o->id, sizeof o->id, "disk");
        snprintf(o->label, sizeof o->label, "/data 空间");
        snprintf(o->detail, sizeof o->detail, "只剩 12 MB");
    } else {
        snprintf(o->id, sizeof o->id, "sms");
        snprintf(o->label, sizeof o->label, "短信告警");
        snprintf(o->detail, sizeof o->detail, "没配置号码：后台挂了你不会知道");
    }
}

/* ---------------------------------------------------------------- netinfo */
int netinfo_poll(int active) { (void)active; return 1; }
static netinfo_t s_ni;
static void ni_oper(ni_oper_t *o, const char *name, const char *country, const char *mcc, const char *mnc)
{
    cp(o->name, sizeof o->name, name); cp(o->country, sizeof o->country, country);
    cp(o->mcc, sizeof o->mcc, mcc); cp(o->mnc, sizeof o->mnc, mnc);
}
const netinfo_t *netinfo_get(void)
{
    netinfo_t *n = &s_ni;
    memset(n, 0, sizeof *n);
    n->now = rt_now;
    n->roaming = -1;
    if (!IS(RT_LOADING) && !IS(RT_DATAD_DOWN)) {
        /* when / does as scenario.rs describe_detect / describe_actions write them */
        static const struct { const char *id, *name; int wifi_off, abroad; const char *when, *does; } k_sc[3] = {
            { "home", "在家", 1, 0, "附近有 Wi-Fi「My-Home-5G」「My-Home」时", "关 Wi-Fi · 不休眠，Tailscale 一直连得上" },
            { "away", "外出", 0, 0, "其他情景都不符合时（默认）", "开 Wi-Fi" },
            { "abroad", "国外", 0, 1, "插的不是中国的卡时（当地卡、境外 eSIM）", "开 Wi-Fi" },
        };
        n->scene_known = 1;
        n->scene_enabled = 1;
        n->nscenes = 3;
        for (int i = 0; i < 3; i++) {
            cp(n->scenes[i].id, sizeof n->scenes[i].id, k_sc[i].id);
            cp(n->scenes[i].name, sizeof n->scenes[i].name, k_sc[i].name);
            n->scenes[i].wifi_off = k_sc[i].wifi_off;
            n->scenes[i].abroad = k_sc[i].abroad;
            cp(n->scenes[i].when, sizeof n->scenes[i].when, k_sc[i].when);
            cp(n->scenes[i].does, sizeof n->scenes[i].does, k_sc[i].does);
        }
        cp(n->scene_current, sizeof n->scene_current, IS(RT_ABROAD) ? "abroad" : "home");
        if (IS(RT_FULL_CHARGING)) cp(n->scene_pin, sizeof n->scene_pin, "home");
    }
    if (IS(RT_DATAD_DOWN)) { cp(n->err, sizeof n->err, "连不上管理后台"); return n; }
    if (IS(RT_LOADING) || EMPTY) return n;
    n->direct.present = 1;
    cp(n->direct.ip, sizeof n->direct.ip, "203.0.113.24");
    cp(n->direct.geo, sizeof n->direct.geo, LONG_NAMES ? "中国台湾 新北市 板桥区 Banqiao District" : "中国 广东 深圳");
    cp(n->direct.isp, sizeof n->direct.isp, LONG_NAMES ? "Chunghwa Telecom Co., Ltd." : "电信");
    ni_oper(&n->home, "中国电信", "中国", "460", "11");
    ni_oper(&n->serving, "中国电信", "中国", "460", "11");
    n->roaming = 0;
    cp(n->selection, sizeof n->selection, "auto");
    /* APN as read on the owner's device 2026-09-25: auto mode dialling ctiot;
     * CTNET saved as the manual pick (not in use). Abroad: manual, CTNET. */
    n->apn_known = 1;
    cp(n->apn_in_use.id, 24, "auto109590"); cp(n->apn_in_use.name, 40, "China Telecom");
    cp(n->apn_in_use.apn, 40, "ctiot"); n->apn_in_use.pdp = 3; n->apn_in_use.in_use = 1;
    n->napns = LONG_NAMES ? 2 : 1;
    cp(n->apns[0].id, 24, "manu1"); cp(n->apns[0].name, 40, "CTNET"); cp(n->apns[0].apn, 40, "ctnet");
    n->apns[0].pdp = 3; n->apns[0].selected = 1;
    cp(n->apns[1].id, 24, "manu2"); cp(n->apns[1].name, 40, "Company private APN with a long name");
    cp(n->apns[1].apn, 40, "corp.example.internal.apn"); n->apns[1].pdp = 1;
    if (IS(RT_ABROAD)) { n->apn_manual = 1; n->apn_in_use = n->apns[0]; n->apns[0].in_use = 1; }
    cp(n->guard_phase, sizeof n->guard_phase, "idle");
    cp(n->scan_state, sizeof n->scan_state, "idle");
    cp(n->nbr_state, sizeof n->nbr_state, "unsupported");
    cp(n->nbr_err, sizeof n->nbr_err, "原厂扫描会断网且拿不到数据，已停用");
    if (IS(RT_GOOD) || LONG_NAMES) {
        /* 邻区：原厂扫描已停用（agent 报 unsupported）；搜过一次网 */
        cp(n->nbr_state, sizeof n->nbr_state, "unsupported");
        cp(n->nbr_err, sizeof n->nbr_err, "原厂扫描会断网且拿不到数据，已停用");
        n->ncells = 0;
        cp(n->cells[0].rat, 4, "NR"); cp(n->cells[0].pci, 8, "101"); cp(n->cells[0].arfcn, 12, "627264"); cp(n->cells[0].rsrp, 8, "-92");
        cp(n->cells[1].rat, 4, "NR"); cp(n->cells[1].pci, 8, "388"); cp(n->cells[1].arfcn, 12, "627264"); cp(n->cells[1].rsrp, 8, "-101");
        cp(n->cells[2].rat, 4, "LTE"); cp(n->cells[2].pci, 8, "57"); cp(n->cells[2].arfcn, 12, "1850"); cp(n->cells[2].rsrp, 8, "-108");
        n->clients_known = 1;
        n->nclients = 2;
        cp(n->clients[0].name, 40, LONG_NAMES ? "a-very-long-hostname-for-a-laptop-0123456" : "MacBook");
        cp(n->clients[0].ip, 20, "192.168.0.21");       /* = MacBook-Pro in the datad list, matched by IP */
        n->clients[0].down = 5368709120LL; n->clients[0].up = 314572800; n->clients[0].down_rate = 262144;
        n->clients[0].up_rate = 12288; n->clients[0].signal = -47;
        cp(n->clients[0].band, 12, "5 GHz"); n->clients[0].wifi_gen = 6; n->clients[0].link_down = 2402;
        cp(n->clients[1].mac, 20, "02:00:00:00:00:04");   /* = Kindle, matched by MAC */
        n->clients[1].down = 1048576; n->clients[1].up = 10240; n->clients[1].down_rate = -1; n->clients[1].up_rate = -1;
        cp(n->clients[1].band, 12, "2.4 GHz"); n->clients[1].signal = -72;
        cp(n->scan_state, sizeof n->scan_state, "done");
        n->nops = 3;
        cp(n->ops[0].plmn, 8, "46011"); cp(n->ops[0].name, 48, "中国电信"); cp(n->ops[0].country, 24, "中国"); cp(n->ops[0].rat, 8, "12"); cp(n->ops[0].status, 4, "2");
        cp(n->ops[1].plmn, 8, "46001"); cp(n->ops[1].name, 48, "中国联通"); cp(n->ops[1].country, 24, "中国"); cp(n->ops[1].rat, 8, "12"); cp(n->ops[1].status, 4, "1");
        cp(n->ops[2].plmn, 8, "46000"); cp(n->ops[2].name, 48, "中国移动"); cp(n->ops[2].country, 24, "中国"); cp(n->ops[2].rat, 8, "7"); cp(n->ops[2].status, 4, "3");
    }
    if (IS(RT_ABROAD)) {
        /* 国内卡在台湾漫游 */
        cp(n->direct.ip, sizeof n->direct.ip, "198.51.100.40");
        cp(n->direct.geo, sizeof n->direct.geo, "中国台湾 台北市");
        cp(n->direct.isp, sizeof n->direct.isp, "中华电信");
        ni_oper(&n->serving, "中华电信", "中国台湾", "466", "92");
        n->roaming = 1;
        cp(n->selection, sizeof n->selection, "manual");
        cp(n->guard_phase, sizeof n->guard_phase, "ok");
        cp(n->guard_target, sizeof n->guard_target, "46692");
    }
    if (IS(RT_WEAK)) {
        cp(n->guard_phase, sizeof n->guard_phase, "reverted");
        cp(n->guard_reason, sizeof n->guard_reason, "超时没注册上");
        cp(n->direct.err, sizeof n->direct.err, "ip-api.com: timeout");
    }
    return n;
}
int  rt_apn_calls;
char rt_apn_last[24];
void netinfo_apn_use(const char *id) { rt_apn_calls++; cp(rt_apn_last, sizeof rt_apn_last, id); }
void netinfo_scan(void) {}
void netinfo_register(int op) { (void)op; }
void netinfo_auto(void) {}
void netinfo_nbr_scan(void) {}
void netinfo_pin(const char *id) { (void)id; }
void netinfo_hurry(int secs) { (void)secs; }
const char *netinfo_action_error(void) { return ""; }

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

/* ---- battery_est: the line itself comes from estimate.c's est_text, so the
 * goldens show the real wording. One per state: discharging (good), low
 * (lowbat), full (full-charging), paused at the limit (weak), charging to a
 * limit (long-names, the longest text), no data (datad-down / loading). */
#include "battery_est.h"
#include "estimate.h"
int battery_est_feed(const devui_data_t *d) { (void)d; return 0; }
void battery_est_override(const char *text) { (void)text; }
const char *battery_est_text(void)
{
    static char t[96];
    est_t e = { EST_UNKNOWN, 0 };
    int target = 100;

    if (IS(RT_GOOD)) e = (est_t){ EST_DISCHARGING, 891 };
    else if (IS(RT_LOW_BAT)) e = (est_t){ EST_DISCHARGING, 92 };
    else if (IS(RT_FULL_CHARGING)) e = (est_t){ EST_REACHED, 0 };
    else if (IS(RT_WEAK)) { e = (est_t){ EST_PAUSED, 0 }; target = 80; }
    else if (LONG_NAMES) { e = (est_t){ EST_CHARGING, 198 }; target = 80; }
    est_text(e, target, t, sizeof t);
    return t;
}
