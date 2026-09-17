/*
 * tailscale.c - 首页 Tailscale 状态卡片：读 tailscaled LocalAPI 的 /localapi/v0/status。
 *
 * 走 unix socket 直连 LocalAPI，而不是每次 exec tailscale CLI：CLI 是几十 MB 的 Go 程序，
 * 在这颗 SoC 上拉起一次的开销比请求本身大几个数量级；LocalAPI 一次 2ms、9 台设备约 14KB。
 *
 * SPDX-License-Identifier: MIT
 */
#include "tailscale.h"
#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define TS_IO_MS      500       /* 本机 socket 正常 2ms；tailscaled 卡住时别拖住首页 */
#define TS_TTL_MS     5000
#define TS_BACKOFF_MS 15000     /* socket 在但没响应 */
#define TS_RESP_MAX   131072    /* 9 台设备约 14KB，给大 tailnet 留余量 */

static const char *const k_socks[] = {
    "/tmp/tailscaled.sock",                 /* u60p 的 tailscale-start.sh 用的 */
    "/var/run/tailscale/tailscaled.sock",   /* 官方默认 */
};

static const char *s_sock;      /* 探测到的 socket，NULL = 设备上没有 tailscaled，卡片不出现 */
static int  s_ok;               /* 上次请求成功 */
static char s_state[24];        /* BackendState */
static int  s_self_online;      /* 控制服务器眼里本机在线 */
static char s_name[64];         /* DNSName 第一段，也就是控制台里的机器名 */
static char s_ip[48];
static char s_relay[32];        /* 首选 DERP */
static char s_routes[160];      /* PrimaryRoutes：本机实际在转发的子网（已批准的） */
static char s_exit[64];         /* 正在用的出口节点，"" = 没用 */
static int  s_exit_online;
static char s_health[160];      /* 第一条健康警告 */
static int  s_peers, s_peers_online, s_active, s_direct;

static int      s_was_active;
static long     s_poll_ms;
static unsigned s_sig;
static char     s_card[2048];

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

/*
 * GET 一次，返回正文（静态缓冲区，下一次调用会覆盖），非 200 或超时返回 NULL。
 * 用 HTTP/1.0：对端不会分块，读到关闭就是完整响应。Host 必须是 local-tailscaled.sock，
 * LocalAPI 会校验。
 */
static char *ts_get(const char *sock, const char *path)
{
    static char resp[TS_RESP_MAX];
    struct sockaddr_un sa;
    char req[256], *p;
    size_t n = 0;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0), f;

    if (fd < 0) return NULL;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock);
    /* 非阻塞 connect：对端 backlog 满时直接失败，而不是卡住 */
    f = fcntl(fd, F_GETFL, 0);
    if (f < 0 || fcntl(fd, F_SETFL, f | O_NONBLOCK) < 0 ||
        connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        close(fd);
        return NULL;
    }
    fcntl(fd, F_SETFL, f);
    snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: local-tailscaled.sock\r\n\r\n", path);
    if (wait_ready(fd, 1, TS_IO_MS) <= 0 || write(fd, req, strlen(req)) < 0) { close(fd); return NULL; }
    for (;;) {
        ssize_t rd;
        if (n + 1 >= sizeof resp) break;
        if (wait_ready(fd, 0, TS_IO_MS) <= 0) break;
        rd = read(fd, resp + n, sizeof resp - 1 - n);
        if (rd <= 0) break;
        n += (size_t)rd;
    }
    close(fd);
    resp[n] = 0;
    if (strncmp(resp, "HTTP/1.", 7) || strncmp(resp + 9, "200", 3) || !(p = strstr(resp, "\r\n\r\n")))
        return NULL;
    return p + 4;
}

/* ---- 解析 ---- */

/* p 指向 '{'：返回配对的 '}'，跳过字符串里的括号；没配上返回 NULL */
static char *match_brace(char *p)
{
    int depth = 0, instr = 0, esc = 0;
    for (; *p; p++) {
        if (instr) {
            if (esc) esc = 0;
            else if (*p == '\\') esc = 1;
            else if (*p == '"') instr = 0;
        } else if (*p == '"') instr = 1;
        else if (*p == '{') depth++;
        else if (*p == '}' && --depth == 0) return p;
    }
    return NULL;
}

static int json_true(const char *obj, const char *key)
{
    char v[8];
    return json_get(obj, key, v, sizeof v) && !strcmp(v, "true");
}

/* DNSName "<hostname>.<tailnet>.ts.net." 取第一段；没有就用 HostName */
static void short_name(const char *obj, char *out, size_t cap)
{
    char dns[256];
    if (json_get(obj, "DNSName", dns, sizeof dns) && dns[0]) {
        char *dot = strchr(dns, '.');
        if (dot) *dot = 0;
        snprintf(out, cap, "%.*s", (int)cap - 1, dns);
    } else if (!json_get(obj, "HostName", out, cap)) {
        out[0] = 0;
    }
}

