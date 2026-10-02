/*
 * speedtest.c - drives zte-agent's built-in speed test (see speedtest.h for
 * why: reusing the existing zte-agent engine instead of the old, never-
 * installed better-speedtest plugin).
 *
 * SPDX-License-Identifier: MIT
 */
#include "speedtest.h"
#include "agent_client.h"
#include "json.h"
#include "lang.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define ST_TTL_MS    1000    /* 测速进行中要看得出数字在跳，刷新快一点 */
#define ST_RESP_MAX  4096


static int    s_online;         /* agent 可达且鉴权通过 */
static long   s_last_ms;

static speedtest_phase_t s_phase = ST_IDLE;
static int    s_progress_pct;
static double s_live_mbps;
static double s_ping_ms = -1, s_jitter_ms = -1;
static double s_dl_mbps = -1, s_ul_mbps = -1;
static char   s_server[96];
static char   s_error[96];

static speedtest_server_t s_srv[ST_MAX_SERVERS];
static int    s_srv_count;
static int    s_srv_selected = -1;      /* -1 = 自动 */
static long   s_srv_last_ms;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 登录、401 重登、密码都在 agent_client.c；正文在本模块自己的缓冲区里，
 * 下一次 st_api() 会覆盖。 */
static int st_api(const char *method, const char *path, const char *json, char **body)
{
    static char resp[ST_RESP_MAX];
    return agent_api(method, path, json, resp, sizeof resp, body);
}

/* JSON 里的浮点值取成字符串再自己转——json_get() 只做标量提取，不认
 * 小数点是不是数字的一部分，但小数点也不是它的字符串终止符，所以
 * "12.34" 会原样提出来，atof() 接得住。Option<f64> 为 None 时序列化
 * 成 null，atof("null") 返回 0，所以先认出这个词单独处理成"未测出"。 */
static double get_double(const char *json, const char *key, double def)
{
    char buf[32];
    if (!json_get(json, key, buf, sizeof buf)) return def;
    if (!strcmp(buf, "null")) return def;
    return atof(buf);
}

int speedtest_poll(int active)
{
    char *body, data[ST_RESP_MAX];
    int code;
    long t = now_ms();

    if (!active) return 0;
    if (t - s_last_ms < ST_TTL_MS) return 0;
    s_last_ms = t;

    if (!agent_has_password()) { s_online = 0; return 1; }

    code = st_api("GET", "/api/speedtest/progress", NULL, &body);
    if (code != 200 || !body || !json_get(body, "data", data, sizeof data)) {
        s_online = 0;
        return 1;
    }
    s_online = 1;

    {
        char ph[16] = "";
        json_get(data, "phase", ph, sizeof ph);
        if      (!strcmp(ph, "idle"))      s_phase = ST_IDLE;
        else if (!strcmp(ph, "latency"))   s_phase = ST_LATENCY;
        else if (!strcmp(ph, "download"))  s_phase = ST_DOWNLOAD;
        else if (!strcmp(ph, "upload"))    s_phase = ST_UPLOAD;
        else if (!strcmp(ph, "complete"))  s_phase = ST_COMPLETE;
        else if (!strcmp(ph, "cancelled")) s_phase = ST_CANCELLED;
        else if (!strcmp(ph, "error"))     s_phase = ST_ERROR;
    }
    s_progress_pct = (int)json_get_int(data, "progress", 0);
    s_live_mbps    = get_double(data, "live_speed_mbps", 0);
    s_ping_ms      = get_double(data, "ping_ms", -1);
    s_jitter_ms    = get_double(data, "jitter_ms", -1);
    s_dl_mbps      = get_double(data, "download_mbps", -1);
    s_ul_mbps      = get_double(data, "upload_mbps", -1);
    json_get(data, "server", s_server, sizeof s_server);
    json_get(data, "error", s_error, sizeof s_error);
    if (!strcmp(s_error, "null")) s_error[0] = 0;
    return 1;
}

speedtest_phase_t speedtest_phase(void) { return s_phase; }

const char *speedtest_phase_label(void)
{
    switch (s_phase) {
    case ST_LATENCY:   return TR("测延迟中…");
    case ST_DOWNLOAD:  return TR("下载中…");
    case ST_UPLOAD:    return TR("上传中…");
    case ST_COMPLETE:  return TR("完成");
    case ST_CANCELLED: return TR("已取消");
    case ST_ERROR:     return TR("出错");
    case ST_IDLE:
    default:           return TR("空闲");
    }
}

