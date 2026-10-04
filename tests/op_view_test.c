/*
 * op_view.c tests: datad's /v2/screen "op" (E4, data-service STATE_V2.md
 * §12) in the shape ops/ui.rs sends it, Chinese and English.
 * Host build with ASan/UBSan: scripts/test/op_view/run.sh
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/op_view.c"

#include <stdio.h>
#include <string.h>

static int s_fail, s_pass_n;
#define CHECK(c) do { if (c) s_pass_n++; else { s_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

/* active: verifying, rollback on; last: rolled back on timeout, not acked */
static const char *k_op =
    "{\"rollback_enabled\":true,"
    "\"active\":{\"action\":\"network.set_mode\",\"can_keep\":true,\"can_revert\":true,\"data_ok\":false,"
    "\"ever_matched\":true,\"item\":\"network.mode\",\"keep_label_en\":\"Keep 4G only\",\"keep_label_zh\":\"保留只用 4G\","
    "\"mark\":null,\"next_en\":\"Back to Auto in {t} if no data\",\"next_zh\":\"{t} 后没通就退回到自动\","
    "\"note_en\":null,\"note_zh\":null,\"old\":\"WL_AND_5G\",\"old_en\":\"Auto\",\"old_zh\":\"自动\",\"op_id\":\"screen-12-3\","
    "\"phase\":\"verifying\",\"readback_en\":\"4G only\",\"readback_zh\":\"只用 4G\",\"reason\":null,\"remaining_ms\":102000,"
    "\"revert_label_en\":\"Revert to Auto\",\"revert_label_zh\":\"退回自动\",\"rollback_enabled\":true,"
    "\"rollback_reason\":null,\"rollback_to\":\"WL_AND_5G\",\"rollback_to_en\":\"Auto\",\"rollback_to_zh\":\"自动\","
    "\"say_en\":\"Checking\",\"say_zh\":\"正在确认\",\"source\":\"web\",\"source_en\":\"Web\",\"source_zh\":\"网页\","
    "\"stay\":\"live\",\"steps\":[{\"done\":true,\"en\":\"Setting applied\",\"key\":\"applied\",\"zh\":\"设置已生效\"},"
    "{\"done\":true,\"en\":\"Registered\",\"key\":\"registered\",\"zh\":\"已注册\"},"
    "{\"done\":false,\"en\":\"Data\",\"key\":\"data\",\"zh\":\"数据\"}],\"target\":\"Only_LTE\",\"target_en\":\"4G only\","
    "\"target_zh\":\"只用 4G\",\"undo\":null,\"what_en\":\"Network mode\",\"what_zh\":\"制式\"},"
    "\"last\":{\"acked\":false,\"action\":\"network.set_mode\",\"can_keep\":false,\"can_revert\":false,\"item\":\"network.mode\","
    "\"mark\":\"warn\",\"needs_ack\":true,\"next_en\":null,\"next_zh\":null,\"note_en\":null,\"note_zh\":null,"
    "\"op_id\":\"web-7\",\"phase\":\"rolled_back\",\"reason\":\"timeout\",\"remaining_ms\":null,"
    "\"say_en\":\"No data · back to Auto\",\"say_zh\":\"没通 · 已退回自动\",\"stay\":\"sticky\","
    "\"steps\":[{\"done\":true,\"en\":\"Setting applied\",\"key\":\"applied\",\"zh\":\"设置已生效\"},"
    "{\"done\":true,\"en\":\"Registered\",\"key\":\"registered\",\"zh\":\"已注册\"},"
    "{\"done\":true,\"en\":\"Data\",\"key\":\"data\",\"zh\":\"数据\"}],"
    "\"undo\":{\"label_en\":\"Undo\",\"label_zh\":\"撤销\",\"ok\":false,\"value\":\"WL_AND_5G\","
    "\"why_en\":\"Nothing to undo\",\"why_zh\":\"设置没变 · 不用撤销\"},\"rollback_to\":\"WL_AND_5G\"}}";