/* 字符串数组里的前 max 项用 sep 连起来（null / 空数组得到 ""）。原样拷贝，不解转义 */
static void join_strings(const char *arr, const char *sep, int max, char *out, size_t cap)
{
    const char *p = arr;
    size_t o = 0;

    out[0] = 0;
    for (int n = 0; n < max && (p = strchr(p, '"')) != NULL; n++) {
        const char *e = p + 1;
        int w;
        while (*e && *e != '"') e += (*e == '\\' && e[1]) ? 2 : 1;
        if (!*e) break;
        w = snprintf(out + o, cap - o, "%s%.*s", n ? sep : "", (int)(e - p - 1), p + 1);
        if (w < 0 || (size_t)w >= cap - o) break;
        o += (size_t)w;
        p = e + 1;
    }
}

static void parse_status(char *b)
{
    static char self[16384], peer[TS_RESP_MAX];
    char arr[1024];

    if (!json_get(b, "BackendState", s_state, sizeof s_state)) s_state[0] = 0;
    if (json_get(b, "Health", arr, sizeof arr)) join_strings(arr, "", 1, s_health, sizeof s_health);
    else s_health[0] = 0;

    s_name[0] = s_ip[0] = s_relay[0] = s_routes[0] = 0;
    s_self_online = 0;
    if (json_get(b, "Self", self, sizeof self)) {
        short_name(self, s_name, sizeof s_name);
        if (json_get(self, "TailscaleIPs", arr, sizeof arr)) join_strings(arr, "", 1, s_ip, sizeof s_ip);
        if (!json_get(self, "Relay", s_relay, sizeof s_relay)) s_relay[0] = 0;
        if (json_get(self, "PrimaryRoutes", arr, sizeof arr)) join_strings(arr, ", ", 4, s_routes, sizeof s_routes);
        s_self_online = json_true(self, "Online");
    }

    /*
     * Peer 是 { "nodekey:…": {…}, … }。json_get 只认第一层键、而且会一直往后扫，
     * 所以逐个对象临时截断成独立字符串再取字段，否则会取到下一台设备上去。
     */
    s_peers = s_peers_online = s_active = s_direct = 0;
    s_exit[0] = 0;
    s_exit_online = 0;
    if (json_get(b, "Peer", peer, sizeof peer) && peer[0] == '{') {
        char *p = peer + 1;
        while ((p = strchr(p, '{')) != NULL) {
            char *e = match_brace(p), save, cur[128];
            if (!e) break;
            save = e[1];
            e[1] = 0;
            s_peers++;
            if (json_true(p, "Online")) s_peers_online++;
            if (json_true(p, "Active")) {
                s_active++;
                /* CurAddr 非空 = 打洞直连；空 = 走 DERP 中继 */
                if (json_get(p, "CurAddr", cur, sizeof cur) && cur[0]) s_direct++;
            }
            if (json_true(p, "ExitNode")) {
                short_name(p, s_exit, sizeof s_exit);
                s_exit_online = json_true(p, "Online");
            }
            e[1] = save;
            p = e + 1;
        }
    }
}

/* ---- 轮询 ---- */

static unsigned fnv(unsigned h, const char *s)
{
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    h ^= 0xFF;
    return h * 16777619u;
}

int tailscale_poll(int active)
{
    char nums[64];
    unsigned h = 2166136261u;
    long t;

    if (!active) { s_was_active = 0; return 0; }
    t = now_ms();
    /* 刚切到首页立刻读；之后按 TTL，socket 在却没响应时放慢 */
    if (s_was_active && t - s_poll_ms < (s_sock && !s_ok ? TS_BACKOFF_MS : TS_TTL_MS)) return 0;
    s_was_active = 1;
    s_poll_ms = t;

    /* 每次都重新探测：开机时 DevUI 往往比 tailscaled 先起来 */
    s_sock = NULL;
    for (size_t i = 0; i < sizeof k_socks / sizeof k_socks[0]; i++)
        if (access(k_socks[i], F_OK) == 0) { s_sock = k_socks[i]; break; }
    s_ok = 0;
    if (s_sock) {
        char *b = ts_get(s_sock, "/localapi/v0/status");
        if (b) { parse_status(b); s_ok = 1; }
    }

    snprintf(nums, sizeof nums, "%d/%d/%d/%d/%d/%d/%d/%d", s_sock != NULL, s_ok, s_self_online,
             s_exit_online, s_peers, s_peers_online, s_active, s_direct);
    h = fnv(h, nums);
    h = fnv(h, s_state);
    h = fnv(h, s_name);
    h = fnv(h, s_ip);
    h = fnv(h, s_relay);
    h = fnv(h, s_routes);
    h = fnv(h, s_exit);
    h = fnv(h, s_health);
    if (h == s_sig) return 0;
    s_sig = h;
    return 1;
}

