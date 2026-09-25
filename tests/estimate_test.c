/*
 * estimate.c 测试：跑 tests/fixtures/battery-estimate.json（manager
 * docs/battery-estimate/fixtures.json 的副本，网页测试跑同一份），外加文案、
 * 采样窗口、sysfs 读取和 charge-control 解析。
 *   scripts/test/estimate/run.sh（它也核对 fixtures 的 sha256）
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/estimate.c"

#include <sys/stat.h>
#include <unistd.h>

static int pass, fail;

#define CHECK(desc, cond)                                              \
    do {                                                               \
        if (cond) { pass++; printf("  ok   %s\n", desc); }             \
        else      { fail++; printf("  FAIL %s  [%s]\n", desc, #cond); } \
    } while (0)

static long long jll(const char *obj, const char *key)
{
    char v[32];
    if (!json_get(obj, key, v, sizeof v) || !strcmp(v, "null")) return EST_NONE;
    return strtoll(v, NULL, 10);
}

/* "[[0, 1000000, true], ...]" → samples */
static int parse_samples(const char *arr, est_sample_t *out)
{
    int n = 0;
    const char *p = arr;
    while ((p = strchr(p + 1, '[')) && n < EST_MAX_SAMPLES) {
        double t; long ua; char b[8];
        if (sscanf(p, "[%lf, %ld, %7[a-z]]", &t, &ua, b) == 3) {
            out[n].t = t; out[n].ua = ua; out[n].online = !strcmp(b, "true");
            n++;
        }
    }
    return n;
}

static const char *kind_name(int k)
{
    static const char *names[] = { "unknown", "charging_eta", "discharging_eta", "reached_target", "paused_at_limit" };
    return names[k];
}

static void run_fixtures(void)
{
    static char file[65536], cases[65536], obj[4096], input[4096], samples[2048], expect[256], name[128], v[32];
    FILE *fp = fopen("tests/fixtures/battery-estimate.json", "r");
    size_t len;
    const char *p;
    int count = 0;

    if (!fp) { fail++; printf("  FAIL fixtures missing\n"); return; }
    len = fread(file, 1, sizeof file - 1, fp);
    fclose(fp);
    file[len] = 0;
    if (!json_get(file, "cases", cases, sizeof cases)) { fail++; printf("  FAIL no cases\n"); return; }

    for (p = strchr(cases, '{'); p; ) {
        const char *q = p;
        int depth = 0;
        est_sample_t s[EST_MAX_SAMPLES];
        est_input_t in;
        est_t r;
        char desc[200];
        long long want_min;

        do {
            if (*q == '{') depth++;
            else if (*q == '}') depth--;
            q++;
        } while (depth && *q);
        snprintf(obj, sizeof obj, "%.*s", (int)(q - p), p);
        json_get(obj, "name", name, sizeof name);
        json_get(obj, "input", input, sizeof input);
        json_get(obj, "expect", expect, sizeof expect);
        json_get(input, "samples", samples, sizeof samples);
        in.s = s;
        in.n = parse_samples(samples, s);
        in.soc = (int)json_get_int(input, "soc", 0);
        in.full_uah = jll(input, "charge_full_uah");
        in.counter_uah = jll(input, "charge_counter_uah");
        in.target_pct = (int)json_get_int(input, "target_pct", 100);
        json_get(input, "paused_at_limit", v, sizeof v);
        in.paused_at_limit = !strcmp(v, "true");
        r = est_compute(&in);
        json_get(expect, "kind", v, sizeof v);
        want_min = jll(expect, "minutes");
        snprintf(desc, sizeof desc, "fixture: %s", name);
        CHECK(desc, !strcmp(v, kind_name(r.kind)) &&
                    (want_min == EST_NONE ? (r.kind != EST_CHARGING && r.kind != EST_DISCHARGING)
                                          : r.minutes == want_min));
        if (strcmp(v, kind_name(r.kind))) printf("       want %s got %s %ld\n", v, kind_name(r.kind), r.minutes);
        count++;
        p = strchr(q, '{');
    }
    CHECK("fixtures: at least 15 cases", count >= 15);
}

int main(void)
{
    char buf[96];
    est_sample_t w[EST_MAX_SAMPLES];
    int n = 0, target, paused;
    long long full, counter;

    run_fixtures();

    est_text((est_t){ EST_CHARGING, 198 }, 80, buf, sizeof buf);
    CHECK("text: to limit", !strcmp(buf, "约 3 小时 18 分充到 80%"));
    est_text((est_t){ EST_CHARGING, 45 }, 100, buf, sizeof buf);
    CHECK("text: minutes to full", !strcmp(buf, "约 45 分钟充满"));
    est_text((est_t){ EST_DISCHARGING, 120 }, 100, buf, sizeof buf);
    CHECK("text: whole hours", !strcmp(buf, "约可用 2 小时"));
    est_text((est_t){ EST_PAUSED, 0 }, 80, buf, sizeof buf);
    CHECK("text: paused", !strcmp(buf, "已到上限，暂停充电"));
    est_text((est_t){ EST_UNKNOWN, 0 }, 100, buf, sizeof buf);
    CHECK("text: unknown", !strcmp(buf, "—"));

    n = est_push(w, n, (est_sample_t){ 0, 1, 1 });
    n = est_push(w, n, (est_sample_t){ 100, 1, 1 });
    n = est_push(w, n, (est_sample_t){ 300, 1, 1 });
    CHECK("push: drops samples outside the window", n == 1 && w[0].t == 300);
    for (int i = 0; i < 100; i++) n = est_push(w, n, (est_sample_t){ 300 + i, 1, 1 });
    CHECK("push: caps the buffer", n == EST_MAX_SAMPLES && w[n - 1].t == 399);

    mkdir("/tmp/est-root", 0755);
    mkdir("/tmp/est-root/battery", 0755);
    { FILE *f = fopen("/tmp/est-root/battery/charge_full", "w"); fputs("11011000\n", f); fclose(f); }
    { FILE *f = fopen("/tmp/est-root/battery/charge_counter", "w"); fputs("n/a\n", f); fclose(f); }
    est_read_capacity("/tmp/est-root", &full, &counter);
    CHECK("sysfs: charge_full read", full == 11011000LL);
    CHECK("sysfs: garbage is missing, not 0", counter == EST_NONE);
    est_read_capacity("/tmp/est-none", &full, &counter);
    CHECK("sysfs: no directory is missing", full == EST_NONE && counter == EST_NONE);

    est_parse_charge_control("{\"charging_stopped\":true,\"charge_limit_enabled\":true,\"charge_limit\":80,\"manual_override\":false}", 1, &target, &paused);
    CHECK("cc: limit stop is paused", target == 80 && paused);
    est_parse_charge_control("{\"charging_stopped\":true,\"charge_limit_enabled\":true,\"charge_limit\":80,\"manual_override\":true}", 1, &target, &paused);
    CHECK("cc: manual stop is not paused", !paused);
    est_parse_charge_control("{\"charging_stopped\":true,\"charge_limit_enabled\":true,\"charge_limit\":80,\"manual_override\":false}", 0, &target, &paused);
    CHECK("cc: unplugged is not paused", !paused);
    est_parse_charge_control("{\"charging_stopped\":false,\"charge_limit_enabled\":false,\"charge_limit\":80}", 1, &target, &paused);
    CHECK("cc: limit off targets 100", target == 100 && !paused);
    est_parse_charge_control(NULL, 1, &target, &paused);
    CHECK("cc: no reply targets 100", target == 100 && !paused);

    printf("%d passed, %d failed\n", pass, fail);
    return fail != 0;
}
