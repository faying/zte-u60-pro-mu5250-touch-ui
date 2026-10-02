/*
 * alerts.c - 触屏上的告警页数据：读 zte-agent 的 /api/alerts、全部标已读。
 *
 * 告警详情要登录（评审决定 F1：免登录的 /api/public/status 只给未读数），
 * 所以和 esim.c 一样用 agent 的密码换 token；HTTP 写法也照它（HTTP/1.0，
 * 不解分块，401 就重登一次）。类别的中文说明和管理网页 web/src/lib/alerts.ts
 * 保持一致，改一边要改另一边。
 *
 * SPDX-License-Identifier: MIT
 */
#include "alerts.h"
#include "agent_client.h"
#include "json.h"
#include "lang.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define AL_TTL_MS    10000
#define AL_RESP_MAX  32768

static alert_item_t s_items[ALERTS_MAX];
static health_item_t s_hc[HEALTH_MAX];
static int  s_hc_n = -1, s_hc_ok;
static int  s_count, s_unread;
static char s_err[96];
static int  s_was_active;
static long s_poll_ms;
static unsigned s_sig;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 登录、401 重登、密码都在 agent_client.c；正文在本模块自己的缓冲区里，
 * 下一次 al_api() 会覆盖。 */
static int al_api(const char *method, const char *path, const char *json, char **body)
{
    static char resp[AL_RESP_MAX];
    return agent_api(method, path, json, resp, sizeof resp, body);
}


static void json_str(const char *obj, const char *key, char *out, size_t cap)
{
    if (!json_get(obj, key, out, cap) || !strcmp(out, "null")) out[0] = 0;
}

/* Text zte-agent words in both languages: in English its <key>_en sibling
 * (alerts.rs kind_label_en, health.rs label_en/detail_en) when that is there
 * and not empty/null, else the Chinese (an agent from before 2026-10-01). */
static void json_text(const char *obj, const char *key, char *out, size_t cap)
{
    if (lang_is_en()) {
        char k[24];
        snprintf(k, sizeof k, "%s_en", key);
        json_str(obj, k, out, cap);
        if (out[0]) return;
    }
    json_str(obj, key, out, cap);
}

/* events 是对象数组：每个对象先截成独立字符串再取字段（json_get 只认第一层、会往后扫） */
static void parse_events(char *arr)
{
    char *p = strchr(arr, '[');
    int n = 0;

    s_count = 0;
    if (!p) return;
    while (n < ALERTS_MAX && (p = strchr(p, '{')) != NULL) {
        alert_item_t *e = &s_items[n];
        char kind[40], unread[8], save, *q;
        int depth = 0, instr = 0, esc = 0;

        for (q = p; *q; q++) {
            if (instr) {
                if (esc) esc = 0;
                else if (*q == '\\') esc = 1;
                else if (*q == '"') instr = 0;
            } else if (*q == '"') instr = 1;
            else if (*q == '{') depth++;
            else if (*q == '}' && --depth == 0) break;
        }
        if (!*q) break;
        save = q[1];
        q[1] = 0;
        e->seq = json_get_int(p, "seq", 0);
        e->time = json_get_int(p, "time", 0);   /* null → 0 */
        e->uptime = json_get_int(p, "uptime", 0);
        json_str(p, "kind", kind, sizeof kind);
        json_text(p, "label", e->label, sizeof e->label);  /* zte-agent words it (alerts.rs kind_label) */
        json_str(p, "text", e->text, sizeof e->text);
        json_str(p, "unread", unread, sizeof unread);
        e->unread = !strcmp(unread, "true");
        q[1] = save;
        p = q + 1;
        if (!e->label[0]) snprintf(e->label, sizeof e->label, TR("其他告警（%s）"), kind);   /* agent before 9-26 */
        n++;
    }
    s_count = n;
}

/* 数组里的下一个对象：就地截成独立字符串（*save 存被截掉的字符），返回对象开头 */
static char *next_obj(char **pp, char *save)
{
    char *p = strchr(*pp, '{'), *q;
    int depth = 0, instr = 0, esc = 0;

    if (!p) return NULL;
    for (q = p; *q; q++) {
        if (instr) {
            if (esc) esc = 0;
            else if (*q == '\\') esc = 1;
            else if (*q == '"') instr = 0;
        } else if (*q == '"') instr = 1;
        else if (*q == '{') depth++;
        else if (*q == '}' && --depth == 0) break;
    }
    if (!*q) return NULL;
    *save = q[1];
    q[1] = 0;
    *pp = q + 1;
    return p;
}

