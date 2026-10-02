/*
 * Unit tests for alerts.c's /api/health parser (health.rs output: serde_json
 * sorts keys, so `checked_at`/`checks`/`crashlogs` come in that order).
 *   scripts/test/alerts/run.sh
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/alerts.c"

static int pass, fail;

#define CHECK(desc, cond)                                              \
    do {                                                               \
        if (cond) { pass++; printf("  ok   %s\n", desc); }             \
        else      { fail++; printf("  FAIL %s  [%s]\n", desc, #cond); } \
    } while (0)

int main(void)
{
    static char d[4096];
    health_item_t h;

    CHECK("nothing read yet: -1", health_count() == -1);

    snprintf(d, sizeof d, "{\"bad\":1,\"checked_at\":1790250000,\"checks\":["
             "{\"detail\":\"在广播\",\"id\":\"wifi\",\"label\":\"Wi-Fi\",\"level\":\"ok\"},"
             "{\"detail\":\"没配置号码：后台挂了你不会知道\",\"id\":\"sms\",\"label\":\"短信告警\",\"level\":\"warn\"},"
             "{\"detail\":\"x [a] \\\"q\\\" }{\",\"id\":\"disk\",\"label\":\"/data 空间\",\"level\":\"bad\"},"
             "{\"detail\":\"能访问\",\"id\":\"agent-http\",\"label\":\"管理网页 :9090\",\"level\":\"ok\"}],"
             "\"crashlogs\":[{\"file\":\"a.log\",\"level\":\"warn\",\"program\":\"p\"}],\"error\":null,\"warn\":1}");
    parse_checks(d);
    CHECK("two not-ok checks", health_count() == 2);
    CHECK("two ok checks counted", health_checked() == 2);
    health_get(0, &h);
    CHECK("first is the SMS warning", !strcmp(h.id, "sms") && !h.bad && !strcmp(h.label, "短信告警"));
    health_get(1, &h);
    CHECK("second is bad, brackets/quotes in detail survive", h.bad && !strcmp(h.id, "disk") && strstr(h.detail, "[a]"));
    CHECK("crashlog objects after the array are not checks", health_count() == 2);
    health_get(5, &h);
    CHECK("out of range: zeroed", !h.id[0]);

    snprintf(d, sizeof d, "{\"checks\":[],\"warn\":0}");
    parse_checks(d);
    CHECK("empty list", health_count() == 0 && health_checked() == 0);

    snprintf(d, sizeof d, "{\"checks\":[");
    for (int i = 0; i < 12; i++)
        snprintf(d + strlen(d), sizeof d - strlen(d), "%s{\"detail\":\"d\",\"id\":\"c%d\",\"label\":\"L\",\"level\":\"warn\"}", i ? "," : "", i);
    strcat(d, "]}");
    parse_checks(d);
    CHECK("capped at HEALTH_MAX", health_count() == HEALTH_MAX);

    {   /* events: the label comes from zte-agent; an older agent sends none */
        char ev[512];
        alert_item_t a;
        snprintf(ev, sizeof ev, "[{\"seq\":3,\"time\":null,\"uptime\":42,\"kind\":\"agent-crash\","
                 "\"label\":\"管理后台意外退出，已自动重启\",\"text\":\"t\",\"unread\":true},"
                 "{\"seq\":2,\"time\":1782396733,\"uptime\":1,\"kind\":\"x-new\",\"text\":\"u\",\"unread\":false}]");
        parse_events(ev);
        alerts_get(0, &a);
        CHECK("event label from the agent", !strcmp(a.label, "管理后台意外退出，已自动重启") && a.unread && a.seq == 3);
        alerts_get(1, &a);
        CHECK("no label (older agent): says what kind", !strcmp(a.label, "其他告警（x-new）") && !a.unread);
    }

    {   /* English: label_en / detail_en from the agent; null or missing → the Chinese */
        char ev[512];
        alert_item_t a;
        lang_set_en(1);
        snprintf(ev, sizeof ev, "[{\"seq\":5,\"time\":null,\"uptime\":1,\"kind\":\"agent-crash\","
                 "\"label\":\"管理后台意外退出\",\"label_en\":\"Admin backend (zte-agent) exited unexpectedly\","
                 "\"text\":\"t\",\"unread\":true},"
                 "{\"seq\":4,\"time\":null,\"uptime\":1,\"kind\":\"y\",\"label\":\"中文\",\"label_en\":\"\","
                 "\"text\":\"u\",\"unread\":false}]");
        parse_events(ev);
        alerts_get(0, &a);
        CHECK("en: label_en", !strcmp(a.label, "Admin backend (zte-agent) exited unexpectedly"));
        alerts_get(1, &a);
        CHECK("en: empty label_en → Chinese", !strcmp(a.label, "中文"));
        snprintf(d, sizeof d, "{\"bad\":1,\"checked_at\":1,\"checks\":["
                 "{\"detail\":\"没在运行\",\"detail_en\":\"Not running\",\"id\":\"screen\",\"label\":\"触屏界面\","
                 "\"label_en\":\"Screen UI\",\"level\":\"bad\"},"
                 "{\"detail\":\"空间不够\",\"detail_en\":null,\"id\":\"disk\",\"label\":\"/data 空间\","
                 "\"label_en\":null,\"level\":\"warn\"}],\"crashlogs\":[]}");
        parse_checks(d);
        health_get(0, &h);
        CHECK("en: health label_en/detail_en", !strcmp(h.label, "Screen UI") && !strcmp(h.detail, "Not running"));
        health_get(1, &h);
        CHECK("en: null *_en → Chinese", !strcmp(h.label, "/data 空间") && !strcmp(h.detail, "空间不够"));
        lang_set_en(0);
    }

    printf("\npassed %d, failed %d\n", pass, fail);
    return fail != 0;
}
