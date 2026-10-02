/*
 * lang.h - screen language (中文 / English), manager docs/designs/ui-english.md.
 *
 * Chinese is the key: TR("网络正常") is the Chinese text in zh mode and the
 * line of ui/lang/en.tsv whose first column is exactly that text in en mode
 * (missing line → the Chinese). The language is chosen once per process
 * (devui.conf lang=, read before the first object); switching execs a fresh
 * copy, so nothing is ever re-translated in place.
 *
 *   TR("中文")            the common case
 *   TRC("场景", "中文")    same Chinese, different English: en.tsv key is 场景|中文
 *   TRN("%d 台设备", n)    en.tsv English is one|other, picked by n == 1
 *   N_("中文")             marks a literal in a static table; TR() it where it is shown
 *   pick(zh, en)          text that came from datad/agent with an *_en sibling
 *
 * Translate only where text is shown: a string that is compared later
 * (esim_state(), story codes) must stay Chinese or, better, become a code.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60_LANG_H
#define U60_LANG_H

/* 0 = zh (default), 1 = en. Set once at startup, before any TR(). */
void lang_set_en(int en);
int  lang_is_en(void);

/* The value after "lang=": exactly "en" (a trailing newline allowed) → 1;
 * anything else, NULL included → 0. Same rule as u60-guard sms_lang. */
int  lang_parse(const char *v);

const char *lang_tr(const char *zh);
const char *lang_trc(const char *key, const char *zh);
const char *lang_trn(const char *zh, long n);

#define TR(s)       lang_tr(s)
#define TRC(c, s)   lang_trc(c "|" s, s)
#define TRN(s, n)   lang_trn(s, (long)(n))
#define N_(s)       (s)

/* Backend text: the *_en sibling in English when it is there and not empty. */
static inline const char *pick(const char *zh, const char *en)
{
    return lang_is_en() && en && en[0] ? en : zh;
}

/* For tests: entries in the compiled table, and entry i (NULL past the end). */
int lang_count(void);
int lang_entry(int i, const char **zh, const char **en, const char **en_other);

#endif /* U60_LANG_H */
