/*
 * estimate.c - 电池预估时间，见 estimate.h 和 manager docs/battery-estimate.md。
 *
 * SPDX-License-Identifier: MIT
 */
#include "estimate.h"
#include "json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double est_average(const est_sample_t *s, int n, int *ok)
{
    int start, pos, k;
    double sum = 0, w = 0;

    *ok = n > 0;
    if (n <= 0) return 0;
    pos = s[n - 1].ua >= 0;
    start = n - 1;
    while (start > 0) {
        const est_sample_t *p = &s[start - 1];
        if (p->t < s[n - 1].t - EST_WINDOW_S || (p->ua >= 0) != pos || p->online != s[n - 1].online) break;
        start--;
    }
    for (k = start; k < n - 1; k++) {
        double dt = s[k + 1].t - s[k].t;
        sum += (double)s[k].ua * dt;
        w += dt;
    }
    return w > 0 ? sum / w : (double)s[n - 1].ua;
}

est_t est_compute(const est_input_t *in)
{
    est_t r = { EST_UNKNOWN, 0 };
    int ok;
    double avg = est_average(in->s, in->n, &ok);
    int online;

    if (!ok) return r;
    online = in->s[in->n - 1].online;
    if (in->paused_at_limit) { r.kind = EST_PAUSED; return r; }
    if (online && in->soc >= in->target_pct && (avg >= 0 || fabs(avg) < EST_MIN_UA)) { r.kind = EST_REACHED; return r; }
    if (fabs(avg) < EST_MIN_UA) return r;
    if (avg > 0) {
        double need;
        if (in->full_uah == EST_NONE) return r;
        need = (double)in->full_uah * (in->target_pct - in->soc) / 100.0;
        r.kind = EST_CHARGING;
        r.minutes = (long)floor(need / avg * 60.0 + 0.5);
    } else {
        double left;
        if (in->counter_uah != EST_NONE) left = (double)in->counter_uah;
        else if (in->full_uah != EST_NONE) left = (double)in->full_uah * in->soc / 100.0;
        else return r;
        r.kind = EST_DISCHARGING;
        r.minutes = (long)floor(left / -avg * 60.0 + 0.5);
    }
    return r;
}

static void dur(long m, char *out, size_t cap)
{
    if (m < 60) snprintf(out, cap, "%ld 分钟", m);
    else if (m % 60 == 0) snprintf(out, cap, "%ld 小时", m / 60);
    else snprintf(out, cap, "%ld 小时 %ld 分", m / 60, m % 60);
}

void est_text(est_t e, int target_pct, char *out, size_t cap)
{
    char d[48];

    switch (e.kind) {
    case EST_CHARGING:
        dur(e.minutes, d, sizeof d);
        if (target_pct >= 100) snprintf(out, cap, "约 %s充满", d);
        else snprintf(out, cap, "约 %s充到 %d%%", d, target_pct);
        break;
    case EST_DISCHARGING:
        dur(e.minutes, d, sizeof d);
        snprintf(out, cap, "约可用 %s", d);
        break;
    case EST_REACHED:
        if (target_pct >= 100) snprintf(out, cap, "已充满");
        else snprintf(out, cap, "已到上限 %d%%", target_pct);
        break;
    case EST_PAUSED:
        snprintf(out, cap, "已到上限，暂停充电");
        break;
    default:
        snprintf(out, cap, "—");
    }
}

int est_push(est_sample_t *buf, int n, est_sample_t s)
{
    int drop = 0;

    if (n >= EST_MAX_SAMPLES) {
        memmove(buf, buf + 1, sizeof *buf * (size_t)(n - 1));
        n--;
    }
    buf[n++] = s;
    while (drop < n - 1 && buf[drop].t < s.t - EST_WINDOW_S) drop++;
    if (drop) {
        memmove(buf, buf + drop, sizeof *buf * (size_t)(n - drop));
        n -= drop;
    }
    return n;
}

static long long read_ll(const char *root, const char *name)
{
    char path[256], buf[32], *end;
    FILE *fp;
    long long v;

    snprintf(path, sizeof path, "%s/battery/%s", root, name);
    if (!(fp = fopen(path, "r"))) return EST_NONE;
    if (!fgets(buf, sizeof buf, fp)) { fclose(fp); return EST_NONE; }
    fclose(fp);
    v = strtoll(buf, &end, 10);
    if (end == buf || (*end && *end != '\n')) return EST_NONE;
    return v;
}

void est_read_capacity(const char *root, long long *full_uah, long long *counter_uah)
{
    *full_uah = read_ll(root, "charge_full");
    *counter_uah = read_ll(root, "charge_counter");
}

static int jbool(const char *obj, const char *key)
{
    char v[8];
    return json_get(obj, key, v, sizeof v) && !strcmp(v, "true");
}

void est_parse_charge_control(const char *data, int charger_connect, int *target_pct, int *paused)
{
    int on = data && jbool(data, "charge_limit_enabled");
    long lim = on ? json_get_int(data, "charge_limit", 100) : 100;

    if (lim < 50 || lim > 100) lim = 100;
    *target_pct = (int)lim;
    *paused = on && jbool(data, "charging_stopped") && !jbool(data, "manual_override") && charger_connect;
}
