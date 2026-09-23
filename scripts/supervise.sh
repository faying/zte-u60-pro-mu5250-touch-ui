#!/bin/sh
# ─────────────────────────────────────────────────────────────────────────────
# supervise.sh — the thin layer between procd and a supervised program.
#
#   supervise.sh <name> <alert-kind> <command> [args…]
#
# procd restarts what dies but has no "it died" hook, so it cannot tell anyone.
# This runs the program as a foreground child, forwards TERM/INT to it, and
# when it exits abnormally keeps the evidence and raises an alert before
# exiting with the same status for procd's respawn rules:
#
#   * output goes to /tmp/<name>.log (trimmed at start when large);
#   * a crash copies the last 200 lines + exit status to
#     /data/crashlog/<name>/ (5 files per program, 1 MB in all), because /tmp
#     does not survive the reboot a crash so often precedes;
#   * an event goes to /data/alerts (alert-lib.sh), which u60-guard turns into
#     an SMS and the admin web into a banner.
#
# A stop requested through procd (TERM to us) is not a crash.
#
# If this wrapper is itself SIGKILLed, its child lives on and procd starts a
# second wrapper — which would start a second copy. So the child's pid is kept
# in /tmp/supervise-<name>.pid and a leftover is stopped before starting, after
# checking its /proc/<pid>/comm really is <name> (prefix: side-by-side test
# builds run as e.g. zte-agent.test).
# SPDX-License-Identifier: MIT
# ─────────────────────────────────────────────────────────────────────────────

HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/alert-lib.sh"

NAME=$1
KIND=$2
shift 2 2>/dev/null
if [ -z "$NAME" ] || [ -z "$KIND" ] || [ $# -eq 0 ]; then
    echo "usage: $0 <name> <alert-kind> <command> [args...]" >&2
    exit 2
fi

LOGFILE=${SUPERVISE_LOG_DIR:-/tmp}/$NAME.log
PIDFILE=${SUPERVISE_RUN_DIR:-/tmp}/supervise-$NAME.pid
CRASHDIR=${SUPERVISE_CRASH_DIR:-/data/crashlog}
PROC=${SUPERVISE_PROC:-/proc}
CRASH_KEEP=5
CRASH_TOTAL_KB=1024

# ── leftovers from a wrapper that was killed outright ───────────────────────
stop_leftover() {
    _old=$(cat "$PIDFILE" 2>/dev/null)
    case "$_old" in '' | *[!0-9]*) return 0 ;; esac
    _comm=$(cat "$PROC/$_old/comm" 2>/dev/null) || return 0
    case "$_comm" in
        "$NAME"*) ;;
        *) return 0 ;; # pid reused by something else: leave it alone
    esac
    echo "$(date '+%Y-%m-%dT%H:%M:%S') supervise: stopping leftover $NAME ($_old)" >>"$LOGFILE"
    kill -TERM "$_old" 2>/dev/null
    _i=0
    while [ -d "$PROC/$_old" ] && [ $_i -lt 10 ]; do
        sleep 1
        _i=$((_i + 1))
    done
    [ -d "$PROC/$_old" ] && kill -KILL "$_old" 2>/dev/null
    return 0
}

trim_log() {
    [ -f "$LOGFILE" ] || return 0
    [ "$(wc -c <"$LOGFILE")" -gt 524288 ] || return 0
    tail -n 2000 "$LOGFILE" >"$LOGFILE.tmp" && mv "$LOGFILE.tmp" "$LOGFILE"
}

describe() { # exit status → words
    if [ "$1" -gt 128 ]; then
        case $(($1 - 128)) in
            6) echo "killed by SIGABRT" ;;
            9) echo "killed by SIGKILL" ;;
            11) echo "killed by SIGSEGV" ;;
            15) echo "killed by SIGTERM" ;;
            *) echo "killed by signal $(($1 - 128))" ;;
        esac
    else
        echo "exit $1"
    fi
}

# Best effort throughout: nothing here may stop procd from restarting it.
record_crash() {
    _why=$1
    _dir=$CRASHDIR/$NAME
    mkdir -p "$_dir" 2>/dev/null || return 0
    _up=$(cut -d. -f1 /proc/uptime 2>/dev/null)
    _f=$_dir/$(date '+%Y%m%d-%H%M%S')-up${_up:-0}.log
    {
        echo "program: $NAME"
        echo "status:  $_why"
        # Device local time: ZTE's SNTP keeps the clock on local wall time
        # under a system zone of UTC, so no %z here — it would say +0000.
        echo "time:    $(date '+%Y-%m-%d %H:%M:%S') device local (uptime ${_up:-?}s; unreliable before the network sets the clock)"
        echo "--- last 200 lines of $LOGFILE ---"
        tail -n 200 "$LOGFILE" 2>/dev/null
    } >"$_f" 2>/dev/null
    # Newest CRASH_KEEP per program…
    ls -t "$_dir"/*.log 2>/dev/null | tail -n +$((CRASH_KEEP + 1)) | while read -r _old; do rm -f "$_old"; done
    # …and never more than CRASH_TOTAL_KB across all of them: drop the oldest.
    while [ "$(du -sk "$CRASHDIR" 2>/dev/null | cut -f1)" -gt "$CRASH_TOTAL_KB" ]; do
        _oldest=$(ls -t "$CRASHDIR"/*/*.log 2>/dev/null | tail -n 1)
        [ -n "$_oldest" ] && [ "$_oldest" != "$_f" ] || break
        rm -f "$_oldest"
    done
}

# ── run ─────────────────────────────────────────────────────────────────────
stop_leftover
trim_log

CHILD=
STOPPING=
on_signal() {
    STOPPING=1
    [ -n "$CHILD" ] && kill -"$1" "$CHILD" 2>/dev/null
}
trap 'on_signal TERM' TERM
trap 'on_signal INT' INT

echo "$(date '+%Y-%m-%dT%H:%M:%S') supervise: starting $NAME: $*" >>"$LOGFILE"
"$@" >>"$LOGFILE" 2>&1 </dev/null &
CHILD=$!
echo "$CHILD" >"$PIDFILE"

# `wait` returns early whenever a trapped signal arrives; keep waiting until
# the child itself is gone, so its real status is what we report.
while :; do
    wait "$CHILD"
    RC=$?
    kill -0 "$CHILD" 2>/dev/null || break
done
rm -f "$PIDFILE"

WHY=$(describe "$RC")
echo "$(date '+%Y-%m-%dT%H:%M:%S') supervise: $NAME ended: $WHY${STOPPING:+ (stop requested)}" >>"$LOGFILE"

if [ -z "$STOPPING" ] && [ "$RC" -ne 0 ]; then
    record_crash "$WHY"
    alert_add "$KIND" "$NAME $WHY" 2>/dev/null
fi
[ -n "$STOPPING" ] && exit 0
exit "$RC"
