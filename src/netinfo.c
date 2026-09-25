/*
 * netinfo.c - 「网络」页的数据：读 zte-agent 的 /api/netinfo，发起搜网、选网、
 * 恢复自动、扫邻区。见 netinfo.h。
 *
 * 要登录，和 alerts.c/esim.c 一样用 agent 的密码换 token；HTTP 写法也照它
 * （HTTP/1.0，不解分块，401 就重登一次）。
 *
 * SPDX-License-Identifier: MIT
 */
#include "netinfo.h"
#include "json.h"
#include "chill.h"

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

#define NI_PORT      9090
#define NI_ENV       "/data/zte-agent.env"
#define NI_AGENT_SH  "/data/local/tmp/start_zte_agent.sh"
#define NI_IO_MS     1500
#define NI_TTL_MS    5000
#define NI_BUSY_MS   2000
#define NI_HOME_MS   30000
#define NI_RESP_MAX  32768   /* 完整读取带 apn（约 1 KB）后留足余量；截断会报「读网络信息失败」 */

static char s_pass[128], s_token[80];
static int  s_pass_loaded;
static netinfo_t s_ni;
static char s_act_err[96];
static int  s_was_mode;
static long s_poll_ms;
static unsigned s_sig;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* agent 密码：env 文件优先，旧启动脚本兜底（同 alerts.c） */
static void load_password(void)
{
    static const char *paths[] = { NI_ENV, NI_AGENT_SH };
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

static int ni_connect(void)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0), f, rc;

    if (fd < 0) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(NI_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    f = fcntl(fd, F_GETFL, 0);
    if (f < 0 || fcntl(fd, F_SETFL, f | O_NONBLOCK) < 0) { close(fd); return -1; }
    rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    if (rc < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    if (rc < 0 && wait_ready(fd, 1, NI_IO_MS) <= 0) { close(fd); return -1; }
    fcntl(fd, F_SETFL, f);
    return fd;
}

/* 返回 HTTP 状态码（0 = 连不上），*body 指向静态缓冲区（下次调用覆盖） */
static int ni_http(const char *method, const char *path, const char *json, char **body)
{
    static char resp[NI_RESP_MAX];
    char req[768], auth[128] = "";
    size_t n = 0;
    char *p;
    int fd;

    *body = NULL;
    if ((fd = ni_connect()) < 0) return 0;
    if (s_token[0]) snprintf(auth, sizeof auth, "Authorization: Bearer %s\r\n", s_token);
    snprintf(req, sizeof req,
             "%s %s HTTP/1.0\r\nHost: 127.0.0.1:%d\r\n%s"
             "Content-Type: application/json\r\nContent-Length: %d\r\n"
             "Connection: close\r\n\r\n%s",
             method, path, NI_PORT, auth, json ? (int)strlen(json) : 0, json ? json : "");
    if (write(fd, req, strlen(req)) < 0) { close(fd); return 0; }
    for (;;) {
        ssize_t rd;
        if (n + 1 >= sizeof resp) break;
        if (wait_ready(fd, 0, NI_IO_MS) <= 0) break;
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

static int ni_login(void)
{
    char js[300], data[160], *b;
    size_t o = (size_t)snprintf(js, sizeof js, "{\"password\":\"");

    for (const char *p = s_pass; *p && o + 4 < sizeof js; p++) {
        if (*p == '"' || *p == '\\') js[o++] = '\\';
        js[o++] = *p;
    }
    snprintf(js + o, sizeof js - o, "\"}");
    s_token[0] = 0;
    if (ni_http("POST", "/api/auth/login", js, &b) != 200 || !b) return 0;
    if (!json_get(b, "data", data, sizeof data)) return 0;
    return json_get(data, "token", s_token, sizeof s_token) && s_token[0];
}

static int ni_api(const char *method, const char *path, const char *json, char **body)
{
    int code;

    load_password();
    if (!s_token[0] && s_pass[0]) ni_login();
    code = ni_http(method, path, json, body);
    if (code == 401 && s_pass[0] && ni_login()) code = ni_http(method, path, json, body);
    return code;
}

/* null / 缺字段 → "" */
static void jstr(const char *obj, const char *key, char *out, size_t cap)
{
    if (!json_get(obj, key, out, cap) || !strcmp(out, "null")) out[0] = 0;
}

/* 数组里的对象逐个截成独立字符串（json_get 只认第一层）。*cur 从 '[' 之后开始，
 * 每次返回下一个对象（写进 buf），没有了返回 0。 */
static int next_obj(const char **cur, char *buf, size_t cap)
{
    const char *p = *cur, *q;
    int depth = 0, instr = 0, esc = 0;
    size_t len;

    while (*p && *p != '{' && *p != ']') p++;
    if (*p != '{') return 0;
    for (q = p; *q; q++) {
        if (instr) {
            if (esc) esc = 0;
            else if (*q == '\\') esc = 1;
            else if (*q == '"') instr = 0;
        } else if (*q == '"') instr = 1;
        else if (*q == '{') depth++;
        else if (*q == '}' && --depth == 0) break;
    }
    if (!*q) return 0;
    len = (size_t)(q - p + 1);
    if (len >= cap) len = cap - 1;
    memcpy(buf, p, len);
    buf[len] = 0;
    *cur = q + 1;
    return 1;
}

static void parse_exit(const char *data, const char *key, ni_exit_t *e)
{
    char o[1024];

    memset(e, 0, sizeof *e);
    if (!json_get(data, key, o, sizeof o) || o[0] != '{') return;
    e->present = 1;
    jstr(o, "ip", e->ip, sizeof e->ip);
    jstr(o, "geo", e->geo, sizeof e->geo);
    jstr(o, "isp", e->isp, sizeof e->isp);
    {
        /* 节点名里的国旗 emoji 设备字体画不出来：和 CHILL 页一样转成国家码 */
        char raw[sizeof e->node * 2];
        jstr(o, "node", raw, sizeof raw);
        chill_sanitize_name(raw, e->node, sizeof e->node);
    }
    jstr(o, "error", e->err, sizeof e->err);
}

static void parse_oper(const char *data, const char *key, ni_oper_t *op)
{
    char o[256];

    memset(op, 0, sizeof *op);
    if (!json_get(data, key, o, sizeof o) || o[0] != '{') return;
    jstr(o, "name", op->name, sizeof op->name);
    jstr(o, "country", op->country, sizeof op->country);
    jstr(o, "mcc", op->mcc, sizeof op->mcc);
    jstr(o, "mnc", op->mnc, sizeof op->mnc);
}

static void parse_apn(const char *obj, ni_apn_t *a)
{
    char v[16];
    jstr(obj, "id", a->id, sizeof a->id);
    jstr(obj, "name", a->name, sizeof a->name);
    jstr(obj, "apn", a->apn, sizeof a->apn);
    jstr(obj, "pdp", v, sizeof v);      a->pdp = (int)strtol(v, NULL, 10);
    jstr(obj, "selected", v, sizeof v); a->selected = !strcmp(v, "true");
    jstr(obj, "in_use", v, sizeof v);   a->in_use = !strcmp(v, "true");
}

static void parse(const char *data)
{
    static char sub[8192];
    char tmp[16], obj[768];
    const char *cur;

    s_ni.now = json_get_int(data, "now", 0);
    parse_exit(data, "direct", &s_ni.direct);
    parse_exit(data, "proxy", &s_ni.proxy);
    jstr(data, "chill_running", tmp, sizeof tmp);
    s_ni.chill_running = !strcmp(tmp, "true");
    parse_oper(data, "home_operator", &s_ni.home);
    parse_oper(data, "serving_operator", &s_ni.serving);
    jstr(data, "roaming", tmp, sizeof tmp);
    s_ni.roaming = !strcmp(tmp, "true") ? 1 : !strcmp(tmp, "false") ? 0 : -1;

    s_ni.selection[0] = 0;
    if (json_get(data, "selection", obj, sizeof obj)) jstr(obj, "mode", s_ni.selection, sizeof s_ni.selection);

    s_ni.guard_phase[0] = s_ni.guard_target[0] = s_ni.guard_reason[0] = 0;
    if (json_get(data, "guard", obj, sizeof obj)) {
        jstr(obj, "phase", s_ni.guard_phase, sizeof s_ni.guard_phase);
        jstr(obj, "target", s_ni.guard_target, sizeof s_ni.guard_target);
        jstr(obj, "reason", s_ni.guard_reason, sizeof s_ni.guard_reason);
    }

    s_ni.scan_state[0] = s_ni.scan_err[0] = 0;
    s_ni.nops = 0;
    if (json_get(data, "scan", sub, sizeof sub)) {
        char arr[6144];
        jstr(sub, "state", s_ni.scan_state, sizeof s_ni.scan_state);
        jstr(sub, "error", s_ni.scan_err, sizeof s_ni.scan_err);
        if (json_get(sub, "operators", arr, sizeof arr)) {
            cur = arr;
            while (s_ni.nops < NI_MAX_OPS && next_obj(&cur, obj, sizeof obj)) {
                ni_scan_op_t *op = &s_ni.ops[s_ni.nops++];
                jstr(obj, "plmn", op->plmn, sizeof op->plmn);
                jstr(obj, "name", op->name, sizeof op->name);
                jstr(obj, "country", op->country, sizeof op->country);
                jstr(obj, "rat", op->rat, sizeof op->rat);
                jstr(obj, "status", op->status, sizeof op->status);
            }
        }
    }

    s_ni.scene_known = 0;
    s_ni.nscenes = 0;
    s_ni.scene_current[0] = s_ni.scene_pin[0] = 0;
    if (json_get(data, "scenes", sub, sizeof sub)) {
        char arr[2048];
        s_ni.scene_known = 1;
        jstr(sub, "enabled", tmp, sizeof tmp);
        s_ni.scene_enabled = !strcmp(tmp, "true");
        jstr(sub, "guard_takeover", tmp, sizeof tmp);
        s_ni.scene_takeover = !strcmp(tmp, "true");
        jstr(sub, "current", s_ni.scene_current, sizeof s_ni.scene_current);
        jstr(sub, "pin", s_ni.scene_pin, sizeof s_ni.scene_pin);
        if (json_get(sub, "list", arr, sizeof arr)) {
            cur = arr;
            while (s_ni.nscenes < NI_MAX_SCENES && next_obj(&cur, obj, sizeof obj)) {
                ni_scene_t *sc = &s_ni.scenes[s_ni.nscenes++];
                jstr(obj, "id", sc->id, sizeof sc->id);
                jstr(obj, "name", sc->name, sizeof sc->name);
                jstr(obj, "wifi_off", tmp, sizeof tmp);
                sc->wifi_off = !strcmp(tmp, "true");
                jstr(obj, "abroad", tmp, sizeof tmp);
                sc->abroad = !strcmp(tmp, "true");
                jstr(obj, "when", sc->when, sizeof sc->when);
                jstr(obj, "does", sc->does, sizeof sc->does);
            }
        }
    }

    s_ni.clients_known = 0;
    s_ni.nclients = 0;
    if (json_get(data, "clients", sub, sizeof sub) && sub[0] == '{') {
        char arr[8192], v[32];
        s_ni.clients_known = json_get_int(sub, "at", 0) > 0;
        if (json_get(sub, "list", arr, sizeof arr)) {
            cur = arr;
            while (s_ni.nclients < NI_MAX_CLIENTS && next_obj(&cur, obj, sizeof obj)) {
                ni_client_t *c = &s_ni.clients[s_ni.nclients++];
                jstr(obj, "name", c->name, sizeof c->name);
                jstr(obj, "mac", c->mac, sizeof c->mac);
                jstr(obj, "ip", c->ip, sizeof c->ip);
                jstr(obj, "down_bytes", v, sizeof v); c->down = strtoll(v, NULL, 10);
                jstr(obj, "up_bytes", v, sizeof v);   c->up = strtoll(v, NULL, 10);
                jstr(obj, "down_rate", v, sizeof v);  c->down_rate = v[0] ? strtol(v, NULL, 10) : -1;
                jstr(obj, "up_rate", v, sizeof v);    c->up_rate = v[0] ? strtol(v, NULL, 10) : -1;
                jstr(obj, "signal", v, sizeof v);     c->signal = (int)strtol(v, NULL, 10);
                jstr(obj, "band", c->band, sizeof c->band);
                jstr(obj, "wifi_gen", v, sizeof v);   c->wifi_gen = (int)strtol(v, NULL, 10);
                jstr(obj, "link_down_mbps", v, sizeof v); c->link_down = (int)strtol(v, NULL, 10);
            }
        }
    }

    s_ni.apn_known = s_ni.apn_manual = 0;
    s_ni.napns = 0;
    memset(&s_ni.apn_in_use, 0, sizeof s_ni.apn_in_use);
    if (json_get(data, "apn", sub, sizeof sub) && sub[0] == '{') {
        char arr[4096];
        jstr(sub, "mode", tmp, sizeof tmp);
        s_ni.apn_known = tmp[0] != 0;
        s_ni.apn_manual = !strcmp(tmp, "manual");
        if (json_get(sub, "in_use", obj, sizeof obj) && obj[0] == '{') parse_apn(obj, &s_ni.apn_in_use);
        if (json_get(sub, "manual", arr, sizeof arr)) {
            cur = arr;
            while (s_ni.napns < NI_MAX_APNS && next_obj(&cur, obj, sizeof obj))
                parse_apn(obj, &s_ni.apns[s_ni.napns++]);
        }
    }

    s_ni.nbr_state[0] = s_ni.nbr_err[0] = 0;
    s_ni.ncells = 0;
    s_ni.nbr_at = 0;
    if (json_get(data, "neighbors", sub, sizeof sub)) {
        char arr[6144];
        jstr(sub, "state", s_ni.nbr_state, sizeof s_ni.nbr_state);
        jstr(sub, "error", s_ni.nbr_err, sizeof s_ni.nbr_err);
        s_ni.nbr_at = json_get_int(sub, "scanned_at", 0);
        if (json_get(sub, "cells", arr, sizeof arr)) {
            cur = arr;
            while (s_ni.ncells < NI_MAX_CELLS && next_obj(&cur, obj, sizeof obj)) {
                ni_cell_t *c = &s_ni.cells[s_ni.ncells++];
                jstr(obj, "rat", c->rat, sizeof c->rat);
                jstr(obj, "pci", c->pci, sizeof c->pci);
                jstr(obj, "arfcn", c->arfcn, sizeof c->arfcn);
                jstr(obj, "rsrp", c->rsrp, sizeof c->rsrp);
                jstr(obj, "rsrq", c->rsrq, sizeof c->rsrq);
                jstr(obj, "sinr", c->sinr, sizeof c->sinr);
            }
        }
    }
}

static int load(int mode)
{
    static char data[NI_RESP_MAX];
    char *b;
    const char *path = mode == NI_PAGE ? "/api/netinfo" : mode == NI_APN ? "/api/netinfo?lite=1&apn=1" :
                       mode == NI_CLIENTS ? "/api/netinfo?lite=1&clients=1" : "/api/netinfo?lite=1";
    int code = ni_api("GET", path, NULL, &b);

    if (code == 0) { snprintf(s_ni.err, sizeof s_ni.err, "连不上管理后台"); return 0; }
    if (code == 401) { snprintf(s_ni.err, sizeof s_ni.err, "登录管理后台失败（密码不对？）"); return 0; }
    if (code == 404) { snprintf(s_ni.err, sizeof s_ni.err, "管理后台版本太旧，没有网络页接口"); return 0; }
    if (code != 200 || !b || !json_get(b, "data", data, sizeof data)) {
        snprintf(s_ni.err, sizeof s_ni.err, "读网络信息失败（HTTP %d）", code);
        return 0;
    }
    s_ni.err[0] = 0;
    parse(data);
    return 1;
}

static unsigned fnv(unsigned h, const void *p, size_t n)
{
    const unsigned char *s = p;
    for (size_t i = 0; i < n; i++) { h ^= s[i]; h *= 16777619u; }
    return h;
}

static long s_hurry_until;   /* 刚发了动作：这段时间按忙时的节奏读，好尽快看到结果 */

void netinfo_hurry(int secs) { s_hurry_until = now_ms() + secs * 1000L; }

static int busy(void)
{
    return now_ms() < s_hurry_until || !strcmp(s_ni.scan_state, "scanning") || !strcmp(s_ni.nbr_state, "scanning") ||
           !strcmp(s_ni.guard_phase, "registering") || !strcmp(s_ni.guard_phase, "reverting");
}

int netinfo_poll(int mode)
{
    int just_shown = mode && mode != s_was_mode;
    long t = now_ms();
    unsigned h = 2166136261u;
    long now_saved, ttl;

    s_was_mode = mode;
    if (!mode) return 0;
    ttl = mode == NI_HOME ? NI_HOME_MS : busy() ? NI_BUSY_MS : NI_TTL_MS;
    if (!just_shown && s_poll_ms && t - s_poll_ms < ttl) return 0;
    s_poll_ms = t;
    load(mode);
    /* now 每次都变，不算进「内容变了」 */
    now_saved = s_ni.now;
    s_ni.now = 0;
    h = fnv(h, &s_ni, sizeof s_ni);
    h = fnv(h, s_act_err, strlen(s_act_err));
    s_ni.now = now_saved;
    if (h == s_sig) return 0;
    s_sig = h;
    return 1;
}

const netinfo_t *netinfo_get(void) { return &s_ni; }
const char *netinfo_action_error(void) { return s_act_err; }

/* 发一个动作；被拒（409 之类）就把 agent 的 error 记下来给页面显示 */
static void act(const char *path, const char *json)
{
    char *b;
    int code = ni_api("POST", path, json, &b);

    s_act_err[0] = 0;
    if (code == 0) snprintf(s_act_err, sizeof s_act_err, "连不上管理后台");
    else if (code >= 300) {
        if (!b || !json_get(b, "error", s_act_err, sizeof s_act_err))
            snprintf(s_act_err, sizeof s_act_err, "被拒绝（HTTP %d）", code);
    }
    s_poll_ms = 0;          /* 下一轮立即重读 */
    s_was_mode = 0;
}

void netinfo_scan(void)     { act("/api/netinfo/scan", NULL); }
void netinfo_auto(void)     { act("/api/modem/netselect/auto", NULL); }
void netinfo_nbr_scan(void) { act("/api/netinfo/neighbors/scan", NULL); }

void netinfo_pin(const char *id)
{
    char js[64];

    if (id && id[0]) snprintf(js, sizeof js, "{\"id\":\"%s\"}", id);
    else snprintf(js, sizeof js, "{\"id\":null}");
    act("/api/scenario/pin", js);
}

void netinfo_apn_use(const char *id)
{
    char js[64];

    if (!id || !id[0] || strlen(id) >= 24 || strchr(id, '"') || strchr(id, '\\')) return;
    snprintf(js, sizeof js, "{\"id\":\"%s\"}", id);
    act("/api/netinfo/apn", js);
}

void netinfo_register(int op)
{
    char js[96];

    if (op < 0 || op >= s_ni.nops) return;
    snprintf(js, sizeof js, "{\"m_mcc_mnc\":\"%s\",\"m_rat\":\"%s\"}", s_ni.ops[op].plmn, s_ni.ops[op].rat);
    act("/api/modem/register", js);
}
