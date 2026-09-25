/*
 * scenario.c - 首页情景卡片的数据源。
 *
 * 走 zte-agent 的 GET /api/public/status（免登录，LAN 只读摘要）：情景只是
 * 其中一段，不为它单独要 token。agent 和 esim.c/speedtest.c 同一个 :9090，
 * HTTP 写法也照它们（HTTP/1.0，不解分块）。
 *
 * SPDX-License-Identifier: MIT
 */
#include "scenario.h"
#include "esim.h"
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

#define SC_PORT       9090
#define SC_IO_MS      800       /* 本机请求正常几十毫秒；agent 卡住时别拖住首页 */
#define SC_TTL_MS     30000     /* 情景最快两次扫描才切一次，30 秒一读足够 */
#define SC_RESP_MAX   16384     /* public/status 约 1KB */
#define SC_LOST_MS    120000    /* 读不到超过 2 分钟才算后台失联：agent 重启只要几秒 */

static int  s_ok, s_configured, s_enabled, s_wifi_off;
static char s_name[48], s_pin[48];
static long s_last_switch;
static int  s_abroad, s_chill_on = -1, s_chill_back, s_auto_direct;
static int  s_dirty;            /* 本地改过状态（开关 CHILL），下一轮要重绘 */

static int      s_was_active;
static long     s_poll_ms;
static unsigned s_sig;

/* 最近一次读成功的时刻；0 = 从未成功，从 s_first_ms 起算 */
static long s_ok_ms, s_first_ms;
static int  s_unread;
static int  s_h_checked, s_h_bad, s_h_warn;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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

