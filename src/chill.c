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
#define SC_MAX_NODE 64
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
static int  s_core_running;

static char s_nodes[SC_MAX_NODE][SC_NAME_MAX];      /* 显示用（已去 emoji） */
static char s_nodes_raw[SC_MAX_NODE][SC_NAME_MAX];  /* 原名，调 API 时用 */
static int  s_node_delay[SC_MAX_NODE];   /* -1 = 未测 */
static int  s_node_count;

static char s_listhtml[24576];
static char s_grphtml[4096];

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
static char *sc_http(const char *method, const char *path, const char *body)
{
    static char resp[SC_RESP_MAX];
    char req[1024];
    size_t n = 0;
    char *p;
    int fd = sc_connect();

    if (fd < 0) return NULL;
    if (body)
        snprintf(req, sizeof req,
                 "%s %s HTTP/1.1\r\nHost: %s:%d\r\n%s%s%s"
                 "Content-Type: application/json\r\nContent-Length: %d\r\n"
                 "Connection: close\r\n\r\n%s",
                 method, path, s_host, s_port,
                 s_secret[0] ? "Authorization: Bearer " : "", s_secret, s_secret[0] ? "\r\n" : "",
                 (int)strlen(body), body);
    else
        snprintf(req, sizeof req,
                 "%s %s HTTP/1.1\r\nHost: %s:%d\r\n%s%s%s"
                 "Connection: close\r\n\r\n",
                 method, path, s_host, s_port,
                 s_secret[0] ? "Authorization: Bearer " : "", s_secret, s_secret[0] ? "\r\n" : "");

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

void chill_refresh(void)
{
    char path[512], enc[384], *b;
    long t = now_ms();

    load_conf();
    delay_poll();                       /* 每次渲染都收一点，不受下面的节流影响 */
    if (s_last_ms && t - s_last_ms < SC_TTL_MS) return;
    s_last_ms = t;

    b = sc_http("GET", "/configs", NULL);
    if (!b) {
        s_online = 0;
        s_core_running = 0;
        return;
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
        /* connections 是数组，数一下逗号级别的元素起始即可 */
        {
            const char *p = strstr(b, "\"connections\"");
            int c = 0;
            if (p && (p = strchr(p, '[')) ) {
                int depth = 0;
                for (const char *q = p; *q; q++) {
                    if (*q == '{' ) { if (depth == 0) c++; depth++; }
                    else if (*q == '}') depth--;
                    else if (*q == ']' && depth == 0) break;
                }
            }
            snprintf(s_conns, sizeof s_conns, "%d", c);
            /* chains 的第一项是实际出口：DIRECT 即直连，其余都算走了代理 */
            s_conn_direct = s_conn_proxy = 0;
            for (const char *q = b; (q = strstr(q, "\"chains\":[")) != NULL; ) {
                q += 10;
                while (*q == ' ') q++;
                if (!strncmp(q, "\"DIRECT\"", 8)) s_conn_direct++;
                else if (*q == '"') s_conn_proxy++;
            }
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
    char *b;
    if (!mode || !*mode) return 0;
    snprintf(body, sizeof body, "{\"mode\":\"%s\"}", mode);
    b = sc_http("PATCH", "/configs", body);
    s_last_ms = 0;              /* 强制下次刷新 */
    return b != NULL;
}

int chill_select_node(int index)
{
    char enc[384], path[512], body[256];
    char *b;
    if (index < 0 || index >= s_node_count) return 0;
    urlenc(s_group, enc, sizeof enc);
    snprintf(path, sizeof path, "/proxies/%s", enc);
    snprintf(body, sizeof body, "{\"name\":\"%s\"}", s_nodes_raw[index]);
    b = sc_http("PUT", path, body);
    s_last_ms = 0;
    return b != NULL;
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