int    speedtest_progress_pct(void)  { return s_progress_pct; }
double speedtest_live_mbps(void)     { return s_live_mbps; }
double speedtest_ping_ms(void)       { return s_ping_ms; }
double speedtest_jitter_ms(void)     { return s_jitter_ms; }
double speedtest_download_mbps(void) { return s_dl_mbps; }
double speedtest_upload_mbps(void)   { return s_ul_mbps; }
const char *speedtest_server(void)   { return s_server; }
const char *speedtest_error(void)    { return s_error; }
int    speedtest_running(void)       { return s_phase == ST_LATENCY || s_phase == ST_DOWNLOAD || s_phase == ST_UPLOAD; }
int    speedtest_agent_reachable(void) { return s_online; }

int speedtest_start(void)
{
    char js[48], *body;
    int code;
    if (s_srv_selected >= 0 && s_srv_selected < s_srv_count)
        snprintf(js, sizeof js, "{\"server_id\":%ld}", s_srv[s_srv_selected].id);
    else
        snprintf(js, sizeof js, "{}");
    code = st_api("POST", "/api/speedtest/start", js, &body);
    if (code == 200) s_last_ms = 0;   /* 强制下次轮询立刻刷新 */
    return code == 200;
}

int speedtest_stop(void)
{
    char *body;
    int code = st_api("POST", "/api/speedtest/stop", "{}", &body);
    if (code == 200) s_last_ms = 0;
    return code == 200;
}

/* ---- 服务器列表 ----
 * json_get() 只做浅层单值提取，一次只能抠一个 key 的第一个匹配——对付
 * "data":[{...},{...}] 这种对象数组不够用，得自己按花括号配对切出每个
 * 对象的完整子串，再对子串分别调用 json_get()。 */
static const char *skip_object(const char *p)
{
    int depth = 0, in_str = 0;
    for (; *p; p++) {
        if (in_str) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == '"') in_str = 0;
            continue;
        }
        if (*p == '"') { in_str = 1; continue; }
        if (*p == '{') depth++;
        else if (*p == '}') { if (--depth == 0) return p; }
    }
    return NULL;
}

static void parse_servers(const char *body)
{
    const char *arr = strstr(body, "\"data\":[");
    const char *p;
    if (!arr) return;
    p = arr + 8;
    s_srv_count = 0;
    while (*p && *p != ']' && s_srv_count < ST_MAX_SERVERS) {
        const char *end;
        while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (*p != '{') break;
        end = skip_object(p);
        if (!end) break;
        {
            char obj[400];
            size_t len = (size_t)(end - p) + 1;
            speedtest_server_t *s = &s_srv[s_srv_count];
            if (len >= sizeof obj) len = sizeof obj - 1;
            memcpy(obj, p, len);
            obj[len] = 0;
            s->id = json_get_int(obj, "id", 0);
            json_get(obj, "name", s->name, sizeof s->name);
            json_get(obj, "sponsor", s->sponsor, sizeof s->sponsor);
            json_get(obj, "country", s->country, sizeof s->country);
            s_srv_count++;
        }
        p = end + 1;
    }
}

int speedtest_servers_poll(int active)
{
    char *body;
    long t = now_ms();

    if (!active || s_srv_count > 0) return 0;   /* 拉过一次就够，agent 侧本来就有缓存 */
    if (t - s_srv_last_ms < ST_TTL_MS) return 0;
    s_srv_last_ms = t;

    if (!agent_has_password()) return 0;
    if (st_api("GET", "/api/speedtest/servers", NULL, &body) != 200 || !body) return 0;
    parse_servers(body);
    return 1;
}

int  speedtest_servers_count(void) { return s_srv_count; }

void speedtest_get_server(int i, speedtest_server_t *out)
{
    if (i < 0 || i >= s_srv_count || !out) return;
    *out = s_srv[i];
}

int  speedtest_selected_index(void) { return s_srv_selected; }

void speedtest_select_server(int index)
{
    s_srv_selected = (index >= 0 && index < s_srv_count) ? index : -1;
}
