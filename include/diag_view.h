/*
 * diag_view.h - zte-agent's network diagnosis run (GET/POST /api/diagnose,
 * manager zte-agent/src/deep_diag.rs; docs/designs/slow-diagnosis.md §4.2,
 * §12.3, §12.5) as the 网络诊断 page shows it. Parsing and the words for
 * layers and levels only: no I/O, no LVGL (unit test: tests/diag_view_test.c).
 *
 * Every text the agent sends in both languages (detail / detail_en, text /
 * text_en, action / action_en) is picked here by lang_is_en(), so the struct
 * holds one language, like net_view.h.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60_DIAG_VIEW_H
#define U60_DIAG_VIEW_H

#define DG_LAYERS_MAX 8

typedef enum { DG_PENDING, DG_RUNNING, DG_OK, DG_WARN, DG_BAD, DG_NA, DG_INFO } dg_level_t;

typedef struct {
    char       id[12];       /* wifi signal limit link crowd proxy / speed */
    dg_level_t level;
    char       detail[192];  /* the value side; for na, why it can't tell     */
    int        counted;      /* 0 = shown, but not in the step count          */
} dg_layer_t;

/* DG_NONE = nothing parsed yet; DG_IDLE = the agent has no run to show
 * (none, or the last one is older than 10 minutes). */
typedef enum { DG_NONE, DG_IDLE, DG_WAITING, DG_RUN, DG_DONE } dg_state_t;

typedef struct {
    dg_state_t state;
    long  id;
    char  waiting_for[16];   /* scan / register / speedtest while waiting    */
    long  asked_at, started_at, finished_at;   /* device clock, 0 = null      */
    int   step, steps;
    dg_layer_t layer[DG_LAYERS_MAX];
    int   n;
    int   has_main;
    char  main_layer[12];    /* "" = nothing found (all good)                 */
    int   main_has_level;
    dg_level_t main_level;
    char  main_text[96];
    char  main_action[160];
    char  action_to[16];     /* placement / proxy / ""                        */
    int   more;              /* other layers at warn or bad                   */
    int   feedback;          /* -1 = not given, 0 = wrong, 1 = right          */
    int   has_speed;
    dg_layer_t speed;
    long  age_s;             /* seconds since finished, -1 = not finished     */
} diag_run_t;

/* Parse the Run object (the "data" of the agent's reply). Returns 1 on a run
 * or on {"state":"idle"}; 0 (and *r = DG_NONE) when it is neither. */
int diag_view_parse(const char *data_json, diag_run_t *r);

/* The row name of a layer id (TR'd): Wi-Fi, 信号, 限速, 蜂窝链路, 基站负载,
 * 代理, 速度; an unknown id is shown as it is. */
const char *diag_layer_name(const char *id);

/* The status word of a level (TR'd): 正常 / 疑点 / 差 / 测不了 / 等待 /
 * 测试中…; "" for info (the value is shown plain). */
const char *diag_level_word(dg_level_t l);

/* The mark in front of the word: ● ▲ ■, ● for na (grey), "" otherwise. */
const char *diag_level_mark(dg_level_t l);

/* The layer with this id in the run, NULL when it has none. */
const dg_layer_t *diag_find(const diag_run_t *r, const char *id);

#endif
