/*
 * chill.c - "CHILL" 面板：ShellCrash / mihomo (clash API) status + control.
 *
 * 屏幕上、设备文件名里一律叫 CHILL，
 * 后端仍是 ShellCrash/mihomo，所以 restart 命令还是 /etc/init.d/shellcrash。
 *
 * SPDX-License-Identifier: MIT
 */
#include "chill.h"
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

#define SC_CONF     "/data/plugins/u60pro-devui/chill.conf"
#define SC_IO_MS    1500
#define SC_TTL_MS   2000
/* 2026-09-22 真机核对：新增的"手动节点"组一个就有 168 个节点（四个 provider
 * 地区并集），64 装不下——家宽/NX（nexi）节点排在 oix/shouhou 后面，正好被
 * 截掉的就是它们，界面上看起来像是"消失了"。留够余量到 200。 */
#define SC_MAX_NODE 200
#define SC_NAME_MAX 96
#define SC_RESP_MAX 262144

static int  s_rot180 = 1;               /* U60 面板倒装，默认转 180°；TopFlow 是正装 */
static char s_host[64] = "127.0.0.1";   /* mihomo 跑在其他 netns 时改成对应地址 */
static int  s_port = 9999;
static char s_secret[128];
static char s_restore_cmd[256] = "/etc/init.d/zte_topsw_devui start";
static char s_group[SC_NAME_MAX] = "\xF0\x9F\x9A\x80 \xE8\x8A\x82\xE7\x82\xB9\xE9\x80\x89\xE6\x8B\xA9"; /* 🚀 节点选择 */
static int  s_conf_loaded;

static int  s_online;
static char s_mode_raw[16];
static char s_node[SC_NAME_MAX];
static char s_traffic[48];
static char s_speed[48];        /* 实时上下行 */
static char s_chain[160];       /* 真实出口链路：组 -> 组 -> 节点 */
static char s_chain_raw[4][SC_NAME_MAX];  /* 链路上各跳的原名，用于标记按钮 */
static int  s_chain_n;
static long s_prev_dl, s_prev_ul, s_prev_t;
static char s_conns[16];
static int  s_conns_truncated;   /* 1 = /connections 被截断，连接数是下限不是准确值 */
static int  s_core_running;

static char s_nodes[SC_MAX_NODE][SC_NAME_MAX];      /* 显示用（已去 emoji） */
static char s_nodes_raw[SC_MAX_NODE][SC_NAME_MAX];  /* 原名，调 API 时用 */
static int  s_node_delay[SC_MAX_NODE];   /* -1 = 未测 */
static int  s_node_count;

/*
 * 按"命中的分流组 -> 实际出口节点"这一对关系汇总流量——不是靠
 * chill_group()/chill_chain() 那一路（只追一个配置好的组，规则模式下大部分
 * 流量根本不走那个组，2026-09-22 的反馈：那两个字段只代表"如果用节点选择
 * 这个组会怎样"，不代表流量实际去哪了）。数据来自 /connections 里每条连接
 * 自带的 chains 数组：chains[0] 是真正落地的节点，chains 最后一项是决定
 * 路由的那个策略组（比如 Apple/VoWiFi/漏网之鱼）。
 *
 * 分组和节点分两张独立 top N 表统计过一版——每张表各自按总字节数排序，
 * 界面上摆在一起容易被看成"第 i 条分组对应第 i 条节点"，实际是两个互相
 * 独立的排名，根本不是同一条流量（2026-09-22 反馈：具体哪条规则走了哪个
 * 节点，没有放出来）。改成直接按 (分流组, 节点) 这一对二元组做 key 累加，
 * 界面上显示"组 -> 节点"，才是真的能回答"这条规则流量去哪了"。
 *
 * 这是持久累计统计，不是"轮询这一刻还活着的连接"快照（后者试过，问题是
 * 连接一关闭它的流量就凭空消失了，用户明确反馈过这不是他们要的"流量统计"）。
 * 做法：每条连接的 upload/download 是它自己的单调递增总量，不是速率；每轮
 * 轮询按连接 id 跟上一轮的值做差，差值才是这段时间真实发生的流量，累加进
 * 下面这张永久表——跟 mihomo 自己的 downloadTotal/uploadTotal 一个道理，
 * 只在进程重启时清零，连接开关不影响。
 */
#define SC_TOP_SHOW    6   /* 界面上只显示 top N，卡片空间有限 */

/* 持久统计表：一行是一个 (分流组, 节点) 组合。容量对齐 SC_MAX_NODE ——
 * 理论上出现的不同组合不会比不同节点数多太多（同一节点常年只挂在一两个
 * 组下面）。 */
#define SC_MAX_STAT_PAIR 200
typedef struct { char grp_raw[SC_NAME_MAX]; char node_raw[SC_NAME_MAX]; long up, down; } sc_pair_t;
static sc_pair_t s_stat_pair[SC_MAX_STAT_PAIR];
static int       s_stat_pair_n;

/* 每条连接按 id 记住上次轮询时的累计字节数，用于求增量；id 在某一轮消失
 * 说明连接关了，摘出表即可——它最后一次的增量在上一轮已经记过账了。 */
#define SC_MAX_TRACK 256
typedef struct { char id[40]; long up, down; unsigned char seen; } sc_track_t;
static sc_track_t s_track[SC_MAX_TRACK];
static int        s_track_n;

static chill_traffic_item_t s_top_pair[SC_TOP_SHOW];
static int                  s_top_pair_n;

static char s_listhtml[24576];
static char s_grphtml[4096];
static char s_card[1024];       /* 首页/锁屏 CHILL 卡片，见 chill_card_html() */

#define SC_MAX_GRP 16
static char s_grp_raw[SC_MAX_GRP][SC_NAME_MAX];   /* 原名，调 API 用 */
static char s_grp_disp[SC_MAX_GRP][SC_NAME_MAX];  /* 显示名，去 emoji */
static char s_grp_type[SC_MAX_GRP][16];
static int  s_grp_count;
static int  s_grp_idx = -1;                       /* 当前操作的组，-1 = 尚未定位 */
static int  s_conn_direct, s_conn_proxy;
static long s_last_ms;

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
    fp = fopen(SC_CONF, "r");
    if (!fp) return;
    while (fgets(line, sizeof line, fp)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (!strncmp(line, "rotate=", 7))      s_rot180 = (atoi(line + 7) == 180);
        else if (!strncmp(line, "host=", 5))   snprintf(s_host, sizeof s_host, "%s", line + 5);
        else if (!strncmp(line, "port=", 5))   s_port = atoi(line + 5);
        else if (!strncmp(line, "secret=", 7)) snprintf(s_secret, sizeof s_secret, "%s", line + 7);
        else if (!strncmp(line, "group=", 6))  snprintf(s_group, sizeof s_group, "%s", line + 6);
        else if (!strncmp(line, "restore_cmd=", 12))
            snprintf(s_restore_cmd, sizeof s_restore_cmd, "%s", line + 12);
    }
    fclose(fp);
}

