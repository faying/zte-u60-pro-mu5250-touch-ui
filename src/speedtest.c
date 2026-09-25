/*
 * speedtest.c - drives zte-agent's built-in speed test (see speedtest.h for
 * why: reusing the existing zte-agent engine instead of the old, never-
 * installed better-speedtest plugin).
 *
 * SPDX-License-Identifier: MIT
 */
#include "speedtest.h"
#include "json.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* Same password source esim.c already reads (u60p/zte-agent's own startup
 * script embeds it) — one zte-agent instance, one password, no reason to
 * make the user configure it twice in two separate conf files. */
#define ST_AGENT_SH  "/data/local/tmp/start_zte_agent.sh"
/* procd 装法（zte-agent.init）把密码放在这里，旧启动脚本可能已不存在；先读它 */
#define ST_AGENT_SH_ENV "/data/zte-agent.env"
#define ST_PORT      9090
#define ST_IO_MS     1500
#define ST_TTL_MS    1000    /* 测速进行中要看得出数字在跳，刷新快一点 */
#define ST_RESP_MAX  4096

static char s_pass[128];
static char s_token[80];
static int  s_conf_loaded;

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

static void load_conf(void)
{
    FILE *fp;
    char line[256];

    if (s_conf_loaded) return;
    s_conf_loaded = 1;
    /* procd 装法的 env 文件优先，旧启动脚本兜底（两种写法同一个解析） */
    fp = fopen(ST_AGENT_SH_ENV, "r");
    if (!fp) fp = fopen(ST_AGENT_SH, "r");
    if (!fp) return;
    while (fgets(line, sizeof line, fp)) {
        char *p = strstr(line, "ZTE_AGENT_PASSWORD="), *e, q = 0;
        if (!p) continue;
        if ((e = strpbrk(p, "\r\n")) != NULL) *e = 0;
        p += 19;
        if (*p == '\'' || *p == '"') q = *p++;
        e = q ? strchr(p, q) : strpbrk(p, " \t;");
        if (e) *e = 0;
        snprintf(s_pass, sizeof s_pass, "%s", p);
        break;
    }
    fclose(fp);
}

/* ---- minimal HTTP (same shape as esim.c) ---- */

static int wait_ready(int fd, int write_side, int ms)
{
    fd_set s;
    struct timeval tv;
    FD_ZERO(&s);
    FD_SET(fd, &s);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return select(fd + 1, write_side ? NULL : &s, write_side ? &s : NULL, NULL, &tv);
}

static int st_connect(void)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int f, rc;

    if (fd < 0) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(ST_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    f = fcntl(fd, F_GETFL, 0);
    if (f < 0 || fcntl(fd, F_SETFL, f | O_NONBLOCK) < 0) { close(fd); return -1; }
    rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    if (rc < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    if (rc < 0 && wait_ready(fd, 1, ST_IO_MS) <= 0) { close(fd); return -1; }
    fcntl(fd, F_SETFL, f);
    return fd;
}

static int st_http(const char *method, const char *path, const char *json, char **body)
{
    static char resp[ST_RESP_MAX];
    char req[512], auth[128] = "";
    size_t n = 0;
    char *p;
    int fd;

    *body = NULL;
    fd = st_connect();
    if (fd < 0) return 0;
    if (s_token[0]) snprintf(auth, sizeof auth, "Authorization: Bearer %s\r\n", s_token);
    snprintf(req, sizeof req,
             "%s %s HTTP/1.0\r\nHost: 127.0.0.1:%d\r\n%s"
             "Content-Type: application/json\r\nContent-Length: %d\r\n"
             "Connection: close\r\n\r\n%s",
             method, path, ST_PORT, auth, json ? (int)strlen(json) : 0, json ? json : "");
    if (write(fd, req, strlen(req)) < 0) { close(fd); return 0; }
    for (;;) {
        ssize_t rd;
        if (n + 1 >= sizeof resp) break;
        if (wait_ready(fd, 0, ST_IO_MS) <= 0) break;
        rd = read(fd, resp + n, sizeof resp - 1 - n);
        if (rd <= 0) break;
        n += (size_t)rd;
    }
    close(fd);
    resp[n] = 0;
    if (strncmp(resp, "HTTP/1.", 7) || !(p = strstr(resp, "\r\n\r\n"))) return 0;
    *body = p + 4;
    return atoi(resp + 9);
}

static int st_login(void)
{
    char js[300], data[160], *b;
    size_t o = (size_t)snprintf(js, sizeof js, "{\"password\":\"");

    for (const char *p = s_pass; *p && o + 4 < sizeof js; p++) {
        if (*p == '"' || *p == '\\') js[o++] = '\\';
        js[o++] = *p;
    }
    snprintf(js + o, sizeof js - o, "\"}");
    s_token[0] = 0;
    if (st_http("POST", "/api/auth/login", js, &b) != 200 || !b) return 0;
    if (!json_get(b, "data", data, sizeof data)) return 0;
    return json_get(data, "token", s_token, sizeof s_token) && s_token[0];
}

/* 401 = token 过期或 agent 重启过，重登一次再试 — 同 esim.c 的 es_api()。 */
static int st_api(const char *method, const char *path, const char *json, char **body)
{
    int code;

    if (!s_token[0] && s_pass[0]) st_login();
    code = st_http(method, path, json, body);
    if (code == 401 && s_pass[0] && st_login())
        code = st_http(method, path, json, body);
    return code;
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

    load_conf();
    if (!active) return 0;
    if (t - s_last_ms < ST_TTL_MS) return 0;
    s_last_ms = t;

    if (!s_pass[0]) { s_online = 0; return 1; }

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
    case ST_LATENCY:   return "\xE6\xB5\x8B\xE5\xBB\xB6\xE8\xBF\x9F\xE4\xB8\xAD\xE2\x80\xA6";       /* 测延迟中… */
    case ST_DOWNLOAD:  return "\xE4\xB8\x8B\xE8\xBD\xBD\xE4\xB8\xAD\xE2\x80\xA6";                     /* 下载中… */
    case ST_UPLOAD:    return "\xE4\xB8\x8A\xE4\xBC\xA0\xE4\xB8\xAD\xE2\x80\xA6";                     /* 上传中… */
    case ST_COMPLETE:  return "\xE5\xAE\x8C\xE6\x88\x90";                                            /* 完成 */
    case ST_CANCELLED: return "\xE5\xB7\xB2\xE5\x8F\x96\xE6\xB6\x88";                                 /* 已取消 */
    case ST_ERROR:     return "\xE5\x87\xBA\xE9\x94\x99";                                            /* 出错 */
    case ST_IDLE:
    default:           return "\xE7\xA9\xBA\xE9\x97\xB2";                                            /* 空闲 */
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

    if (!s_pass[0]) return 0;
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
