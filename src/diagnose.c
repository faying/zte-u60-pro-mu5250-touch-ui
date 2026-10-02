/*
 * diagnose.c - zte-agent's network diagnosis, for the 网络诊断 page (see
 * diagnose.h). One module buffer per request, like esim.c / speedtest.c.
 *
 * SPDX-License-Identifier: MIT
 */
#include "diagnose.h"
#include "agent_client.h"
#include "json.h"
#include "lang.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DG_FAST_MS   1000      /* a run or its speed test is going */
#define DG_SLOW_MS   5000      /* a finished run: the age and a late speed row */
#define DG_BACKOFF_MS 5000     /* the agent did not answer: don't stall every second */
#define DG_RESP_MAX  8192

static diag_run_t s_run;
static int  s_idle;            /* the agent has nothing to show (s_run: what it had) */
static int  s_err;
static char s_refused[160];
static long s_poll_ms;
static int  s_kick = 1;
static unsigned s_sig;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int dg_api(const char *method, const char *path, const char *json, char **body)
{
    static char resp[DG_RESP_MAX];
    return agent_api(method, path, json, resp, sizeof resp, body);
}

static unsigned fnv(unsigned h, const void *p, size_t n)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

static int going(void)
{
    return s_run.state == DG_WAITING || s_run.state == DG_RUN ||
           (s_run.has_speed && s_run.speed.level == DG_RUNNING);
}

/* code → s_err; 2xx with a run in "data" → s_run. Returns 1 when taken. */
static int take(int code, const char *body)
{
    static char data[DG_RESP_MAX];
    diag_run_t r;
    s_err = code == 0 ? 1 : code == 401 ? 2 : 0;
    if (code < 200 || code >= 300 || !body) return 0;
    if (!json_get(body, "data", data, sizeof data) || !diag_view_parse(data, &r)) return 0;
    /* idle = none, or the last one is over 10 minutes old: keep showing it */
    s_idle = r.state == DG_IDLE;
    if (!s_idle || s_run.state == DG_NONE) s_run = r;
    return 1;
}

static void refused(int code, const char *body)
{
    s_refused[0] = 0;
    if (code >= 200 && code < 300) return;
    if (code == 0) { snprintf(s_refused, sizeof s_refused, "%s", TR("后台没回应")); return; }
    if (body) {
        char zh[160] = "", en[160] = "";
        json_get(body, "error", zh, sizeof zh);
        json_get(body, "error_en", en, sizeof en);
        if (!strcmp(zh, "null")) zh[0] = 0;
        if (!strcmp(en, "null")) en[0] = 0;
        snprintf(s_refused, sizeof s_refused, "%s", pick(zh, en));
    }
    if (!s_refused[0]) snprintf(s_refused, sizeof s_refused, TR("被拒绝（HTTP %d）"), code);
}

int diagnose_poll(int active)
{
    char *body;
    long t = now_ms();
    if (!active) return 0;
    long gap = s_err ? DG_BACKOFF_MS : going() ? DG_FAST_MS : DG_SLOW_MS;
    if (!s_kick && s_poll_ms && t - s_poll_ms < gap) return 0;
    s_kick = 0;
    s_poll_ms = t;
    int code = agent_has_password() ? dg_api("GET", "/api/diagnose", NULL, &body) : 401;
    if (code != 200) body = NULL;
    take(code, body);
    unsigned h = fnv(2166136261u, &s_run, sizeof s_run);
    h = fnv(h, &s_err, sizeof s_err);
    h = fnv(h, &s_idle, sizeof s_idle);
    if (h == s_sig) return 0;
    s_sig = h;
    return 1;
}

void diagnose_kick(void) { s_kick = 1; }
const diag_run_t *diagnose_run(void) { return &s_run; }
int diagnose_agent_err(void) { return s_err; }
int diagnose_idle(void) { return s_idle || s_run.state == DG_NONE || s_run.state == DG_IDLE; }
const char *diagnose_error(void) { return s_refused; }

int diagnose_start(void)
{
    char *body;
    int code = dg_api("POST", "/api/diagnose", "{}", &body);
    refused(code, body);
    s_sig = 0;
    s_poll_ms = now_ms();          /* the reply carries the run: next read in a second */
    return take(code, body);
}

int diagnose_speed(void)
{
    char js[48], *body;
    snprintf(js, sizeof js, "{\"id\":%ld}", s_run.id);
    int code = dg_api("POST", "/api/diagnose/speed", js, &body);
    refused(code, body);
    s_sig = 0;
    s_poll_ms = now_ms();
    if (code >= 200 && code < 300) {
        take(code, body);
        return 1;
    }
    s_err = code == 0 ? 1 : code == 401 ? 2 : 0;
    return 0;
}

int diagnose_feedback(int right)
{
    char js[64], *body;
    snprintf(js, sizeof js, "{\"id\":%ld,\"right\":%s}", s_run.id, right ? "true" : "false");
    int code = dg_api("POST", "/api/diagnose/feedback", js, &body);
    s_err = code == 0 ? 1 : code == 401 ? 2 : 0;
    if (code != 200) return 0;
    s_run.feedback = right ? 1 : 0;
    s_sig = 0;
    return 1;
}
