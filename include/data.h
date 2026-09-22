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
    char band[16];
    char nr_band[16];
    int  nr_rsrp, nr_rsrq, nr_rssi;
    char nr_snr[12];
    int  lte_rsrp, lte_rsrq, lte_rssi;
    char lte_snr[12];
    int  rssi, mcc, mnc, nr_pci;
    long nr_cell_id, nr_channel;
    char nr_bw[12];
    char nrca[256], lteca[256], ltecasig[256];
    char wan_status[32];
    char net_select[16];
    int  hsr;
    char sa_bands[256], nsa_bands[256], lte_bands[256];

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
 * SMS actions — fire-and-forget POST to zwrt-datad's /control (mirrors
 * chill.c's sc_send_async: waiting for the ubus round-trip here would
 * freeze the UI thread the same way chill_select_node() used to before
 * that was fixed). Results show up on the next /state refresh, not as a
 * return value.
 *
 * `index` is into the UI-visible snapshot (the one data_refresh() fills),
 * so callers pass the same index used to render the row.
 */
int sms_mark_read(int index);        /* no-op if the row is already read */
int sms_delete_arm(int index);       /* long-press: arms the row, doesn't delete yet */
int sms_delete_armed(int index);     /* is this row currently armed? (for the UI to paint) */
int sms_delete_tap(int index);       /* tap while armed (past a short debounce): deletes */

#endif /* U60PRO_DATA_H */