void tailscale_get_status(tailscale_status_t *out)
{
    out->available = s_sock != NULL;
    out->ok = s_ok;
    snprintf(out->state, sizeof out->state, "%s", s_state);
    out->self_online = s_self_online;
    snprintf(out->name, sizeof out->name, "%s", s_name);
    snprintf(out->ip, sizeof out->ip, "%s", s_ip);
    snprintf(out->relay, sizeof out->relay, "%s", s_relay);
    snprintf(out->routes, sizeof out->routes, "%s", s_routes);
    snprintf(out->exit_node, sizeof out->exit_node, "%s", s_exit);
    out->exit_online = s_exit_online;
    snprintf(out->health, sizeof out->health, "%s", s_health);
    out->peers = s_peers;
    out->peers_online = s_peers_online;
    out->active = s_active;
    out->direct = s_direct;
}

/* ---- 卡片 ---- */

static void ts_esc(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (const char *s = src; *s && o + 7 < cap; s++) {
        const char *r = NULL;
        if      (*s == '&') r = "&amp;";
        else if (*s == '<') r = "&lt;";
        else if (*s == '>') r = "&gt;";
        if (r) { size_t L = strlen(r); memcpy(dst + o, r, L); o += L; }
        else dst[o++] = *s;
    }
    dst[o] = 0;
}

#define CARD_APPEND(...) do { \
        if (o < (int)sizeof s_card) { \
            int w_ = snprintf(s_card + o, sizeof s_card - (size_t)o, __VA_ARGS__); \
            if (w_ > 0) o += w_; \
        } \
    } while (0)

const char *tailscale_card_html(int locked)
{
    const char *st, *cls;
    char name[192], a[192], b[192];
    int running = s_ok && !strcmp(s_state, "Running");
    int o = 0;

    if (!s_sock) return "";
    if (!s_ok)                                   { st = "未运行";   cls = "q-off"; }
    else if (running)                            { st = s_self_online ? "已连接" : "离线";
                                                   cls = s_self_online ? "q-good" : "q-bad"; }
    else if (!strcmp(s_state, "Starting"))       { st = "连接中";   cls = "q-mid"; }
    else if (!strcmp(s_state, "NeedsLogin"))     { st = "需要登录"; cls = "q-mid"; }
    else if (!strcmp(s_state, "NeedsMachineAuth")) { st = "等待批准"; cls = "q-mid"; }
    else if (!strcmp(s_state, "Stopped"))        { st = "已停止";   cls = "q-off"; }
    else                                         { st = s_state[0] ? s_state : "-"; cls = "q-off"; }

    /* 锁屏预览只露状态和在线数：机器名、地址、子网都藏起来 */
    ts_esc(name, sizeof name, s_name);
    CARD_APPEND("<div class='card'><div class='title'>Tailscale");
    if (!locked && name[0]) CARD_APPEND(" <span class='sub'>%s</span>", name);
    CARD_APPEND("<span class='r ts-st %s'>%s</span></div>", cls, st);

    if (!s_ok) {
        CARD_APPEND("<div class='sec'>tailscaled 没有响应</div></div>");
        return s_card;
    }
    if (running) {
        if (!locked && (s_ip[0] || s_relay[0])) {
            ts_esc(a, sizeof a, s_ip);
            ts_esc(b, sizeof b, s_relay);
            CARD_APPEND("<div class='sec'>%s%s%s%s</div>", a,
                        a[0] && b[0] ? " · " : "", b[0] ? "DERP " : "", b);
        }
        CARD_APPEND("<table>");
        if (!locked && s_routes[0]) {
            ts_esc(a, sizeof a, s_routes);
            CARD_APPEND("<tr><td class='kv-l'>子网路由</td><td class='val'>%s</td></tr>", a);
        }
        CARD_APPEND("<tr><td class='kv-l'>在线设备</td><td class='val'>%d / %d</td></tr>",
                    s_peers_online, s_peers);
        if (!locked) {
            if (s_active)
                CARD_APPEND("<tr><td class='kv-l'>活跃连接</td><td class='val'>%d · 直连 %d · 中继 %d</td></tr>",
                            s_active, s_direct, s_active - s_direct);
            else
                CARD_APPEND("<tr><td class='kv-l'>活跃连接</td><td class='val'>无</td></tr>");
            if (s_exit[0]) {
                ts_esc(a, sizeof a, s_exit);
                CARD_APPEND("<tr><td class='kv-l'>出口节点</td><td class='val'>%s%s</td></tr>",
                            a, s_exit_online ? "" : " · 离线");
            }
        }
        CARD_APPEND("</table>");
    }
    if (!locked && s_health[0]) {
        ts_esc(a, sizeof a, s_health);
        CARD_APPEND("<div class='ts-warn'>%s</div>", a);
    }
    CARD_APPEND("</div>");
    return s_card;
}
