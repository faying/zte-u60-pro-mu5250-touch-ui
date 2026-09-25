#!/bin/sh
# ─────────────────────────────────────────────────────────────────────────────
# datad-trial.sh — watch a side-by-side zwrt-datad test run on the U60 Pro
# (MU5250) and put the production datad back by itself.
#
# The old touch UI only reads 127.0.0.1:9460, so the test build cannot run
# next to the production one. The manual steps before this script:
#   1. push the new build as /data/plugins/zwrt-datad/zwrt-datad.test
#   2. /etc/init.d/zwrt-datad stop
#   3. start zwrt-datad.test with nohup (same arguments as production)
# then
#   sh /data/u60-guard/datad-trial.sh start    checks, then watches in the
#                                               background (setsid + nohup,
#                                               independent of the SSH session)
#   sh /data/u60-guard/datad-trial.sh status   is it running + last log lines
#   sh /data/u60-guard/datad-trial.sh abort    stop now and restore production
#
# Every 10 s it checks; the first failure aborts the trial:
#   1. screen data stopped: /state's "ts" has not changed for 30 s
#      (a failed or timed-out fetch counts as "not changed")
#   2. test build crashed: `pidof zwrt-datad.test` is empty
#   3. screen trouble: u60-uid logged a give-up / vendor-UI hand-over / an
#      unrequested u60pro-devui exit (logread), or u60pro-devui is gone
#   4. zte-agent netwatch errors went up. netwatch keeps no counter and logs
#      nothing, so by default this reads /state the way netwatch does and
#      counts replies that carry no "wan_status" (netwatch then sees
#      connected=None, i.e. it is blind). DT_NETWATCH_CMD may name a command
#      printing a real counter instead, once zte-agent exposes one.
# Abort: kill the test build (and a start.test.sh wrapper, so nothing
# relaunches it), `/etc/init.d/zwrt-datad start`, reason into the log.
# After the window (1 h) it restores production the same way and logs PASS.
# Promoting the test build to the live slot is NOT done here (separate step).
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
LOGREAD=${DT_LOGREAD:-logread}
KILL=${DT_KILL:-kill}
SLEEP=${DT_SLEEP:-sleep}
DATE=${DT_DATE:-date}
SETSID=${DT_SETSID:-setsid}
NETWATCH_CMD=${DT_NETWATCH_CMD:-}

TEST_NAME=${DT_TEST_NAME:-zwrt-datad.test}
PROD_NAME=${DT_PROD_NAME:-zwrt-datad}
UI_NAME=${DT_UI_NAME:-u60pro-devui}
WRAPPER=${DT_WRAPPER:-start.test.sh}

INTERVAL=${DT_INTERVAL:-10}
STALE=${DT_STALE:-30}
WINDOW=${DT_WINDOW:-3600}

# u60-uid's own wording (src/uid.c logf_, stderr → logread via procd).
UID_BAD='u60-uid.*(giving up|starting the vendor UI|vendor UI on screen|u60pro-devui pid [0-9]+ ended:)'

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

state_ts() {
    printf '%s' "$STATE" | head -c 200 | sed -n 's/^[[:space:]]*{[[:space:]]*"ts":[[:space:]]*\([0-9][0-9]*\).*/\1/p' | head -n 1
}

uid_bad_count() {
    $LOGREAD 2>/dev/null | grep -E "$UID_BAD" | grep -vc '(requested)'
}

wrapper_pids() {
    [ -n "$WRAPPER" ] || return 0
    $PS 2>/dev/null | grep -F "$WRAPPER" | grep -v -e grep -e datad-trial | awk '{ print $1 }'
}

# netwatch errors so far: the injected counter, or our own count of replies
# netwatch could not read.
netwatch_errs() {
    if [ -n "$NETWATCH_CMD" ]; then
        _n=$($NETWATCH_CMD 2>/dev/null | head -n 1 | tr -dc '0-9')
        echo "${_n:-0}"
    else
        echo "$NW_OWN"
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
    $SLEEP 1
    [ -z "$($PIDOF "$TEST_NAME" 2>/dev/null)" ]
}

