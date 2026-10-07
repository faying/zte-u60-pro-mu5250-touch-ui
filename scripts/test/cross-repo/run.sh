#!/bin/sh
# ─────────────────────────────────────────────────────────────────────────────
# Lists that must agree with files outside scripts/ (docs/SHIP.md「第二期」):
#   1. u60-ship.sh's GUARD_FILES = the install kit's guard list
#      (manager onboard/build-kit.sh) minus u60-ship.sh, datad-trial.sh,
#      u60-recover.sh, exactly.
#   2. the touch trial starts the test build the way u60-uid starts the UI
#      (src/uid.c: chdir(DEVUI_DIR), execl(DEVUI_BIN)).
#   3. every ship/<comp>/state-files (three repos) is within u60-ship.sh's
#      C_STATE_ALLOW for it, or the device refuses to stage (touch, 10-01).
# English words that must agree (manager docs/designs/ui-english.md 更正 15, DT6/ET5):
#   4. manager DESIGN.md §4「首页结论表」= datad's headlines (screen/tests.rs
#      HEADLINES, pinned to screen.rs by datad's own test; each 中文/English
#      pair also as written in screen.rs); glossary §4 gives the same English;
#      the two rows the screen writes itself = en.tsv.
#   5. page names: glossary §1–2 = ui/lang/en.tsv (+ src/lang_en.inc) for the
#      screen, = web src/lib/i18n/en.ts nav for the web menu column.
#   6. operator English: glossary §8 = zte-agent operator_en(); datad's 4
#      mainland carriers are in both, word for word.
#   7. every <key>_en the screen reads is in what datad (net_view.c) or
#      zte-agent (the rest) sends; test modules don't count.
# Run it on the host from the touch-ui checkout (sh scripts/test/cross-repo/
# run.sh); in a container that only has /scripts each part says it skipped.
# SPDX-License-Identifier: MIT
# ─────────────────────────────────────────────────────────────────────────────

export LC_ALL=C   # macOS awk/sort in a UTF-8 locale compare Chinese wrongly (all equal)
SCRIPTS=${SCRIPTS:-$(cd "$(dirname "$0")/../.." && pwd)}
REPO=$(cd "$SCRIPTS/.." && pwd)
KIT=${BUILD_KIT:-$REPO/../manager/onboard/build-kit.sh}
MANAGER=${MANAGER:-$REPO/../manager}
DATAD=${DATAD:-$REPO/../data-service}
PASS=0
FAIL=0
SKIP=0
ok() { PASS=$((PASS + 1)); echo "  ok   $1"; }
bad() { FAIL=$((FAIL + 1)); echo "  FAIL $1"; }
skip() { SKIP=$((SKIP + 1)); echo "  skip $1"; }
# same <what> <want> <have>: two line lists, order ignored
same() {
    if [ "$(printf '%s\n' "$2" | sort -u)" = "$(printf '%s\n' "$3" | sort -u)" ]; then
        ok "$1"
    else
        bad "$1: differ (< want, > have)"
        printf '%s\n' "$2" | sort -u >/tmp/cross-repo.want.$$
        printf '%s\n' "$3" | sort -u >/tmp/cross-repo.have.$$
        diff /tmp/cross-repo.want.$$ /tmp/cross-repo.have.$$ | grep '^[<>]' | sed 's/^/       /'
        rm -f /tmp/cross-repo.want.$$ /tmp/cross-repo.have.$$
    fi
}

echo "== cross-repo =="

GF=$(sed -n 's/^GUARD_FILES="\(.*\)"$/\1/p' "$SCRIPTS/u60-ship.sh")
[ -n "$GF" ] && ok "GUARD_FILES found in u60-ship.sh" || bad "no GUARD_FILES line in u60-ship.sh"
for f in $GF; do
    [ -f "$SCRIPTS/$f" ] || bad "GUARD_FILES names $f, not in scripts/"
done