/* ---- minimal HTTP ---- */

static int set_nonblock(int fd, int on)
{
    int f = fcntl(fd, F_GETFL, 0);
    if (f < 0) return -1;
    return fcntl(fd, F_SETFL, on ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK));
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

static int sc_connect(void)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int rc;

    if (fd < 0) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)s_port);
    if (inet_pton(AF_INET, s_host, &sa.sin_addr) != 1) { close(fd); return -1; }
    if (set_nonblock(fd, 1) < 0) { close(fd); return -1; }
    rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    if (rc < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    if (rc < 0 && wait_ready(fd, 1, SC_IO_MS) <= 0) { close(fd); return -1; }
    set_nonblock(fd, 0);
    return fd;
}

/* Case-insensitive substring search — header names are case-insensitive per
 * RFC 7230 and mihomo's Go http server doesn't normalize casing for us. Not
 * using strcasestr(): musl has it, but it's a nonstandard extension and the
 * rest of this file (json.c) already avoids depending on libc parsing help. */
static const char *ci_find(const char *hay, const char *hay_end, const char *needle)
{
    size_t nl = strlen(needle);
    for (const char *h = hay; h + nl <= hay_end; h++) {
        size_t i = 0;
        for (; i < nl; i++) {
            char a = h[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
            if (a != b) break;
        }
        if (i == nl) return h;
    }
    return NULL;
}

/*
 * Decode an HTTP/1.1 chunked body IN PLACE — write position never runs ahead
 * of read position, so overwriting as we go is always safe. Stops at the
 * terminating 0-size chunk (trailer headers after it, if any, are ignored).
 * Any malformed chunk just stops decoding where we are rather than looping
 * or reading out of bounds — a truncated list beats a crash.
 */
static size_t dechunk(char *body, size_t len)
{
    size_t r = 0, w = 0;
    for (;;) {
        char *end;
        long sz;
        if (r >= len) break;
        sz = strtol(body + r, &end, 16);
        if (end == body + r || sz < 0) break;              /* not a hex size */
        r = (size_t)(end - body);
        if (r + 1 >= len || body[r] != '\r' || body[r + 1] != '\n') break;
        r += 2;
        if (sz == 0) break;                                 /* last chunk */
        if (r + (size_t)sz > len) sz = (long)(len - r);     /* short read, use what we have */
        memmove(body + w, body + r, (size_t)sz);
        w += (size_t)sz;
        r += (size_t)sz;
        if (r + 1 < len && body[r] == '\r' && body[r + 1] == '\n') r += 2;
    }
    body[w] = 0;
    return w;
}

/*
 * One request. `body` NULL => GET. Returns the response body in a STATIC buffer,
 * or NULL.
 *
 * 调用方注意：下一次调用会覆盖上一次的返回内容。要用同一份响应里的多个字段，
 * 必须在发下一个请求之前全部取完，不能一边解析一边发新请求。
 */
/* 1 = 上一次 sc_http() 调用因为 SC_RESP_MAX 装不下而被截断（不是连接关闭/
 * 超时正常结束）。目前只有 /connections 的调用方会看这个标志——那是唯一一个
 * 大小随设备负载（活跃连接数）变化、有可能撑爆 256KB 缓冲区的接口。 */
static int s_last_truncated;

static void sc_build_req(char *req, size_t cap, const char *method,
                          const char *path, const char *body)
{
    if (body)
        snprintf(req, cap,
                 "%s %s HTTP/1.1\r\nHost: %s:%d\r\n%s%s%s"
                 "Content-Type: application/json\r\nContent-Length: %d\r\n"
                 "Connection: close\r\n\r\n%s",
                 method, path, s_host, s_port,
                 s_secret[0] ? "Authorization: Bearer " : "", s_secret, s_secret[0] ? "\r\n" : "",
                 (int)strlen(body), body);
    else
        snprintf(req, cap,
                 "%s %s HTTP/1.1\r\nHost: %s:%d\r\n%s%s%s"
                 "Connection: close\r\n\r\n",
                 method, path, s_host, s_port,
                 s_secret[0] ? "Authorization: Bearer " : "", s_secret, s_secret[0] ? "\r\n" : "");
}

/*
 * 控制类请求（选节点/切模式）只关心"发出去了没有"，不需要等回包——mihomo
 * 收到请求就会执行，结果反正靠下一轮 chill_poll() 刷新体现在界面上。等回包
 * 的话就是 chill_select_node() 这种在 LVGL 触摸回调里同步调用的函数会占住
 * 主线程最多 SC_IO_MS=1.5 秒，界面在这段时间里完全不响应触摸——2026-09-21
 * 真机反馈"选节点不跟手"就是这个。写完请求立刻关连接，不读、不等。
 */
static int sc_send_async(const char *method, const char *path, const char *body)
{
    char req[1024];
    int fd = sc_connect();
    if (fd < 0) return 0;
    sc_build_req(req, sizeof req, method, path, body);
    if (write(fd, req, strlen(req)) < 0) { close(fd); return 0; }
    close(fd);
    return 1;
}

static char *sc_http(const char *method, const char *path, const char *body)
{
    static char resp[SC_RESP_MAX];
    char req[1024];
    size_t n = 0;
    char *p;
    int fd = sc_connect();

    if (fd < 0) return NULL;
    sc_build_req(req, sizeof req, method, path, body);

    if (write(fd, req, strlen(req)) < 0) { close(fd); return NULL; }
    s_last_truncated = 0;
    for (;;) {
        ssize_t rd;
        if (n + 1 >= sizeof resp) { s_last_truncated = 1; break; }
        if (wait_ready(fd, 0, SC_IO_MS) <= 0) break;
        rd = read(fd, resp + n, sizeof resp - 1 - n);
        if (rd <= 0) break;
        n += (size_t)rd;
    }
    close(fd);
    resp[n] = 0;
    if (strncmp(resp, "HTTP/1.", 7)) return NULL;
    p = strstr(resp, "\r\n\r\n");
    if (!p) return NULL;
    p += 4;
    /*
     * mihomo streams some endpoints (/configs, /group — confirmed via
     * `curl -i`) as Transfer-Encoding: chunked instead of Content-Length.
     * Handing that straight to json_get() mixes hex chunk-size/CRLF framing
     * into the JSON and silently breaks parsing on exactly those endpoints
     * (2026-09-17, found chasing an empty "查看组" list on-device).
     */
    {
        const char *te = ci_find(resp, p - 4, "transfer-encoding:");
        if (te && ci_find(te, p - 4, "chunked"))
            dechunk(p, n - (size_t)(p - resp));
    }
    return p;
}

/* 组名含空格和 emoji，必须百分号编码才能进请求行 */
static void urlenc(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~')
            out[o++] = (char)*p;
        else
            o += (size_t)snprintf(out + o, cap - o, "%%%02X", *p);
    }
    out[o] = 0;
}

