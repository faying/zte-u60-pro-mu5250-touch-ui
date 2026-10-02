/*
 * estimate.c - 电池预估时间的文案和 agent 结果解析，见 estimate.h。
 *
 * SPDX-License-Identifier: MIT
 */
#include "estimate.h"
#include "json.h"
#include "lang.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void dur(long m, char *out, size_t cap)
{
    if (m < 60) snprintf(out, cap, TR("%ld 分钟"), m);
    else if (m % 60 == 0) snprintf(out, cap, TR("%ld 小时"), m / 60);
    else snprintf(out, cap, TR("%ld 小时 %ld 分"), m / 60, m % 60);
}

void est_text(est_t e, int target_pct, char *out, size_t cap)
{
    char d[48];

    switch (e.kind) {
    case EST_CHARGING:
        dur(e.minutes, d, sizeof d);
        if (target_pct >= 100) snprintf(out, cap, TR("约 %s充满"), d);
        else snprintf(out, cap, TR("约 %s充到 %d%%"), d, target_pct);
        break;
    case EST_DISCHARGING:
        dur(e.minutes, d, sizeof d);
        snprintf(out, cap, TR("约可用 %s"), d);
        break;
    case EST_REACHED:
        if (target_pct >= 100) snprintf(out, cap, "%s", TR("已充满"));
        else snprintf(out, cap, TR("已到上限 %d%%"), target_pct);
        break;
    case EST_PAUSED:
        snprintf(out, cap, "%s", TR("已到上限，暂停充电"));
        break;
    default:
        snprintf(out, cap, "—");
    }
}

static int kind_of(const char *name)
{
    if (!strcmp(name, "charging_eta"))    return EST_CHARGING;
    if (!strcmp(name, "discharging_eta")) return EST_DISCHARGING;
    if (!strcmp(name, "reached_target"))  return EST_REACHED;
    if (!strcmp(name, "paused_at_limit")) return EST_PAUSED;
    return EST_UNKNOWN;
}

int est_from_report(const char *obj, est_t *e, int *target_pct)
{
    char state[16], kind[24];
    long t;

    e->kind = EST_UNKNOWN;
    e->minutes = 0;
    *target_pct = 100;
    if (!obj) return 0;
    t = json_get_int(obj, "target_pct", 100);
    if (t >= 1 && t <= 100) *target_pct = (int)t;
    if (!json_get(obj, "state", state, sizeof state) || strcmp(state, "ok")) return 0;
    if (!json_get(obj, "kind", kind, sizeof kind)) return 1;
    e->kind = kind_of(kind);
    if (e->kind == EST_CHARGING || e->kind == EST_DISCHARGING) {
        e->minutes = json_get_int(obj, "minutes", -1);
        if (e->minutes < 0) e->kind = EST_UNKNOWN;          /* null / missing */
    }
    return 1;
}