if [ -f "$KIT" ]; then
    # the guard loop: "for f in alert-lib.sh … \ … ; do"
    KF=$(awk '/^for f in alert-lib\.sh/ { on = 1 } on { print; if ($0 !~ /\\$/) exit }' "$KIT" |
        sed 's/^for f in//; s/; *do.*$//; s/\\$//' | tr -s ' \t' '\n\n' | sed '/^$/d')
    [ -n "$KF" ] && ok "guard list found in build-kit.sh" || bad "no guard list in $KIT"
    want=$(printf '%s\n' $KF | grep -v -x -e u60-ship.sh -e datad-trial.sh -e u60-recover.sh | sort)
    have=$(printf '%s\n' $GF | sort)
    what="GUARD_FILES = build-kit guard list − u60-ship.sh, datad-trial.sh, u60-recover.sh"
    if [ "$want" = "$have" ]; then
        ok "$what"
    else
        bad "GUARD_FILES and the build-kit guard list differ:"
        printf '%s\n' "$want" >/tmp/cross-repo.want.$$
        printf '%s\n' "$have" >/tmp/cross-repo.have.$$
        diff /tmp/cross-repo.want.$$ /tmp/cross-repo.have.$$ | sed 's/^/       /'
        rm -f /tmp/cross-repo.want.$$ /tmp/cross-repo.have.$$
    fi
    for f in u60-ship.sh datad-trial.sh u60-recover.sh; do
        printf '%s\n' $KF | grep -qx "$f" && ok "build-kit still ships $f" || bad "build-kit no longer ships $f"
    done
else
    skip "GUARD_FILES vs build-kit: $KIT not here (container run; run on the host)"
fi

UIDC=$REPO/src/uid.c
if [ -f "$UIDC" ]; then
    D=$(sed -n 's/^#define DEVUI_DIR *"\(.*\)"/\1/p' "$UIDC")
    B=$(sed -n 's/^#define DEVUI_BIN *DEVUI_DIR "\(.*\)"/\1/p' "$UIDC")
    GOT=$(env -u U60S_ROOT sh "$SCRIPTS/u60-ship.sh" print-launch touch)
    WANT="nohup sh -c cd \"\$1\" && exec \"\$2\" sh $D $D$B.test"
    if [ -n "$D" ] && [ "$GOT" = "$WANT" ] && grep -q 'chdir(DEVUI_DIR)' "$UIDC" && grep -q 'execl(DEVUI_BIN, DEVUI_BIN' "$UIDC"; then
        ok "touch trial = u60-uid's launch (chdir $D, exec $D$B.test)"
    else
        bad "touch trial launch: $GOT (uid.c: dir $D, bin $B)"
    fi
    grep -q 'find_comm("u60pro-devui")' "$UIDC" && ok "u60-uid adopts by comm prefix (why it is stopped for the touch trial)" ||
        bad "u60-uid no longer adopts by comm prefix: recheck the touch trial"
else
    skip "touch launch vs src/uid.c: $UIDC not here"
fi