int main(void)
{
    op_view_t v;
    char b[128];

    CHECK(!op_view_parse("", &v) && !v.active.have && !v.last.have);
    CHECK(!op_view_parse(NULL, &v));
    CHECK(op_view_parse("{\"rollback_enabled\":false,\"active\":null,\"last\":null}", &v) &&
          !v.rollback_enabled && !v.active.have && !v.last.have && !v.notice[0]);
    /* DD18: the one-time notice; null or absent = none */
    CHECK(op_view_parse("{\"rollback_enabled\":true,\"active\":null,\"last\":null,\"notice\":\"rollback_on\"}", &v) &&
          !strcmp(v.notice, "rollback_on"));
    CHECK(op_view_parse("{\"rollback_enabled\":true,\"active\":null,\"last\":null,\"notice\":null}", &v) && !v.notice[0]);

    CHECK(op_view_parse(k_op, &v) && v.rollback_enabled);
    op_item_t *a = &v.active, *l = &v.last;
    CHECK(a->have && !strcmp(a->op_id, "screen-12-3") && !strcmp(a->phase, "verifying") && !op_phase_final(a->phase));
    CHECK(!strcmp(a->say, "正在确认") && !strcmp(a->next, "{t} 后没通就退回到自动") && !a->note[0]);
    CHECK(!strcmp(a->source, "网页") && !strcmp(a->what, "制式") && !strcmp(a->old_v, "自动") && !strcmp(a->target, "只用 4G"));
    CHECK(!strcmp(a->revert_label, "退回自动") && !strcmp(a->keep_label, "保留只用 4G") && a->can_revert && a->can_keep);
    CHECK(a->remaining_ms == 102000 && a->mark == OP_MARK_NONE && a->stay == OP_STAY_LIVE && a->undo_ok == -1);
    CHECK(!strcmp(a->step[0], "设置已生效") && a->step_done[0] && a->step_done[1] && !a->step_done[2]);
    CHECK(!strcmp(a->rollback_to_raw, "WL_AND_5G"));
    CHECK(l->have && op_phase_final(l->phase) && l->mark == OP_MARK_WARN && l->stay == OP_STAY_STICKY);
    CHECK(!strcmp(l->say, "没通 · 已退回自动") && l->needs_ack && !l->acked && l->remaining_ms == -1);
    CHECK(l->undo_ok == 0 && !strcmp(l->undo_why, "设置没变 · 不用撤销") && !strcmp(l->undo_label, "撤销"));

    op_fill_clock(b, sizeof b, a->next, a->remaining_ms);
    CHECK(!strcmp(b, "1:42 后没通就退回到自动"));
    op_fill_clock(b, sizeof b, "还剩 {t} · 自动退回没开", 48001);
    CHECK(!strcmp(b, "还剩 0:49 · 自动退回没开"));
    op_fill_clock(b, sizeof b, "{t} left", -5);
    CHECK(!strcmp(b, "0:00 left"));
    op_fill_clock(b, sizeof b, "no clock", 1000);
    CHECK(!strcmp(b, "no clock"));

    /* 改动记录: journal.list as datad decorates it (journal_view.rs) */
    {
        static op_log_t lg[OP_LOG_MAX];
        const char *rep =
            "{\"action\":\"journal.list\",\"ok\":true,\"result\":{\"entries\":["
            "{\"source\":\"screen\",\"action\":\"op.ack\",\"result\":\"ok\",\"hide\":true,\"what_zh\":\"op.ack\"},"
            "{\"op_id\":\"b\",\"action\":\"network.set_mode\",\"item\":\"network.mode\",\"source\":\"web\",\"undo\":false,"
            "\"t\":\"2026-10-03 14:32:07\",\"what_zh\":\"制式\",\"what_en\":\"Network mode\","
            "\"change_zh\":\"自动 → 只用 4G\",\"change_en\":\"Auto → 4G only\",\"result_zh\":\"已切到只用 4G\","
            "\"result_en\":\"Now 4G only\",\"mark\":\"ok\",\"source_zh\":\"网页\",\"source_en\":\"Web\",\"hide\":false,"
            "\"undo_view\":{\"ok\":true,\"label_zh\":\"撤销\",\"label_en\":\"Undo\",\"why_zh\":null,\"why_en\":null,"
            "\"request\":{\"action\":\"network.set_mode\",\"undo\":true,\"params\":{\"mode\":\"WL_AND_5G\"}}}},"
            "{\"action\":\"cellular.set\",\"source\":\"screen\",\"params\":{\"enabled\":0},\"result\":\"failed\","
            "\"t\":\"2026-10-03 09:01:00\",\"what_zh\":\"移动数据\",\"change_zh\":\"关掉数据\",\"result_zh\":\"没改成\","
            "\"mark\":\"bad\",\"source_zh\":\"触屏\",\"hide\":false,\"undo_view\":null}"
            "],\"owners\":{}}}";
        int n = op_log_parse(rep, lg, OP_LOG_MAX);
        CHECK(n == 2);
        CHECK(!strcmp(lg[0].what, "制式") && !strcmp(lg[0].change, "自动 → 只用 4G") && !strcmp(lg[0].result, "已切到只用 4G"));
        CHECK(!strcmp(lg[0].when, "10-03 14:32") && !strcmp(lg[0].source, "网页") && lg[0].mark == OP_MARK_OK);
        CHECK(lg[0].undo_have && lg[0].undo_ok && !strcmp(lg[0].undo_label, "撤销"));
        CHECK(!strcmp(lg[0].undo_action, "network.set_mode") && !strcmp(lg[0].undo_params, "{\"mode\":\"WL_AND_5G\"}"));
        CHECK(!strcmp(lg[1].what, "移动数据") && lg[1].mark == OP_MARK_BAD && !lg[1].undo_have && !strcmp(lg[1].when, "10-03 09:01"));
        CHECK(op_log_parse("{\"ok\":false}", lg, OP_LOG_MAX) == -1);
        CHECK(op_log_parse("{\"result\":{\"entries\":[]}}", lg, OP_LOG_MAX) == 0);
        CHECK(op_log_parse(rep, lg, 1) == 1);
        CHECK(!strcmp(lg[0].op_id, "b") && lg[1].op_id[0] == 0);

        /* 「上次改动」: owners carry the raw source, the name is made here */
        op_owner_t ow;
        const char *own =
            "{\"ok\":true,\"result\":{\"entries\":[],\"owners\":{\"network.mode\":{\"source\":\"scenario\","
            "\"user\":false,\"undo\":false,\"value\":\"Only_LTE\",\"op_id\":\"sc-7\",\"ts\":1,"
            "\"t\":\"2026-10-04 09:15:02\"},\"wifi\":{\"source\":\"mystery\",\"op_id\":null,"
            "\"t\":\"2026-10-04 08:00:00\"}}}}";
        CHECK(op_owner_parse(own, "network.mode", &ow) == 1 && ow.have);
        CHECK(!strcmp(ow.when, "10-04 09:15") && !strcmp(ow.source, "情景") && !strcmp(ow.op_id, "sc-7"));
        CHECK(op_owner_parse(own, "wifi", &ow) == 1 && !strcmp(ow.source, "mystery") && ow.op_id[0] == 0);
        CHECK(op_owner_parse(own, "band.lock", &ow) == 0 && !ow.have);
        CHECK(op_owner_parse(rep, "network.mode", &ow) == 0 && !ow.have);
        CHECK(op_owner_parse("{\"ok\":false}", "network.mode", &ow) == -1);
        lang_set_en(1);
        CHECK(op_owner_parse(own, "network.mode", &ow) == 1 && !strcmp(ow.source, "Scene"));
        lang_set_en(0);
    }

    lang_set_en(1);
    CHECK(op_view_parse(k_op, &v));
    CHECK(!strcmp(a->say, "Checking") && !strcmp(a->revert_label, "Revert to Auto") && !strcmp(a->step[2], "Data"));
    CHECK(!strcmp(l->say, "No data · back to Auto") && !strcmp(l->undo_why, "Nothing to undo"));
    CHECK(!strcmp(a->source, "Web") && !strcmp(a->old_v, "Auto"));

    printf("passed %d, failed %d\n", s_pass_n, s_fail);
    return s_fail != 0;
}
