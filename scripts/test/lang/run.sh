#!/bin/sh
# The screen's English table (include/lang.h, ui/lang/en.tsv):
#   1. src/lang_en.inc is what scripts/lang-gen.sh makes from en.tsv now
#   2. every TR/TRC/TRN/N_ literal in src/ has a row (and no row is unused);
#      | in the English only on TRN rows, exactly one (one|other)
#   3. tests/lang_test.c: same printf conversions in both columns, no Chinese
#      in the English, manager DESIGN.md §1 6 wording, lang_parse = guard's
#   4. (info) rows whose English is longer in bytes than the Chinese: the
#      buffers they are printed into were sized for that (10-01 review)
# Host script (docker for 1 and 3). SPDX-License-Identifier: MIT
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
exec docker run --rm -v "$ROOT":/src:ro -w /src rust:1.99.0-slim@sha256:24e632c09342c20abf8312cf4f61430a911c01ed3a5e4c02b87292b1c39c5273 sh -c '
rc=0
sh scripts/lang-gen.sh ui/lang/en.tsv > /tmp/inc || rc=1
if cmp -s /tmp/inc src/lang_en.inc; then echo "  ok   src/lang_en.inc is current"
else echo "  FAIL src/lang_en.inc is stale: make src/lang_en.inc"; rc=1; fi

# keys used in the code (public-build-only lines included), C-escaped as written; adjacent literals joined
perl scripts/test/lang/keys.pl src/*.c src/ui_parts/*.c | sort -u > /tmp/used
awk -F "\t" "!/^#/ && NF == 2 { print \$1 }" ui/lang/en.tsv | sort -u > /tmp/have
miss=$(comm -23 /tmp/used /tmp/have); extra=$(comm -13 /tmp/used /tmp/have)
if [ -z "$miss" ]; then echo "  ok   every TR/TRC/TRN/N_ text has an English row ($(wc -l < /tmp/used))"
else echo "  FAIL no English row for:"; printf "%s\n" "$miss" | sed "s/^/         /"; rc=1; fi
if [ -z "$extra" ]; then echo "  ok   no unused rows"
else echo "  FAIL rows no code uses (or used through a variable without N_):"; printf "%s\n" "$extra" | sed "s/^/         /"; rc=1; fi

# | in the English column only as one|other, and only on TRN rows (ui-english.md 更正 16)
perl scripts/test/lang/keys.pl --trn src/*.c src/ui_parts/*.c | sort -u > /tmp/trn
awk -F "\t" "!/^#/ && NF == 2 && index(\$2, \"|\") { print \$1 }" ui/lang/en.tsv | sort -u > /tmp/piped
awk -F "\t" "!/^#/ && NF == 2 && split(\$2, p, \"|\") > 2 { print \$1 \"  →  \" \$2 }" ui/lang/en.tsv > /tmp/pipe2
notrn=$(comm -23 /tmp/piped /tmp/trn); noother=$(comm -13 /tmp/piped /tmp/trn)
if [ -z "$notrn$noother" ] && [ ! -s /tmp/pipe2 ]; then echo "  ok   | in English only as one|other on the $(wc -l < /tmp/trn) TRN rows"
else
    [ -z "$notrn" ] || { echo "  FAIL | in the English of a row not used through TRN:"; printf "%s\n" "$notrn" | sed "s/^/         /"; }
    [ -z "$noother" ] || { echo "  FAIL TRN row without one|other English:"; printf "%s\n" "$noother" | sed "s/^/         /"; }
    [ ! -s /tmp/pipe2 ] || { echo "  FAIL more than one | in the English:"; sed "s/^/         /" /tmp/pipe2; }
    rc=1
fi

cc -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra \
   -I. -Iinclude tests/lang_test.c src/lang.c -o /tmp/lang_test && /tmp/lang_test || rc=1

echo "  info English longer than the Chinese (bytes), check the buffer:"
awk -F "\t" "!/^#/ && NF == 2 && length(\$2) > length(\$1) { printf \"         %3d > %3d  %s\n\", length(\$2), length(\$1), \$2 }" ui/lang/en.tsv | sort -rn | head -15
exit $rc'