/* checks: [{level,id,label,detail}]，只留 warn/bad */
static void parse_checks(char *data)
{
    char *p = strstr(data, "\"checks\"");
    char save, *o;

    s_hc_n = 0;
    s_hc_ok = 0;
    if (!p || !(p = strchr(p, '['))) return;
    /* 数组到配对的 ] 为止（后面还有 crashlogs 的对象，不能扫过去） */
    char *end = p;
    {
        int depth = 0, instr = 0, esc = 0;
        for (; *end; end++) {
            if (instr) {
                if (esc) esc = 0;
                else if (*end == '\\') esc = 1;
                else if (*end == '"') instr = 0;
            } else if (*end == '"') instr = 1;
            else if (*end == '[') depth++;
            else if (*end == ']' && --depth == 0) break;
        }
    }
    while ((o = next_obj(&p, &save)) != NULL) {
        char level[8];
        if (o > end) { *p = save; break; }
        json_str(o, "level", level, sizeof level);
        if (!strcmp(level, "ok")) s_hc_ok++;
        else if (s_hc_n < HEALTH_MAX && (!strcmp(level, "warn") || !strcmp(level, "bad"))) {
            health_item_t *h = &s_hc[s_hc_n++];
            h->bad = !strcmp(level, "bad");
            json_str(o, "id", h->id, sizeof h->id);
            json_text(o, "label", h->label, sizeof h->label);
            json_text(o, "detail", h->detail, sizeof h->detail);
        }
        *p = save;
    }
}

static void load_health(void)
{
    static char data[AL_RESP_MAX];
    char *b;
    int code = al_api("GET", "/api/health", NULL, &b);

    if (code != 200 || !b || !json_get(b, "data", data, sizeof data)) {
        if (s_hc_n == -1) s_hc_n = -2;   /* 从没读到过：页面写「读不到」，别一直「读取中」；读到过就留上一次的 */
        return;
    }
    parse_checks(data);
}

static int load(void)
{
    static char data[AL_RESP_MAX];
    char *b;
    int code = al_api("GET", "/api/alerts", NULL, &b);

    if (code == 0) { snprintf(s_err, sizeof s_err, "%s", TR("连不上管理后台")); return 0; }
    if (code == 401) { snprintf(s_err, sizeof s_err, "%s", TR("登录管理后台失败（密码不对？）")); return 0; }
    if (code != 200 || !b || !json_get(b, "data", data, sizeof data)) {
        snprintf(s_err, sizeof s_err, TR("读告警失败（HTTP %d）"), code);
        return 0;
    }
    s_err[0] = 0;
    s_unread = (int)json_get_int(data, "unread", 0);
    parse_events(data);
    return 1;
}

static unsigned fnv(unsigned h, const char *s)
{
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

int alerts_poll(int active)
{
    int just_shown = active && !s_was_active;
    long t = now_ms();
    unsigned h = 2166136261u;
    char nums[48];

    s_was_active = active;
    if (!active) return 0;
    if (!just_shown && s_poll_ms && t - s_poll_ms < AL_TTL_MS) return 0;
    s_poll_ms = t;
    load();
    load_health();
    snprintf(nums, sizeof nums, "%d/%d/%d/%d", s_count, s_unread, s_hc_n, s_hc_ok);
    h = fnv(h, nums);
    h = fnv(h, s_err);
    for (int i = 0; i < s_count; i++) {
        snprintf(nums, sizeof nums, "%ld/%d", s_items[i].seq, s_items[i].unread);
        h = fnv(h, nums);
    }
    for (int i = 0; i < s_hc_n; i++) {
        h = fnv(h, s_hc[i].id);
        h = fnv(h, s_hc[i].detail);
    }
    if (h == s_sig) return 0;
    s_sig = h;
    return 1;
}

int alerts_count(void) { return s_count; }
int alerts_unread(void) { return s_unread; }
const char *alerts_error(void) { return s_err; }

void alerts_get(int index, alert_item_t *out)
{
    if (index < 0 || index >= s_count) { memset(out, 0, sizeof *out); return; }
    *out = s_items[index];
}

int health_count(void) { return s_hc_n; }
int health_checked(void) { return s_hc_ok; }

void health_get(int index, health_item_t *out)
{
    if (index < 0 || index >= s_hc_n) { memset(out, 0, sizeof *out); return; }
    *out = s_hc[index];
}

void alerts_mark_all_read(void)
{
    char js[48], *b;

    if (!s_count) return;
    snprintf(js, sizeof js, "{\"seq\":%ld}", s_items[0].seq);   /* 列表最新在前 */
    al_api("POST", "/api/alerts/read", js, &b);
    s_poll_ms = 0;          /* 下一轮立即重读 */
    s_was_active = 0;
}
