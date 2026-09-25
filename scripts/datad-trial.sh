#!/bin/sh
# ─────────────────────────────────────────────────────────────────────────────
# datad-trial.sh — run a side-by-side zwrt-datad test build on the U60 Pro
# (MU5250), watch it, and put the production datad back by itself.
#
# The old touch UI only reads 127.0.0.1:9460, so the test build cannot run
# next to the production one: it runs INSTEAD of it for an hour.
#
# On the device:
#   1. push the new build as /data/plugins/zwrt-datad/zwrt-datad.test
#      (chmod +x) and this script as /data/u60-guard/datad-trial.sh
#   2. sh /data/u60-guard/datad-trial.sh launch [VAR=value …]
#        preflight; /etc/init.d/zwrt-datad stop; start zwrt-datad.test with
#        the arguments and environment of the init script's start_service
#        (extra VAR=value, e.g. ZWRT_DATAD_UBUS=cli|socket, are added);
#        once it serves /state, `start` the watcher. If it does not come up,
#        production is started again at once and launch fails.
#        The test build runs WITHOUT supervise.sh: a crash is not respawned,
#        so the watcher sees it.
#   3. sh /data/u60-guard/datad-trial.sh status   running? + last log lines
#      sh /data/u60-guard/datad-trial.sh abort    stop now, restore production
#   Logs: /data/u60-guard/datad-trial.log       (watcher: reasons, outcome)
#         /data/u60-guard/datad-trial.test.log  (test build stdout/stderr)
# Detaching from the SSH session (the device's BusyBox has no setsid and no
# timeout): setsid if present, else `start-stop-daemon -S -b` (BusyBox: fork +
# new session, stdio to /dev/null, so it runs /bin/sh -c 'exec … >>log' to keep
# the output), else plain `nohup … &` (logged: the trial may die with the SSH
# session then). See detach().
#   `start` alone watches an already running test build (production stopped
#   by hand); `print-launch` shows the launch command without running it.
#
# Every 10 s it checks; the first failure aborts the trial:
#   1. screen data stopped: /state's "ts" has not changed for 30 s
#      (a failed or timed-out fetch counts as "not changed")
#   2. test build crashed: `pidof zwrt-datad.test` is empty
#   3. screen trouble: u60-uid logged a give-up / vendor-UI hand-over / an
#      unrequested u60pro-devui exit (/tmp/u60-uid.log; the device has no
#      logd, so logread is empty), or u60pro-devui is gone
#   4. zte-agent netwatch errors went up. Signal, first that works:
#      a. DT_NETWATCH_CMD, a command printing a counter (override);
#      b. /tmp/netwatch.errors, zte-agent's own counter (one decimal line,
#         only grows, back to 0 when the agent restarts);
#      c. no usable file: our own count of /state replies that have a "ts"
#         but no "wan_status" (netwatch then sees connected=None, i.e. blind).
#      A smaller counter means the agent restarted: new baseline, no abort.
#      The signal in use is logged when it is first used or changes.
# Abort: kill the test build, `/etc/init.d/zwrt-datad start`, reason into the
# log. Production counts as back only when its /state answers with a ts. If
# the test build survives kill -9, production is NOT started (both would want
# 9460): loud log line, exit non-0. An unexpected watcher exit (script error,
# signal other than KILL) restores too (EXIT trap); launch fails and restores
# if the watcher is not confirmed running within 5 s. After the window (1 h) it restores production the same way and logs
# PASS. Promoting the test build to the live slot is NOT done here.
#
# Time comes from /proc/uptime (monotonic): the device clock is local time
# labelled UTC and SNTP can step it during the hour.
# Every command and path can be overridden from the environment for tests.
# SPDX-License-Identifier: MIT
# ─────────────────────────────────────────────────────────────────────────────

