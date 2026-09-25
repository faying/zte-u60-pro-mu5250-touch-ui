/*
 * battery_est.c - 见 battery_est.h。显示用的文字和取数分开，渲染测试只用
 * battery_est_override()。
 *
 * SPDX-License-Identifier: MIT
 */
#include "battery_est.h"
#include "estimate.h"
#include "netinfo.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define BE_SAMPLE_S    5
#define BE_CAP_S       600
#define BE_CC_S        30
#define BE_SYSFS       "/sys/class/power_supply"

static est_sample_t s_buf[EST_MAX_SAMPLES];
static int s_n;
static double s_last_sample, s_last_cap, s_last_cc;
static long long s_full = EST_NONE, s_counter = EST_NONE;
static int s_target = 100, s_paused;
static char s_text[96] = "—";
static char s_override[96];
static int s_overridden;

static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

int battery_est_feed(const devui_data_t *d)
{
    double now = mono_s();
    est_input_t in;
    char text[sizeof s_text];

    if (s_overridden || !d || !d->valid) return 0;
    if (s_n && now - s_last_sample < BE_SAMPLE_S) return 0;
    s_last_sample = now;

    if (!s_last_cap || now - s_last_cap >= BE_CAP_S) {
        est_read_capacity(BE_SYSFS, &s_full, &s_counter);
        s_last_cap = now;
    }
    /* 没插电时上限和暂停都不影响放电预估：不发请求 */
    if (!d->charger_connect) {
        s_target = 100;
        s_paused = 0;
        s_last_cc = 0;
    } else if (!s_last_cc || now - s_last_cc >= BE_CC_S) {
        static char cc[1024];
        netinfo_agent_get("/api/device/charge-control", cc, sizeof cc);
        est_parse_charge_control(cc[0] ? cc : NULL, 1, &s_target, &s_paused);
        s_last_cc = now;
    }

    s_n = est_push(s_buf, s_n, (est_sample_t){ now, d->bat_ua, d->charger_connect != 0 });
    in.s = s_buf;
    in.n = s_n;
    in.soc = d->bat_percent;
    in.full_uah = s_full;
    /* charge_counter 每 10 分钟才读一次，放电时按电量估更跟手 */
    in.counter_uah = EST_NONE;
    in.target_pct = s_target;
    in.paused_at_limit = s_paused;
    est_text(est_compute(&in), s_target, text, sizeof text);
    if (!strcmp(text, s_text)) return 0;
    snprintf(s_text, sizeof s_text, "%s", text);
    return 1;
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
