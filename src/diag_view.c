/*
 * diag_view.c - parse zte-agent's diagnosis run (see diag_view.h).
 *
 * SPDX-License-Identifier: MIT
 */
#include "diag_view.h"
#include "json.h"
#include "lang.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void str(const char *obj, const char *key, char *out, size_t cap)
{
    if (!json_get(obj, key, out, cap) || !strcmp(out, "null")) out[0] = 0;
}

/* <key>_en in English when it is there and not empty, else <key> */
static void text(const char *obj, const char *key, char *out, size_t cap)
{
    if (lang_is_en()) {
        char k[24];
        snprintf(k, sizeof k, "%s_en", key);
        str(obj, k, out, cap);
        if (out[0]) return;
    }
    str(obj, key, out, cap);
}

/* a number that may be null: def then */
static long num(const char *obj, const char *key, long def)
{
    char v[24];
    if (!json_get(obj, key, v, sizeof v) || !strcmp(v, "null") || !v[0]) return def;
    return strtol(v, NULL, 10);
}

static dg_level_t level(const char *s)
{
    static const char *const names[] = { "pending", "running", "ok", "warn", "bad", "na", "info" };
    for (int i = 0; i < (int)(sizeof names / sizeof *names); i++)
        if (!strcmp(s, names[i])) return (dg_level_t)i;
    return DG_NA;   /* a level from a later agent: "can't tell", never good */
}

static void layer(const char *o, dg_layer_t *l)
{
    char v[16];
    memset(l, 0, sizeof *l);
    str(o, "id", l->id, sizeof l->id);
    str(o, "level", v, sizeof v);
    l->level = level(v);
    text(o, "detail", l->detail, sizeof l->detail);
    l->counted = !(json_get(o, "counted", v, sizeof v) && !strcmp(v, "false"));
}

int diag_view_parse(const char *d, diag_run_t *r)
{
    static char arr[4096], item[768], obj[1024];
    char v[24];

    memset(r, 0, sizeof *r);
    r->feedback = -1;
    r->age_s = -1;
    if (!d || !json_get(d, "state", v, sizeof v)) return 0;
    if (!strcmp(v, "idle")) { r->state = DG_IDLE; return 1; }
    if (!strcmp(v, "waiting")) r->state = DG_WAITING;
    else if (!strcmp(v, "running")) r->state = DG_RUN;
    else if (!strcmp(v, "done")) r->state = DG_DONE;
    else return 0;

    r->id = num(d, "id", 0);
    str(d, "waiting_for", r->waiting_for, sizeof r->waiting_for);
    r->asked_at = num(d, "asked_at", 0);
    r->started_at = num(d, "started_at", 0);
    r->finished_at = num(d, "finished_at", 0);
    r->step = (int)num(d, "step", 0);
    r->steps = (int)num(d, "steps", 0);
    r->age_s = num(d, "age_s", -1);
    if (json_get(d, "layers", arr, sizeof arr) && arr[0] == '[') {
        const char *p = arr;
        while (r->n < DG_LAYERS_MAX && (p = json_arr_next(p, item, sizeof item)) != NULL)
            if (item[0] == '{') layer(item, &r->layer[r->n++]);
    }
    if (json_get(d, "main", obj, sizeof obj) && obj[0] == '{') {
        r->has_main = 1;
        str(obj, "layer", r->main_layer, sizeof r->main_layer);
        str(obj, "level", v, sizeof v);
        if (v[0]) { r->main_has_level = 1; r->main_level = level(v); }
        text(obj, "text", r->main_text, sizeof r->main_text);
        text(obj, "action", r->main_action, sizeof r->main_action);
        str(obj, "action_to", r->action_to, sizeof r->action_to);
        r->more = (int)num(obj, "more", 0);
    }
    if (json_get(d, "feedback", v, sizeof v))
        r->feedback = !strcmp(v, "true") ? 1 : !strcmp(v, "false") ? 0 : -1;
    if (json_get(d, "speed", obj, sizeof obj) && obj[0] == '{') {
        r->has_speed = 1;
        layer(obj, &r->speed);
    }
    return 1;
}

const char *diag_layer_name(const char *id)
{
    if (!strcmp(id, "wifi"))   return "Wi-Fi";
    if (!strcmp(id, "signal")) return TR("信号");
    if (!strcmp(id, "limit"))  return TR("限速");
    if (!strcmp(id, "link"))   return TR("蜂窝链路");
    if (!strcmp(id, "crowd"))  return TR("基站负载");
    if (!strcmp(id, "speed"))  return TR("速度");
    return id;
}

const char *diag_level_word(dg_level_t l)
{
    switch (l) {
    case DG_PENDING: return TR("等待");
    case DG_RUNNING: return TR("测试中…");
    case DG_OK:      return TR("正常");
    case DG_WARN:    return TR("疑点");
    case DG_BAD:     return TRC("诊断", "差");
    case DG_NA:      return TR("测不了");
    default:         return "";
    }
}

const char *diag_level_mark(dg_level_t l)
{
    switch (l) {
    case DG_OK: case DG_NA: return "●";
    case DG_WARN:           return "▲";
    case DG_BAD:            return "■";
    default:                return "";
    }
}

const dg_layer_t *diag_find(const diag_run_t *r, const char *id)
{
    for (int i = 0; i < r->n; i++)
        if (!strcmp(r->layer[i].id, id)) return &r->layer[i];
    return NULL;
}