static void human(long v, char *out, size_t cap)
{
    if (v >= 1073741824L) snprintf(out, cap, "%.1fG", (double)v / 1073741824.0);
    else if (v >= 1048576L) snprintf(out, cap, "%ldM", v / 1048576L);
    else if (v >= 1024L) snprintf(out, cap, "%ldK", v / 1024L);
    else snprintf(out, cap, "%ldB", v);
}


/*
 * 设备上的 CJK 字体没有 emoji 字形，节点名里的 🇯🇵/🚀 会渲染成豆腐块。
 * 国旗由两个「区域指示符」(U+1F1E6..U+1F1FF) 组成，正好一一对应 A..Z，
 * 所以还原成国家码字母（🇯🇵 -> JP）既能显示又不丢信息；其余 emoji 直接丢掉。
 */
static int utf8_next(const unsigned char *p, unsigned *cp)
{
    if (p[0] < 0x80) { *cp = p[0]; return p[0] ? 1 : 0; }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *cp = ((unsigned)(p[0] & 0x1F) << 6) | (unsigned)(p[1] & 0x3F); return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = ((unsigned)(p[0] & 0x0F) << 12) | ((unsigned)(p[1] & 0x3F) << 6)
            | (unsigned)(p[2] & 0x3F); return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
        (p[3] & 0xC0) == 0x80) {
        *cp = ((unsigned)(p[0] & 0x07) << 18) | ((unsigned)(p[1] & 0x3F) << 12)
            | ((unsigned)(p[2] & 0x3F) << 6) | (unsigned)(p[3] & 0x3F); return 4;
    }
    *cp = p[0];
    return 1;                       /* 非法序列：当单字节跳过 */
}

static int cp_is_emoji(unsigned cp)
{
    return (cp >= 0x1F000 && cp <= 0x1FAFF) ||   /* 各 emoji 区 */
           (cp >= 0x2600  && cp <= 0x27BF)  ||   /* 杂项符号 + 装饰符 */
           (cp >= 0x2B00  && cp <= 0x2BFF)  ||
           (cp >= 0xFE00  && cp <= 0xFE0F)  ||   /* 变体选择符 */
            cp == 0x200D;                        /* 零宽连接符 */
}

static void sanitize_name(const char *in, char *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)in;
    size_t o = 0;
    int prev_space = 1;             /* 用来吃掉 emoji 去掉后留下的连续空格 */

    while (*p && o + 8 < cap) {
        unsigned cp = 0;
        int len = utf8_next(p, &cp);
        if (len <= 0) break;
        if (cp >= 0x1F1E6 && cp <= 0x1F1FF) {           /* 区域指示符 -> 字母 */
            out[o++] = (char)('A' + (int)(cp - 0x1F1E6));
            prev_space = 0;
        } else if (cp_is_emoji(cp)) {
            /* 丢弃 */
        } else if (cp == ' ') {
            if (!prev_space) { out[o++] = ' '; prev_space = 1; }
        } else {
            memcpy(out + o, p, (size_t)len);
            o += (size_t)len;
            prev_space = 0;
        }

        p += len;
    }
    while (o > 0 && out[o - 1] == ' ') o--;             /* 去掉尾部空格 */
    out[o] = 0;

    /*
     * 国旗还原出的国家码常常和名字本身重复（🇯🇵 JP2-… -> "JP JP2-…"）。
     * 320px 宽的列表本来就要截断，重复前缀是纯浪费——名字里已经带了就去掉。
     */
    if (o > 3 && out[2] == ' ' &&
        out[0] >= 'A' && out[0] <= 'Z' && out[1] >= 'A' && out[1] <= 'Z') {
        const char *rest = out + 3;
        if ((rest[0] & 0x80) == 0 && (rest[1] & 0x80) == 0 &&
            (rest[0] | 0x20) == (out[0] | 0x20) &&
            (rest[1] | 0x20) == (out[1] | 0x20)) {
            memmove(out, out + 3, (size_t)(o - 3) + 1);
            o -= 3;
        }
    }

    if (!o) snprintf(out, cap, "-");                    /* 全是 emoji 的名字 */
}

static void pair_add(const char *grp, const char *node, long up, long down)
{
    if (!grp || !grp[0] || !node || !node[0]) return;
    for (int i = 0; i < s_stat_pair_n; i++) {
        if (!strcmp(s_stat_pair[i].grp_raw, grp) && !strcmp(s_stat_pair[i].node_raw, node)) {
            s_stat_pair[i].up += up;
            s_stat_pair[i].down += down;
            return;
        }
    }
    if (s_stat_pair_n >= SC_MAX_STAT_PAIR) return;   /* 长尾丢掉，top N 排序不受影响 */
    snprintf(s_stat_pair[s_stat_pair_n].grp_raw, SC_NAME_MAX, "%s", grp);
    snprintf(s_stat_pair[s_stat_pair_n].node_raw, SC_NAME_MAX, "%s", node);
    s_stat_pair[s_stat_pair_n].up = up;
    s_stat_pair[s_stat_pair_n].down = down;
    s_stat_pair_n++;
}

/* 按连接 id 求这一轮相对上一轮的增量（新连接则增量就是当前值本身）。 */
static void track_update(const char *id, long up, long down, long *dup, long *ddown)
{
    for (int i = 0; i < s_track_n; i++) {
        if (!strcmp(s_track[i].id, id)) {
            long u = up - s_track[i].up, d = down - s_track[i].down;
            *dup = u > 0 ? u : 0;
            *ddown = d > 0 ? d : 0;
            s_track[i].up = up; s_track[i].down = down; s_track[i].seen = 1;
            return;
        }
    }
    *dup = up; *ddown = down;
    if (s_track_n < SC_MAX_TRACK) {
        snprintf(s_track[s_track_n].id, sizeof s_track[s_track_n].id, "%s", id);
        s_track[s_track_n].up = up; s_track[s_track_n].down = down; s_track[s_track_n].seen = 1;
        s_track_n++;
    }
}

/* 轮询结束后调用：这一轮没见到的连接已经关了，摘出追踪表防止它一直占着
 * 位置——它关闭前最后一次的增量在上一轮已经记过账，这里只是清理。 */
