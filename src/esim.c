/*
 * esim.c - eSIM 切换页：调本机 zte-agent 的 eSIM 接口，只做「列出 + 切换」。
 *
 * 为什么不直接跑 lpac：切换本身只是一条 `profile enable`，难的是之后让 ZTE 协议栈
 * 认到新卡——UIM 重上电、重启 zte_topsw_mdm、等 IMSI 变化，不收敛就重启整机。
 * 这套在 zte-agent（u60p 仓库 esim.rs）里已经做好并实测过，这里再写一遍只会分叉。
 *
 * SPDX-License-Identifier: MIT
 */
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

#define ES_CONF       "/data/plugins/u60pro-devui/esim.conf"
#define ES_AGENT_SH   "/data/local/tmp/start_zte_agent.sh"
/* procd 装法（zte-agent.init）把密码放在这里，旧启动脚本可能已不存在；先读它 */
#define ES_AGENT_SH_ENV "/data/zte-agent.env"
#define ES_IO_MS      1500
#define ES_IDLE_MS    2000      /* 页面开着时查 job 的间隔 */
#define ES_JOB_MS     1000      /* 本机发起的切换进行中 */
#define ES_BACKOFF_MS 5000      /* agent 没响应时放慢，免得每轮都卡 1.5 秒 */
#define ES_ARM_MS     4000
#define ES_GIVEUP_MS  120000    /* agent 自己最长约 60 秒（enable 30s + 等收敛 30s） */
#define ES_MAX        16
#define ES_RESP_MAX   65536     /* profile 可能带 base64 图标 */

typedef struct {
    char iccid[24];
    char name[96];              /* 有备注名用备注名，否则 profileName */
    char sub[192];              /* 运营商 · 原名 · 尾号 */
    int  enabled;
} es_prof_t;

static int  s_port = 9090;
static char s_pass[128];
static char s_token[80];
static int  s_conf_loaded;

static es_prof_t s_prof[ES_MAX];
static int  s_count;
static int  s_loaded;         /* a profile list has been read at least once */
static char s_err[96];          /* 读列表失败的原因，"" = 正常 */
static int  s_offline;          /* agent 没响应：列表还留着，但不给点 */

static long s_seen_id = -1;     /* 上次看到的 job；id 或状态变了就重读列表 */
static char s_seen_status[16];
static int  s_busy;             /* agent 上有 job 在跑，不管是谁发起的 */
static char s_busy_kind[16];
static long s_my_job;           /* 本机发起、还没拿到结果的 job id，0 = 无 */
static char s_target_iccid[24];
static char s_target_name[96];
static long s_t0;
static char s_msg[160];         /* 最近一次切换的结果 */
static long s_msg_ms;           /* 出结果的时间：离开页面期间出的结果，回来还要看得到 */

static char s_arm_iccid[24];
static long s_arm_ms;

static int      s_was_active;
static long     s_poll_ms;
static long     s_poll_gap = ES_IDLE_MS;
static unsigned s_sig;

static char s_statebuf[192];
static char s_listhtml[8192];

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* agent 的密码：procd 装法在 /data/zte-agent.env（ZTE_AGENT_PASSWORD=...），
 * 旧装法在启动脚本里（export ZTE_AGENT_PASSWORD='...'）。两种写法这里都认。 */
static void read_agent_password(void)
{
    static const char *paths[] = { ES_AGENT_SH_ENV, ES_AGENT_SH };
    FILE *fp = NULL;
    char line[256];

    for (size_t i = 0; i < sizeof paths / sizeof *paths && !fp; i++)
        fp = fopen(paths[i], "r");
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

static void load_conf(void)
{
    FILE *fp;
    char line[256];

    if (s_conf_loaded) return;
    s_conf_loaded = 1;
    fp = fopen(ES_CONF, "r");
    if (fp) {
        while (fgets(line, sizeof line, fp)) {
            char *nl = strpbrk(line, "\r\n");
            if (nl) *nl = 0;
            if (!strncmp(line, "port=", 5))          s_port = atoi(line + 5);
            else if (!strncmp(line, "password=", 9)) snprintf(s_pass, sizeof s_pass, "%.*s", (int)sizeof s_pass - 1, line + 9);
        }
        fclose(fp);
    }
    if (!s_pass[0]) read_agent_password();
}

/* ---- minimal HTTP（和 speedtest.c 同一套写法，多返回一个状态码用来认 401）---- */

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

static int es_connect(void)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int f, rc;

    if (fd < 0) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)s_port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    f = fcntl(fd, F_GETFL, 0);
    if (f < 0 || fcntl(fd, F_SETFL, f | O_NONBLOCK) < 0) { close(fd); return -1; }
    rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    if (rc < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    if (rc < 0 && wait_ready(fd, 1, ES_IO_MS) <= 0) { close(fd); return -1; }
    fcntl(fd, F_SETFL, f);
    return fd;
}

