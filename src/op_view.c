/*
 * op_view.c - see op_view.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "op_view.h"
#include "json.h"
#include "lang.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void str(const char *obj, const char *key, char *out, size_t cap)
{
    if (!json_get(obj, key, out, cap) || !strcmp(out, "null")) out[0] = 0;
}

/* datad sends every sentence twice: <key>_zh and <key>_en */
static void text(const char *obj, const char *key, char *out, size_t cap)
{
    char k[32];
    if (lang_is_en()) {
        snprintf(k, sizeof k, "%s_en", key);
        str(obj, k, out, cap);
        if (out[0]) return;
    }
    snprintf(k, sizeof k, "%s_zh", key);
    str(obj, k, out, cap);
}

static int flag(const char *obj, const char *key)
{
    char v[8];
    return json_get(obj, key, v, sizeof v) && !strcmp(v, "true");
}

static int mark_of(const char *v);

int op_phase_final(const char *p)
{
    return p && p[0] && strcmp(p, "accepted") && strcmp(p, "applying") &&
           strcmp(p, "verifying") && strcmp(p, "rolling_back");
}

static void item(const char *o, op_item_t *it)
{
    static char steps[1024], st[256], undo[512];
    char v[24];
    const char *p;
    int i;

    memset(it, 0, sizeof *it);
    if (!o || o[0] != '{') return;
    str(o, "op_id", it->op_id, sizeof it->op_id);
    str(o, "phase", it->phase, sizeof it->phase);
    if (!it->op_id[0] || !it->phase[0]) return;
    it->have = 1;
    str(o, "action", it->action, sizeof it->action);
    str(o, "item", it->item, sizeof it->item);
    str(o, "reason", it->reason, sizeof it->reason);
    str(o, "mark", v, sizeof v);
    it->mark = mark_of(v);
    str(o, "stay", v, sizeof v);
    it->stay = !strcmp(v, "brief") ? OP_STAY_BRIEF : !strcmp(v, "sticky") ? OP_STAY_STICKY :
               !strcmp(v, "alert") ? OP_STAY_ALERT : !strcmp(v, "none") ? OP_STAY_NONE : OP_STAY_LIVE;
    text(o, "say", it->say, sizeof it->say);
    text(o, "next", it->next, sizeof it->next);
    text(o, "note", it->note, sizeof it->note);
    text(o, "what", it->what, sizeof it->what);
    text(o, "source", it->source, sizeof it->source);
    text(o, "old", it->old_v, sizeof it->old_v);
    text(o, "target", it->target, sizeof it->target);
    text(o, "rollback_to", it->rollback_to, sizeof it->rollback_to);
    text(o, "readback", it->readback, sizeof it->readback);
    str(o, "rollback_to", it->rollback_to_raw, sizeof it->rollback_to_raw);
    text(o, "revert_label", it->revert_label, sizeof it->revert_label);
    text(o, "keep_label", it->keep_label, sizeof it->keep_label);
    it->can_revert = flag(o, "can_revert");
    it->can_keep = flag(o, "can_keep");
    it->remaining_ms = json_get_int(o, "remaining_ms", -1);
    it->acked = flag(o, "acked");
    it->needs_ack = flag(o, "needs_ack");
    if (json_get(o, "steps", steps, sizeof steps) && steps[0] == '[')
        for (p = steps, i = 0; i < OP_STEPS && (p = json_arr_next(p, st, sizeof st)) != NULL; i++) {
            const char *k = lang_is_en() ? "en" : "zh";
            str(st, k, it->step[i], sizeof it->step[i]);
            it->step_done[i] = flag(st, "done");
        }
    it->undo_ok = -1;
    if (json_get(o, "undo", undo, sizeof undo) && undo[0] == '{') {
        it->undo_ok = flag(undo, "ok");
        text(undo, "label", it->undo_label, sizeof it->undo_label);
        text(undo, "why", it->undo_why, sizeof it->undo_why);
    }
}

int op_view_parse(const char *json, op_view_t *v)
{
    static char a[4096];
    memset(v, 0, sizeof *v);
    if (!json || json[0] != '{') return 0;
    v->rollback_enabled = flag(json, "rollback_enabled");
    str(json, "notice", v->notice, sizeof v->notice);
    if (json_get(json, "active", a, sizeof a)) item(a, &v->active);
    if (json_get(json, "last", a, sizeof a)) item(a, &v->last);
    return 1;
}