for comp in datad:data-service agent:manager web:manager touch:touch-ui uid:touch-ui guard:touch-ui; do
    c=${comp%%:*}
    SF=$REPO/../${comp#*:}/ship/$c/state-files
    [ "${comp#*:}" = touch-ui ] && SF=$REPO/ship/$c/state-files
    if [ ! -f "$SF" ]; then
        skip "$c state-files: $SF not here"
        continue
    fi
    allow=$(env -u U60S_ROOT sh "$SCRIPTS/u60-ship.sh" print-state "$c")
    miss=$(sed 's/#.*//' "$SF" | awk NF | while read -r f; do printf '%s\n' "$allow" | grep -qx "$f" || echo "$f"; done)
    [ -z "$miss" ] && ok "$c state-files within u60-ship.sh's list" || bad "$c state-files not in u60-ship.sh's list: $(echo $miss)"
done

# ---- English words (4–7) ----------------------------------------------------
DESIGN=$MANAGER/docs/DESIGN.md
GLOSS=$MANAGER/docs/ui-glossary.md
TSV=$REPO/ui/lang/en.tsv
INC=$REPO/src/lang_en.inc
SCREEN_RS=$DATAD/rust/src/project/screen.rs
SCREEN_T=$DATAD/rust/src/project/screen/tests.rs
OPS_UI=$DATAD/rust/src/ops/ui.rs
AGENT=$MANAGER/zte-agent/src
WEB_EN=$MANAGER/web/src/lib/i18n/en.ts

# en.tsv's English for a plain (not TRC) key, empty when there is no row
tsv_en() { [ -f "$TSV" ] && awk -F '\t' -v k="$1" '!/^#/ && NF == 2 && $1 == k { print $2; exit }' "$TSV"; }
# rows of the first markdown table after the line starting with $2 in $1, cells
# trimmed and tab-separated; header and --- rows dropped, <!-- --> lines skipped
md_rows() {
    awk -v h="$2" 'index($0, h) == 1 { on = 1; next }
        !on || /^<!--/ { next }
        !/^\|/ { if (rows) exit; next }
        rows++ == 0 || /^\|[-| ]+\|$/ { next }
        { n = split($0, c, "|"); o = ""
          for (i = 2; i < n; i++) { gsub(/^ +| +$/, "", c[i]); o = o (i > 2 ? "\t" : "") c[i] }
          print o }' "$1"
}

# 4. home headline table
if [ -f "$DESIGN" ] && [ -f "$SCREEN_T" ] && [ -f "$TSV" ]; then
    # code<TAB>中文<TAB>English<TAB>tone, one line per code ("a / b" cells pair
    # up by position; "慢：限速 / 信号弱" carries 慢： on; one English = all)
    DT=$(md_rows "$DESIGN" '**首页结论表**' | awk -F '\t' '
        $2 ~ /^（/ { next }
        { nc = split($2, c, " / "); nz = split($3, z, " / "); ne = split($4, e, " / ")
          t = $5 == "中性" ? "neutral" : $5
          p = ""; if (match(z[1], /^.*：/)) p = substr(z[1], 1, RLENGTH)
          for (i = 1; i <= nc; i++) {
              zz = nz == nc ? z[i] : z[1]; if (i > 1 && p != "" && index(zz, p) != 1) zz = p zz
              print c[i] "\t" zz "\t" (ne == nc ? e[i] : e[1]) "\t" t } }')
    # changing / revert_fail are laid over the story by a write (E4, screen.rs
    # with_op()); their words come from ops/ui.rs, not from HEADLINES
    OV='^(changing|revert_fail)	'
    DO=$(printf '%s\n' "$DT" | grep -E "$OV")
    DT=$(printf '%s\n' "$DT" | grep -vE "$OV")
    miss=$(md_rows "$DESIGN" '**首页结论表**' | awk -F '\t' '$2 ~ /^(changing|revert_fail)$/ {
            nz = split($3, z, " / "); ne = split($4, e, " / "); for (i = 1; i <= nz; i++) print z[i] "\t" (ne == nz ? e[i] : e[1]) }' |
        while IFS='	' read -r z e; do
            { grep -qF "\"$z\"" "$OPS_UI" || grep -qF "\"$z\"" "$SCREEN_RS"; } && grep -qF "\"$e\"" "$OPS_UI" || echo "($z, $e)"; done)
    [ -n "$DO" ] && [ -z "$miss" ] && ok "write overlays (changing, revert_fail) are written as such in ops/ui.rs" ||
        bad "write overlays not in ops/ui.rs: ${miss:-no changing/revert_fail rows in DESIGN.md §4}"
    HT=$(sed -n 's/^ *("\([a-z0-9]*\)", "\([^"]*\)", "\([^"]*\)", Tone::\([A-Za-z]*\)),$/\1	\2	\3	\4/p' "$SCREEN_T" |
        awk -F '\t' '{ print $1 "\t" $2 "\t" $3 "\t" tolower($4) }')
    [ "$(printf '%s\n' "$DT" | awk NF | wc -l)" -ge 13 ] && ok "DESIGN.md §4 headline table read ($(printf '%s\n' "$DT" | wc -l | tr -d ' ') codes)" ||
        bad "DESIGN.md §4 headline table: only $(printf '%s\n' "$DT" | awk NF | wc -l | tr -d ' ') codes read"
    same "DESIGN.md §4 headlines (code, 中文, English, tone) = datad screen/tests.rs HEADLINES" "$DT" "$HT"
    miss=$(printf '%s\n' "$HT" | awk -F '\t' 'NF { print $2 "\t" $3 }' | sort -u | while IFS='	' read -r z e; do
        grep -qF "(\"$z\", \"$e\")" "$SCREEN_RS" || echo "($z, $e)"; done)
    [ -z "$miss" ] && ok "each headline (中文, English) is written as such in screen.rs" || bad "headlines not in screen.rs: $(echo $miss)"
    GT=$(md_rows "$GLOSS" '## 4.' | awk -F '\t' '$1 !~ /^（/ {
        nc = split($1, c, " / "); ne = split($3, e, " / ")
        for (i = 1; i <= nc; i++) print c[i] "\t" (ne == nc ? e[i] : e[1]) }')
    same "glossary §4 state words = DESIGN.md §4" "$(printf '%s\n' "$DT" | cut -f 1,3)" "$GT"
    # the table leaves off the … the screen writes (读取中… = Loading…)
    miss=$(md_rows "$DESIGN" '**首页结论表**' | awk -F '\t' '$2 ~ /^（/ { print $3 "\t" $4 }' | while IFS='	' read -r z e; do
        [ "$(tsv_en "$z")" = "$e" ] || [ "$(tsv_en "$z…")" = "$e…" ] || echo "$z → $e (en.tsv: $(tsv_en "$z")$(tsv_en "$z…"))"; done)
    [ -z "$miss" ] && ok "the screen's own headlines (读取中, 读不到数据) = en.tsv" || bad "screen's own headlines: $miss"
else
    skip "home headline table: $DESIGN or $SCREEN_T not here"
fi

# 4b. datad codes this screen doesn't know yet go through net_view.c's
#     unknown-code path (datad's own headline, hint and tone), which
#     tests/net_view_test.c must cover with that code (E2 D7②)
if [ -n "${HT:-}" ] && [ -f "$REPO/src/net_view.c" ]; then
    NVC=$(sed -n '/^static nv_state_t state/,/^}/p' "$REPO/src/net_view.c" | grep -o '"[a-z0-9]*"' | tr -d '"')
    new=$(printf '%s\n' "$HT" | cut -f 1 | while read -r c; do [ -n "$c" ] && ! printf '%s\n' "$NVC" | grep -qx "$c" && echo "$c"; done)
    miss=$(for c in $new; do grep -qF "\\\"state\\\":\\\"$c\\\"" "$REPO/tests/net_view_test.c" || echo "$c"; done)
    [ -z "$miss" ] && ok "datad codes the screen doesn't know yet ($(echo ${new:-none})) are covered by net_view_test" ||
        bad "datad codes unknown to net_view.c with no net_view_test case: $(echo $miss)"
fi

# 5. page names
if [ -f "$GLOSS" ] && [ -f "$TSV" ]; then
    PG=$( { md_rows "$GLOSS" '## 1.' | cut -f 1,2; md_rows "$GLOSS" '## 2.' | cut -f 1,2; } |
        sed 's/（.*）$//')
    [ "$(printf '%s\n' "$PG" | awk NF | wc -l)" -ge 15 ] && ok "glossary §1–2 read ($(printf '%s\n' "$PG" | wc -l | tr -d ' ') names)" || bad "glossary §1–2: too few names read"
    bad_pg=$(printf '%s\n' "$PG" | awk NF | while IFS='	' read -r z e; do
        if printf '%s' "$z" | LC_ALL=C grep -q '^[ -~]*$'; then
            [ "$z" = "$e" ] || echo "$z: English $e, but an ASCII name stays as it is"
            continue
        fi
        have=$(tsv_en "$z")
        if [ -n "$have" ]; then
            [ "$have" = "$e" ] || echo "$z: glossary $e, en.tsv $have"
            grep -qF "{ \"$z\", \"$e\", NULL }," "$INC" || echo "$z: not \"$e\" in src/lang_en.inc (make src/lang_en.inc)"
        else
            # no row of its own (管理网页 = web admin, only inside sentences)
            awk -F '\t' -v e="$e" '!/^#/ && NF == 2 && index($2, e) { f = 1 } END { exit !f }' "$TSV" ||
                echo "$z: no en.tsv row, and $e in no English"
        fi
    done)
    [ -z "$bad_pg" ] && ok "page names: glossary = ui/lang/en.tsv = src/lang_en.inc" || { bad "page names (screen):"; printf '%s\n' "$bad_pg" | sed 's/^/       /'; }
    if [ -f "$WEB_EN" ]; then
        NAV=$(awk '/^  nav: \{/ { on = 1; next } on && /^  \},?$/ { exit } on' "$WEB_EN" | sed -n 's/^ *[A-Za-z0-9]*: "\(.*\)",$/\1/p')
        bad_web=$(md_rows "$GLOSS" '## 2.' | cut -f 3 | grep -v '^—$' | sed 's/（.*）$//' | perl -pe 's/、/\n/g' | sed '/^$/d' | sort -u |
            while read -r w; do printf '%s\n' "$NAV" | grep -qxF "$w" || echo "$w"; done)
        [ -z "$bad_web" ] && ok "page names: glossary web column ⊂ web en.ts nav" || bad "glossary web menu names not in web en.ts nav: $(echo $bad_web)"
    else
        skip "web page names: $WEB_EN not here"
    fi
else
    skip "page names: $GLOSS not here"
fi

# 6. operator English
if [ -f "$GLOSS" ] && [ -d "$AGENT" ]; then
    GO=$(md_rows "$GLOSS" '## 8.' | cut -f 2,3)
    AO=$(awk '/^fn operator_en\(/ { on = 1; next } on && /^}/ { exit } on' "$AGENT/netinfo.rs" |
        sed -n 's/^ *"\([^"]*\)" => "\([^"]*\)",$/\1	\2/p')
    [ -n "$AO" ] && ok "zte-agent operator_en() read ($(printf '%s\n' "$AO" | wc -l | tr -d ' ') names)" || bad "no operator_en() table in $AGENT/netinfo.rs"
    same "operator English: glossary §8 = zte-agent operator_en()" "$GO" "$AO"
    if [ -f "$SCREEN_RS" ]; then
        DO=$(sed -n 's/^ *const C[A-Z]: (&str, &str) = ("\([^"]*\)", "\([^"]*\)");$/\1	\2/p' "$SCREEN_RS")
        [ "$(printf '%s\n' "$DO" | awk NF | wc -l)" -eq 4 ] && ok "datad's 4 mainland carriers read" || bad "datad mainland_operator(): $(printf '%s\n' "$DO" | awk NF | wc -l | tr -d ' ') names read, want 4"
        miss=$(printf '%s\n' "$DO" | awk NF | while read -r l; do printf '%s\n' "$AO" | grep -qxF "$l" || echo "$l"; done)
        [ -z "$miss" ] && ok "datad's 4 carriers = zte-agent's, word for word" || bad "datad carriers not in zte-agent operator_en(): $(echo $miss)"
    else
        skip "datad carriers: $SCREEN_RS not here"
    fi
    miss=$(printf '%s\n' "$GO" | awk NF | while IFS='	' read -r z e; do
        have=$(tsv_en "$z"); [ -z "$have" ] || [ "$have" = "$e" ] || echo "$z: $have"; done)
    [ -z "$miss" ] && ok "operator rows in en.tsv = glossary §8" || bad "operator rows in en.tsv differ: $miss"
else
    skip "operator English: $GLOSS or $AGENT not here"
fi

# 7. *_en fields the screen reads
# files whose *_en come from datad's write-transaction data, not zte-agent
OPS_C='/(net_view|data|op_view)\.c$|/ui_parts/op\.c$'
if [ -f "$REPO/src/net_view.c" ]; then
    FD=$(perl -ne 'print "$1_en\n" while /\btext\(\s*\w+\s*,\s*"(\w+)"/g' "$REPO/src/net_view.c" | sort -u)
    FA=$( { perl -ne 'print "$1_en\n" while /\bjson_text\(\s*\w+\s*,\s*"(\w+)"/g' "$REPO/src/alerts.c"
        ls "$REPO"/src/*.c "$REPO"/src/ui_parts/*.c | grep -vE "$OPS_C" | xargs perl -ne 'print "$1\n" while /"(\w+_en)"/g'; } | sort -u)
    # E4: write transactions and refused writes come from datad's /control and
    # /v2/screen "op" (data-service rust/src/ops/ui.rs, STATE_V2.md §12)
    FO=$(ls "$REPO"/src/*.c "$REPO"/src/ui_parts/*.c | grep -E "$OPS_C" | grep -v '/net_view\.c$' |
         xargs perl -ne 'print "$1\n" while /"(\w+_en)"/g' | sort -u)
    [ -n "$FD" ] && [ -n "$FA" ] && ok "_en fields the screen reads: $(echo $FD $FA | wc -w | tr -d ' ')" || bad "found no _en fields in src/net_view.c or the agent parsers"
    if [ -f "$SCREEN_RS" ]; then
        miss=$(for f in $FD; do grep -q "^ *pub $f:" "$SCREEN_RS" || echo "$f"; done)
        [ -z "$miss" ] && ok "datad sends every _en net_view.c reads" || bad "net_view.c reads, datad screen.rs has no pub field: $(echo $miss)"
    else
        skip "datad _en fields: $SCREEN_RS not here"
    fi
    if [ -f "$DATAD/rust/src/ops/ui.rs" ]; then
        miss=$(for f in $FO; do cat "$DATAD"/rust/src/ops/ui.rs "$DATAD"/rust/src/ops/engine.rs | grep -q "\"$f\"" || echo "$f"; done)
        [ -z "$miss" ] && ok "datad sends every write-transaction _en the screen reads ($(echo $FO | wc -w | tr -d ' '))" ||
            bad "the screen reads, datad ops/ui.rs never sends: $(echo $miss)"
    else
        skip "datad write-transaction _en fields: $DATAD/rust/src/ops/ui.rs not here"
    fi
    if [ -d "$AGENT" ]; then
        # each .rs up to its first #[cfg(test)]
        for r in $(find "$AGENT" -name '*.rs'); do awk '/^ *#\[cfg\(test\)\]/ { exit } 1' "$r"; done >/tmp/cross-repo.agent.$$
        miss=$(for f in $FA; do grep -qE "\"$f\"|\\b$f:" /tmp/cross-repo.agent.$$ || echo "$f"; done)
        rm -f /tmp/cross-repo.agent.$$
        [ -z "$miss" ] && ok "zte-agent sends every other _en the screen reads" || bad "the screen reads, zte-agent (non-test code) never sends: $(echo $miss)"
    else
        skip "zte-agent _en fields: $AGENT not here"
    fi
else
    skip "_en fields: $REPO/src not here"
fi

echo "cross-repo: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" = 0 ]
