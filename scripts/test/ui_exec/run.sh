#!/bin/sh
# Theme-switch exec tests, run in an arm64 busybox container:
#   docker run --rm --platform linux/arm64 -v "$PWD/scripts":/scripts busybox /scripts/test/ui_exec/run.sh
# Runs the binary under three names: its own, the live name and the side-test
# name u60-uid must still recognise, plus the replaced-on-disk refusal.
# SPDX-License-Identifier: MIT
T=${SCRIPTS:-/scripts}/test/ui_exec/ui_exec_test
[ -x "$T" ] || { echo "  FAIL ui_exec_test not built (scripts/test/ui_exec/build.sh)"; echo "passed 0, failed 1"; exit 1; }
rc=0
d=$(mktemp -d)
for name in ui_exec_test u60pro-devui u60pro-devui.test; do
    echo "== as $name"
    cp "$T" "$d/$name" && "$d/$name" || rc=1
done
echo "== replaced on disk"
cp "$T" "$d/u60pro-devui.gone" && "$d/u60pro-devui.gone" --delete-self || rc=1
rm -rf "$d"
exit $rc