static void track_prune(void)
{
    int w = 0;
    for (int i = 0; i < s_track_n; i++) {
        if (s_track[i].seen) {
            if (w != i) s_track[w] = s_track[i];
            s_track[w].seen = 0;
            w++;
        }
    }
    s_track_n = w;
}

/* 取数组里第一个/最后一个带引号的字符串（用于 "chains":["a","b","c"] 这种，
 * 不需要真正的 JSON 解析——parse_all_array() 已经证明这个套路在这个项目里
 * 对 mihomo 的输出够用，字段里不会出现转义引号）。last=0 取第一个（真实出口
 * 节点），last=1 取数组里最后一个逗号之后的那个（命中的分流组）。*/
static void chains_pick(const char *arr, int last, char *out, size_t cap)
{
    const char *q = arr;
    out[0] = 0;
    /* 正向扫描，不用 memrchr——这个项目连标准库里 musl 有的非标准扩展
     * （strcasestr）都特意不用，理由写在上面 ci_find() 那段注释里，这里
     * 跟着同一个规矩。last=0 拿到第一个就回；last=1 每找到一个就覆盖
     * out，扫完剩的就是最后一个——单元素数组两种取法结果一样，不用
     * 特判。 */
    for (;;) {
        const char *e;
        size_t l;
        q = strchr(q, '"');
        if (!q) break;
        e = strchr(q + 1, '"');
        if (!e) break;
        l = (size_t)(e - q - 1);
        if (l >= cap) l = cap - 1;
        memcpy(out, q + 1, l);
        out[l] = 0;
        if (!last) return;
        q = e + 1;
    }
}

static int pair_cmp(const void *a, const void *b)
{
    const sc_pair_t *x = a, *y = b;
    long sx = x->up + x->down, sy = y->up + y->down;
    return sy > sx ? 1 : (sy < sx ? -1 : 0);
}

static void pair_finish(void)
{
    qsort(s_stat_pair, (size_t)s_stat_pair_n, sizeof s_stat_pair[0], pair_cmp);
    s_top_pair_n = s_stat_pair_n < SC_TOP_SHOW ? s_stat_pair_n : SC_TOP_SHOW;
    for (int i = 0; i < s_top_pair_n; i++) {
        char gd[SC_NAME_MAX], nd[SC_NAME_MAX], ds[24], us[24];
        sanitize_name(s_stat_pair[i].grp_raw, gd, sizeof gd);
        sanitize_name(s_stat_pair[i].node_raw, nd, sizeof nd);
        snprintf(s_top_pair[i].name, sizeof s_top_pair[i].name,
                 "%s \xE2\x86\x92 %s", gd, nd);
        human(s_stat_pair[i].down, ds, sizeof ds);
        human(s_stat_pair[i].up, us, sizeof us);
        snprintf(s_top_pair[i].traffic, sizeof s_top_pair[i].traffic,
                 "\xE2\x86\x93%s \xE2\x86\x91%s", ds, us);
        s_top_pair[i].bytes = s_stat_pair[i].up + s_stat_pair[i].down;
    }
}

int chill_top_pair_count(void) { return s_top_pair_n; }
void chill_get_top_pair(int i, chill_traffic_item_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (i < 0 || i >= s_top_pair_n) return;
    *out = s_top_pair[i];
}

/* "a","b","c" -> s_nodes[]. Keeps the order the API returned. */
static void parse_all_array(const char *arr)
{
    /*
     * 刷新每 2 秒跑一次，而一轮组测延迟要好几秒——如果这里无条件清零，
     * 测完的结果会在下一次刷新时立刻被抹掉，界面上永远看不到延迟。
     * 所以按原名把已有结果继承过来，只有新出现的节点才置为「未测」。
     */
    static char old_names[SC_MAX_NODE][SC_NAME_MAX];
    static int  old_delay[SC_MAX_NODE];
    int old_count = s_node_count;
    const char *p = arr;

    memcpy(old_names, s_nodes_raw, sizeof old_names);
    memcpy(old_delay, s_node_delay, sizeof old_delay);
    s_node_count = 0;
    while (*p && s_node_count < SC_MAX_NODE) {
        const char *q, *e;
        q = strchr(p, '"');
        if (!q) break;
        e = strchr(q + 1, '"');
        if (!e) break;
        {
            char raw[SC_NAME_MAX];
            size_t l = (size_t)(e - q - 1);
            if (l >= SC_NAME_MAX) l = SC_NAME_MAX - 1;
            memcpy(raw, q + 1, l);
            raw[l] = 0;
            /* 原名留着给 API 用，显示名去掉 emoji */
            snprintf(s_nodes_raw[s_node_count], SC_NAME_MAX, "%s", raw);
            sanitize_name(raw, s_nodes[s_node_count], SC_NAME_MAX);
            s_node_delay[s_node_count] = -1;
            for (int k = 0; k < old_count; k++)
                if (!strcmp(old_names[k], raw)) { s_node_delay[s_node_count] = old_delay[k]; break; }
            s_node_count++;
        }
        p = e + 1;
    }
}

/*
 * 扫描 /group：每个组对象里 "name" 只出现一次，且 "type" 排在它之后
 * （字段是按字母序输出的），所以顺着 name -> 下一个 type 配对即可，
 * 不需要真正的 JSON 解析器。
 */
