/*
 * estimate.h - 电池预估时间的文案。预估本身由 zte-agent 算（battery_eta.rs，
 * 规则见 manager docs/battery-estimate.md），经 GET /api/screen 的 battery 给过来；
 * 这里只把它解析出来、写成一句话。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60_ESTIMATE_H
#define U60_ESTIMATE_H

#include <stddef.h>

enum { EST_UNKNOWN = 0, EST_CHARGING, EST_DISCHARGING, EST_REACHED, EST_PAUSED };

typedef struct {
    int  kind;
    long minutes;  /* 只有 EST_CHARGING / EST_DISCHARGING 有效 */
} est_t;

/* 首页那一句，UTF-8，不用 %f */
void est_text(est_t e, int target_pct, char *out, size_t cap);

/* 解析 /api/screen 的 battery 对象（或 /api/battery 的 estimate）。
 * state 不是 "ok"、字段缺失或看不懂时 *e = EST_UNKNOWN；*target_pct 读不到给 100。
 * 返回 1 = state 是 ok。 */
int est_from_report(const char *obj, est_t *e, int *target_pct);

#endif