start_prod() {
    _try=1
    while [ $_try -le 2 ]; do
        "$INITD_DATAD" start >/dev/null 2>&1
        _i=0
        while [ $_i -lt 10 ]; do
            _p=$($PIDOF "$PROD_NAME" 2>/dev/null)
            if [ -n "$_p" ]; then
                log "正式版已恢复（pid $_p）"
                return 0
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
        log "！！测试版杀不掉（pid $($PIDOF "$TEST_NAME" 2>/dev/null)），仍尝试启动正式版"
    fi
    start_prod
    _rc=$?
    rm -f "$PIDFILE"
    return $_rc
}

# ── preflight ───────────────────────────────────────────────────────────────

# Prints one reason per problem; empty output = OK to start.
preflight() {
    if [ ! -f "$INITD_DATAD" ]; then
        echo "$INITD_DATAD 不存在，恢复正式版没有办法"
    elif grep -q '\.test' "$INITD_DATAD"; then
        echo "$INITD_DATAD 里有 .test：它必须只指向正式程序"
    fi
    if grep -v '^[[:space:]]*#' "$RC" 2>/dev/null | grep 'zwrt-datad' | grep -q '\.test'; then
        echo "$RC 里 zwrt-datad 那行指向 .test：它必须只指向正式程序"
    fi
    if [ -n "$($PIDOF "$PROD_NAME" 2>/dev/null)" ]; then
        echo "正式版 $PROD_NAME 还在跑：先 $INITD_DATAD stop（两个 datad 会抢 9460）"
    fi
    if [ -z "$($PIDOF "$TEST_NAME" 2>/dev/null)" ]; then
        echo "测试版 $TEST_NAME 没在跑：先用 nohup 起它"
    fi
    if [ -f "$PIDFILE" ] && [ -d "/proc/$(cat "$PIDFILE" 2>/dev/null)" ]; then
        echo "已经有一个守护在跑（pid $(cat "$PIDFILE")）"
    fi
}

# ── the watch loop ──────────────────────────────────────────────────────────

run() {
    RESTORED=
    echo $$ >"$PIDFILE"
    trap 'restore "中止：守护被停止（abort 或信号）"; exit 1' INT TERM  # not HUP: nohup's ignore must stay

    T0=$(now)
    fetch_state
    LAST_TS=$(state_ts)
    LAST_CHANGE=$T0
    UID_BASE=$(uid_bad_count)
    NW_OWN=0
    NW_BASE=$(netwatch_errs)
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
        if [ -n "$STATE" ] && [ -n "$TS" ] && ! printf '%s' "$STATE" | grep -q '"wan_status":"'; then
            NW_OWN=$((NW_OWN + 1))
        fi
        NW=$(netwatch_errs)
        if [ "$NW" -gt "$NW_BASE" ]; then
            restore "中止：zte-agent netwatch 报错上升（$NW_BASE → $NW；/state 里读不到 wan_status）"
            exit 1
        fi

        if [ $((T - T0)) -ge "$WINDOW" ]; then
            restore "通过：${WINDOW}s 窗口内没有异常，恢复正式版（升级到正式位置另做）"
            exit 0
        fi
    done
}

case "$1" in
    start)
        problems=$(preflight)
        if [ -n "$problems" ]; then
            say "拒绝开始："
            echo "$problems" | sed 's/^/  - /'
            log "拒绝开始：$(echo "$problems" | tr '\n' ';')"
            exit 1
        fi
        $SETSID nohup sh "$0" run >/dev/null 2>&1 </dev/null &
        say "已开始，日志：$LOG"
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
        if [ -n "$p" ] && [ -d "/proc/$p" ]; then
            $KILL "$p" # the watcher restores production itself
            say "已通知守护（pid $p）中止，看日志：$LOG"
        else
            RESTORED=
            restore "中止：手动 abort（守护没在运行）"
        fi
        ;;
    *)
        echo "usage: $0 start|status|abort" >&2
        exit 2
        ;;
esac
