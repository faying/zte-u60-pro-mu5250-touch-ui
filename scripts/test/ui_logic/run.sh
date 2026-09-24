#!/bin/sh
# Touch-UI pure-decision tests (appearance, exec guard, status bar, battery,
# signal card). Cross-built static aarch64, runs natively in Docker on an
# Apple-silicon Mac. Build it first:  scripts/test/ui_logic/build.sh
# SPDX-License-Identifier: MIT
T=${SCRIPTS:-/scripts}/test/ui_logic/ui_logic_test
[ -x "$T" ] || { echo "  FAIL ui_logic_test not built (scripts/test/ui_logic/build.sh)"; echo "passed 0, failed 1"; exit 1; }
exec "$T"
