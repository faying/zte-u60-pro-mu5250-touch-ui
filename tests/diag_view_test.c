/*
 * diag_view.c tests: zte-agent's diagnosis run (deep_diag.rs Run) as the
 * 网络诊断 page reads it. Runs from tests/render/diag_runs.h, the ones the
 * render scenes draw. Host build with ASan/UBSan: scripts/test/diag_view/run.sh
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/diag_view.c"
#include "render/diag_runs.h"

static int s_fail, s_pass_n;
#define CHECK(c) do { if (c) s_pass_n++; else { s_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
    diag_run_t r;
    const dg_layer_t *l;

    /* nothing, or not a run */
    CHECK(diag_view_parse(NULL, &r) == 0 && r.state == DG_NONE);
    CHECK(diag_view_parse("{}", &r) == 0 && r.state == DG_NONE);
    CHECK(diag_view_parse("{\"state\":\"later\"}", &r) == 0 && r.state == DG_NONE);
    CHECK(diag_view_parse("not json", &r) == 0);
    CHECK(diag_view_parse("{\"state\":\"idle\"}", &r) == 1 && r.state == DG_IDLE && r.n == 0 && r.feedback == -1);

    /* running: rows in order, pending / running ones empty, no main yet */
    lang_set_en(0);
    CHECK(diag_view_parse(k_diag_running, &r) == 1 && r.state == DG_RUN && r.id == 7);
    CHECK(r.step == 3 && r.steps >= 5 && r.n >= 5 && !r.has_main && !r.has_speed);
    CHECK(!strcmp(r.layer[0].id, "wifi") && r.layer[0].level == DG_OK && strstr(r.layer[0].detail, "iPad"));
    CHECK(r.layer[3].level == DG_RUNNING && !r.layer[3].detail[0]);
    CHECK(r.layer[4].level == DG_PENDING && r.layer[4].counted);
    CHECK(r.finished_at == 0 && r.age_s == -1 && r.feedback == -1);

    /* done: main, more, na rows (one not counted), the speed row */
    CHECK(diag_view_parse(k_diag_result, &r) == 1 && r.state == DG_DONE);
    CHECK(r.has_main && !strcmp(r.main_layer, "link") && r.main_has_level && r.main_level == DG_WARN && r.more == 1);
    CHECK(!strcmp(r.main_text, "蜂窝链路不稳") && !strcmp(r.main_action, "过几分钟再试，或换个地方") && !r.action_to[0]);
    CHECK((l = diag_find(&r, "wifi")) && l->level == DG_NA && !l->counted && !strcmp(l->detail, "没有设备连着"));
    CHECK((l = diag_find(&r, "limit")) && l->level == DG_NA && l->counted);
    CHECK(r.has_speed && r.speed.level == DG_INFO && !strcmp(r.speed.detail, "直连 ↓ 86 Mbps"));
    CHECK(r.age_s == 12 && r.finished_at == 1790249511L && r.feedback == -1);
    CHECK(diag_find(&r, "nope") == NULL);

    /* English: every *_en sibling */
    lang_set_en(1);
    CHECK(diag_view_parse(k_diag_result, &r) == 1 && !strcmp(r.main_text, "Cellular link unstable"));
    CHECK(!strcmp(r.main_action, "Try again in a few minutes, or move"));
    CHECK((l = diag_find(&r, "wifi")) && !strcmp(l->detail, "No devices on Wi-Fi"));
    CHECK(!strcmp(r.speed.detail, "Direct ↓ 86 Mbps"));
    lang_set_en(0);

    /* the weak-signal run: placement action, feedback already given */
    CHECK(diag_view_parse(k_diag_weak, &r) == 1 && !strcmp(r.action_to, "placement") && r.main_level == DG_BAD);
    CHECK(r.feedback == 1 && r.more == 0);

    /* all good: main without a layer or level */
    CHECK(diag_view_parse("{\"id\":2,\"state\":\"done\",\"layers\":[],\"main\":{\"layer\":\"\",\"level\":null,"
                          "\"text\":\"没查到问题\",\"text_en\":\"No problem found\",\"action\":\"可能是对方网站慢；也可以加测速度\","
                          "\"action_en\":\"\",\"action_to\":\"\",\"more\":0},\"feedback\":false}", &r) == 1);
    CHECK(r.has_main && !r.main_layer[0] && !r.main_has_level && r.feedback == 0 && r.n == 0);
    lang_set_en(1);   /* an empty _en falls back to the Chinese */
    CHECK(diag_view_parse("{\"state\":\"done\",\"main\":{\"text\":\"中\",\"text_en\":\"\"}}", &r) == 1 && !strcmp(r.main_text, "中"));
    lang_set_en(0);

    /* a level from a later agent reads as "can't tell"; more rows than slots */
    CHECK(diag_view_parse("{\"state\":\"done\",\"layers\":[{\"id\":\"x\",\"level\":\"new\"}]}", &r) == 1 && r.layer[0].level == DG_NA);
    {
        char big[4096];
        size_t o = (size_t)snprintf(big, sizeof big, "{\"state\":\"running\",\"layers\":[");
        for (int i = 0; i < 12; i++)
            o += (size_t)snprintf(big + o, sizeof big - o, "%s{\"id\":\"l%d\",\"level\":\"ok\"}", i ? "," : "", i);
        snprintf(big + o, sizeof big - o, "]}");
        CHECK(diag_view_parse(big, &r) == 1 && r.n == DG_LAYERS_MAX && !strcmp(r.layer[7].id, "l7"));
    }
    /* a detail longer than the slot is cut, still terminated */
    {
        char big[600];
        size_t o = (size_t)snprintf(big, sizeof big, "{\"state\":\"done\",\"layers\":[{\"id\":\"wifi\",\"level\":\"ok\",\"detail\":\"");
        for (int i = 0; i < 300; i++) big[o++] = 'a';
        snprintf(big + o, sizeof big - o, "\"}]}");
        CHECK(diag_view_parse(big, &r) == 1 && strlen(r.layer[0].detail) == sizeof r.layer[0].detail - 1);
    }

    /* words */
    CHECK(!strcmp(diag_layer_name("wifi"), "Wi-Fi") && !strcmp(diag_layer_name("crowd"), "基站负载"));
    CHECK(!strcmp(diag_layer_name("future"), "future"));
    CHECK(!strcmp(diag_level_word(DG_OK), "正常") && !strcmp(diag_level_word(DG_BAD), "差") && !diag_level_word(DG_INFO)[0]);
    CHECK(!strcmp(diag_level_mark(DG_WARN), "▲") && !strcmp(diag_level_mark(DG_BAD), "■") && !diag_level_mark(DG_PENDING)[0]);

    printf("diag_view: passed %d, failed %d\n", s_pass_n, s_fail);
    return s_fail ? 1 : 0;
}
