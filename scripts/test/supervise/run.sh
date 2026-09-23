#!/bin/sh
# Tests for supervise.sh, in a busybox container (real /proc, real signals).
# SPDX-License-Identifier: MIT

SCRIPTS=${SCRIPTS:-/scripts}
SUP=$SCRIPTS/supervise.sh
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "  ok   $1"; }
bad() { FAIL=$((FAIL + 1)); echo "  FAIL $1"; }
check() { _d=$1; shift; if eval "$@"; then ok "$_d"; else bad "$_d  [$*]"; fi; }

setup() {
    T=$(mktemp -d)
    export SUPERVISE_LOG_DIR=$T SUPERVISE_RUN_DIR=$T SUPERVISE_CRASH_DIR=$T/crash ALERT_DIR=$T/alerts
    # A long-running program whose /proc comm is "fakesvc" (a script's comm
    # is its own file name). Dies on TERM like the real binaries do.
    printf '#!/bin/sh\ntrap "exit 143" TERM\nsleep "$1" &\nwait\n' >"$T/fakesvc"
    chmod +x "$T/fakesvc"
}
teardown() { rm -rf "$T"; }
kinds() { awk -F'\t' '{print $4 ":" $5}' "$T/alerts/queue" 2>/dev/null; }
wait_for() { _i=0; while ! eval "$1" && [ $_i -lt 50 ]; do sleep 0.1; _i=$((_i + 1)); done; }

echo "abnormal exit"
setup
sh "$SUP" fakesvc agent-crash sh -c 'echo hello from child; exit 3'
rc=$?
check "exits with the child's status" '[ $rc = 3 ]'
check "one crash file with status and output" '[ $(ls $T/crash/fakesvc | wc -l) = 1 ] && grep -q "status:  exit 3" $T/crash/fakesvc/*.log && grep -q "hello from child" $T/crash/fakesvc/*.log'
check "alert raised" '[ "$(kinds)" = "agent-crash:fakesvc exit 3" ]'
check "pidfile removed" '[ ! -f $T/supervise-fakesvc.pid ]'
teardown

echo "killed by a signal"
setup
sh "$SUP" fakesvc agent-crash sh -c 'kill -SEGV $$' 
rc=$?
check "status 139" '[ $rc = 139 ]'
check "described as SIGSEGV" '[ "$(kinds)" = "agent-crash:fakesvc killed by SIGSEGV" ]'
teardown

echo "clean exit"
setup
sh "$SUP" fakesvc agent-crash true
check "no crash, no alert" '[ ! -d $T/crash/fakesvc ] && [ -z "$(kinds)" ]'
teardown

echo "stop requested (TERM to the wrapper)"
setup
sh "$SUP" fakesvc agent-crash "$T/fakesvc" 100 &
w=$!
wait_for '[ -s $T/supervise-fakesvc.pid ]'
child=$(cat "$T/supervise-fakesvc.pid")
kill -TERM "$w"
wait "$w"
rc=$?
check "wrapper exits 0" '[ $rc = 0 ]'
check "child got the TERM and is gone" '! kill -0 $child 2>/dev/null'
check "not recorded as a crash" '[ ! -d $T/crash/fakesvc ] && [ -z "$(kinds)" ]'
teardown

echo "stop requested, child ignores TERM"
setup
printf '#!/bin/sh\ntrap "" TERM\nwhile :; do sleep 1; done\n' >"$T/stubborn"; chmod +x "$T/stubborn"
export SUPERVISE_STOP_GRACE=2
sh "$SUP" stubborn agent-crash "$T/stubborn" &
w=$!
wait_for '[ -s $T/supervise-stubborn.pid ]'
child=$(cat "$T/supervise-stubborn.pid")
t0=$(date +%s)
kill -TERM "$w"
wait "$w"
rc=$?
t1=$(date +%s)
check "wrapper still exits 0" '[ $rc = 0 ]'
check "stubborn child killed after the grace period" '! kill -0 $child 2>/dev/null'
check "within grace + a little (not procd's 10 s)" '[ $((t1 - t0)) -le 4 ]'
check "not recorded as a crash" '[ ! -d $T/crash/stubborn ] && [ -z "$(kinds)" ]'
unset SUPERVISE_STOP_GRACE
teardown

echo "child dies while the wrapper waits"
setup
sh "$SUP" fakesvc agent-crash "$T/fakesvc" 100 &
w=$!
wait_for '[ -s $T/supervise-fakesvc.pid ]'
kill -KILL "$(cat "$T/supervise-fakesvc.pid")"
wait "$w"
rc=$?
check "kill -9 of the program: 137 and an alert" '[ $rc = 137 ] && [ "$(kinds)" = "agent-crash:fakesvc killed by SIGKILL" ]'
teardown

echo "leftover from a SIGKILLed wrapper"
setup
"$T/fakesvc" 100 &
orphan=$!
echo "$orphan" >"$T/supervise-fakesvc.pid"
sh "$SUP" fakesvc agent-crash true
check "orphan of the same name stopped first" '! kill -0 $orphan 2>/dev/null'
wait "$orphan" 2>/dev/null
teardown

setup
sleep 100 &
other=$!
echo "$other" >"$T/supervise-fakesvc.pid"
sh "$SUP" fakesvc agent-crash true
check "a reused pid (other program) is left alone" 'kill -0 $other 2>/dev/null'
kill "$other"
teardown

echo "crash log retention"
setup
for i in 1 2 3 4 5 6 7; do sh "$SUP" fakesvc agent-crash sh -c "exit $i"; sleep 1; done
check "keeps the newest 5" '[ $(ls $T/crash/fakesvc | wc -l) = 5 ] && grep -q "exit 7" $T/crash/fakesvc/* && ! grep -q "status:  exit 1\$" $T/crash/fakesvc/*'
teardown

setup
mkdir -p "$T/crash/other"
dd if=/dev/zero of="$T/crash/other/old.log" bs=1024 count=1100 2>/dev/null
touch -d '2020-01-01 00:00' "$T/crash/other/old.log"
sh "$SUP" fakesvc agent-crash false
check "total over 1 MB: oldest dropped, new one kept" '[ ! -f $T/crash/other/old.log ] && [ $(ls $T/crash/fakesvc | wc -l) = 1 ]'
teardown

echo "no alert dir writable"
setup
export ALERT_DIR=/proc/nope
sh "$SUP" fakesvc agent-crash sh -c 'exit 4'
rc=$?
check "still exits with the status (procd must restart it)" '[ $rc = 4 ]'
teardown

echo
echo "passed $PASS, failed $FAIL"
[ "$FAIL" = 0 ]
