/*
 * data.h - device state consumed from the zwrt-datad backend.
 *
 * Reads the zwrt-datad HTTP/SSE backend snapshot.
 * The GUI never calls ubus directly. If the backend isn't running, refresh
 * returns 0 and the UI shows placeholders.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60PRO_DATA_H
#define U60PRO_DATA_H

#include <stdint.h>

#define DEVUI_SMS_MAX 32
#define DEVUI_SMS_TEXT_MAX 16384

typedef struct {
    int  valid;

    /* network */
    char net_type[16];
    int  bars;
    char operator_name[48];
    char roaming[16];       /* datad net.roaming 原样：Home / Roaming / …，"" = 不知道 */
    char band[16];
    char nr_band[16];
    int  nr_rsrp, nr_rsrq, nr_rssi;
    char nr_snr[12];
    int  lte_rsrp, lte_rsrq, lte_rssi;
    char lte_snr[12];
    int  lte_pci;           /* LTE serving cell (4G, and the NSA anchor) */
    long channel;           /* datad net.channel: wan_active_channel */
    char bandwidth[12];     /* datad net.bandwidth, MHz, when it knows */
    char operate_mode[16];  /* ONLINE / LPM (airplane) / OFFLINE, "" = unknown */
    int  rssi, mcc, mnc, nr_pci;
    long nr_cell_id, nr_channel;
    long lte_cell_id;          /* net.cell_id: the LTE serving cell */
    char nr_bw[12];
    char nrca[256], lteca[256], ltecasig[256];
    char wan_status[32];
    char net_select[16];
    int  hsr;
    char sa_bands[256], nsa_bands[256], lte_bands[256];          /* current lock sets */
    char sa_sup[256], nsa_sup[256], lte_sup[256];             /* what the modem supports (datad *_supported_bands) */

    /* battery */
    int  bat_percent, bat_temp, charging, charger_connect;
    long chg_uv, chg_ua, bat_uv, bat_ua;   /* charger/battery voltage(µV)/current(µA) */

    /* clients */
    int  clients_total, clients_wifi, clients_lan;
    struct { char name[40], ip[24], mac[20]; } client[16];
    int  client_n;

    /* sms (read-only) */
    int  sms_unread;
    struct { long id; char num[40], date[16], text[DEVUI_SMS_TEXT_MAX]; int unread; } sms[DEVUI_SMS_MAX];
    int  sms_n;

    /* wifi (main SSID) */
    char wifi_ssid[64], wifi_key[64], wifi_enc[24];
    int  wifi_enabled;

    /* nfc tap-to-share */
    int  nfc_switch;

    /* dhcp / lan */
    char dhcp_ip[24], dhcp_start[24], dhcp_limit[8], dhcp_leasetime[12];

    /* power.direct_supply.mode and interfaces.cellular.{enable,roam_enable}:
     * 1 / 0, -1 = not in this /state. The touch UI used to read these with
     * its own ubus calls (T13: datad is the only periodic ubus reader). */
    int dps_mode, cell_data, cell_roam;

    /* traffic (bytes, bytes/s). day_ and month_ fields are the firmware's
     * own zwrt_data ubus counters (already aggregated by calendar day/
     * month, persisted across reboots in UCI) — not derived from
     * rx_bytes/tx_bytes, which is this boot's session total and resets
     * on restart. */
    long rx_speed, tx_speed, rx_bytes, tx_bytes;
    long day_rx_bytes, day_tx_bytes, month_rx_bytes, month_tx_bytes;

    /* qos (parsed from modem key.log by the backend) */
    int    qci;
    double ambr_dl, ambr_ul;   /* Mbps */
    char   usb_mode[16];       /* "user" = adb off, "debug" = adb on */

    /* system */
    long uptime, cpu_temp, cpu_usage, mem_used_pct, mem_total, mem_avail;
    char model[64], fw[80], sw_version[80], imei[24];

    /* sim.state as datad reports it ("sim ready", …); "" when not reported.
     * Only "has no usable SIM" is read from it (ui_logic.h ui_sig_state). */
    char sim_state[24];
    /* sim.iccid / imsi / msisdn: the card the modem is using (a plain SIM or
     * the active profile of an eSIM card). iccid may end in an F pad. */
    char sim_iccid[24], sim_imsi[20], sim_msisdn[24];
    /* sim.spn: the card's own brand name (EF_SPN), e.g. "CMLink"; "" = none or old datad */
    char sim_spn[32];
} devui_data_t;