static void parse_groups(const char *j)
{
    const char *p = j;
    s_grp_count = 0;
    while (s_grp_count < SC_MAX_GRP) {
        const char *n = strstr(p, "\"name\":\"");
        const char *e, *t;
        if (!n) break;
        n += 8;
        e = strchr(n, '"');
        if (!e) break;
        {
            size_t l = (size_t)(e - n);
            if (l >= SC_NAME_MAX) l = SC_NAME_MAX - 1;
            memcpy(s_grp_raw[s_grp_count], n, l);
            s_grp_raw[s_grp_count][l] = 0;
            sanitize_name(s_grp_raw[s_grp_count], s_grp_disp[s_grp_count], SC_NAME_MAX);
        }
        s_grp_type[s_grp_count][0] = 0;
        t = strstr(e, "\"type\":\"");
        if (t) {
            const char *te;
            t += 8;
            te = strchr(t, '"');
            if (te && (size_t)(te - t) < sizeof s_grp_type[0]) {
                memcpy(s_grp_type[s_grp_count], t, (size_t)(te - t));
                s_grp_type[s_grp_count][te - t] = 0;
            }
        }
        s_grp_count++;
        p = e + 1;
    }
    /*
     * mihomo 的 /group 用 Go map 迭代顺序返回，**每次调用顺序都可能不同**。
     * 不排序的话按钮会来回跳；而且原来用「序号」记忆选中项，重排之后序号指向
     * 别的组，节点列表会跟着整个换掉——现象就是「代理组和节点一直在跳」。
     * 所以这里按名字排序保证显示稳定，并且**始终按名字重新定位**选中项。
     */
    for (int i = 1; i < s_grp_count; i++) {
        char r[SC_NAME_MAX], d[SC_NAME_MAX], ty[16];
        int j = i - 1;
        snprintf(r,  sizeof r,  "%s", s_grp_raw[i]);
        snprintf(d,  sizeof d,  "%s", s_grp_disp[i]);
        snprintf(ty, sizeof ty, "%s", s_grp_type[i]);
        while (j >= 0 && strcmp(s_grp_raw[j], r) > 0) {
            snprintf(s_grp_raw[j + 1],  SC_NAME_MAX, "%s", s_grp_raw[j]);
            snprintf(s_grp_disp[j + 1], SC_NAME_MAX, "%s", s_grp_disp[j]);
            snprintf(s_grp_type[j + 1], sizeof s_grp_type[0], "%s", s_grp_type[j]);
            j--;
        }
        snprintf(s_grp_raw[j + 1],  SC_NAME_MAX, "%s", r);
        snprintf(s_grp_disp[j + 1], SC_NAME_MAX, "%s", d);
        snprintf(s_grp_type[j + 1], sizeof s_grp_type[0], "%s", ty);
    }

    /* 选中项以名字为准，每次都重新定位；名字还在就跟着走，不在了才退回第一个 */
    s_grp_idx = -1;
    for (int i = 0; i < s_grp_count; i++)
        if (!strcmp(s_grp_raw[i], s_group)) { s_grp_idx = i; break; }
    if (s_grp_idx < 0 && s_grp_count) {
        s_grp_idx = 0;
        snprintf(s_group, sizeof s_group, "%s", s_grp_raw[0]);
    }
}

static void delay_poll(void);
static void delay_cancel(void);

/*
 * 只在调用方判定"这块屏幕当前用得到 CHILL 数据"时才真的发请求——首页卡片
 * 和 CHILL 面板页都要，别的页面（Wi-Fi 设置、短信……）不需要。之前这里叫
 * chill_refresh()，从 build_kv() 里无条件调用，等于每次渲染任何页面都会
 * 触发一轮 /configs+/group+/connections+最多 3 跳 /proxies 请求（靠 2 秒
 * 内部节流兜底，但节流窗口内只要有任何原因触发渲染——时钟跳字、Wi-Fi 客户端
 * 变化、测速——就照样打一轮，跟看没看 CHILL 页面无关）。改成跟 tailscale_poll
 * /esim_poll 一样接收 active，由主循环按 path_is_signal_home()/path_is_chill()
 * 门控（2026-09-17 设计审查提的"无条件轮询"问题）。
 *
 * active=0 时把 s_last_ms 清零而不是什么都不做：这样下次页面变回可见、
 * active 重新变 1 时会立刻发一轮新请求，而不是要等最多 2 秒的节流窗口才
 * 刷新——避免"切回 CHILL 页先看一眼旧数据"的观感。
 *
 * 返回 1 = 这次真的发了请求（无论各子请求成功与否），调用方据此决定要不要
 * 强制重绘；返回 0 = 什么都没做（不活跃，或者还在节流窗口内）。
 */
int chill_poll(int active)
{
    char path[512], enc[384], *b;
    long t = now_ms();

    load_conf();
    delay_poll();                       /* 每次渲染都收一点，不受下面的节流影响 */
    if (!active) { s_last_ms = 0; return 0; }
    if (s_last_ms && t - s_last_ms < SC_TTL_MS) return 0;
    s_last_ms = t;

    b = sc_http("GET", "/configs", NULL);
    if (!b) {
        s_online = 0;
        s_core_running = 0;
        return 1;
    }
    s_online = 1;
    s_core_running = 1;
    json_get(b, "mode", s_mode_raw, sizeof s_mode_raw);

    b = sc_http("GET", "/group", NULL);
    if (b) parse_groups(b);

    b = sc_http("GET", "/connections", NULL);
    if (b) {
        char dl[24], ul[24];
        human(json_get_int(b, "downloadTotal", 0), dl, sizeof dl);
        human(json_get_int(b, "uploadTotal", 0), ul, sizeof ul);
        snprintf(s_traffic, sizeof s_traffic, "\xE2\x86\x93%s \xE2\x86\x91%s", dl, ul);
        /* 累计值的差分即为实时速率；首次没有基准，先留空 */
        {
            long cdl = json_get_int(b, "downloadTotal", 0);
            long cul = json_get_int(b, "uploadTotal", 0);
            long dt  = t - s_prev_t;
            if (s_prev_t && dt > 0 && cdl >= s_prev_dl && cul >= s_prev_ul) {
                char ds[24], us[24];
                human((cdl - s_prev_dl) * 1000 / dt, ds, sizeof ds);
                human((cul - s_prev_ul) * 1000 / dt, us, sizeof us);
                snprintf(s_speed, sizeof s_speed,
                         "\xE2\x86\x93%s/s \xE2\x86\x91%s/s", ds, us);
            }
            s_prev_dl = cdl; s_prev_ul = cul; s_prev_t = t;
        }
        /* connections 是数组，数一下逗号级别的元素起始即可。响应大小随设备
         * 当时的活跃连接数变化，可能超过 sc_http() 的 256KB 静态缓冲区——
         * 截断时这里数到的只是缓冲区装下的那部分，是下限不是准确值，加个
         * "+" 提示而不是悄悄显示一个偏小的数字（2026-09-17 设计审查提的
         * 已知问题）。 */
        {
            const char *p = strstr(b, "\"connections\"");
            int c = 0;
            static char objbuf[2048];   /* 单条连接记录一般几百字节，2K 留够余量 */
            s_conn_direct = s_conn_proxy = 0;
            if (p && (p = strchr(p, '[')) ) {
                int depth = 0;
                const char *obj_start = NULL;
                for (const char *q = p; *q; q++) {
                    if (*q == '{') {
                        if (depth == 0) { c++; obj_start = q; }
                        depth++;
                    } else if (*q == '}') {
                        depth--;
                        if (depth == 0 && obj_start) {
                            size_t olen = (size_t)(q - obj_start) + 1;
                            /* 按节点、按分流组汇总当前连接的流量——数据就是
                             * 这一条记录自带的 chains/upload/download，不用
                             * 额外发请求，见上面那段大注释。 */
                            if (olen < sizeof objbuf) {
                                char chains_arr[768], node[SC_NAME_MAX], grp[SC_NAME_MAX], id[40];
                                memcpy(objbuf, obj_start, olen);
                                objbuf[olen] = 0;
                                if (json_get(objbuf, "chains", chains_arr, sizeof chains_arr) &&
                                    json_get(objbuf, "id", id, sizeof id)) {
                                    chains_pick(chains_arr, 0, node, sizeof node);
                                    chains_pick(chains_arr, 1, grp, sizeof grp);
                                    if (node[0] && id[0]) {
                                        long up = json_get_int(objbuf, "upload", 0);
                                        long down = json_get_int(objbuf, "download", 0);
                                        long dup, ddown;
                                        track_update(id, up, down, &dup, &ddown);
                                        if (dup || ddown) pair_add(grp, node, dup, ddown);
                                        /* chains 的第一项是实际出口：DIRECT 即直连，其余都算走了代理 */
                                        if (!strcmp(node, "DIRECT")) s_conn_direct++;
                                        else s_conn_proxy++;
                                    }
                                }
                            }
                            obj_start = NULL;
                        }
                    } else if (*q == ']' && depth == 0) {
                        break;
                    }
                }
            }
            s_conns_truncated = s_last_truncated;
            snprintf(s_conns, sizeof s_conns, s_conns_truncated ? "%d+" : "%d", c);
            track_prune();
            pair_finish();
        }
    }

    urlenc(s_group, enc, sizeof enc);
    snprintf(path, sizeof path, "/proxies/%s", enc);
    b = sc_http("GET", path, NULL);
    if (b) {
        char arr[8192];
        char rawnow[SC_NAME_MAX] = "";
        int have_now = json_get(b, "now", rawnow, sizeof rawnow);

        /*
         * 顺序要紧：sc_http() 返回静态缓冲区，下面追链路会再次调用它并覆盖 b。
         * 所以本次响应要用的东西必须在发下一个请求之前全部取完。
         * （这里踩过一次：先追链路，导致节点列表读不到、界面空白。）
         */
        if (json_get(b, "all", arr, sizeof arr)) parse_all_array(arr);
        if (have_now) sanitize_name(rawnow, s_node, sizeof s_node);
        b = NULL;                       /* 明确失效，避免后面误用 */

        if (have_now) {
            char disp[SC_NAME_MAX];
            int o;
            /*
             * 主组常常指向另一个组（如 SNTP），此时 "now" 不是真实出口。
             * 顺着 now 往下追最多 3 层，把真正落地的节点显示出来。
             */
            sanitize_name(rawnow, disp, sizeof disp);
            o = snprintf(s_chain, sizeof s_chain, "%s", disp);
            s_chain_n = 0;
            snprintf(s_chain_raw[s_chain_n++], SC_NAME_MAX, "%s", rawnow);
            for (int hop = 0; hop < 3; hop++) {
                char enc2[384], p2[512], *b2, nxt[SC_NAME_MAX];
                urlenc(rawnow, enc2, sizeof enc2);
                snprintf(p2, sizeof p2, "/proxies/%s", enc2);
                b2 = sc_http("GET", p2, NULL);
                if (!b2 || !strstr(b2, "\"all\"")) break;   /* 不是组，到底了 */
                if (!json_get(b2, "now", nxt, sizeof nxt) || !nxt[0]) break;
                if (!strcmp(nxt, rawnow)) break;             /* 自指，防死循环 */
                snprintf(rawnow, sizeof rawnow, "%s", nxt);
                if (s_chain_n < 4) snprintf(s_chain_raw[s_chain_n++], SC_NAME_MAX, "%s", rawnow);
                sanitize_name(rawnow, disp, sizeof disp);
                o += snprintf(s_chain + o, sizeof s_chain - (size_t)o,
                              " \xE2\x86\x92 %s", disp);
                if (o >= (int)sizeof s_chain - 32) break;
            }
        }
    }
    return 1;
}