void op_fill_clock(char *out, size_t n, const char *line, long ms)
{
    char t[24];
    const char *at;
    long s;

    if (!out || !n) return;
    out[0] = 0;
    if (!line) return;
    s = ms > 0 ? (ms + 999) / 1000 : 0;
    if (s > 99 * 60) s = 99 * 60;
    snprintf(t, sizeof t, "%ld:%02ld", s / 60, s % 60);
    at = strstr(line, "{t}");
    if (!at) { snprintf(out, n, "%s", line); return; }
    snprintf(out, n, "%.*s%s%s", (int)(at - line), line, t, at + 3);
}

static int mark_of(const char *v)
{
    return !strcmp(v, "ok") ? OP_MARK_OK : !strcmp(v, "warn") ? OP_MARK_WARN :
           !strcmp(v, "bad") ? OP_MARK_BAD : OP_MARK_NONE;
}

int op_log_parse(const char *reply, op_log_t *out, int max)
{
    static char res[49152], arr[49152], e[4096], u[1024], req[512];
    const char *p;
    char t[32], v[16];
    int n = 0;

    if (!reply || !json_get(reply, "result", res, sizeof res) || res[0] != '{') return -1;
    if (!json_get(res, "entries", arr, sizeof arr) || arr[0] != '[') return -1;
    for (p = arr; n < max && (p = json_arr_next(p, e, sizeof e)) != NULL; ) {
        op_log_t *o = &out[n];
        if (e[0] != '{' || flag(e, "hide")) continue;
        memset(o, 0, sizeof *o);
        text(e, "what", o->what, sizeof o->what);
        text(e, "change", o->change, sizeof o->change);
        text(e, "result", o->result, sizeof o->result);
        text(e, "source", o->source, sizeof o->source);
        str(e, "op_id", o->op_id, sizeof o->op_id);
        str(e, "mark", v, sizeof v);
        o->mark = mark_of(v);
        str(e, "t", t, sizeof t);   /* "2026-10-03 14:32:00" */
        if (strlen(t) >= 16) snprintf(o->when, sizeof o->when, "%.5s %.5s", t + 5, t + 11);
        if (json_get(e, "undo_view", u, sizeof u) && u[0] == '{') {
            o->undo_have = 1;
            o->undo_ok = flag(u, "ok");
            text(u, "label", o->undo_label, sizeof o->undo_label);
            text(u, "why", o->undo_why, sizeof o->undo_why);
            if (json_get(u, "request", req, sizeof req) && req[0] == '{') {
                str(req, "action", o->undo_action, sizeof o->undo_action);
                if (!json_get(req, "params", o->undo_params, sizeof o->undo_params) || o->undo_params[0] != '{')
                    o->undo_params[0] = 0;
            }
            if (!o->undo_action[0] || !o->undo_params[0]) o->undo_ok = 0;
        }
        n++;
    }
    return n;
}

/* DD14 display names; datad's journal rows carry them, owners don't */
static const char *const k_src[][3] = {
    { "screen", "触屏", "Screen" },     { "legacy", "触屏", "Screen" },
    { "web", "网页", "Web" },           { "scenario", "情景", "Scene" },
    { "scheduler", "定时任务", "Schedule" }, { "auto", "自动", "Auto" },
    { "guard", "自动恢复", "Auto-recovery" },
};

int op_owner_parse(const char *reply, const char *item, op_owner_t *o)
{
    static char res[49152], own[4096], it[1024];
    char t[32], src[24];

    memset(o, 0, sizeof *o);
    if (!reply || !json_get(reply, "result", res, sizeof res) || res[0] != '{') return -1;
    if (!json_get(res, "owners", own, sizeof own) || own[0] != '{') return 0;
    if (!json_get(own, item, it, sizeof it) || it[0] != '{') return 0;
    str(it, "t", t, sizeof t);          /* "2026-10-03 14:32:07", device clock */
    if (strlen(t) < 16) return 0;
    snprintf(o->when, sizeof o->when, "%.5s %.5s", t + 5, t + 11);
    str(it, "source", src, sizeof src);
    snprintf(o->source, sizeof o->source, "%s", src);
    for (size_t i = 0; i < sizeof k_src / sizeof k_src[0]; i++)
        if (!strcmp(src, k_src[i][0])) snprintf(o->source, sizeof o->source, "%s", k_src[i][lang_is_en() ? 2 : 1]);
    str(it, "op_id", o->op_id, sizeof o->op_id);
    o->have = 1;
    return 1;
}
