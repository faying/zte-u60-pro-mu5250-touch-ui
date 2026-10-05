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
    RT_BANDLOCK,       /* SA locked to n78, LTE to B3: chips still list every band      */
    RT_DATAD_SILENT,   /* datad answered, then went quiet: banner, dimmed home card      */
    RT_OLD_DATAD,      /* datad too old for /v2/screen: the home card says so            */
    RT_STALL,          /* connected, sending, nothing back → 连上了但不通 (datad stall)    */
    RT_MF_BACKUP,      /* manual-first notice: the hand-picked node is down, on the backup */
    RT_MF_ALLDOWN,     /* manual-first notice: the node and its backup both down           */
    RT_DIAG_RUNNING,   /* 网络诊断 open, 3 of the layers done                            */
    RT_DIAG_RESULT,    /* 网络诊断 done: main cause, can't-tell rows, a speed row          */
    RT_PLACEMENT,      /* 摆放模式: SINR best 18.0, now 14.5                              */
    RT_OP_BUSY,        /* E4: a write turned down, another change in progress (409)      */
    RT_OP_STUCK,       /* E4: datad answers but its executor is stuck (exec_age ≥ 20 s)  */
    RT_OP_VERIFYING,   /* E4: network mode changed from the web, checking, revert in 1:42 */
    RT_OP_ROLLBACK_OFF,/* E4: the same with automatic revert off (DD6)                    */
    RT_OP_ROLLED_BACK, /* E4: result: no data, back to Auto, not acknowledged             */
    RT_OP_ROLLBACK_FAILED, /* E4: result: the revert did not come through either (DD9)   */
    RT_OP_NOT_APPLIED, /* E4: result: the modem ignored the write (DD17)                 */
    RT_JOURNAL,        /* E4: 系统 › 改动记录 with one of each kind of line (DD5, DD11)   */
    RT_JOURNAL_EMPTY,  /* E4: 改动记录 with nothing in it                               */
    RT_OP_NOTICE,      /* E4: auto revert just turned on, nobody acked (DD18)           */
    RT_HOME_EXIT,      /* E3: home exit, abroad: CN media + rest on (1:32 left), direct path */
    RT_HOME_FALLBACK,  /* E3: home not answering: rest on its fallback, ▲ on the exit line */
    RT_HOME_BLOCKED,   /* E3: proxy in Global mode: the switches grey out, reason below */
    RT_HOME_NOCOUNTRY, /* E3: country unknown (media / rest hidden), home IP list empty */
    RT_SCENES
} rt_scene_t;

extern int  rt_scene;
extern long rt_now;                   /* what time() returns (device-local labelled UTC) */
extern int  rt_refreshes;             /* data_refresh() calls so far                     */
extern int  rt_busy;                  /* speedtest_running(), for the busy-switch tests  */
extern int  rt_exec_calls;            /* ui_exec_self() calls                            */
extern ui_launch_t rt_exec_last;      /* what the last one asked for                     */
extern int  rt_apn_calls;             /* netinfo_apn_use() calls                         */
extern char rt_apn_last[24];
extern int  rt_system_calls;
/* data_control() calls (E4): how many, and the last action and params */
extern int  rt_control_calls;
extern char rt_control_last[48], rt_control_params[160];
int rt_scene_is_op(void);
extern int  rt_undo_calls;
extern char rt_undo_last[48];   /* one of the RT_OP_VERIFYING … RT_OP_NOT_APPLIED scenes */
extern char rt_system_last[256];
extern int  rt_place_t0;              /* rt_refreshes when 摆放模式 was opened (its SINR series) */
extern int  rt_diag_err;              /* diagnose_agent_err(): the agent not answering */
extern int  rt_diag_starts, rt_diag_speeds, rt_diag_feedback;   /* diagnose_* POSTs */

const char *rt_scene_name(int scene);

#endif
