/*
 * estimate.c 测试：预估的文案，和 zte-agent 结果（/api/screen 的 battery）的解析。
 * 预估本身在 agent 的 battery_eta.rs 里算、在那边测。
 *   scripts/test/estimate/run.sh
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/estimate.c"

static int pass, fail;

#define CHECK(desc, cond)                                              \
    do {                                                               \
        if (cond) { pass++; printf("  ok   %s\n", desc); }             \
        else      { fail++; printf("  FAIL %s  [%s]\n", desc, #cond); } \
    } while (0)

static void text_of(const char *obj, char *buf, size_t cap)
{
    est_t e;
    int target;
    est_from_report(obj, &e, &target);
    est_text(e, target, buf, cap);
}

int main(void)
{
    char buf[96];
    est_t e;
    int target;

    est_text((est_t){ EST_CHARGING, 198 }, 80, buf, sizeof buf);
    CHECK("text: to limit", !strcmp(buf, "约 3 小时 18 分充到 80%"));
    est_text((est_t){ EST_CHARGING, 45 }, 100, buf, sizeof buf);
    CHECK("text: minutes to full", !strcmp(buf, "约 45 分钟充满"));
    est_text((est_t){ EST_DISCHARGING, 120 }, 100, buf, sizeof buf);
    CHECK("text: whole hours", !strcmp(buf, "约可用 2 小时"));
    est_text((est_t){ EST_REACHED, 0 }, 100, buf, sizeof buf);
    CHECK("text: full", !strcmp(buf, "已充满"));
    est_text((est_t){ EST_REACHED, 0 }, 80, buf, sizeof buf);
    CHECK("text: at the limit", !strcmp(buf, "已到上限 80%"));
    est_text((est_t){ EST_PAUSED, 0 }, 80, buf, sizeof buf);
    CHECK("text: paused", !strcmp(buf, "已到上限，暂停充电"));
    est_text((est_t){ EST_UNKNOWN, 0 }, 100, buf, sizeof buf);
    CHECK("text: unknown", !strcmp(buf, "—"));

    /* agent 的结果（battery_eta.rs Report 的 JSON） */
    text_of("{\"state\":\"ok\",\"kind\":\"discharging_eta\",\"minutes\":891,\"target_pct\":100,\"observed_at\":1,\"samples\":36}",
            buf, sizeof buf);
    CHECK("report: discharging", !strcmp(buf, "约可用 14 小时 51 分"));
    text_of("{\"state\":\"ok\",\"kind\":\"charging_eta\",\"minutes\":198,\"target_pct\":80}", buf, sizeof buf);
    CHECK("report: charging to the limit", !strcmp(buf, "约 3 小时 18 分充到 80%"));
    text_of("{\"state\":\"ok\",\"kind\":\"paused_at_limit\",\"minutes\":null,\"target_pct\":80}", buf, sizeof buf);
    CHECK("report: paused", !strcmp(buf, "已到上限，暂停充电"));
    text_of("{\"state\":\"ok\",\"kind\":\"reached_target\",\"minutes\":null,\"target_pct\":100}", buf, sizeof buf);
    CHECK("report: full", !strcmp(buf, "已充满"));

    CHECK("report: stale is not shown",
          est_from_report("{\"state\":\"stale\",\"kind\":\"discharging_eta\",\"minutes\":300,\"target_pct\":100}", &e, &target) == 0 &&
          e.kind == EST_UNKNOWN);
    CHECK("report: estimating is not shown",
          est_from_report("{\"state\":\"estimating\",\"kind\":\"unknown\",\"minutes\":null,\"target_pct\":100}", &e, &target) == 0 &&
          e.kind == EST_UNKNOWN);
    CHECK("report: unavailable keeps the limit",
          est_from_report("{\"state\":\"unavailable\",\"kind\":\"unknown\",\"minutes\":null,\"target_pct\":80}", &e, &target) == 0 &&
          target == 80);
    CHECK("report: minutes null on an eta is unknown",
          est_from_report("{\"state\":\"ok\",\"kind\":\"charging_eta\",\"minutes\":null,\"target_pct\":100}", &e, &target) == 1 &&
          e.kind == EST_UNKNOWN);
    CHECK("report: a kind from a newer agent is unknown",
          est_from_report("{\"state\":\"ok\",\"kind\":\"warp_speed\",\"minutes\":5,\"target_pct\":100}", &e, &target) == 1 &&
          e.kind == EST_UNKNOWN);
    CHECK("report: bad target reads as 100",
          est_from_report("{\"state\":\"ok\",\"kind\":\"unknown\",\"target_pct\":500}", &e, &target) == 1 && target == 100);
    CHECK("report: NULL", est_from_report(NULL, &e, &target) == 0 && e.kind == EST_UNKNOWN && target == 100);
    CHECK("report: garbage", est_from_report("not json", &e, &target) == 0 && e.kind == EST_UNKNOWN);

    printf("%d passed, %d failed\n", pass, fail);
    return fail != 0;
}