/* Start the backend transport and seed the first visible snapshot if available. */
int  data_backend_init(void);

/* Drain SSE events and reconnect as needed. Returns 1 when the live snapshot changed. */
int  data_backend_poll(uint32_t now_ms);

/* Promote the latest live snapshot to the UI-visible snapshot. */
int  data_backend_commit_latest(void);

/* Close the transport socket. Safe to call during shutdown. */
void data_backend_close(void);

/* Copy the current UI-visible snapshot. Returns 1 on success. */
int data_refresh(devui_data_t *d);

/* Copy the latest live snapshot even if UI-visible refresh is paused. */
int data_refresh_live(devui_data_t *d);

/*
 * SMS actions — fire-and-forget POST to zwrt-datad's /control (waiting for
 * the ubus round-trip here would freeze the UI thread, which also drives
 * touch and rendering). Results show up on the next /state refresh, not as a
 * return value.
 *
 * `index` is into the UI-visible snapshot (the one data_refresh() fills),
 * so callers pass the same index used to render the row.
 */
int sms_mark_read(int index);

/*
 * Device writes from the touch UI: POST to datad's /control with
 * "source":"screen" and an op_id, same non-blocking path as the SMS actions.
 * The reply is read in the background (data_control_notice).
 * `fallback_args` ("key=value …", "" for none; NULL = no emergency write) is
 * for the emergency script /data/u60-guard/u60-fallback.sh: it runs only
 * when datad cannot be reached at all — never on 503, 409 or no answer, as
 * datad may still be doing it (E4 write-op-layer.md 前提 6, D18). The script
 * checks again that datad is gone. Returns 1 if the request went to datad.
 */
int data_control(const char *action, const char *params_json, const char *fallback_args);

/* data_control whose emergency write is a different script action, where
 * datad's action has no script counterpart (Wi-Fi: datad wifi.apply, the
 * script's wifi.radio). */
int data_control_fb(const char *action, const char *params_json, const char *fallback_action,
                    const char *fallback_args);

/* 撤销 from the change log (E4 DD10): the same write with "undo":true, as
 * datad's journal.list undo_view.request says. No emergency write. */
int data_control_undo(const char *action, const char *params_json);

/* The last write datad turned down: kind UI_CTL_BUSY (say_* = datad's
 * "正在换制式（触屏发起，32 秒），稍等"), UI_CTL_FULL or UI_CTL_FAILED.
 * Returns 1 when there is one younger than max_age_ms. */
typedef struct {
    int kind;
    char action[40];
    char say_zh[160], say_en[160];
    uint32_t at;
} data_notice_t;
int data_control_notice(data_notice_t *out, uint32_t max_age_ms);
        /* no-op if the row is already read */
int sms_delete_arm(int index);       /* long-press: arms the row, doesn't delete yet */
int sms_delete_armed(int index);     /* is this row currently armed? (for the UI to paint) */
int sms_delete_tap(int index);       /* tap while armed (past a short debounce): deletes */
/* By message id — the detail page keeps showing one message while the list
 * underneath is re-read and reordered, so row numbers are not stable there. */
int sms_mark_read_id(long id);       /* no-op if already read or gone */
int sms_mark_all_read(void);         /* returns how many were unread */
int sms_delete_id(long id);

/*
 * zwrt-datad's polling pace: 1 s while the panel is lit, 5 s while it is dark
 * (state.set_interval). Nothing else on the device reads datad, so a dark
 * screen needs no 1 s data. Sends only on a change, plus once after start and
 * after every SSE reconnect (a restarted datad is back at its -i 1000). Never
 * on a timer: each call makes datad drop its slow-data cache. Measured
 * 2026-09-23: datad ~6% → ~2% of a core with the screen off.
 */
void data_set_pace(int panel_lit);

/* The SSE socket, or -1 while there is none (main.c waits on it while dark). */
int data_backend_fd(void);
/* Wall clock (s) of the last byte from datad; 0 = never. */
long data_backend_alive_wall(void);
/* 1 = datad answered before but has said nothing (no snapshot, no keep-alive,
 * no HTTP reply) for 45 s. data_refresh() still hands out the last snapshot;
 * the caller must not show it as live. */
int data_backend_silent(void);
/* Bumps each time a changed snapshot is committed (what data_refresh() hands out). */
unsigned long long data_backend_version(void);

#endif /* U60PRO_DATA_H */
