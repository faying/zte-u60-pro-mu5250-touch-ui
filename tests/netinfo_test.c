/*
 * Unit tests for netinfo.c's parser against /api/netinfo bodies shaped like
 * zte-agent's netinfo.rs output (serde_json sorts keys, so `error` comes
 * first in every exit object). No device, no agent:
 *   scripts/test/netinfo/run.sh
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/netinfo.c"


static int pass, fail;

#define CHECK(desc, cond)                                              \
    do {                                                               \
        if (cond) { pass++; printf("  ok   %s\n", desc); }             \
        else      { fail++; printf("  FAIL %s  [%s]\n", desc, #cond); } \
    } while (0)

static char body[NI_RESP_MAX];

static void cells_and_ops(char *out, size_t cap, int ncells, int nops)
{
    size_t o = 0;
    o += (size_t)snprintf(out + o, cap - o, "\"neighbors\":{\"cells\":[");
    for (int i = 0; i < ncells; i++)
        o += (size_t)snprintf(out + o, cap - o, "%s{\"arfcn\":\"627264\",\"pci\":%d,\"rat\":\"NR\",\"rsrp\":\"-%d\",\"rsrq\":null,\"sinr\":null}",
                              i ? "," : "", 100 + i, 90 + i);
    o += (size_t)snprintf(out + o, cap - o, "],\"error\":null,\"scanned_at\":1790250000,\"state\":\"done\"},");
    o += (size_t)snprintf(out + o, cap - o, "\"network_type\":\"SA\",\"now\":1790250112,");
    o += (size_t)snprintf(out + o, cap - o, "\"proxy\":null,\"roaming\":true,\"roaming_raw\":\"Roaming\",");
    o += (size_t)snprintf(out + o, cap - o, "\"scan\":{\"error\":null,\"finished_at\":1,\"last_status\":\"2\",\"operators\":[");
    for (int i = 0; i < nops; i++)
        o += (size_t)snprintf(out + o, cap - o, "%s{\"country\":\"日本\",\"name\":\"Op%d\",\"plmn\":\"440%02d\",\"rat\":\"12\",\"raw_name\":\"X\",\"status\":\"1\"}",
                              i ? "," : "", i, i);
    snprintf(out + o, cap - o, "],\"started_at\":0,\"state\":\"done\"},");
}

int main(void)
{
    char mid[16384];
    const netinfo_t *n = &s_ni;
    char longerr[200];

    memset(longerr, 'e', 150);
    longerr[150] = 0;

    puts("full body, no second exit, roaming, more rows than the screen holds");
    cells_and_ops(mid, sizeof mid, 16, 12);
    snprintf(body, sizeof body,
             "{\"apn\":{\"auto\":[{\"apn\":\"ctiot\",\"id\":\"auto109590\",\"in_use\":true,\"iot\":true,\"name\":\"China Telecom\",\"pdp\":3,\"selected\":true},"
             "{\"apn\":\"ctnet\",\"id\":\"auto109600\",\"in_use\":false,\"iot\":false,\"name\":\"China Telecom 4G\",\"pdp\":3,\"selected\":false}],"
             "\"picked_id\":null,\"notice\":{\"at\":1,\"text\":\"old\",\"text_en\":\"old\"},"
             "\"in_use\":{\"apn\":\"ctiot\",\"id\":\"auto109590\",\"in_use\":true,\"iot\":true,\"name\":\"China Telecom\",\"pdp\":3,\"selected\":false},"
             "\"manual\":[{\"apn\":\"ctnet\",\"id\":\"manu1\",\"in_use\":false,\"name\":\"CTNET\",\"pdp\":3,\"selected\":true}],\"mode\":\"auto\"},"
             "\"clients\":{\"at\":1790250000,\"list\":["
             "{\"connected_secs\":360,\"down_bytes\":5000000000,\"down_rate\":250000,\"iface\":\"wlan1\",\"ip\":\"192.168.0.109\",\"mac\":\"02:00:00:00:00:0a\",\"name\":\"MacBook\",\"signal\":-47,\"signal_tier\":\"great\",\"up_bytes\":1000,\"up_rate\":null,"
             "\"band\":\"5 GHz\",\"channel\":44,\"width_mhz\":160,\"wifi_gen\":6,\"link_down_mbps\":2402,\"link_up_mbps\":1201},"
             "{\"connected_secs\":5,\"down_bytes\":20,\"down_rate\":null,\"iface\":\"wlan1\",\"ip\":null,\"mac\":\"02:00:00:00:00:0b\",\"name\":null,\"signal\":null,\"up_bytes\":10,\"up_rate\":null,\"band\":null,\"wifi_gen\":null,\"link_down_mbps\":null}]},"
             "\"data_connected\":true,"
             "\"direct\":{\"error\":\"%s\",\"fetched_at\":1790250000,\"geo\":\"\\u65e5\\u672c \\u5927\\u962a\\u5e9c\",\"ip\":\"198.51.100.40\",\"isp\":\"SoftBank\",\"node\":null,\"source\":\"ip-api.com\"},"
             "\"guard\":{\"finished_at\":0,\"last_result\":\"\",\"phase\":\"registering\",\"rat\":\"12\",\"reason\":\"\",\"started_at\":1,\"target\":\"44020\"},"
             "\"home_operator\":{\"country\":\"中国\",\"mcc\":\"460\",\"mnc\":\"01\",\"name\":\"中国联通\"},"
             "%s"
             "\"scenes\":{\"current\":\"abroad\",\"enabled\":true,\"guard_takeover\":false,\"list\":["
             "{\"abroad\":false,\"id\":\"home\",\"name\":\"在家\",\"wifi_off\":true},"
             "{\"abroad\":false,\"id\":\"away\",\"name\":\"外出\",\"wifi_off\":false},"
             "{\"abroad\":true,\"id\":\"abroad\",\"name\":\"国外\",\"wifi_off\":false}],\"pin\":null},"
             "\"selection\":{\"checked_at\":1,\"mode\":\"manual\"},"
             "\"serving_operator\":{\"country\":\"日本\",\"mcc\":\"440\",\"mnc\":\"20\",\"name\":\"SoftBank\"}}",
             longerr, mid);
    parse(body);
    CHECK("direct present", n->direct.present);
    CHECK("direct ip survives a long error before it", !strcmp(n->direct.ip, "198.51.100.40"));
    CHECK("\\u escapes decoded", !strcmp(n->direct.geo, "日本 大阪府"));
    CHECK("error truncated, not overflowed", strlen(n->direct.err) == sizeof n->direct.err - 1);
    CHECK("proxy null → absent", !n->proxy.present && !n->proxy.ip[0]);
    CHECK("home operator", !strcmp(n->home.name, "中国联通") && !strcmp(n->home.mnc, "01"));
    CHECK("serving operator (not confused with home)", !strcmp(n->serving.name, "SoftBank") && !strcmp(n->serving.mcc, "440"));
    CHECK("roaming", n->roaming == 1);
    CHECK("selection manual", !strcmp(n->selection, "manual"));
    CHECK("guard", !strcmp(n->guard_phase, "registering") && !strcmp(n->guard_target, "44020"));
    CHECK("cells capped", n->ncells == NI_MAX_CELLS);
    CHECK("numeric pci read as text", !strcmp(n->cells[0].pci, "100"));
    CHECK("null rsrq → empty", n->cells[0].rsrq[0] == 0);
    CHECK("last kept cell", !strcmp(n->cells[NI_MAX_CELLS - 1].rsrp, "-97"));
    CHECK("ops capped", n->nops == NI_MAX_OPS);
    CHECK("op fields", !strcmp(n->ops[3].plmn, "44003") && !strcmp(n->ops[3].rat, "12") && !strcmp(n->ops[3].country, "日本"));
    CHECK("scan state", !strcmp(n->scan_state, "done"));
    CHECK("nbr time", n->nbr_at == 1790250000);
    CHECK("scenes", n->scene_known && n->scene_enabled && n->nscenes == 3);
    CHECK("apn mode auto", n->apn_known && !n->apn_manual);
    CHECK("apn in use = dialled, not the nested auto entry", !strcmp(n->apn_in_use.id, "auto109590") && !strcmp(n->apn_in_use.apn, "ctiot") && n->apn_in_use.pdp == 3);
    CHECK("apn manual list", n->napns == 1 && !strcmp(n->apns[0].name, "CTNET") && n->apns[0].selected && !n->apns[0].in_use);
    CHECK("apn candidates", n->ncands == 2 && !strcmp(n->cands[1].id, "auto109600") && n->cands[0].iot && !n->cands[1].iot);
    CHECK("apn in use iot", n->apn_in_use.iot);
    CHECK("apn picked null = none", n->apn_picked[0] == 0);
    CHECK("apn notice older than 10 min dropped", n->apn_notice[0] == 0);
    CHECK("clients", n->clients_known && n->nclients == 2);
    CHECK("client bytes > 4 GB", n->clients[0].down == 5000000000LL && n->clients[0].down_rate == 250000);
    CHECK("client no rate yet", n->clients[0].up_rate == -1 && n->clients[1].name[0] == 0);
    CHECK("client signal", n->clients[0].signal == -47 && n->clients[1].signal == 0);
    CHECK("client signal tier from the agent", !strcmp(n->clients[0].signal_tier, "great") && !n->clients[1].signal_tier[0]);
    CHECK("client band", !strcmp(n->clients[0].band, "5 GHz") && n->clients[0].wifi_gen == 6 && n->clients[0].link_down == 2402);
    CHECK("client band null → empty", n->clients[1].band[0] == 0 && n->clients[1].wifi_gen == 0 && n->clients[1].link_down == 0);
    CHECK("scene fields", !strcmp(n->scenes[0].name, "在家") && n->scenes[0].wifi_off && n->scenes[2].abroad);
    CHECK("scene current, no pin", !strcmp(n->scene_current, "abroad") && n->scene_pin[0] == 0);

    puts("APN: every manual profile the web added (up to NI_MAX_APNS)");
    {
        static char b2[8192];
        size_t o = (size_t)snprintf(b2, sizeof b2, "{\"apn\":{\"auto\":[],\"in_use\":null,\"manual\":[");
        for (int i = 0; i < 12; i++)
            o += (size_t)snprintf(b2 + o, sizeof b2 - o, "%s{\"apn\":\"apn%d\",\"id\":\"manu%d\",\"in_use\":false,\"name\":\"Web %d\",\"pdp\":1,\"selected\":%s}",
                                  i ? "," : "", i, i, i, i == 7 ? "true" : "false");
        snprintf(b2 + o, sizeof b2 - o, "],\"mode\":\"manual\"},\"direct\":null,\"proxy\":null}");
        parse(b2);
        CHECK("ten manual APNs listed", n->napns == 10 && NI_MAX_APNS == 10);
        CHECK("8th one (added in the web) is there and marked", !strcmp(n->apns[7].name, "Web 7") && n->apns[7].selected);
    }

    puts("empty body: nothing known yet");
    parse("{\"direct\":null,\"guard\":{\"phase\":\"idle\"},\"home_operator\":null,"
          "\"neighbors\":{\"cells\":[],\"state\":\"idle\"},\"proxy\":null,\"roaming\":null,"
          "\"scan\":{\"operators\":[],\"state\":\"idle\"},\"selection\":{\"mode\":null},\"serving_operator\":null}");
    CHECK("no exits", !n->direct.present && !n->proxy.present);
    CHECK("roaming unknown", n->roaming == -1);
    CHECK("selection unknown", n->selection[0] == 0);
    CHECK("home cleared", n->home.name[0] == 0 && n->home.mcc[0] == 0);
    CHECK("no rows", n->ncells == 0 && n->nops == 0);
    CHECK("no scenes section → unknown", !n->scene_known && n->nscenes == 0);

    puts("garbage");
    parse("not json at all");
    CHECK("garbage parses to nothing", !n->direct.present && n->ncells == 0);
    parse("{\"neighbors\":{\"cells\":[{\"pci\":\"1\"");
    CHECK("truncated body does not crash", n->ncells == 0);

    puts("L2: English twins (operator_en, country_en/_iso, reason_en/_code, error_en, scenes.list_en)");
    {
        static const char l2[] =
            "{\"guard\":{\"phase\":\"reverted\",\"reason\":\"手动恢复自动\",\"reason_code\":\"manual_auto\","
            "\"reason_en\":\"Back to automatic on request\",\"target\":\"44020\"},"
            "\"home_operator\":{\"country\":\"中国\",\"country_en\":\"China\",\"country_iso\":\"CN\",\"mcc\":\"460\",\"mnc\":\"01\","
            "\"name\":\"中国联通\",\"operator_en\":\"China Unicom\"},"
            "\"neighbors\":{\"cells\":[],\"error\":\"原厂扫描会断网且拿不到数据，已停用\","
            "\"error_en\":\"Off: the stock scan drops data, finds nothing\",\"state\":\"unsupported\"},"
            "\"scan\":{\"error\":\"没搜到网络\",\"error_en\":\"No networks found\",\"operators\":["
            "{\"country\":\"中国台湾\",\"country_en\":\"Taiwan\",\"country_iso\":\"TW\",\"name\":\"中华电信\","
            "\"operator_en\":\"Chunghwa Telecom\",\"plmn\":\"46692\",\"rat\":\"7\",\"status\":\"1\"},"
            "{\"country\":null,\"country_en\":null,\"country_iso\":null,\"name\":\"X\",\"operator_en\":\"X\",\"plmn\":\"52098\",\"rat\":\"7\",\"status\":\"1\"}],"
            "\"state\":\"error\"},"
            "\"scenes\":{\"current\":\"home\",\"enabled\":true,\"guard_takeover\":false,\"list\":["
            "{\"abroad\":false,\"does\":\"关 Wi-Fi\",\"id\":\"home\",\"name\":\"在家\",\"when\":\"附近有 Wi-Fi「A」时\",\"wifi_off\":true},"
            "{\"abroad\":false,\"does\":\"开 Wi-Fi\",\"id\":\"office\",\"name\":\"办公室\",\"when\":\"插的是日本的卡时\",\"wifi_off\":false}],"
            "\"list_en\":["
            "{\"does_en\":\"Wi-Fi off\",\"id\":\"home\",\"name_en\":\"Home\",\"when_en\":\"When Wi-Fi \\\"A\\\" is nearby\"},"
            "{\"does_en\":\"Wi-Fi on\",\"id\":\"office\",\"name_en\":null,\"when_en\":\"When the SIM is from Japan\"}],"
            "\"pin\":null},"
            "\"serving_operator\":{\"country\":\"日本\",\"country_en\":\"Japan\",\"country_iso\":\"JP\",\"mcc\":\"440\",\"mnc\":\"20\","
            "\"name\":\"SoftBank\",\"operator_en\":\"SoftBank\"}}";
        /* before L2: the same body without any *_en / *_iso / reason_code */
        static const char old[] =
            "{\"guard\":{\"phase\":\"reverted\",\"reason\":\"注册失败\",\"target\":\"44020\"},"
            "\"home_operator\":{\"country\":\"中国\",\"mcc\":\"460\",\"mnc\":\"01\",\"name\":\"中国联通\"},"
            "\"scan\":{\"error\":\"没搜到网络\",\"operators\":[],\"state\":\"error\"},"
            "\"scenes\":{\"current\":\"home\",\"enabled\":true,\"list\":["
            "{\"abroad\":false,\"does\":\"关 Wi-Fi\",\"id\":\"home\",\"name\":\"在家\",\"when\":\"w\",\"wifi_off\":true}],\"pin\":null}}";

        lang_set_en(0);
        parse(l2);
        CHECK("zh: operator stays Chinese", !strcmp(n->home.name, "中国联通") && !strcmp(n->home.country, "中国"));
        CHECK("zh: iso read for comparing", !strcmp(n->home.country_iso, "CN") && !strcmp(n->serving.country_iso, "JP"));
        CHECK("zh: reason Chinese, code read", !strcmp(n->guard_reason, "手动恢复自动") && !strcmp(n->guard_reason_code, "manual_auto"));
        CHECK("zh: errors Chinese", !strcmp(n->scan_err, "没搜到网络") && !strcmp(n->nbr_err, "原厂扫描会断网且拿不到数据，已停用"));
        CHECK("zh: scan op Chinese", !strcmp(n->ops[0].name, "中华电信") && !strcmp(n->ops[0].country, "中国台湾") && !strcmp(n->ops[0].country_iso, "TW"));
        CHECK("zh: scenes Chinese", n->nscenes == 2 && !strcmp(n->scenes[0].name, "在家") && !strcmp(n->scenes[0].when, "附近有 Wi-Fi「A」时"));

        lang_set_en(1);
        parse(l2);
        CHECK("en: operator_en", !strcmp(n->home.name, "China Unicom") && !strcmp(n->serving.name, "SoftBank"));
        CHECK("en: country_en", !strcmp(n->home.country, "China") && !strcmp(n->serving.country, "Japan"));
        CHECK("en: reason_en, code", !strcmp(n->guard_reason, "Back to automatic on request") && !strcmp(n->guard_reason_code, "manual_auto"));
        CHECK("en: scan error_en", !strcmp(n->scan_err, "No networks found"));
        CHECK("en: neighbors error_en, whole", !strcmp(n->nbr_err, "Off: the stock scan drops data, finds nothing"));
        CHECK("en: scan op English", !strcmp(n->ops[0].name, "Chunghwa Telecom") && !strcmp(n->ops[0].country, "Taiwan"));
        CHECK("en: op with null country", !strcmp(n->ops[1].name, "X") && !n->ops[1].country[0] && !n->ops[1].country_iso[0]);
        CHECK("en: factory scene name_en", !strcmp(n->scenes[0].name, "Home"));
        CHECK("en: when_en / does_en (escaped quotes)", !strcmp(n->scenes[0].when, "When Wi-Fi \"A\" is nearby") && !strcmp(n->scenes[0].does, "Wi-Fi off"));
        CHECK("en: renamed scene keeps its name (name_en null)", !strcmp(n->scenes[1].name, "办公室") && !strcmp(n->scenes[1].when, "When the SIM is from Japan"));
        CHECK("en: ids untouched", !strcmp(n->scenes[0].id, "home") && !strcmp(n->scene_current, "home"));

        parse(old);
        CHECK("en, old agent: falls back to the Chinese", !strcmp(n->home.name, "中国联通") && !strcmp(n->guard_reason, "注册失败") && !strcmp(n->scan_err, "没搜到网络"));
        CHECK("en, old agent: no code / iso", !n->guard_reason_code[0] && !n->home.country_iso[0]);
        CHECK("en, old agent: scenes Chinese", n->nscenes == 1 && !strcmp(n->scenes[0].name, "在家") && !strcmp(n->scenes[0].when, "w"));
        lang_set_en(0);
    }

    puts("L2: longest twins still leave the fields after them");
    {
        static char lb[4096];
        char zh[200] = "", en[140], nm[60];
        for (int i = 0; i < 60; i++) strcat(zh, "长");          /* 180 bytes, the agent's clip */
        memset(en, 'e', 120); en[120] = 0;
        memset(nm, 'N', 47); nm[47] = 0;
        snprintf(lb, sizeof lb,
                 "{\"guard\":{\"finished_at\":1790250000,\"last_result\":\"%s\",\"phase\":\"revert_failed\",\"rat\":\"12\","
                 "\"reason\":\"%s\",\"reason_code\":\"auto_retry\",\"reason_en\":\"%s\",\"started_at\":1790240000,\"target\":\"44020\"},"
                 "\"home_operator\":{\"country\":\"阿联酋\",\"country_en\":\"United Arab Emirates\",\"country_iso\":\"AE\","
                 "\"mcc\":\"424\",\"mnc\":\"02\",\"name\":\"%s\",\"operator_en\":\"%s\"}}",
                 zh, zh, en, nm, nm);
        lang_set_en(1);
        parse(lb);
        CHECK("en: long reason_en whole", strlen(n->guard_reason) == 120);
        CHECK("en: target after it still read", !strcmp(n->guard_target, "44020") && !strcmp(n->guard_reason_code, "auto_retry"));
        CHECK("en: long operator, mnc after it", !strcmp(n->home.mnc, "02") && !strcmp(n->home.country, "United Arab Emirates"));
        lang_set_en(0);
        parse(lb);
        CHECK("zh: long Chinese reason whole", !strcmp(n->guard_reason, zh) && !strcmp(n->guard_target, "44020"));
        CHECK("zh: operator fields", !strcmp(n->home.country, "阿联酋") && !strcmp(n->home.country_iso, "AE"));
    }

    puts("L2: exit geo_en / isp_en");
    {
        static const char eb[] =
            "{\"direct\":{\"geo\":\"日本 东京都\",\"geo_en\":\"Japan\",\"ip\":\"1.2.3.4\",\"isp\":\"电信\",\"isp_en\":\"China Telecom\"},"
            "\"proxy\":{\"geo\":\"广东 深圳\",\"ip\":\"5.6.7.8\",\"isp\":\"某某宽带\"}}";
        lang_set_en(1);
        parse(eb);
        CHECK("en: geo_en / isp_en", !strcmp(n->direct.geo, "Japan") && !strcmp(n->direct.isp, "China Telecom"));
        CHECK("en: no twin → the Chinese", !strcmp(n->proxy.geo, "广东 深圳") && !strcmp(n->proxy.isp, "某某宽带"));
        lang_set_en(0);
        parse(eb);
        CHECK("zh: geo / isp", !strcmp(n->direct.geo, "日本 东京都") && !strcmp(n->direct.isp, "电信"));
    }

    printf("passed %d, failed %d\n", pass, fail);
    return fail != 0;
}