const char *chill_core(void)    { return s_core_running ? "\xE8\xBF\x90\xE8\xA1\x8C\xE4\xB8\xAD" : "\xE5\xB7\xB2\xE5\x81\x9C\xE6\xAD\xA2"; }
const char *chill_mode_raw(void){ return s_mode_raw; }
const char *chill_group(void)
{
    static char disp[SC_NAME_MAX];
    sanitize_name(s_group, disp, sizeof disp);
    return disp;
}
const char *chill_node(void)    { return s_node[0] ? s_node : "-"; }
const char *chill_traffic(void) { return s_traffic[0] ? s_traffic : "-"; }
const char *chill_speed(void)   { return s_speed[0] ? s_speed : "-"; }
const char *chill_chain(void)   { return s_chain[0] ? s_chain : "-"; }
const char *chill_conns(void)   { return s_conns[0] ? s_conns : "0"; }
int         chill_online(void)  { return s_online; }

const char *chill_mode(void)
{
    if (!strcmp(s_mode_raw, "rule"))   return "\xE8\xA7\x84\xE5\x88\x99";
    if (!strcmp(s_mode_raw, "global")) return "\xE5\x85\xA8\xE5\xB1\x80";
    if (!strcmp(s_mode_raw, "direct")) return "\xE7\x9B\xB4\xE8\xBF\x9E";
    return "-";
}

/*
 * 首页/锁屏卡片，跟 tailscale_card_html() 是同一个模式：纯格式化，不发请求
 * ——数据是不是新鲜由主循环里门控的 chill_poll() 决定，这里只管拼 HTML。
 * 复用 .card/.q-good/.q-mid/.q-bad/.q-off/.ts-st/.kv-l/.val 这些已有样式，
 * 不额外加 CSS。
 *
 * 锁屏（locked=1）只露运行状态，模式/节点/速率这些藏起来——跟 Tailscale
 * 卡片同一个安全考虑：锁屏时是给别人看的，代理细节不该露。
 */
#define SC_CARD_APPEND(...) do { \
        if (o < (int)sizeof s_card) { \
            int w_ = snprintf(s_card + o, sizeof s_card - (size_t)o, __VA_ARGS__); \
            if (w_ > 0) o += w_; \
        } \
    } while (0)

const char *chill_card_html(int locked)
{
    int o = 0;
    const char *cls = s_online ? "q-good" : "q-off";

    SC_CARD_APPEND("<div class='card'><div class='title'>CHILL");
    SC_CARD_APPEND("<span class='r ts-st %s'>%s</span></div>", cls, chill_core());
    if (!s_online) { SC_CARD_APPEND("</div>"); return s_card; }
    if (!locked) {
        SC_CARD_APPEND("<table>");
        SC_CARD_APPEND("<tr><td class='kv-l'>\xE6\xA8\xA1\xE5\xBC\x8F</td><td class='val'>%s</td></tr>", chill_mode());
        if (s_node[0])
            SC_CARD_APPEND("<tr><td class='kv-l'>\xE8\x8A\x82\xE7\x82\xB9</td><td class='val'>%s</td></tr>", s_node);
        SC_CARD_APPEND("<tr><td class='kv-l'>\xE9\x80\x9F\xE7\x8E\x87</td><td class='val'>%s</td></tr>", chill_speed());
        SC_CARD_APPEND("</table>");
    }
    SC_CARD_APPEND("</div>");
    return s_card;
}