/*
 * 一次请求。返回 HTTP 状态码（0 = 连不上/没回包），*body 指向静态缓冲区里的正文。
 * 和 sc_http 一样：下一次调用会覆盖上一次的内容，要用的字段先取完再发下一个请求。
 *
 * 用 HTTP/1.0：这里不解分块编码，1.0 保证对端不会分块（tiny_http 本来就回
 * Content-Length，但换个服务端就不一定了）。
 */
static int es_http(const char *method, const char *path, const char *json, char **body)
{
    static char resp[ES_RESP_MAX];
    char req[1024], auth[128] = "";
    size_t n = 0;
    char *p;
    int fd;

    *body = NULL;
    fd = es_connect();
    if (fd < 0) return 0;
    if (s_token[0]) snprintf(auth, sizeof auth, "Authorization: Bearer %s\r\n", s_token);
    snprintf(req, sizeof req,
             "%s %s HTTP/1.0\r\nHost: 127.0.0.1:%d\r\n%s"
             "Content-Type: application/json\r\nContent-Length: %d\r\n"
             "Connection: close\r\n\r\n%s",
             method, path, s_port, auth, json ? (int)strlen(json) : 0, json ? json : "");
    if (write(fd, req, strlen(req)) < 0) { close(fd); return 0; }
    for (;;) {
        ssize_t rd;
        if (n + 1 >= sizeof resp) break;
        if (wait_ready(fd, 0, ES_IO_MS) <= 0) break;
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

static int es_login(void)
{
    char js[300], data[160], *b;
    size_t o = (size_t)snprintf(js, sizeof js, "{\"password\":\"");

    for (const char *p = s_pass; *p && o + 4 < sizeof js; p++) {
        if (*p == '"' || *p == '\\') js[o++] = '\\';
        js[o++] = *p;
    }
    snprintf(js + o, sizeof js - o, "\"}");
    s_token[0] = 0;
    if (es_http("POST", "/api/auth/login", js, &b) != 200 || !b) return 0;
    if (!json_get(b, "data", data, sizeof data)) return 0;
    return json_get(data, "token", s_token, sizeof s_token) && s_token[0];
}

/* 带登录的请求。401 = token 过期（agent 给 1 小时）或 agent 重启过，重登一次再试 */
static int es_api(const char *method, const char *path, const char *json, char **body)
{
    int code;

    if (!s_token[0] && s_pass[0]) es_login();
    code = es_http(method, path, json, body);
    if (code == 401 && s_pass[0] && es_login())
        code = es_http(method, path, json, body);
    return code;
}

int agent_request(const char *method, const char *path, const char *json)
{
    char *b;
    load_conf();
    return es_api(method, path, json, &b);
}

int agent_post(const char *path) { return agent_request("POST", path, NULL); }

/* ---- 数据 ---- */

/* 字符串字段；JSON 的 null 当空串 */
static void json_str(const char *obj, const char *key, char *out, size_t cap)
{
    if (!json_get(obj, key, out, cap) || !strcmp(out, "null")) out[0] = 0;
}

/*
 * profiles 是对象数组。json_get 只认第一层的键、而且会一直往后扫，
 * 所以每个对象要先临时截断成独立字符串再取字段，否则会取到下一个对象里去。
 */
static void parse_profiles(char *arr)
{
    char *p = strchr(arr, '[');
    int n = 0;

    if (!p) { s_count = 0; return; }
    while (n < ES_MAX && (p = strchr(p, '{')) != NULL) {
        es_prof_t *e = &s_prof[n];
        char pname[96], nick[96], sp[64], state[16], tail[8], save;
        char *q;
        int depth = 0, instr = 0, esc = 0;
        size_t l;

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
        json_str(p, "iccid", e->iccid, sizeof e->iccid);
        json_str(p, "profileName", pname, sizeof pname);
        json_str(p, "profileNickname", nick, sizeof nick);
        json_str(p, "serviceProviderName", sp, sizeof sp);
        json_str(p, "profileState", state, sizeof state);
        q[1] = save;
        p = q + 1;
        if (!e->iccid[0]) continue;

        e->enabled = !strcmp(state, "enabled");
        snprintf(e->name, sizeof e->name, "%s", nick[0] ? nick : (pname[0] ? pname : e->iccid));
        l = strlen(e->iccid);
        snprintf(tail, sizeof tail, "%s", e->iccid + (l > 4 ? l - 4 : 0));
        /* 有备注名时把原名也带上，不然认不出是哪张 */
        snprintf(e->sub, sizeof e->sub, "%s%s%s%s\xE5\xB0\xBE\xE5\x8F\xB7 %s",   /* 尾号 */
                 sp[0] ? sp : "", sp[0] ? " \xC2\xB7 " : "",
                 nick[0] && pname[0] ? pname : "", nick[0] && pname[0] ? " \xC2\xB7 " : "",
                 tail);
        n++;
    }
    s_count = n;
}

/* 返回 0 = agent 没响应 */
static int load_list(void)
{
    static char data[ES_RESP_MAX], arr[ES_RESP_MAX];
    char v[16], *b;
    int code = es_api("GET", "/api/esim/profiles", NULL, &b);

    if (!code || !b) {
        snprintf(s_err, sizeof s_err, "\xE8\xBF\x9E\xE4\xB8\x8D\xE4\xB8\x8A zte-agent");   /* 连不上 */
        return 0;
    }
    if (code == 401) {
        snprintf(s_err, sizeof s_err, "zte-agent \xE7\x99\xBB\xE5\xBD\x95\xE5\xA4\xB1\xE8\xB4\xA5");   /* 登录失败 */
        return 1;
    }
    /* 502 = lpac 读卡失败，多半插的是普通 SIM 卡 */
    if (code != 200 || !json_get(b, "data", data, sizeof data)) {
        snprintf(s_err, sizeof s_err, "\xE6\x9C\xAA\xE8\xAF\xBB\xE5\x88\xB0 eSIM \xE5\x8D\xA1");   /* 未读到 eSIM 卡 */
        s_count = 0;
        return 1;
    }
    if (json_get(data, "installed", v, sizeof v) && !strcmp(v, "false")) {
        snprintf(s_err, sizeof s_err, "\xE6\x9C\xAA\xE5\xAE\x89\xE8\xA3\x85 eSIM \xE7\xBB\x84\xE4\xBB\xB6");   /* 未安装 eSIM 组件 */
        s_count = 0;
        return 1;
    }
    if (json_get(data, "busy", v, sizeof v) && !strcmp(v, "true"))
        return 1;                       /* 有操作在跑，agent 不读卡；保留旧列表 */
    if (!json_get(data, "profiles", arr, sizeof arr)) {
        snprintf(s_err, sizeof s_err, "\xE6\x9C\xAA\xE8\xAF\xBB\xE5\x88\xB0 eSIM \xE5\x8D\xA1");
        s_count = 0;
        return 1;
    }
    parse_profiles(arr);
    s_err[0] = 0;
    s_loaded = 1;
    return 1;
}

int esim_loaded(void) { return s_loaded; }

/* 查 job。*reload 置 1 = 有操作刚结束，卡上的列表可能变了。返回 0 = agent 没响应 */
static int poll_job(int *reload)
{
    char data[1024], status[16] = "", msg[128] = "", reboot[8] = "", *b;
    long id;
    int code = es_api("GET", "/api/esim/job", NULL, &b);

    if (code != 200 || !b || !json_get(b, "data", data, sizeof data)) return 0;
    id = json_get_int(data, "id", 0);
    json_str(data, "status", status, sizeof status);
    json_str(data, "message", msg, sizeof msg);
    json_str(data, "kind", s_busy_kind, sizeof s_busy_kind);
    json_str(data, "rebooting", reboot, sizeof reboot);
    s_busy = !strcmp(status, "running");

    /* 任何 job 结束都算（网页端的下载/删除也会改列表） */
    if (!s_busy && s_seen_id >= 0 && (id != s_seen_id || strcmp(status, s_seen_status))) {
        *reload = 1;
        if (id != s_my_job) s_msg[0] = 0;   /* 别人的操作结束了，本机上一次的结果已经过时 */
    }
    s_seen_id = id;
    snprintf(s_seen_status, sizeof s_seen_status, "%s", status);

    if (s_my_job) {
        long secs = (now_ms() - s_t0) / 1000;
        if (id != s_my_job) {           /* agent 重启过，job 丢了 */
            snprintf(s_msg, sizeof s_msg,
                     "\xE5\x88\x87\xE6\x8D\xA2\xE7\xBB\x93\xE6\x9E\x9C\xE6\x9C\xAA\xE7\x9F\xA5\xEF\xBC\x8C"
                     "\xE8\xAF\xB7\xE7\x9C\x8B\xE5\xBD\x93\xE5\x89\x8D\xE9\x85\x8D\xE7\xBD\xAE");   /* 切换结果未知，请看当前配置 */
        } else if (!strcmp(status, "done")) {
            if (!strcmp(reboot, "true"))
                snprintf(s_msg, sizeof s_msg,
                         "\xE5\xB7\xB2\xE5\x88\x87\xE6\x8D\xA2\xEF\xBC\x8C\xE8\xAE\xBE\xE5\xA4\x87"
                         "\xE5\x8D\xB3\xE5\xB0\x86\xE9\x87\x8D\xE5\x90\xAF");   /* 已切换，设备即将重启 */
            else
                snprintf(s_msg, sizeof s_msg,
                         "\xE5\xB7\xB2\xE5\x88\x87\xE6\x8D\xA2\xE5\x88\xB0 %s\xEF\xBC\x88%ld \xE7\xA7\x92\xEF\xBC\x89",
                         s_target_name, secs);   /* 已切换到 X（N 秒） */
        } else if (!strcmp(status, "error")) {
            snprintf(s_msg, sizeof s_msg, "\xE5\x88\x87\xE6\x8D\xA2\xE5\xA4\xB1\xE8\xB4\xA5\xEF\xBC\x9A%s", msg);   /* 切换失败： */
        } else {
            return 1;                   /* 还在跑 */
        }
        fprintf(stderr, "esim: job %ld %s after %ld s: %s\n", s_my_job, status, secs, msg);
        s_msg_ms = now_ms();
        s_my_job = 0;
        *reload = 1;
    }
    return 1;
}

/* 显示内容的指纹：变了才让主循环重绘 */
static unsigned fnv(unsigned h, const char *s)
{
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    h ^= 0xFF;
    return h * 16777619u;
}

static unsigned display_sig(long t)
{
    char nums[64];
    unsigned h = 2166136261u;

    snprintf(nums, sizeof nums, "%d/%d/%d/%ld/%ld/%d", s_count, s_busy, s_offline, s_my_job,
             s_my_job ? (t - s_t0) / 1000 : 0,
             s_arm_iccid[0] && t - s_arm_ms <= ES_ARM_MS);
    h = fnv(h, nums);
    h = fnv(h, s_err);
    h = fnv(h, s_msg);
    h = fnv(h, s_busy_kind);
    h = fnv(h, s_arm_iccid);
    for (int i = 0; i < s_count; i++) {
        h = fnv(h, s_prof[i].iccid);
        h = fnv(h, s_prof[i].name);
        h = fnv(h, s_prof[i].sub);
        h = fnv(h, s_prof[i].enabled ? "1" : "0");
    }
    return h;
}

int esim_poll(int active)
{
    long t;
    int entering;
    unsigned sig;

    if (!active) s_was_active = 0;
    if (!active && !s_my_job) return 0;
    load_conf();
    t = now_ms();
    entering = active && !s_was_active;
    if (active) s_was_active = 1;
    if (entering) {
        s_arm_iccid[0] = 0;
        if (!s_my_job && t - s_msg_ms > 60000) s_msg[0] = 0;   /* 太久以前的结果别再挂着 */
    }

    if (entering || t - s_poll_ms >= (s_my_job ? ES_JOB_MS : s_poll_gap)) {
        int reload = 0, ok;
        s_poll_ms = t;
        ok = poll_job(&reload);
        if (s_my_job && t - s_t0 > ES_GIVEUP_MS) {
            s_my_job = 0;
            snprintf(s_msg, sizeof s_msg,
                     "\xE5\x88\x87\xE6\x8D\xA2\xE8\xB6\x85\xE6\x97\xB6\xEF\xBC\x8C"
                     "\xE8\xAF\xB7\xE7\x9C\x8B\xE5\xBD\x93\xE5\x89\x8D\xE9\x85\x8D\xE7\xBD\xAE");   /* 切换超时，请看当前配置 */
            s_msg_ms = t;
            reload = 1;
        }
        /*
         * 读列表要跑一次 lpac：只在刚进页面、有操作结束、或 agent 刚恢复时读。
         * 读卡失败（插的是普通 SIM）不自动重试，免得页面开着就每 2 秒去敲一次卡。
         */
        if (ok && active && (entering || reload || s_offline)) ok = load_list();
        if (!ok) {
            snprintf(s_err, sizeof s_err, "\xE8\xBF\x9E\xE4\xB8\x8D\xE4\xB8\x8A zte-agent");
            s_busy = 0;
        }
        s_offline = !ok;
        s_poll_gap = ok ? ES_IDLE_MS : ES_BACKOFF_MS;
    }

    sig = display_sig(t);
    if (sig == s_sig) return 0;
    s_sig = sig;
    return 1;
}

/* ---- 显示 ---- */

static void es_esc(char *dst, size_t cap, const char *src)
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

const char *esim_enabled_iccid(void)
{
    for (int i = 0; i < s_count; i++)
        if (s_prof[i].enabled) return s_prof[i].iccid;
    return "";
}

int esim_prefetch(const char *key)
{
    static char done[32];

    /* 每张卡只试一次，成败都算：读列表会卡界面一两秒、还要敲 eSIM 卡（有过
     * catBusy），不能失败了就反复试。换卡（ICCID 变了）或打开 eSIM 页才再读。 */
    if (!key || !key[0] || !strcmp(done, key) || s_my_job) return 0;
    snprintf(done, sizeof done, "%s", key);
    return load_list();
}

const char *esim_current(void)
{
    for (int i = 0; i < s_count; i++)
        if (s_prof[i].enabled) return s_prof[i].name;
    return "-";
}

const char *esim_state(void)
{
    char tmp[192];

    if (s_my_job)
        snprintf(tmp, sizeof tmp, "\xE5\x88\x87\xE6\x8D\xA2\xE4\xB8\xAD \xC2\xB7 \xE5\xB7\xB2 %ld \xE7\xA7\x92",
                 (now_ms() - s_t0) / 1000);   /* 切换中 · 已 N 秒 */
    else if (s_busy) {
        const char *k = !strcmp(s_busy_kind, "switch")   ? "\xE5\x88\x87\xE6\x8D\xA2" :      /* 切换 */
                        !strcmp(s_busy_kind, "download") ? "\xE4\xB8\x8B\xE8\xBD\xBD" :      /* 下载 */
                        !strcmp(s_busy_kind, "delete")   ? "\xE5\x88\xA0\xE9\x99\xA4" :      /* 删除 */
                                                           "\xE6\x93\x8D\xE4\xBD\x9C";       /* 操作 */
        snprintf(tmp, sizeof tmp, "\xE7\xBD\x91\xE9\xA1\xB5\xE7\xAB\xAF\xE6\xAD\xA3\xE5\x9C\xA8%s", k);   /* 网页端正在 */
    }
    else if (s_err[0]) snprintf(tmp, sizeof tmp, "%s", s_err);
    else if (s_msg[0]) snprintf(tmp, sizeof tmp, "%s", s_msg);
    else snprintf(tmp, sizeof tmp, "\xE5\xB0\xB1\xE7\xBB\xAA");   /* 就绪 */
    es_esc(s_statebuf, sizeof s_statebuf, tmp);
    return s_statebuf;
}

const char *esim_list_html(void)
{
    long t = now_ms();
    int armed_live = s_arm_iccid[0] && t - s_arm_ms <= ES_ARM_MS;
    int locked = s_busy || s_my_job || s_offline;
    int o = 0;

    s_listhtml[0] = 0;
    if (s_my_job) {
        char nm[192];
        es_esc(nm, sizeof nm, s_target_name);
        o += snprintf(s_listhtml + o, sizeof s_listhtml - (size_t)o,
                      "<div class='es-note'>\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x88\x87\xE6\x8D\xA2\xE5\x88\xB0 %s\xEF\xBC\x8C"
                      "\xE7\xBD\x91\xE7\xBB\x9C\xE4\xBC\x9A\xE4\xB8\xAD\xE6\x96\xAD\xE7\x89\x87\xE5\x88\xBB\xEF\xBC\x9B"
                      "\xE4\xB8\x8D\xE6\x88\x90\xE5\x8A\x9F\xE4\xBC\x9A\xE8\x87\xAA\xE5\x8A\xA8\xE9\x87\x8D\xE5\x90\xAF"
                      "\xE8\xAE\xBE\xE5\xA4\x87</div>", nm);   /* 正在切换到 X，网络会中断片刻；不成功会自动重启设备 */
    }
    if (!s_count) {
        snprintf(s_listhtml + o, sizeof s_listhtml - (size_t)o, "<div class='es-empty'>%s</div>",
                 s_err[0] ? s_err : "\xE8\xAF\xBB\xE5\x8F\x96\xE4\xB8\xAD\xE2\x80\xA6");   /* 读取中… */
        return s_listhtml;
    }
    for (int i = 0; i < s_count && o < (int)sizeof s_listhtml - 800; i++) {
        const es_prof_t *e = &s_prof[i];
        int going = s_my_job && !strcmp(s_target_iccid, e->iccid);
        int armed = armed_live && !strcmp(s_arm_iccid, e->iccid);
        const char *cls = going ? " go" : armed ? " armed" : e->enabled ? " cur" : "";
        const char *tag = going   ? "\xE5\x88\x87\xE6\x8D\xA2\xE4\xB8\xAD\xE2\x80\xA6" :   /* 切换中… */
                          armed   ? "\xE5\x86\x8D\xE7\x82\xB9\xE4\xB8\x80\xE6\xAC\xA1" :   /* 再点一次 */
                          e->enabled ? "\xE4\xBD\xBF\xE7\x94\xA8\xE4\xB8\xAD" : "";         /* 使用中 */
        char nm[192], sub[384];

        es_esc(nm, sizeof nm, e->name);
        es_esc(sub, sizeof sub, e->sub);
        if (locked)                     /* 有操作在跑：只展示，不给点 */
            o += snprintf(s_listhtml + o, sizeof s_listhtml - (size_t)o,
                          "<div class='es-prof ro%s'><span class='es-nm'>%s</span>"
                          "<span class='es-tag'>%s</span><span class='es-sub'>%s</span></div>",
                          cls, nm, tag, sub);
        else
            o += snprintf(s_listhtml + o, sizeof s_listhtml - (size_t)o,
                          "<a href='act:esim:%d' class='es-prof%s'><span class='es-nm'>%s</span>"
                          "<span class='es-tag'>%s</span><span class='es-sub'>%s</span></a>",
                          i, cls, nm, tag, sub);
    }
    return s_listhtml;
}

int esim_profile_count(void) { return s_count; }

int esim_locked(void)
{
    return s_busy || s_my_job || s_offline;
}

void esim_get_profile(int index, esim_profile_t *out)
{
    long t = now_ms();
    int armed_live = s_arm_iccid[0] && t - s_arm_ms <= ES_ARM_MS;
    const es_prof_t *e;

    memset(out, 0, sizeof *out);
    if (index < 0 || index >= s_count) return;
    e = &s_prof[index];
    snprintf(out->name, sizeof out->name, "%s", e->name);
    snprintf(out->sub, sizeof out->sub, "%s", e->sub);
    out->enabled = e->enabled;
    out->going = s_my_job && !strcmp(s_target_iccid, e->iccid);
    out->armed = armed_live && !strcmp(s_arm_iccid, e->iccid);
}

/* ---- 切换 ---- */

int esim_select(int index)
{
    char js[64], data[256], *b;
    long t = now_ms();
    const es_prof_t *e;
    int code;

    if (index < 0 || index >= s_count) return ESIM_SEL_FAIL;
    e = &s_prof[index];
    if (s_busy || s_my_job) return ESIM_SEL_BUSY;
    if (e->enabled) { s_arm_iccid[0] = 0; return ESIM_SEL_CURRENT; }

    /*
     * 切换会断网片刻、最坏重启整机，误触代价大：照搬重启内核的两段式确认。
     * 按 ICCID 记亮起的是哪张，列表在两次点击之间重读过也不会点错。
     */
    if (strcmp(s_arm_iccid, e->iccid) || t - s_arm_ms > ES_ARM_MS) {
        snprintf(s_arm_iccid, sizeof s_arm_iccid, "%s", e->iccid);
        s_arm_ms = t;
        return ESIM_SEL_ARMED;
    }
    s_arm_iccid[0] = 0;

    load_conf();
    snprintf(js, sizeof js, "{\"iccid\":\"%s\"}", e->iccid);
    code = es_api("POST", "/api/esim/switch", js, &b);
    if (code == 409) { s_busy = 1; return ESIM_SEL_BUSY; }
    if (code == 429) {
        /* agent 的冷却保护（catBusy 卡片，見 zte-agent esim.rs switch()）。
         * 错误文本形如 "...wait 480s and retry"，抠出秒数拼中文提示；
         * 抠不出来就给个不带数字的通用提示。这个格式跟 agent 耦合，
         * agent 那边措辞变了这里要跟着改。 */
        char err[128] = {0};
        const char *w;
        int wait = 0;
        if (b) json_str(b, "error", err, sizeof err);
        w = strstr(err, "wait ");
        if (w) wait = atoi(w + 5);
        if (wait > 0)
            snprintf(s_msg, sizeof s_msg,
                     "\xE5\x8D\xA1\xE5\x88\x9A\xE5\x88\x87\xE6\x8D\xA2\xE8\xBF\x87\xEF\xBC\x8C"
                     "\xE8\xBF\x98\xE8\xA6\x81\xE7\xAD\x89 %d \xE7\xA7\x92\xE5\x86\x8D\xE8\xAF\x95",
                     wait);   /* 卡刚切换过，还要等 N 秒再试 */
        else
            snprintf(s_msg, sizeof s_msg,
                     "\xE5\x8D\xA1\xE5\x88\x9A\xE5\x88\x87\xE6\x8D\xA2\xE8\xBF\x87\xEF\xBC\x8C"
                     "\xE7\xA8\x8D\xE5\x90\x8E\xE5\x86\x8D\xE8\xAF\x95");   /* 卡刚切换过，稍后再试 */
        s_msg_ms = now_ms();
        return ESIM_SEL_COOLDOWN;
    }
    if (code != 200 || !b || !json_get(b, "data", data, sizeof data) ||
        (s_my_job = json_get_int(data, "job_id", 0)) <= 0) {
        s_my_job = 0;
        fprintf(stderr, "esim: switch request failed (HTTP %d)\n", code);
        return ESIM_SEL_FAIL;
    }
    snprintf(s_target_iccid, sizeof s_target_iccid, "%s", e->iccid);
    snprintf(s_target_name, sizeof s_target_name, "%s", e->name);
    s_t0 = t;
    s_poll_ms = t;
    s_busy = 1;
    s_msg[0] = 0;
    fprintf(stderr, "esim: switch to %s started (job %ld)\n", e->iccid, s_my_job);
    return ESIM_SEL_STARTED;
}
