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
    CHECK(net_view_parse(view("nosvc"), &v) && v.nosvc && (v.state == NV_STATE_NOSVC || v.state == NV_STATE_SOS));
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

    /* story.state as a code; an old datad without it, or a new code → unknown */
    {
        static const struct { const char *s; nv_state_t want; } st[] = {
            { "ok", NV_STATE_OK }, { "nosim", NV_STATE_NOSIM }, { "airplane", NV_STATE_AIRPLANE },
            { "sos", NV_STATE_SOS }, { "nosvc", NV_STATE_NOSVC }, { "nodata", NV_STATE_NODATA },
            { "limit", NV_STATE_LIMIT }, { "weak", NV_STATE_WEAK }, { "noise", NV_STATE_NOISE },
            { "crowd", NV_STATE_CROWD }, { "only2g", NV_STATE_ONLY2G }, { "only3g", NV_STATE_ONLY3G },
            { "narrow", NV_STATE_NARROW }, { "", NV_STATE_UNKNOWN }, { "later", NV_STATE_UNKNOWN },
        };
        char js[160];
        for (size_t i = 0; i < sizeof st / sizeof *st; i++) {
            snprintf(js, sizeof js, "{\"story\":{\"headline\":\"x\",\"state\":\"%s\"}}", st[i].s);
            CHECK(net_view_parse(js, &v) == 1 && v.state == st[i].want);
        }
        CHECK(net_view_parse("{\"story\":{\"headline\":\"x\"}}", &v) == 1 && v.state == NV_STATE_UNKNOWN);
    }

    /* English: every *_en sibling replaces its field; missing / empty → the Chinese */
    {
        static const char js[] =
            "{\"story\":{\"tone\":\"warn\",\"cause\":\"weak\",\"headline\":\"慢：信号弱\",\"hint\":\"离基站远\","
            "\"rat\":\"5G\",\"link\":\"单载波 · 带宽一般\",\"sig\":\"弱\",\"noise\":\"大\",\"load\":\"高\",\"limit\":\"无\","
            "\"state\":\"weak\",\"headline_en\":\"Slow\",\"hint_en\":\"Weak signal; try near a window\","
            "\"link_en\":\"Single carrier · Fair\",\"sig_en\":\"Weak\",\"noise_en\":\"high\",\"load_en\":\"\","
            "\"limit_en\":\"none\"},"
            "\"fine\":\"5G NSA · 4G 锚点\",\"fine_en\":\"5G NSA · 4G anchor\",\"name\":\"中国移动\",\"name_en\":\"China Mobile\","
            "\"where\":\"本地\",\"where_en\":\"Local\",\"ca_val\":\"单载波\",\"ca_val_en\":\"Single carrier\","
            "\"ca_sub\":\"↓ n78   ↑ n78\",\"mode_word\":\"自动\",\"mode_word_en\":\"Auto\"}";
        CHECK(net_view_parse(js, &v) == 1 && v.state == NV_STATE_WEAK);
        CHECK(!strcmp(v.story.headline, "慢：信号弱") && !strcmp(v.name, "中国移动") && !strcmp(v.story.load, "高"));
        lang_set_en(1);
        CHECK(net_view_parse(js, &v) == 1 && v.state == NV_STATE_WEAK && v.story.cause == UI_CAUSE_WEAK);
        CHECK(!strcmp(v.story.headline, "Slow") && !strcmp(v.story.hint, "Weak signal; try near a window"));
        CHECK(!strcmp(v.story.rat, "5G") && !strcmp(v.story.link, "Single carrier · Fair"));
        CHECK(!strcmp(v.story.sig, "Weak") && !strcmp(v.story.noise, "high") && !strcmp(v.story.limit, "none"));
        CHECK(!strcmp(v.story.load, "高"));                       /* empty _en → the Chinese */
        CHECK(!strcmp(v.fine, "5G NSA · 4G anchor") && !strcmp(v.name, "China Mobile") && !strcmp(v.where, "Local"));
        CHECK(!strcmp(v.ca_val, "Single carrier") && !strcmp(v.ca_sub, "↓ n78   ↑ n78") && !strcmp(v.mode_word, "Auto"));
        /* an old datad (no *_en at all) still shows its Chinese */
        CHECK(net_view_parse("{\"story\":{\"headline\":\"顺畅\"},\"name\":\"未注册\"}", &v) == 1 &&
              !strcmp(v.story.headline, "顺畅") && !strcmp(v.name, "未注册"));
        CHECK(net_view_parse(view("good"), &v) == 1 && !strcmp(v.story.headline, "All good") && v.state == NV_STATE_OK);
        lang_set_en(0);
    }

    net_view_placeholder(&v, "读取中…", "");
    CHECK(!strcmp(v.story.headline, "读取中…") && v.story.tone == UI_NET_NEUTRAL && v.sim_usable && v.ca_n == 0);
    net_view_placeholder(&v, "—", "数据服务版本太旧");
    CHECK(!strcmp(v.name, "数据服务版本太旧") && !v.story.hint[0] && v.bars_tier == -1);

    printf("passed %d, failed %d\n", s_pass_n, s_fail);
    return s_fail != 0;
}
