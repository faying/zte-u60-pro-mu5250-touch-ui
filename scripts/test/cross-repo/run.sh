#!/bin/sh
# ─────────────────────────────────────────────────────────────────────────────
# Lists that must agree with files outside scripts/ (docs/SHIP.md「第二期」):
#   1. u60-ship.sh's GUARD_FILES = the install kit's guard list
#      (manager onboard/build-kit.sh) minus u60-ship.sh, datad-trial.sh,
#      u60-recover.sh, exactly (wifi-ab.sh too: manager 8ef099a).
#   2. the touch trial starts the test build the way u60-uid starts the UI
#      (src/uid.c: chdir(DEVUI_DIR), execl(DEVUI_BIN)).
#   3. every ship/<comp>/state-files (three repos) is within u60-ship.sh's
#      C_STATE_ALLOW for it, or the device refuses to stage (touch, 10-01).
# Run it on the host from the touch-ui checkout (sh scripts/test/cross-repo/
# run.sh); in a container that only has /scripts each part says it skipped.
# SPDX-License-Identifier: MIT
# ─────────────────────────────────────────────────────────────────────────────

SCRIPTS=${SCRIPTS:-$(cd "$(dirname "$0")/../.." && pwd)}
REPO=$(cd "$SCRIPTS/.." && pwd)
KIT=${BUILD_KIT:-$REPO/../manager/onboard/build-kit.sh}
PASS=0
FAIL=0
SKIP=0
ok() { PASS=$((PASS + 1)); echo "  ok   $1"; }
bad() { FAIL=$((FAIL + 1)); echo "  FAIL $1"; }
skip() { SKIP=$((SKIP + 1)); echo "  skip $1"; }

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

echo "cross-repo: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" = 0 ]
