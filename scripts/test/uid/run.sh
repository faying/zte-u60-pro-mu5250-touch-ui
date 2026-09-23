#!/bin/sh
# u60-uid decision tests. The test program is C, cross-built for the device's
# architecture (aarch64, static), which is also what Docker on an Apple-silicon Mac runs
# natively. Build it first:  scripts/test/uid/build.sh
# SPDX-License-Identifier: MIT
T=${SCRIPTS:-/scripts}/test/uid/uid_core_test
[ -x "$T" ] || { echo "  FAIL uid_core_test not built (scripts/test/uid/build.sh)"; echo "passed 0, failed 1"; exit 1; }
exec "$T"
