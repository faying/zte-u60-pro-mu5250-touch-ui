/*
 * net_view.c + json_arr_next tests: parsing zwrt-datad's /v2/screen "net".
 * Uses the render fixtures' views (tests/render/views.h), which have the
 * shape datad sends. Host build with ASan/UBSan: scripts/test/net_view/run.sh
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/net_view.c"
#include "render/views.h"

static int s_fail, s_pass_n;
#define CHECK(c) do { if (c) s_pass_n++; else { s_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static const char *view(const char *scene)
{
    for (size_t i = 0; i < sizeof k_views / sizeof *k_views; i++)
        if (!strcmp(k_views[i].scene, scene)) return k_views[i].json;
    return NULL;
}

int main(void)
{
    char out[64];
    const char *p;
    net_view_t v;

    /* json_arr_next */
    p = json_arr_next("[{\"a\":[1,{\"b\":\"]}\"}]},2, \"x,y\" ,[3]]", out, sizeof out);
    CHECK(p && !strcmp(out, "{\"a\":[1,{\"b\":\"]}\"}]}"));
    p = json_arr_next(p, out, sizeof out);  CHECK(p && !strcmp(out, "2"));
    p = json_arr_next(p, out, sizeof out);  CHECK(p && !strcmp(out, "\"x,y\""));
    p = json_arr_next(p, out, sizeof out);  CHECK(p && !strcmp(out, "[3]"));
    CHECK(json_arr_next(p, out, sizeof out) == NULL);
    CHECK(json_arr_next("[]", out, sizeof out) == NULL);
    CHECK(json_arr_next("", out, sizeof out) == NULL && json_arr_next(NULL, out, sizeof out) == NULL);
    /* malformed: never the same position twice (the caller loops on it) */
    CHECK(json_arr_next("}", out, sizeof out) == NULL);
    CHECK(json_arr_next("[}", out, sizeof out) == NULL);
    CHECK(json_arr_next("[1}", out, sizeof out) != NULL);
    {
        static const char *junk[] = { "[", "[,", "[,,]", "[{", "[\"", "[\"\\", "[{\"a\":[}", "[1,", "{]", "[[[[", "[{]}]" };
        for (size_t i = 0; i < sizeof junk / sizeof *junk; i++) {
            const char *q = junk[i];
            int steps = 0;
            while ((q = json_arr_next(q, out, sizeof out)) != NULL && steps < 100) steps++;
            CHECK(steps < 100);
        }
    }
    CHECK(net_view_parse("{\"story\":{\"headline\":\"x\"},\"carriers\":\"}\"}", &v) == 1 && v.ca_n == 0);
    CHECK(net_view_parse("{\"story\":{\"headline\":\"x\"},\"carriers\":[}]}", &v) == 1 && v.ca_n == 0);
    p = json_arr_next("[{\"long\":\"0123456789abcdef\"}]", out, 8);
    CHECK(p && strlen(out) == 7);                              /* cut, still terminated */

    /* the good scene: 5G SA, three NR carriers */
    CHECK(net_view_parse(view("good"), &v) == 1);
    CHECK(!strcmp(v.story.headline, "顺畅") && v.story.tone == UI_NET_OK && v.story.cause == UI_CAUSE_NONE);
    CHECK(v.ca_n >= 1 && v.ca[0].kind == 'n' && v.ca[0].active && !strcmp(v.ca[0].label, "n78"));
    CHECK(v.ca[0].rsrp[0] && v.ca[0].sinr[0] && v.ca[0].sinr_tone == UI_NET_OK);
    CHECK(v.sim_usable && v.bars_tier == 2 && v.story.sig_tone == UI_NET_OK);
    CHECK(!strcmp(v.story.sig, "强") && v.fine[0] && v.name[0] && v.ca_val[0]);

    /* every scene parses and carries a headline, except the ones datad never answered in */
    for (size_t i = 0; i < sizeof k_views / sizeof *k_views; i++) {
        int ok = net_view_parse(k_views[i].json, &v);
        CHECK(ok && v.ca_n <= NV_CA_MAX && v.bars_tier >= -1 && v.bars_tier <= 2);
    }
    CHECK(net_view_parse(view("nosvc"), &v) && v.nosvc);
    CHECK(net_view_parse(view("nosim"), &v) && !v.sim_usable);

    /* broken or empty input: nothing claimed */
    CHECK(net_view_parse(NULL, &v) == 0 && v.ca_n == 0 && v.bars_tier == -1);
    CHECK(net_view_parse("{}", &v) == 0);
    CHECK(net_view_parse("{\"story\":{}}", &v) == 0);
    CHECK(net_view_parse("not json", &v) == 0);
    CHECK(net_view_parse("{\"story\":{\"headline\":\"x\",\"tone\":\"new-tone\"},\"carriers\":\"oops\"}", &v) == 1 &&
          v.story.tone == UI_NET_NEUTRAL && v.ca_n == 0);
    {   /* more carriers than slots */
        char big[4096];
        size_t o = (size_t)snprintf(big, sizeof big, "{\"story\":{\"headline\":\"x\"},\"carriers\":[");
        for (int i = 0; i < 9; i++)
            o += (size_t)snprintf(big + o, sizeof big - o, "%s{\"kind\":\"lte\",\"band\":%d,\"active\":true}", i ? "," : "", i + 1);
        snprintf(big + o, sizeof big - o, "]}");
        CHECK(net_view_parse(big, &v) == 1 && v.ca_n == NV_CA_MAX && v.ca[4].band == 5 && v.ca[4].kind == 'B');
    }

    net_view_placeholder(&v, "读取中…", "");
    CHECK(!strcmp(v.story.headline, "读取中…") && v.story.tone == UI_NET_NEUTRAL && v.sim_usable && v.ca_n == 0);
    net_view_placeholder(&v, "—", "数据服务版本太旧");
    CHECK(!strcmp(v.name, "数据服务版本太旧") && !v.story.hint[0] && v.bars_tier == -1);

    printf("passed %d, failed %d\n", s_pass_n, s_fail);
    return s_fail != 0;
}
