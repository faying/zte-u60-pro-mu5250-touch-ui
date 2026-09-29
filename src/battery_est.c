/*
 * battery_est.c - 见 battery_est.h。显示用的文字和取数分开，渲染测试只用
 * battery_est_override()。
 *
 * SPDX-License-Identifier: MIT
 */
#include "battery_est.h"
#include "agent_client.h"
#include "estimate.h"
#include "json.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define BE_POLL_MS   10000
#define BE_STALE_MS  30000
#define BE_IO_MS     800        /* 本机请求几十毫秒；agent 卡住时别拖住界面 */

static char s_text[96] = "—";
static char s_override[96];
static int  s_overridden, s_was_active;
static long s_last_try, s_last_ok;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int set_text(const char *t)
{
    if (!strcmp(t, s_text)) return 0;
    snprintf(s_text, sizeof s_text, "%s", t);
    return 1;
}

int battery_est_poll(int active)
{
    static char resp[4096];
    char data[1024], bat[512], text[sizeof s_text], *b;
    long t = now_ms();
    int came_back = active && !s_was_active;
    est_t e;
    int target;

    s_was_active = active;
    if (s_overridden) return 0;
    if (!active) return 0;
    if (!came_back && s_last_try && t - s_last_try < BE_POLL_MS) {
        /* 过期了就别再显示旧的那一句 */
        return (s_last_ok && t - s_last_ok > BE_STALE_MS) ? set_text("—") : 0;
    }
    s_last_try = t;
    if (agent_api_ms("GET", "/api/screen", NULL, resp, sizeof resp, BE_IO_MS, &b) != 200 || !b ||
        !json_get(b, "data", data, sizeof data) || !json_get(data, "battery", bat, sizeof bat)) {
        if (s_last_ok && t - s_last_ok <= BE_STALE_MS) return 0;
        return set_text("—");
    }
    s_last_ok = t;
    est_from_report(bat, &e, &target);
    est_text(e, target, text, sizeof text);
    return set_text(text);
}

const char *battery_est_text(void)
{
    return s_overridden ? s_override : s_text;
}

void battery_est_override(const char *text)
{
    s_overridden = text != NULL;
    if (text) snprintf(s_override, sizeof s_override, "%s", text);
}
