/*
 * agent_client.c - logged-in requests to zte-agent. See agent_client.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "agent_client.h"

#include "http.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* procd install: /data/zte-agent.env (ZTE_AGENT_PASSWORD=...); older installs
 * export it from the start script. Both spellings are understood. */
#define AGENT_ENV      "/data/zte-agent.env"
#define AGENT_START_SH "/data/local/tmp/start_zte_agent.sh"
/* optional override (port=, password=), historically eSIM's; now every module's */
#define AGENT_CONF     "/data/plugins/u60pro-devui/esim.conf"

static int  s_port = AGENT_PORT_DEFAULT;
static char s_pass[128], s_token[80];
static int  s_pass_loaded;

static void load_password(void)
{
    static const char *paths[] = { AGENT_ENV, AGENT_START_SH };
    FILE *fp = NULL;
    char line[256];

    if (s_pass_loaded) return;
    s_pass_loaded = 1;
    if ((fp = fopen(AGENT_CONF, "r")) != NULL) {
        while (fgets(line, sizeof line, fp)) {
            char *nl = strpbrk(line, "\r\n");
            if (nl) *nl = 0;
            if (!strncmp(line, "port=", 5) && atoi(line + 5) > 0) s_port = atoi(line + 5);
            else if (!strncmp(line, "password=", 9)) snprintf(s_pass, sizeof s_pass, "%.*s", (int)sizeof s_pass - 1, line + 9);
        }
        fclose(fp);
        fp = NULL;
    }
    if (s_pass[0]) return;
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

void agent_client_config(int port, const char *pass)
{
    if (port > 0) s_port = port;
    if (pass && pass[0]) {
        snprintf(s_pass, sizeof s_pass, "%s", pass);
        s_pass_loaded = 1;
    }
    s_token[0] = 0;
}

int agent_has_password(void)
{
    load_password();
    return s_pass[0] != 0;
}

static int request(const char *method, const char *path, const char *json, int with_token,
                   char *buf, size_t cap, int io_ms, char **body)
{
    char req[1024], auth[128] = "", host[32];
    http_resp_t r;
    int fd;

    *body = NULL;
    load_password();
    if (with_token && s_token[0]) snprintf(auth, sizeof auth, "Authorization: Bearer %s\r\n", s_token);
    snprintf(host, sizeof host, "127.0.0.1:%d", s_port);
    /* always send Content-Type/Content-Length, as the per-module copies did */
    if (!http_build(req, sizeof req, method, path, host, auth, json ? json : "")) return 0;
    if ((fd = http_connect_tcp("127.0.0.1", s_port, io_ms)) < 0) return 0;
    if (!http_exchange(fd, req, buf, cap, io_ms, &r)) return 0;
    *body = r.body;
    return r.status;
}

int agent_http(const char *method, const char *path, const char *json,
               char *buf, size_t cap, int io_ms, char **body)
{
    return request(method, path, json, 0, buf, cap, io_ms, body);
}

static int login(void)
{
    char js[300], data[160], buf[1024], *b;
    size_t o = (size_t)snprintf(js, sizeof js, "{\"password\":\"");

    for (const char *p = s_pass; *p && o + 4 < sizeof js; p++) {
        if (*p == '"' || *p == '\\') js[o++] = '\\';
        js[o++] = *p;
    }
    snprintf(js + o, sizeof js - o, "\"}");
    s_token[0] = 0;
    if (request("POST", "/api/auth/login", js, 0, buf, sizeof buf, AGENT_IO_MS, &b) != 200 || !b) return 0;
    if (!json_get(b, "data", data, sizeof data)) return 0;
    return json_get(data, "token", s_token, sizeof s_token) && s_token[0];
}

int agent_api_ms(const char *method, const char *path, const char *json,
                 char *buf, size_t cap, int io_ms, char **body)
{
    int code;

    load_password();
    if (!s_token[0] && s_pass[0]) login();
    code = request(method, path, json, 1, buf, cap, io_ms, body);
    /* 401 = token expired (the agent hands out 1 h) or the agent restarted */
    if (code == 401 && s_pass[0] && login())
        code = request(method, path, json, 1, buf, cap, io_ms, body);
    return code;
}

int agent_api(const char *method, const char *path, const char *json,
              char *buf, size_t cap, char **body)
{
    return agent_api_ms(method, path, json, buf, cap, AGENT_IO_MS, body);
}

int agent_request(const char *method, const char *path, const char *json)
{
    static char buf[16384];
    char *b;
    return agent_api(method, path, json, buf, sizeof buf, &b);
}

int agent_post(const char *path) { return agent_request("POST", path, NULL); }
