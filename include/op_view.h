/*
 * op_view.h - parse the "op" object zwrt-datad sends on /v2/screen (E4 write
 * transactions, data-service docs/STATE_V2.md §12, V2-34–V2-38): the change
 * in progress and the last one that ended. Every sentence is datad's (中文 or
 * its *_en sibling in English); the screen only lays it out. No LVGL here.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60_OP_VIEW_H
#define U60_OP_VIEW_H

#include <stddef.h>

enum { OP_MARK_NONE = 0, OP_MARK_OK, OP_MARK_WARN, OP_MARK_BAD };
/* stay: how long a result stays (DD16) */
enum { OP_STAY_LIVE = 0, OP_STAY_BRIEF, OP_STAY_STICKY, OP_STAY_ALERT, OP_STAY_NONE };
enum { OP_STEPS = 3 };

typedef struct {
    int  have;
    char op_id[72];
    char action[40];
    char item[32];
    char phase[16];         /* accepted … rolling_back, or a final phase    */
    char reason[20];
    int  mark, stay;
    char say[96];           /* 正在确认 / 没通 · 已退回自动 …                */
    char next[96];          /* the countdown line, "{t}" = m:ss; "" none    */
    char note[48];          /* 重启过 · 重新确认, "" none                   */
    char what[32];          /* 制式                                         */
    char source[24];        /* 触屏 / 网页 / 情景 …                          */
    char old_v[48], target[48], rollback_to[48], readback[48];
    char rollback_to_raw[40];   /* the value itself, for 再试一次退回       */
    char revert_label[64], keep_label[64];
    int  can_revert, can_keep;
    long remaining_ms;      /* -1 when not counting                         */
    char step[OP_STEPS][32];
    int  step_done[OP_STEPS];
    int  undo_ok;           /* -1: no undo (in progress)                    */
    char undo_label[16], undo_why[48];
    int  acked, needs_ack;
} op_item_t;

typedef struct {
    int rollback_enabled;
    op_item_t active, last;
    char notice[24];        /* "rollback_on": the one-time 自动退回已打开 (DD18,
                             * until op.notice_ack from here or the web); "" none */
} op_view_t;

/* `json` = the "op" object text (screen_feed_op()); "" or garbage → 0 and a
 * view with nothing in it. */
int op_view_parse(const char *json, op_view_t *v);

/* ---- 改动记录: datad's journal.list (STATE_V2.md V2-41) ---- */
#define OP_LOG_MAX 20
typedef struct {
    char what[40];          /* 制式 / 移动数据 / eSIM …                      */
    char change[96];        /* 自动 → 只用 4G, "" none                       */
    char result[96];        /* 已切到只用 4G / 已改 / 情景跳过 ×5（…）         */
    char when[16];          /* 10-03 14:32 (device clock = local time)      */
    char source[24];
    int  mark;              /* OP_MARK_*                                    */
    int  undo_have, undo_ok;
    char undo_label[16], undo_why[48];
    char undo_action[40], undo_params[160];   /* the write to send for 撤销 */
    char op_id[72];         /* "" for writes that are not transactions      */
} op_log_t;

/* `reply` = the whole /control reply JSON ({"ok":true,"result":{"entries":…}}).
 * Fills up to `max` entries that are not hidden, newest first. Returns how
 * many, -1 when it is not a journal.list reply. */
int op_log_parse(const char *reply, op_log_t *out, int max);

/* 「上次改动 10-03 14:32 · 网页 ›」 under a setting (DD5): who last wrote
 * `item` through datad and when, from the same reply's "owners" (D16).
 * owners carry only the raw source; the display name is made here (DD14). */
typedef struct {
    int  have;
    char when[16];          /* 10-03 14:32                                   */
    char source[24];        /* 触屏 / 网页 / 情景 …                           */
    char op_id[72];         /* the change log row to open, "" none           */
} op_owner_t;

/* Returns 1 and fills `o` when the reply has an owner for `item`, 0 when it
 * has none (o->have = 0), -1 when it is not a journal.list reply. */
int op_owner_parse(const char *reply, const char *item, op_owner_t *o);

/* phase is a final one (confirmed, unverified, rolled_back, …) */
int op_phase_final(const char *phase);

/* "{t}" in `line` replaced by m:ss of `ms` (rounded up) into `out`. */
void op_fill_clock(char *out, size_t n, const char *line, long ms);

#endif
