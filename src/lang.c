/*
 * lang.c - TR()/TRC()/TRN() lookup; see lang.h. The table is
 * src/lang_en.inc, generated from ui/lang/en.tsv by scripts/lang-gen.sh.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lang.h"

#include <stdlib.h>
#include <string.h>

typedef struct { const char *zh, *en, *en_other; } lang_row_t;

static const lang_row_t k_rows[] = {
#include "src/lang_en.inc"
    { NULL, NULL, NULL }
};
#define N_ROWS ((int)(sizeof k_rows / sizeof k_rows[0]) - 1)

static int s_en;
static const lang_row_t *s_sorted[N_ROWS > 0 ? N_ROWS : 1];
static int s_ready;

static int cmp_row(const void *a, const void *b)
{
    return strcmp((*(const lang_row_t *const *)a)->zh, (*(const lang_row_t *const *)b)->zh);
}

/* Sorted at first use by the compiler's own bytes, so the .tsv order and its
 * escapes never matter. */
static void ready(void)
{
    if (s_ready) return;
    for (int i = 0; i < N_ROWS; i++) s_sorted[i] = &k_rows[i];
    qsort(s_sorted, (size_t)N_ROWS, sizeof s_sorted[0], cmp_row);
    s_ready = 1;
}

static const lang_row_t *find(const char *key)
{
    if (!key || N_ROWS == 0) return NULL;
    ready();
    int lo = 0, hi = N_ROWS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(key, s_sorted[mid]->zh);
        if (c == 0) return s_sorted[mid];
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return NULL;
}

void lang_set_en(int en) { s_en = !!en; }
int  lang_is_en(void) { return s_en; }

/* Same rule as u60-guard's sms_lang (last lang= line, value exactly "en"),
 * so the screen and the alert SMS never disagree: no trimming beyond the
 * line's own newline. */
int lang_parse(const char *v)
{
    return v && v[0] == 'e' && v[1] == 'n' && (v[2] == '\0' || (v[2] == '\n' && v[3] == '\0'));
}

const char *lang_tr(const char *zh)
{
    if (!s_en) return zh;
    const lang_row_t *r = find(zh);
    return r ? r->en : zh;
}

const char *lang_trc(const char *key, const char *zh)
{
    if (!s_en) return zh;
    const lang_row_t *r = find(key);
    return r ? r->en : zh;
}

const char *lang_trn(const char *zh, long n)
{
    if (!s_en) return zh;
    const lang_row_t *r = find(zh);
    if (!r) return zh;
    return r->en_other && n != 1 ? r->en_other : r->en;
}

int lang_count(void) { return N_ROWS; }

int lang_entry(int i, const char **zh, const char **en, const char **en_other)
{
    if (i < 0 || i >= N_ROWS) return 0;
    *zh = k_rows[i].zh; *en = k_rows[i].en; *en_other = k_rows[i].en_other;
    return 1;
}
