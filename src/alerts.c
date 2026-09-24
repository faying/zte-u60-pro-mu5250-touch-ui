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

#define AL_PORT      9090
#define AL_ENV       "/data/zte-agent.env"
#define AL_AGENT_SH  "/data/local/tmp/start_zte_agent.sh"
#define AL_IO_MS     1500
#define AL_TTL_MS    10000
#define AL_RESP_MAX  32768

static char s_pass[128], s_token[80];
static int  s_pass_loaded;
static alert_item_t s_items[ALERTS_MAX];
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

/* agent 密码：env 文件优先，旧启动脚本兜底（同 esim.c） */
static void load_password(void)
{
    static const char *paths[] = { AL_ENV, AL_AGENT_SH };
    char line[256];
    FILE *fp = NULL;

    if (s_pass_loaded) return;
    s_pass_loaded = 1;
    for (size_t i = 0; i < sizeof paths / sizeof *paths && !fp; i++) fp = fopen(paths[i], "r");
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

static int al_connect(void)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0), f, rc;

    if (fd < 0) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(AL_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    f = fcntl(fd, F_GETFL, 0);
    if (f < 0 || fcntl(fd, F_SETFL, f | O_NONBLOCK) < 0) { close(fd); return -1; }
    rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    if (rc < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    if (rc < 0 && wait_ready(fd, 1, AL_IO_MS) <= 0) { close(fd); return -1; }
    fcntl(fd, F_SETFL, f);
    return fd;
}

/* 返回 HTTP 状态码（0 = 连不上），*body 指向静态缓冲区（下次调用覆盖） */
static int al_http(const char *method, const char *path, const char *json, char **body)
{
    static char resp[AL_RESP_MAX];
    char req[768], auth[128] = "";
    size_t n = 0;
    char *p;
    int fd;

    *body = NULL;
    if ((fd = al_connect()) < 0) return 0;
    if (s_token[0]) snprintf(auth, sizeof auth, "Authorization: Bearer %s\r\n", s_token);
    snprintf(req, sizeof req,
             "%s %s HTTP/1.0\r\nHost: 127.0.0.1:%d\r\n%s"
             "Content-Type: application/json\r\nContent-Length: %d\r\n"
             "Connection: close\r\n\r\n%s",
             method, path, AL_PORT, auth, json ? (int)strlen(json) : 0, json ? json : "");
    if (write(fd, req, strlen(req)) < 0) { close(fd); return 0; }
    for (;;) {
        ssize_t rd;
        if (n + 1 >= sizeof resp) break;
        if (wait_ready(fd, 0, AL_IO_MS) <= 0) break;
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

static int al_login(void)
{
    char js[300], data[160], *b;
    size_t o = (size_t)snprintf(js, sizeof js, "{\"password\":\"");

    for (const char *p = s_pass; *p && o + 4 < sizeof js; p++) {
        if (*p == '"' || *p == '\\') js[o++] = '\\';
        js[o++] = *p;
    }
    snprintf(js + o, sizeof js - o, "\"}");
    s_token[0] = 0;
    if (al_http("POST", "/api/auth/login", js, &b) != 200 || !b) return 0;
    if (!json_get(b, "data", data, sizeof data)) return 0;
    return json_get(data, "token", s_token, sizeof s_token) && s_token[0];
}

static int al_api(const char *method, const char *path, const char *json, char **body)
{
    int code;

    load_password();
    if (!s_token[0] && s_pass[0]) al_login();
    code = al_http(method, path, json, body);
    if (code == 401 && s_pass[0] && al_login()) code = al_http(method, path, json, body);
    return code;
}

/* 和管理网页 web/src/lib/alerts.ts 的 kindLabel 一致 */
static const char *kind_label(const char *kind)
{
    static const struct { const char *k, *zh; } t[] = {
        { "agent-crash",         "管理后台意外退出，已自动重启" },
        { "agent-silent",        "管理后台失去响应" },
        { "agent-hung",          "管理后台卡死，已被强制重启" },
        { "datad-crash",         "数据服务意外退出，已自动重启" },
        { "devui-crash",         "触屏界面闪退，已自动重新打开" },
        { "devui-gave-up",       "触屏界面反复打不开，已换回原厂界面" },
        { "devui-theme-paused",  "自动切换深浅色已暂停，重启后恢复" },
        { "wifi-takeover",       "Wi-Fi 看门狗重新打开了 Wi-Fi" },
        { "wifi-restore-failed", "Wi-Fi 看门狗没能打开 Wi-Fi" },
        { "sms-failed",          "告警短信发送失败" },
        { "sms-test",            "测试短信" },
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++)
        if (!strcmp(kind, t[i].k)) return t[i].zh;
    return NULL;
}

static void json_str(const char *obj, const char *key, char *out, size_t cap)
{
    if (!json_get(obj, key, out, cap) || !strcmp(out, "null")) out[0] = 0;
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
        const char *zh;

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
        json_str(p, "text", e->text, sizeof e->text);
        json_str(p, "unread", unread, sizeof unread);
        e->unread = !strcmp(unread, "true");
        q[1] = save;
        p = q + 1;
        zh = kind_label(kind);
        if (zh) snprintf(e->label, sizeof e->label, "%s", zh);
        else    snprintf(e->label, sizeof e->label, "其他告警（%s）", kind);
        n++;
    }
    s_count = n;
}

static int load(void)
{
    static char data[AL_RESP_MAX];
    char *b;
    int code = al_api("GET", "/api/alerts", NULL, &b);

    if (code == 0) { snprintf(s_err, sizeof s_err, "连不上管理后台"); return 0; }
    if (code == 401) { snprintf(s_err, sizeof s_err, "登录管理后台失败（密码不对？）"); return 0; }
    if (code != 200 || !b || !json_get(b, "data", data, sizeof data)) {
        snprintf(s_err, sizeof s_err, "读告警失败（HTTP %d）", code);
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
    snprintf(nums, sizeof nums, "%d/%d", s_count, s_unread);
    h = fnv(h, nums);
    h = fnv(h, s_err);
    for (int i = 0; i < s_count; i++) {
        snprintf(nums, sizeof nums, "%ld/%d", s_items[i].seq, s_items[i].unread);
        h = fnv(h, nums);
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

void alerts_mark_all_read(void)
{
    char js[48], *b;

    if (!s_count) return;
    snprintf(js, sizeof js, "{\"seq\":%ld}", s_items[0].seq);   /* 列表最新在前 */
    al_api("POST", "/api/alerts/read", js, &b);
    s_poll_ms = 0;          /* 下一轮立即重读 */
    s_was_active = 0;
}