/* 返回正文（静态缓冲区，下次调用覆盖），失败返回 NULL */
static char *sc_get(const char *path)
{
    static char resp[SC_RESP_MAX];
    struct sockaddr_in sa;
    char req[256];
    size_t n = 0;
    char *p;
    int fd, f, rc;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(SC_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    f = fcntl(fd, F_GETFL, 0);
    if (f < 0 || fcntl(fd, F_SETFL, f | O_NONBLOCK) < 0) { close(fd); return NULL; }
    rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    if (rc < 0 && errno != EINPROGRESS) { close(fd); return NULL; }
    if (rc < 0 && wait_ready(fd, 1, SC_IO_MS) <= 0) { close(fd); return NULL; }
    fcntl(fd, F_SETFL, f);

    snprintf(req, sizeof req,
             "GET %s HTTP/1.0\r\nHost: 127.0.0.1:%d\r\nConnection: close\r\n\r\n",
             path, SC_PORT);
    if (write(fd, req, strlen(req)) < 0) { close(fd); return NULL; }
    for (;;) {
        ssize_t rd;
        if (n + 1 >= sizeof resp) break;
        if (wait_ready(fd, 0, SC_IO_MS) <= 0) break;
        rd = read(fd, resp + n, sizeof resp - 1 - n);
        if (rd <= 0) break;
        n += (size_t)rd;
    }
    close(fd);
    resp[n] = 0;
    if (strncmp(resp, "HTTP/1.", 7) || atoi(resp + 9) != 200) return NULL;
    if (!(p = strstr(resp, "\r\n\r\n"))) return NULL;
    return p + 4;
}

static unsigned fnv(unsigned h, const char *s)
{
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

/* 字符串字段；JSON 的 null 当空串 */
static void str_field(const char *obj, const char *key, char *out, size_t cap)
{
    if (!json_get(obj, key, out, cap) || !strcmp(out, "null")) out[0] = 0;
}

static int bool_field(const char *obj, const char *key)
{
    char v[8];
    return json_get(obj, key, v, sizeof v) && !strcmp(v, "true");
}

int scenario_poll(int active)
{
    static char data[SC_RESP_MAX], sc[1024];
    char nums[64], *b;
    unsigned h = 2166136261u;
    long t;

    int just_shown = active && !s_was_active;

    t = now_ms();
    if (!s_first_ms) s_first_ms = t;
    s_was_active = active;
    /* 不在首页也按 30 秒读：顶栏的失联提示每一页都要准。刚切到首页时抢先读
     * 一次，但 agent 正读不到时不抢（卡住的 agent 会让每次切页顿 0.8 秒）。 */
    if (s_poll_ms && t - s_poll_ms < SC_TTL_MS && !(just_shown && s_ok)) {
        if (!s_dirty) return 0;
        s_dirty = 0;
        return 1;
    }
    s_dirty = 0;
    s_poll_ms = t;

    s_ok = 0;
    b = sc_get("/api/public/status");
    /* 一层一层取：json_get 只认当前对象最外层的键，scenario 在 data 里面。 */
    if (b && json_get(b, "data", data, sizeof data)) {
        char al[128];
        s_ok_ms = t;
        s_unread = json_get(data, "alerts", al, sizeof al) ? (int)json_get_int(al, "unread", 0) : 0;
        if (json_get(data, "health", al, sizeof al)) {
            s_h_checked = bool_field(al, "checked");
            s_h_bad = (int)json_get_int(al, "bad", 0);
            s_h_warn = (int)json_get_int(al, "warn", 0);
        } else {
            s_h_checked = 0;
        }
        /* services.chill.on：总开关（/data/chill/disabled 不存在）。旧 agent 没有这个键 → -1 */
        {
            char svc[1024], ch[256], v[8];
            s_chill_on = -1;
            if (json_get(data, "services", svc, sizeof svc) && json_get(svc, "chill", ch, sizeof ch) &&
                json_get(ch, "on", v, sizeof v))
                s_chill_on = !strcmp(v, "true");
        }
    } else {
        data[0] = 0;
    }
    if (data[0] && json_get(data, "scenario", sc, sizeof sc)) {
        s_ok = 1;
        s_configured = bool_field(sc, "configured");
        s_enabled = bool_field(sc, "enabled");
        s_wifi_off = bool_field(sc, "wifi_off");
        str_field(sc, "name", s_name, sizeof s_name);
        str_field(sc, "pin", s_pin, sizeof s_pin);
        s_last_switch = json_get_int(sc, "last_switch", 0);
        s_abroad = bool_field(sc, "abroad");
        s_chill_back = bool_field(sc, "chill_on_when_home");
        s_auto_direct = bool_field(sc, "auto_direct");
    }

    snprintf(nums, sizeof nums, "%d/%d/%d/%d/%ld/%d/%d%d%d", s_ok, s_configured, s_enabled,
             s_wifi_off, s_last_switch, s_unread, s_abroad, s_chill_on, s_chill_back);
    h = fnv(h, nums);
    h = fnv(h, s_name);
    h = fnv(h, s_pin);
    if (h == s_sig) return 0;
    s_sig = h;
    return 1;
}

void scenario_get_status(scenario_status_t *out)
{
    out->available = s_ok && s_configured;
    out->enabled = s_enabled;
    snprintf(out->name, sizeof out->name, "%s", s_name);
    out->wifi_off = s_wifi_off;
    snprintf(out->pin, sizeof out->pin, "%s", s_pin);
    out->last_switch = s_last_switch;
    out->abroad = s_abroad;
    out->chill_on = s_chill_on;
    out->chill_back = s_chill_back;
    out->auto_direct = s_auto_direct;
}

int scenario_chill_set(int on)
{
    int code = agent_post(on ? "/api/services/chill/enable" : "/api/services/chill/disable");
    if (code < 200 || code >= 300) return 0;
    /* 先按请求显示；agent 那边 chill.sh 要跑几秒，4 秒后再读真实状态 */
    s_chill_on = on;
    if (on) s_chill_back = 0;
    else if (s_abroad) s_chill_back = 1;
    s_dirty = 1;
    s_sig = 0;
    s_poll_ms = now_ms() - SC_TTL_MS + 4000;
    return 1;
}

void scenario_kick(void)
{
    s_poll_ms = 0;
    s_sig = 0;
}

void agent_health(agent_health_t *out)
{
    long t = now_ms(), since = s_ok_ms ? s_ok_ms : s_first_ms;
    out->lost_secs = (since && t - since > SC_LOST_MS) ? (t - since) / 1000 : 0;
    out->unread = out->lost_secs ? 0 : s_unread;
    out->checked = out->lost_secs ? 0 : s_h_checked;
    out->bad = s_h_bad;
    out->warn = s_h_warn;
}
