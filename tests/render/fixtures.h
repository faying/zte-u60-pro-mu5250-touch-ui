/*
 * fixtures.h - stand-ins for everything ui.c reads from the device, driven
 * by one scene number. Used only by tests/render/render_test.c.
 *
 * Values are shaped like a real MU5250 /state (n78 100 MHz, CA strings in
 * datad's 11-field format) but every identifier is made up: no real IMEI,
 * ICCID, SSID, number or tailnet name.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60_RENDER_FIXTURES_H
#define U60_RENDER_FIXTURES_H

#include "ui_logic.h"

typedef enum {
    RT_GOOD = 0,       /* 3 NR carriers (1 inactive), everything running             */
    RT_WEAK,           /* 2 bars, SINR below 0                                        */
    RT_NOSIGNAL,       /* SIM fine, no service                                        */
    RT_DATAD_DOWN,     /* had data, then the data service went away                   */
    RT_LOADING,        /* nothing has arrived since start                             */
    RT_NOSIM,          /* sim.state says no SIM                                       */
    RT_ABROAD,         /* 国外 scenario, roaming on a local network                    */
    RT_LOW_BAT,        /* 8 %, not charging                                           */
    RT_FULL_CHARGING,  /* 100 % + charging + unread alerts + large rates (worst bar)  */
    RT_LONG_NAMES,     /* every name at its buffer limit                              */
    RT_EMPTY,          /* no SMS, no clients, services stopped, Tailscale needs login,
                          no eSIM profile, speed test service unreachable            */
    RT_NSA,            /* 5G NSA: NR n78 + LTE anchor B3 + B1                          */
    RT_LTE,            /* 4G, one carrier, no lteca (serving cell only in lte_*)      */
    RT_3G,             /* WCDMA, no carriers at all                                   */
    RT_NODATA,         /* registered on 5G, but the data call is down                 */
    RT_5GA,            /* SA with three active NR carriers → 5G-A                     */
    RT_EDGE,           /* EDGE (2G)                                                   */
    RT_CROWD,          /* good RSRP, poor RSRQ, downloading → 疑似拥挤                  */
    RT_TODAY,          /* the device on 2026-09-25: SA, one n5 15 MHz, SINR -1.9       */
    RT_US,             /* mainland SIM roaming on T-Mobile n41 → 5G UC                 */
    RT_JP,             /* mainland SIM roaming on SoftBank, LTE 2 carriers → 4G+       */
    RT_NOSVC,          /* SIM fine, not even emergency service → 无服务 in the status bar */
    RT_SCENES
} rt_scene_t;

extern int  rt_scene;
extern long rt_now;                   /* what time() returns (device-local labelled UTC) */
extern int  rt_refreshes;             /* data_refresh() calls so far                     */
extern int  rt_exec_calls;            /* ui_exec_self() calls                            */
extern ui_launch_t rt_exec_last;      /* what the last one asked for                     */
extern int  rt_apn_calls;             /* netinfo_apn_use() calls                         */
extern char rt_apn_last[24];
extern int  rt_system_calls;
extern char rt_system_last[256];

const char *rt_scene_name(int scene);

#endif
