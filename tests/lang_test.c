/*
 * lang_test.c - the screen's English table (ui/lang/en.tsv via
 * src/lang_en.inc) and TR()/TRC()/TRN() (scripts/test/lang/run.sh).
 *
 * Every row: the English uses the same printf conversions as the Chinese
 * (by position when %1$ is used), both halves of a one|other row too; no
 * Chinese in the English; manager DESIGN.md §1 6 wording (no trailing
 * full stop, no Please, unit case). Plus lang_parse() = u60-guard's rule.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lang.h"

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int s_fail, s_pass;
#define CHECK(c, ...) do { if (c) s_pass++; else { s_fail++; printf("  FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* The conversions of a format, as "1d 2s …" ordered by argument position.
 * Returns -1 for a format that mixes positional and plain conversions. */
static int convs(const char *f, char *out, size_t n)
{
    char slot[16][8] = {{0}};
    int plain = 0, posn = 0, next = 0, max = 0;
    for (const char *p = f; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        int pos = 0;
        const char *q = p;
        while (isdigit((unsigned char)*q)) q++;
        if (*q == '$' && q > p) { pos = atoi(p); p = q + 1; posn = 1; }
        else { pos = ++next; plain = 1; }
        while (*p && strchr("-+ #0'", *p)) p++;
        while (*p && (isdigit((unsigned char)*p) || *p == '.' || *p == '*')) p++;
        char len[3] = {0};
        while (*p && strchr("hlLqjzt", *p) && strlen(len) < 2) len[strlen(len)] = *p++;
        if (!*p || pos < 1 || pos > 15) return -1;
        snprintf(slot[pos], sizeof slot[pos], "%s%c", len, *p == 'i' ? 'd' : *p);
        if (pos > max) max = pos;
    }
    if (plain && posn) return -1;
    out[0] = 0;
    for (int i = 1; i <= max; i++) {
        size_t l = strlen(out);
        snprintf(out + l, n - l, "%d%s ", i, slot[i][0] ? slot[i] : "?");
    }
    return 0;
}

static int has_cjk(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    for (; *p; p++) {
        if (*p < 0x80) continue;
        unsigned cp = 0; int k = 0;
        if ((*p & 0xe0) == 0xc0) { cp = *p & 0x1f; k = 1; }
        else if ((*p & 0xf0) == 0xe0) { cp = *p & 0x0f; k = 2; }
        else if ((*p & 0xf8) == 0xf0) { cp = *p & 0x07; k = 3; }
        for (int i = 0; i < k && p[1]; i++) cp = (cp << 6) | (*++p & 0x3f);
        if ((cp >= 0x3000 && cp <= 0x9fff) || (cp >= 0xff00 && cp <= 0xffef)) return 1;
    }
    return 0;
}

/* Whole word w (case-sensitive) in s. */
static int word(const char *s, const char *w)
{
    size_t n = strlen(w);
    for (const char *p = s; (p = strstr(p, w)); p++)
        if ((p == s || !isalnum((unsigned char)p[-1])) && !isalnum((unsigned char)p[n])) return 1;
    return 0;
}

static void wording(const char *zh, const char *en)
{
    size_t n = strlen(en);
    CHECK(!has_cjk(en), "[%s] English has Chinese: %s", zh, en);
    CHECK(!(n && en[n - 1] == '.' && !(n >= 3 && !strcmp(en + n - 3, "..."))),
          "[%s] ends with a full stop: %s", zh, en);
    CHECK(!strstr(en, "Please") && !strstr(en, "please"), "[%s] says Please: %s", zh, en);
    static const char *const wrong[] = { "mbps", "MBPS", "mhz", "MHZ", "Mhz", "dbm", "DBM", "Dbm", "gb", "Gb" };
    for (size_t i = 0; i < sizeof wrong / sizeof wrong[0]; i++)
        CHECK(!word(en, wrong[i]), "[%s] unit case \"%s\": %s", zh, wrong[i], en);
}

static void row(const char *zh, const char *en, const char *other)
{
    const char *key = strchr(zh, '|') ? strchr(zh, '|') + 1 : zh;   /* TRC: 场景|中文 */
    char a[160], b[160];
    int za = convs(key, a, sizeof a);
    CHECK(za == 0, "[%s] Chinese format mixes %%1$ and plain %%", zh);
    for (int half = 0; half < (other ? 2 : 1); half++) {
        const char *e = half ? other : en;
        CHECK(convs(e, b, sizeof b) == 0, "[%s] English format mixes %%1$ and plain %%: %s", zh, e);
        CHECK(za != 0 || !strcmp(a, b), "[%s] conversions differ: zh \"%s\" en \"%s\" (%s)", zh, a, b, e);
        wording(zh, e);
    }
}

int main(void)
{
    printf("== lang ==\n");
    const char *zh, *en, *other;
    for (int i = 0; lang_entry(i, &zh, &en, &other); i++) row(zh, en, other);
    printf("  %d rows\n", lang_count());

    CHECK(lang_parse("en"), "lang_parse en");
    CHECK(lang_parse("en\n"), "lang_parse en\\n");
    CHECK(!lang_parse(" en"), "lang_parse ' en' must be zh (guard: case \"$_lg\" in en)");
    CHECK(!lang_parse("en\r\n"), "lang_parse en\\r\\n must be zh");
    CHECK(!lang_parse("EN") && !lang_parse("zh") && !lang_parse("") && !lang_parse(NULL), "lang_parse others → zh");

    const char *probe = "这句没有英文 xyzzy";
    lang_set_en(0);
    CHECK(TR(probe) == probe, "zh: TR returns its argument");
    CHECK(TRN("%d xyzzy", 2) != NULL && !strcmp(TRN("%d xyzzy", 2), "%d xyzzy"), "zh: TRN returns its argument");
    lang_set_en(1);
    CHECK(TR(probe) == probe, "en: a missing row falls back to the Chinese");
    if (lang_entry(0, &zh, &en, &other) && !strchr(zh, '|')) {
        CHECK(!strcmp(other ? lang_trn(zh, 1) : TR(zh), en), "en: TR finds row 0");
        if (other) CHECK(!strcmp(lang_trn(zh, 2), other), "en: TRN n=2 → other");
    }
    CHECK(!strcmp(pick("中", "en"), "en") && !strcmp(pick("中", ""), "中") && !strcmp(pick("中", NULL), "中"), "en: pick");
    lang_set_en(0);
    CHECK(!strcmp(pick("中", "en"), "中"), "zh: pick");

    printf("lang: passed %d, failed %d\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
