/*
 * net_view.h - what the home signal card and the status bar show, as
 * zwrt-datad's GET /v2/screen "net" sends it. The rules (carrier assembly,
 * the verdict, the badges, roaming, the SIM logo) live in datad's screen.rs
 * since 2026-09-26 (manager docs/screen-logic-move.md); the screen only
 * parses and draws. They were checked field for field against the rules this
 * file used to hold (touch-ui tag parity-net-v1).
 *
 * Still on the screen on purpose: the 15 s hold before the headline changes
 * and the "已 N 分钟" no-service timer — they are about what this screen has
 * shown and when.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60_NET_VIEW_H
#define U60_NET_VIEW_H

#include "ui_logic.h"

#define NV_CA_MAX 5

typedef struct {
    char   kind;           /* 'n' = NR, 'B' = LTE                                  */
    int    band;           /* 0 = not in the carrier record (serving NR cell)      */
    int    pci, bw, active;
    long   arfcn;
    char   rsrp[12], rsrq[12], sinr[12];   /* display text: %.0f / %.0f / %.1f     */
    char   label[16];      /* row label: n78 / B3 / net.nr_band as-is / "-"        */
    char   label_short[16];/* summary label: n78 / B3                              */
    ui_net_tone_t sinr_tone; /* row colour of the SINR                             */
} nv_carrier_t;

typedef struct {
    nv_carrier_t ca[NV_CA_MAX];
    int  ca_n;
    int  act_n;            /* active carriers                                      */
    int  roam_known, roam; /* net.roaming given / not "Home"                        */
    ui_net_story_t story;  /* the verdict for this snapshot (before the 15 s hold)  */
    int  nosvc;            /* headline is 无服务 / 只能紧急呼叫                      */
    int  sim_usable;
    int  other;            /* roaming on a different network than the SIM's own    */
    char logo[24];         /* SIM operator logo slug, "" = none                     */
    char fine[32];         /* 5G SA / 5G NSA · 4G 锚点 / 4G LTE-A …                */
    char name[48];         /* operator name, or 未注册                              */
    char where[64];        /* 漫游到X / 漫游 / 本地 / "" (no service or unknown)     */
    char ca_val[48];       /* 激活 a/c · N 载波聚合 · 单载波 · 无聚合 · 没连上基站 · — */
    char ca_sub[160];      /* ↓ list   ↑ primary, or the 3G/2G band, or ""          */
    int  bars_tier;        /* status-bar dot colour: 2 / 1 / 0, -1 = none           */
    char mode_word[32];    /* radio-mode preference: 自动 / 只用 4G …, "-" unknown   */
    int  mode_auto;        /* one of the automatic values                           */
} net_view_t;

/* Parse /v2/screen's "net" object. Returns 1 when it carried a verdict. */
int net_view_parse(const char *net_json, net_view_t *v);

/* A view that only says datad has not given one (yet): the card shows
 * `headline` in the neutral tone and `why` on its top line, nothing else. */
void net_view_placeholder(net_view_t *v, const char *headline, const char *why);

/* net.lteca records (11 fields, or the 5-field form some firmware sends):
 * PCI, band, EARFCN, bandwidth only. kind = 'n' or 'B'. Returns how many
 * (≤ max). Still used by the 信令 page's LTE serving-cell block. */
int nv_parse_ca(const char *s, nv_carrier_t *out, int max, char kind);

#endif