const char *chill_conn_split(void)
{
    static char out[48];
    snprintf(out, sizeof out, "\xE4\xBB\xA3\xE7\x90\x86 %d \xC2\xB7 \xE7\x9B\xB4\xE8\xBF\x9E %d",
             s_conn_proxy, s_conn_direct);   /* 代理 N · 直连 M */
    return out;
}

const char *chill_grouplist_html(void)
{
    int o = 0;
    s_grphtml[0] = 0;
    for (int i = 0; i < s_grp_count && o < (int)sizeof s_grphtml - 256; i++) {
        /* URLTest / Fallback 组不能手动选节点，标灰提示 */
        int sel = !strcmp(s_grp_type[i], "Selector");
        /* 流量实际经过的组标个点，和「正在查看」区分开——这两件事很容易混 */
        int on_path = 0;
        for (int k = 0; k < s_chain_n; k++)
            if (!strcmp(s_chain_raw[k], s_grp_raw[i])) { on_path = 1; break; }
        o += snprintf(s_grphtml + o, sizeof s_grphtml - (size_t)o,
                      "<a href='act:scgrp:%d' class='sc-grp%s%s'>%s%s</a>",
                      i, i == s_grp_idx ? " cur" : "", sel ? "" : " ro",
                      on_path ? "\xE2\x97\x8F " : "", s_grp_disp[i]);
    }
    if (!o) snprintf(s_grphtml, sizeof s_grphtml,
                     "<div class='sc-empty'>\xE6\x9C\xAA\xE8\xAF\xBB\xE5\x88\xB0\xE4\xBB\xA3\xE7\x90\x86\xE7\xBB\x84</div>");
    return s_grphtml;
}

int chill_select_group(int index)
{
    if (index < 0 || index >= s_grp_count) return 0;
    s_grp_idx = index;
    snprintf(s_group, sizeof s_group, "%s", s_grp_raw[index]);
    s_node_count = 0;          /* 换组了，旧节点和延迟都作废 */
    s_last_ms = 0;
    delay_cancel();            /* 正在跑的测延迟结果属于旧组，丢掉 */
    return 1;
}

/* 当前组是否可手动选节点（URLTest/Fallback 由内核自己挑） */
int chill_group_selectable(void)
{
    if (s_grp_idx < 0 || s_grp_idx >= s_grp_count) return 1;
    return !strcmp(s_grp_type[s_grp_idx], "Selector");
}

int chill_group_count(void) { return s_grp_count; }

void chill_get_group(int i, chill_group_info_t *out)
{
    if (!out) return;
    out->name[0] = 0;
    out->selected = 0;
    out->auto_pick = 0;
    if (i < 0 || i >= s_grp_count) return;
    snprintf(out->name, sizeof out->name, "%s", s_grp_disp[i]);
    out->selected = (i == s_grp_idx);
    out->auto_pick = strcmp(s_grp_type[i], "Selector") != 0;
}

int chill_node_count(void) { return s_node_count; }

void chill_get_node(int i, chill_node_info_t *out)
{
    if (!out) return;
    out->name[0] = 0;
    out->delay = -1;
    out->selected = 0;
    if (i < 0 || i >= s_node_count) return;
    snprintf(out->name, sizeof out->name, "%s", s_nodes[i]);
    out->delay = s_node_delay[i];
    out->selected = !strcmp(s_nodes[i], s_node);
}

const char *chill_nodelist_html(void)
{
    int o = 0;
    s_listhtml[0] = 0;
    if (!s_online) {
        snprintf(s_listhtml, sizeof s_listhtml,
                 "<div class='sc-empty'>\xE5\x86\x85\xE6\xA0\xB8\xE6\x9C\xAA\xE8\xBF\x90\xE8\xA1\x8C</div>");
        return s_listhtml;
    }
    if (!chill_group_selectable())
        o += snprintf(s_listhtml + o, sizeof s_listhtml - (size_t)o,
                      "<div class='sc-note'>\xE6\xAD\xA4\xE7\xBB\x84\xE7\x94\xB1\xE5\x86\x85\xE6\xA0\xB8"
                      "\xE8\x87\xAA\xE5\x8A\xA8\xE6\x8C\x91\xE9\x80\x89\xEF\xBC\x8C\xE7\x82\xB9\xE5\x87\xBB"
                      "\xE6\x97\xA0\xE6\x95\x88\xE3\x80\x82\xE8\xA6\x81\xE6\x94\xB9\xE5\x87\xBA\xE5\x8F\xA3"
                      "\xEF\xBC\x8C\xE8\xAF\xB7\xE5\x9B\x9E\xE5\x88\xB0\xE4\xB8\xBB\xE9\x80\x89\xE6\x8B\xA9"
                      "\xE5\x99\xA8\xE7\xBB\x84</div>");
    for (int i = 0; i < s_node_count && o < (int)sizeof s_listhtml - 512; i++) {
        int cur = !strcmp(s_nodes[i], s_node);
        int ms = s_node_delay[i];
        const char *dcls = "";
        char d[24] = "";
        if (ms > 0) {
            snprintf(d, sizeof d, "%dms", ms);
            dcls = ms < 150 ? " ok" : (ms < 400 ? " mid" : " bad");   /* 快/中/慢 */
        } else if (ms == 0) {
            snprintf(d, sizeof d, "\xE8\xB6\x85\xE6\x97\xB6");   /* 超时 */
            dcls = " bad";
        }                                                              /* -1 = 未测，留空 */
        if (chill_group_selectable())
            o += snprintf(s_listhtml + o, sizeof s_listhtml - (size_t)o,
                          "<a href='act:scnode:%d' class='sc-node%s'>"
                          "<span class='sc-nm'>%s</span><span class='sc-dl%s'>%s</span></a>",
                          i, cur ? " cur" : "", s_nodes[i], dcls, d);
        else                     /* 自动组：只展示，不给点击 */
            o += snprintf(s_listhtml + o, sizeof s_listhtml - (size_t)o,
                          "<div class='sc-node ro%s'>"
                          "<span class='sc-nm'>%s</span><span class='sc-dl%s'>%s</span></div>",
                          cur ? " cur" : "", s_nodes[i], dcls, d);
    }
    if (!o) snprintf(s_listhtml, sizeof s_listhtml,
                     "<div class='sc-empty'>\xE6\x9C\xAA\xE8\xAF\xBB\xE5\x88\xB0\xE8\x8A\x82\xE7\x82\xB9</div>");
    return s_listhtml;
}