INITD_DATAD=${DT_INITD:-/etc/init.d/zwrt-datad}
RC=${DT_RC:-/etc/rc.local}
LOG=${DT_LOG:-/data/u60-guard/datad-trial.log}
PIDFILE=${DT_PIDFILE:-/tmp/datad-trial.pid}
UPTIME_FILE=${DT_UPTIME:-/proc/uptime}
STATE_URL=${DT_STATE_URL:-http://127.0.0.1:9460/state}
CURL=${DT_CURL:-/usr/bin/curl}
PIDOF=${DT_PIDOF:-pidof}
PS=${DT_PS:-ps w}
UID_LOG=${DT_UID_LOG:-/tmp/u60-uid.log}
LOGREAD=${DT_LOGREAD:-cat $UID_LOG}
KILL=${DT_KILL:-kill}
SLEEP=${DT_SLEEP:-sleep}
DATE=${DT_DATE:-date}
SETSID=${DT_SETSID:-setsid}
SSD=${DT_SSD:-start-stop-daemon}
SH=${DT_SH:-/bin/sh}
NETWATCH_CMD=${DT_NETWATCH_CMD:-}
NETWATCH_FILE=${DT_NETWATCH_FILE:-/tmp/netwatch.errors}
TEST_BIN=${DT_TEST_BIN:-/data/plugins/zwrt-datad/zwrt-datad.test}
TEST_LOG=${DT_TEST_LOG:-/data/u60-guard/datad-trial.test.log}

TEST_NAME=${DT_TEST_NAME:-zwrt-datad.test}
PROD_NAME=${DT_PROD_NAME:-zwrt-datad}
UI_NAME=${DT_UI_NAME:-u60pro-devui}
# Old manual flow (start.sh copied to start.test.sh): still killed on
# restore so a leftover wrapper cannot relaunch the test build. launch does
# not use a wrapper.
WRAPPER=${DT_WRAPPER:-start.test.sh}

# What /etc/init.d/zwrt-datad's start_service runs (minus supervise.sh).
# scripts/test/datad-trial checks these against scripts/zwrt-datad.init.
LAUNCH_ARGS="-i 1000 --lan-bind 0.0.0.0 --lan-port 9461 --auth-token-file /data/plugins/zwrt-datad/auth.token"
LAUNCH_ENV="DATAD_MODEM_REMOTE_STREAM=1 DATAD_MODEM_REMOTE_STALE_SEC=6 ZWRT_DATAD_OTA_DISABLE_AUTO=1"

INTERVAL=${DT_INTERVAL:-10}
STALE=${DT_STALE:-30}
WINDOW=${DT_WINDOW:-3600}

# u60-uid's own wording (src/uid.c logf_). Its file lines have no "u60-uid:"
# prefix, so the pattern must not require one.
UID_BAD='(giving up|starting the vendor UI|vendor UI on screen|u60pro-devui pid [0-9]+ ended:)'

now() { cut -d. -f1 "$UPTIME_FILE"; }

log() {
    mkdir -p "$(dirname "$LOG")" 2>/dev/null
    echo "$($DATE '+%Y-%m-%d %H:%M:%S') $*" >>"$LOG"
}

say() { echo "datad-trial: $*"; }

# ── checks ──────────────────────────────────────────────────────────────────

# Fetch /state once; sets STATE (empty on failure).
fetch_state() {
    STATE=$($CURL -s -m 3 --noproxy '*' "$STATE_URL" 2>/dev/null)
}

# First "ts": <digits> anywhere in the reply (not only right after "{").
state_ts() {
    printf '%s' "$STATE" | grep -o '"ts"[[:space:]]*:[[:space:]]*[0-9][0-9]*' | head -n 1 | sed 's/.*://; s/[^0-9]//g'
}

# num <string>: decimal digits only, leading zeros dropped ("08" → 8; ash
# arithmetic would read 08 as bad octal and kill the script), empty → 0.
num() {
    _v=$(printf '%s' "$1" | tr -dc '0-9' | sed 's/^0*//')
    echo "${_v:-0}"
}

# watcher_pid_ok <pid>: that pid is alive AND is this script (pid reuse).
watcher_pid_ok() {
    [ -n "$1" ] && [ -d "/proc/$1" ] && tr '\0' ' ' <"/proc/$1/cmdline" 2>/dev/null | grep -q 'datad-trial\.sh'
}

uid_bad_count() {
    $LOGREAD 2>/dev/null | grep -E "$UID_BAD" | grep -vc '(requested)'
}

wrapper_pids() {
    [ -n "$WRAPPER" ] || return 0
    $PS 2>/dev/null | grep -F "$WRAPPER" | grep -v -e grep -e datad-trial | awk '{ print $1 }'
}

# netwatch errors so far; sets NW_SRC (which signal) and NW_VAL.
netwatch_read() {
    if [ -n "$NETWATCH_CMD" ]; then
        NW_SRC="命令 $NETWATCH_CMD"
        NW_VAL=$(num "$($NETWATCH_CMD 2>/dev/null | head -n 1)")
        return
    fi
    _n=$(head -n 1 "$NETWATCH_FILE" 2>/dev/null)
    if printf '%s' "$_n" | grep -qx '[0-9][0-9]*'; then
        NW_SRC="zte-agent 计数文件 $NETWATCH_FILE"
        NW_VAL=$(num "$_n")
    else
        NW_SRC="自己读 /state（有 ts 没有 wan_status 算一次；$NETWATCH_FILE 不存在或不是数字）"
        NW_VAL=$NW_OWN
    fi
}

# ── restore production ──────────────────────────────────────────────────────

kill_test() {
    for _p in $(wrapper_pids); do $KILL "$_p" 2>/dev/null; done
    _pids=$($PIDOF "$TEST_NAME" 2>/dev/null)
    [ -n "$_pids" ] && $KILL $_pids 2>/dev/null
    _i=0
    while [ $_i -lt 5 ]; do
        _pids=$($PIDOF "$TEST_NAME" 2>/dev/null)
        [ -z "$_pids" ] && return 0
        $SLEEP 1
        _i=$((_i + 1))
    done
    for _p in $(wrapper_pids); do $KILL -9 "$_p" 2>/dev/null; done
    $KILL -9 $_pids 2>/dev/null
    # -9 can take a moment (D state): wait, try -9 again, wait again.
    _i=0
    while [ $_i -lt 6 ]; do
        $SLEEP 1
        _pids=$($PIDOF "$TEST_NAME" 2>/dev/null)
        [ -z "$_pids" ] && return 0
        [ $_i = 2 ] && $KILL -9 $_pids 2>/dev/null
        _i=$((_i + 1))
    done
    return 1
}

start_prod() {
    _try=1
    while [ $_try -le 2 ]; do
        "$INITD_DATAD" start >/dev/null 2>&1
        # Back = the process exists AND /state answers with a ts, within 20 s.
        _i=0
        while [ $_i -lt 20 ]; do
            _p=$($PIDOF "$PROD_NAME" 2>/dev/null)
            if [ -n "$_p" ]; then
                fetch_state
                if [ -n "$(state_ts)" ]; then
                    log "正式版已恢复（pid $_p，/state 有 ts）"
                    return 0
                fi
            fi
            $SLEEP 1
            _i=$((_i + 1))
        done
        _try=$((_try + 1))
    done
    log "！！正式版没起来：请手动 $INITD_DATAD start 并检查"
    return 1
}

# restore <outcome line for the log>
restore() {
    [ -n "$RESTORED" ] && return 0
    RESTORED=1
    log "$1"
    if kill_test; then
        log "测试版已停止"
    else
        # Starting production now would fight it for 9460.
        log "！！测试版杀不掉（pid $($PIDOF "$TEST_NAME" 2>/dev/null)），没有启动正式版：请手动 kill -9 后 $INITD_DATAD start"
        rm -f "$PIDFILE"
        return 1
    fi
    start_prod
    _rc=$?
    rm -f "$PIDFILE"
    return $_rc
}

# ── preflight ───────────────────────────────────────────────────────────────

# Checks shared by start and launch; one reason per line, empty = OK.
preflight_common() {
    if [ ! -f "$INITD_DATAD" ]; then
        echo "$INITD_DATAD 不存在，恢复正式版没有办法"
    elif grep -q '\.test' "$INITD_DATAD"; then
        echo "$INITD_DATAD 里有 .test：它必须只指向正式程序"
    fi
    if grep -v '^[[:space:]]*#' "$RC" 2>/dev/null | grep 'zwrt-datad' | grep -q '\.test'; then
        echo "$RC 里 zwrt-datad 那行指向 .test：它必须只指向正式程序"
    fi
    if [ -f "$PIDFILE" ] && [ -d "/proc/$(cat "$PIDFILE" 2>/dev/null)" ]; then
        echo "已经有一个守护在跑（pid $(cat "$PIDFILE")）"
    fi
}

# start: production already stopped, test build already running.
preflight() {
    preflight_common
    if [ -n "$($PIDOF "$PROD_NAME" 2>/dev/null)" ]; then
        echo "正式版 $PROD_NAME 还在跑：先 $INITD_DATAD stop（两个 datad 会抢 9460），或者用 launch"
    fi
    if [ -z "$($PIDOF "$TEST_NAME" 2>/dev/null)" ]; then
        echo "测试版 $TEST_NAME 没在跑：用 launch 起它"
    fi
}

# launch: production running is the normal case; the test build must exist
# and not be running yet. Extra arguments must be VAR=value.
preflight_launch() {
    preflight_common
    if [ ! -f "$TEST_BIN" ] || [ ! -x "$TEST_BIN" ]; then
        echo "$TEST_BIN 不存在或不可执行：先推送并 chmod +x"
    fi
    if [ -n "$($PIDOF "$TEST_NAME" 2>/dev/null)" ]; then
        echo "测试版 $TEST_NAME 已经在跑（pid $($PIDOF "$TEST_NAME")）：用 start 守护它，或先 abort"
    fi
    for _kv in "$@"; do
        printf '%s' "$_kv" | grep -qE '^[A-Za-z_][A-Za-z0-9_]*=[^[:space:]]*$' ||
            echo "额外参数 “$_kv” 不是 VAR=value（不带空格）"
    done
}

refuse() { # refuse <verb> <problems>
    say "拒绝$1："
    echo "$2" | sed 's/^/  - /'
    log "拒绝$1：$(echo "$2" | tr '\n' ';')"
    exit 1
}

# The command launch runs, one line (the extra VAR=value go after LAUNCH_ENV).
launch_cmd() {
    echo "env $LAUNCH_ENV${*:+ $*} nohup $TEST_BIN $LAUNCH_ARGS"
}

# detach <outfile> <command…>: run the command in the background, out of this
# session (survives the SSH logout), stdin /dev/null, stdout+stderr appended to
# <outfile>. The command and its arguments are passed as separate words, never
# pasted into a shell string.
detach() {
    _out=$1
    shift
    if command -v "$SETSID" >/dev/null 2>&1; then
        "$SETSID" "$@" >>"$_out" 2>&1 </dev/null &
    elif command -v "$SSD" >/dev/null 2>&1 || command -v "/sbin/$SSD" >/dev/null 2>&1; then
        command -v "$SSD" >/dev/null 2>&1 || SSD=/sbin/$SSD
        # -p on a file that never exists: BusyBox then matches no running
        # process (with only -x it would match every busybox /bin/sh and
        # refuse "already running"). $0 of the inner sh = the output file.
        # shellcheck disable=SC2016
        "$SSD" -S -b -p "$PIDFILE.none" -x "$SH" -- \
            -c 'exec "$@" >>"$0" 2>&1 </dev/null' "$_out" "$@"
    else
        log "没有 setsid 也没有 start-stop-daemon：用 nohup … & 后台启动，SSH 断开时可能被一起带走"
        "$@" >>"$_out" 2>&1 </dev/null &   # both callers already start with nohup
    fi
}

# Absolute path of this script: the detached watcher must not depend on the cwd.
SELF=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")

# Starts the watcher and confirms (≤ 5 s) it runs: pidfile written, pid alive,
# and it is this script. If not: restore production, return 1.
do_start() {
    problems=$(preflight)
    [ -n "$problems" ] && refuse 开始 "$problems"
    detach /dev/null nohup sh "$SELF" run
    _i=0
    while [ $_i -lt 5 ]; do
        $SLEEP 1
        _i=$((_i + 1))
        if watcher_pid_ok "$(cat "$PIDFILE" 2>/dev/null)"; then
            say "已开始守护（pid $(cat "$PIDFILE")），日志：$LOG"
            return 0
        fi
    done
    RESTORED=
    restore "启动失败：守护 5s 内没起来（$PIDFILE 没有或进程不在），已恢复正式版"
    say "失败：守护没起来，已停测试版并恢复正式版；看 $LOG"
    return 1
}

# launch [VAR=value …]
do_launch() {
    problems=$(preflight_launch "$@")
    [ -n "$problems" ] && refuse 启动 "$problems"

    log "launch：停正式版（$INITD_DATAD stop）"
    "$INITD_DATAD" stop >/dev/null 2>&1
    _i=0
    while [ -n "$($PIDOF "$PROD_NAME" 2>/dev/null)" ]; do
        if [ $_i -ge 15 ]; then
            RESTORED=
            restore "启动失败：正式版 15s 内没有退出（pid $($PIDOF "$PROD_NAME" 2>/dev/null)），没有起测试版"
            say "失败：正式版停不下来，已恢复，看日志 $LOG"
            exit 1
        fi
        $SLEEP 1
        _i=$((_i + 1))
    done

    mkdir -p "$(dirname "$TEST_LOG")" 2>/dev/null
    echo "=== $($DATE '+%Y-%m-%d %H:%M:%S') $(launch_cmd "$@")" >>"$TEST_LOG"
    log "launch：$(launch_cmd "$@")"
    # No supervise.sh: a crash must stay a crash so the watcher sees it.
    # shellcheck disable=SC2086
    detach "$TEST_LOG" env $LAUNCH_ENV "$@" nohup "$TEST_BIN" $LAUNCH_ARGS

    # Up = the process exists and /state answers with a ts, within 20 s.
    _i=0
    _why="进程没出现"
    while [ $_i -lt 20 ]; do
        $SLEEP 1
        _i=$((_i + 1))
        [ -n "$($PIDOF "$TEST_NAME" 2>/dev/null)" ] || continue
        _why="/state 没有 ts"
        fetch_state
        [ -n "$(state_ts)" ] || continue
        log "launch：测试版已起来（pid $($PIDOF "$TEST_NAME" 2>/dev/null)，${_i}s）"
        say "测试版已起来，输出：$TEST_LOG"
        do_start
        exit $?
    done
    RESTORED=
    restore "启动失败：测试版 20s 内没起来（$_why），看 $TEST_LOG"
    say "失败：测试版没起来（$_why），已恢复正式版；看 $LOG 和 $TEST_LOG"
    exit 1
}

# ── the watch loop ──────────────────────────────────────────────────────────

run() {
    RESTORED=
    # EXIT: a script error or an unexpected exit still restores production
    # (restore runs once: RESTORED). SIGKILL (OOM) cannot be caught.
    trap 'restore "中止：守护意外退出"' EXIT
    trap 'restore "中止：守护被停止（abort 或信号）"; exit 1' INT TERM  # not HUP: nohup's ignore must stay
    echo $$ >"$PIDFILE"

    T0=$(now)
    fetch_state
    LAST_TS=$(state_ts)
    LAST_CHANGE=$T0
    UID_BASE=$(uid_bad_count)
    NW_OWN=0
    netwatch_read
    NW_BASE=$NW_VAL
    NW_LAST_SRC=$NW_SRC
    log "netwatch 信号：$NW_SRC（基线 $NW_BASE）"
    log "开始：测试版 pid $($PIDOF "$TEST_NAME" 2>/dev/null)，窗口 ${WINDOW}s，每 ${INTERVAL}s 查一次；ts=${LAST_TS:-无}"

    while :; do
        $SLEEP "$INTERVAL"
        T=$(now)

        # 1. screen data. A failed fetch only counts here, never as a
        #    netwatch error, so a dead datad reports one reason.
        fetch_state
        TS=$(state_ts)
        if [ -n "$TS" ] && [ "$TS" != "$LAST_TS" ]; then
            LAST_TS=$TS
            LAST_CHANGE=$T
        elif [ $((T - LAST_CHANGE)) -gt "$STALE" ]; then
            restore "中止：屏幕数据停了（/state 的 ts ${LAST_TS:-无} 已 $((T - LAST_CHANGE))s 没变）"
            exit 1
        fi

        # 2. test build
        if [ -z "$($PIDOF "$TEST_NAME" 2>/dev/null)" ]; then
            restore "中止：测试版崩溃（pidof $TEST_NAME 为空）"
            exit 1
        fi

        # 3. screen owner / UI
        N=$(uid_bad_count)
        if [ "$N" -lt "$UID_BASE" ]; then
            UID_BASE=$N # log buffer wrapped
        elif [ "$N" -gt "$UID_BASE" ]; then
            LINE=$($LOGREAD 2>/dev/null | grep -E "$UID_BAD" | grep -v '(requested)' | tail -n 1 | cut -c1-200)
            restore "中止：界面异常（u60-uid：$LINE）"
            exit 1
        fi
        if [ -z "$($PIDOF "$UI_NAME" 2>/dev/null)" ]; then
            restore "中止：界面异常（$UI_NAME 进程消失）"
            exit 1
        fi

        # 4. netwatch
        # a string, or null (no SIM / dialling), is a reply; missing is not
        if [ -n "$STATE" ] && [ -n "$TS" ] && ! printf '%s' "$STATE" | grep -qE '"wan_status"[[:space:]]*:[[:space:]]*("|null)'; then
            NW_OWN=$((NW_OWN + 1))
        fi
        netwatch_read
        if [ "$NW_SRC" != "$NW_LAST_SRC" ]; then
            log "netwatch 信号改为：$NW_SRC（基线 $NW_VAL）"
            NW_LAST_SRC=$NW_SRC
            NW_BASE=$NW_VAL
        elif [ "$NW_VAL" -lt "$NW_BASE" ]; then
            log "netwatch 计数变小（$NW_BASE → $NW_VAL），当作 zte-agent 重启，重设基线"
            NW_BASE=$NW_VAL
        elif [ "$NW_VAL" -gt "$NW_BASE" ]; then
            restore "中止：zte-agent netwatch 报错上升（$NW_BASE → $NW_VAL；来源：$NW_SRC）"
            exit 1
        fi

        if [ $((T - T0)) -ge "$WINDOW" ]; then
            restore "通过：${WINDOW}s 窗口内没有异常，恢复正式版（升级到正式位置另做）" && exit 0
            exit 1
        fi
    done
}

cmd=$1
[ $# -gt 0 ] && shift
case "$cmd" in
    launch)
        do_launch "$@"
        ;;
    start)
        do_start || exit 1
        ;;
    print-launch)
        launch_cmd "$@"
        ;;
    run)
        run
        ;;
    status)
        if [ -f "$PIDFILE" ] && [ -d "/proc/$(cat "$PIDFILE" 2>/dev/null)" ]; then
            say "运行中（pid $(cat "$PIDFILE")）"
        else
            say "没在运行"
        fi
        tail -n 5 "$LOG" 2>/dev/null
        ;;
    abort)
        p=$(cat "$PIDFILE" 2>/dev/null)
        if watcher_pid_ok "$p"; then
            $KILL "$p" # the watcher restores production itself
            say "已通知守护（pid $p）中止，看日志：$LOG"
        else
            [ -n "$p" ] && [ -d "/proc/$p" ] && log "abort：pidfile 里的 pid $p 不是守护（pid 复用），不动它"
            RESTORED=
            restore "中止：手动 abort（守护没在运行）"
        fi
        ;;
    *)
        echo "usage: $0 launch [VAR=value ...]|start|status|abort|print-launch" >&2
        exit 2
        ;;
esac