int chill_set_mode(const char *mode)
{
    char body[64];
    int ok;
    if (!mode || !*mode) return 0;
    snprintf(body, sizeof body, "{\"mode\":\"%s\"}", mode);
    ok = sc_send_async("PATCH", "/configs", body);
    s_last_ms = 0;              /* 强制下次刷新 */
    return ok;
}

int chill_select_node(int index)
{
    char enc[384], path[512], body[256];
    int ok;
    if (index < 0 || index >= s_node_count) return 0;
    urlenc(s_group, enc, sizeof enc);
    snprintf(path, sizeof path, "/proxies/%s", enc);
    snprintf(body, sizeof body, "{\"name\":\"%s\"}", s_nodes_raw[index]);
    ok = sc_send_async("PUT", path, body);
    s_last_ms = 0;
    return ok;
}

/*
 * 重启内核会断掉所有连接，误触代价大，所以照搬电源菜单的两段式确认：
 * 第一次点亮起「再按一次」，4 秒内再点才真的执行。
 */
static long s_restart_arm_ms;

int chill_restart_armed(void)
{
    if (!s_restart_arm_ms) return 0;
    if (now_ms() - s_restart_arm_ms > 4000) { s_restart_arm_ms = 0; return 0; }
    return 1;
}

int chill_restart_core(void)
{
    int rc;
    if (!chill_restart_armed()) { s_restart_arm_ms = now_ms(); return 0; }
    s_restart_arm_ms = 0;
    rc = system("/etc/init.d/shellcrash restart >/dev/null 2>&1");
    s_last_ms = 0;
    return rc == 0;
}

/*
 * 一键退回原厂/注入版 UI。
 *
 * 关键在顺序：DevUI 持有 DRM 主控权，原厂 UI 在我们退出之前抢不到 /dev/dri/card0。
 * 所以不能「先拉起再退出」，只能派一个后台子进程延迟启动，自己立刻退出释放显示设备。
 * 子进程会被 init 收养，不受我们退出影响。
 *
 * 万一原厂 UI 起不来，两个 UI 都不在，厂商的 zte_topsw_daemon 会把整机重启——
 * 重启后 rc.local 照常拉起原厂/注入版 UI，所以最坏代价是一次重启，不会变砖。
 *
 * 返回 1 = 后台启动已排好，调用方可以退出进程。
 */
/* 面板安装方向：1 = 需要转 180°（U60），0 = 正装（TopFlow）。 */
int devui_rotate180(void)
{
    load_conf();
    return s_rot180;
}

int devui_restore_stock(void)
{
    char cmd[400];

    load_conf();
    snprintf(cmd, sizeof cmd,
             "( sleep 2; %s ) >/dev/null 2>&1 &", s_restore_cmd);
    return system(cmd) >= 0;
}

/*
 * ---- 组测延迟：异步 ----
 * 组测延迟要等最慢的那个节点（timeout=3000 ⇒ 最长 3 秒才回包），而 sc_http 的读超时
 * 是 SC_IO_MS = 1.5 秒：同步等会在结果回来之前放弃 ⇒ 界面报「测试失败」（实测：
 * 一半节点超时时必现）。又不能让 UI 线程干等 3 秒，所以连上、发完请求后把 socket 留着，
 * 每次 chill_refresh() 顺手非阻塞地收一点，对端关闭（Connection: close）就解析。
 */
#define SC_DELAY_MAX_MS 8000
static int    s_delay_fd = -1;
static long   s_delay_t0;
static char   s_delay_buf[16384];
static size_t s_delay_n;

static void delay_cancel(void)
{
    if (s_delay_fd >= 0) close(s_delay_fd);
    s_delay_fd = -1;
    s_delay_n = 0;
}

/* 返回 {"节点名":延迟,...}；没出现在结果里的节点就是超时（内核只回成功的） */
static void delay_apply(const char *b)
{
    for (int i = 0; i < s_node_count; i++) {
        char key[SC_NAME_MAX + 4];
        const char *p;
        snprintf(key, sizeof key, "\"%s\"", s_nodes_raw[i]);
        p = strstr(b, key);
        if (!p) { s_node_delay[i] = 0; continue; }
        p = strchr(p + strlen(key), ':');
        s_node_delay[i] = p ? atoi(p + 1) : 0;
    }
}

static void delay_poll(void)
{
    if (s_delay_fd < 0) return;
    for (;;) {
        ssize_t rd;
        if (s_delay_n + 1 >= sizeof s_delay_buf) break;        /* 满了：当收完处理 */
        rd = read(s_delay_fd, s_delay_buf + s_delay_n, sizeof s_delay_buf - 1 - s_delay_n);
        if (rd > 0) { s_delay_n += (size_t)rd; continue; }
        if (rd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            long waited = now_ms() - s_delay_t0;
            if (waited > SC_DELAY_MAX_MS) {
                fprintf(stderr, "chill: delay test gave up after %ld ms\n", waited);
                delay_cancel();
            }
            return;                                             /* 还没收完，下次再来 */
        }
        break;                                                  /* 0 = 对端关闭；<0 = 出错 */
    }
    s_delay_buf[s_delay_n] = 0;
    {
        const char *p = strncmp(s_delay_buf, "HTTP/1.", 7) ? NULL : strstr(s_delay_buf, "\r\n\r\n");
        if (p) {
            delay_apply(p + 4);
            fprintf(stderr, "chill: delay test done in %ld ms (%zu bytes)\n",
                    now_ms() - s_delay_t0, s_delay_n);
        } else {
            fprintf(stderr, "chill: delay test got no HTTP response\n");
        }
    }
    delay_cancel();
}

int chill_delay_pending(void)
{
    return s_delay_fd >= 0;
}

int chill_test_delay(void)
{
    char enc[384], req[1024];
    int fd;

    if (s_delay_fd >= 0) return 2;                              /* 上一轮还没回来 */
    fd = sc_connect();
    if (fd < 0) return 0;
    urlenc(s_group, enc, sizeof enc);
    snprintf(req, sizeof req,
             "GET /group/%s/delay?url=http%%3A%%2F%%2Fwww.gstatic.com%%2Fgenerate_204&timeout=3000 HTTP/1.1\r\n"
             "Host: %s:%d\r\n%s%s%s"
             "Connection: close\r\n\r\n",
             enc, s_host, s_port,
             s_secret[0] ? "Authorization: Bearer " : "", s_secret, s_secret[0] ? "\r\n" : "");
    if (write(fd, req, strlen(req)) < 0) { close(fd); return 0; }
    set_nonblock(fd, 1);
    s_delay_fd = fd;
    s_delay_n = 0;
    s_delay_t0 = now_ms();
    fprintf(stderr, "chill: delay test started for group %s\n", s_group);
    return 1;
}
