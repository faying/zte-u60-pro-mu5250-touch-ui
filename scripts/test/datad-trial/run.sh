#!/bin/sh
# ─────────────────────────────────────────────────────────────────────────────
# Tests for datad-trial.sh (side-by-side zwrt-datad trial watcher), every
# device command stubbed. Runs in a plain busybox container:
#
#   scripts/test/docker.sh        (runs every suite under scripts/test/)
#
# Time is fake: $T/uptime is /proc/uptime and the stub `sleep` advances it,
# then runs $T/hook, which each case uses to break one thing at a set time.
# /state's ts moves with the fake clock unless $T/frozen exists.
# SPDX-License-Identifier: MIT
# ─────────────────────────────────────────────────────────────────────────────

SCRIPTS=${SCRIPTS:-/scripts}
TRIAL=$SCRIPTS/datad-trial.sh
PASS=0
FAIL=0

ok() { PASS=$((PASS + 1)); echo "  ok   $1"; }
bad() { FAIL=$((FAIL + 1)); echo "  FAIL $1"; }
check() { # check <description> <shell test…>
    _d=$1
    shift
    if eval "$@"; then ok "$_d"; else bad "$_d  [$*]"; fi
}

setup() {
    T=$(mktemp -d)
    mkdir -p "$T/bin" "$T/pids"
    echo 1000 >"$T/uptime"
    echo 4242 >"$T/pids/zwrt-datad.test"
    echo 3131 >"$T/pids/u60pro-devui"
    : >"$T/logread"
    : >"$T/kill.log"
    : >"$T/initd.log"
    : >"$T/setsid.log"
    echo '  PID USER       VSZ STAT COMMAND' >"$T/ps"
    printf '#!/bin/sh\nexit 0\n' >"$T/hook"

    cat >"$T/bin/sleep" <<EOF
#!/bin/sh
echo \$(( \$(cut -d. -f1 $T/uptime) + \${1%%.*} )) >$T/uptime
. $T/hook
EOF
    cat >"$T/bin/pidof" <<EOF
#!/bin/sh
cat $T/pids/"\$1" 2>/dev/null | grep . || exit 1
EOF
    # /state: ts follows the clock unless frozen; wan_status unless no-wan.
    cat >"$T/bin/curl" <<EOF
#!/bin/sh
[ -f $T/curl-fail ] && exit 7
if [ -f $T/frozen ]; then ts=\$(cat $T/frozen-ts); else ts=\$(cut -d. -f1 $T/uptime); echo \$ts >$T/frozen-ts; fi
if [ -f $T/no-wan ]; then echo "{\"ts\":\$ts,\"datad\":{\"name\":\"zwrt-datad\"},\"net\":{\"wan_dns\":\"1.1.1.1\"}}"
else echo "{\"ts\":\$ts,\"datad\":{\"name\":\"zwrt-datad\"},\"net\":{\"wan_status\":\"ipv4_ipv6_connected\"}}"; fi
EOF
    cat >"$T/bin/logread" <<EOF
#!/bin/sh
cat $T/logread
EOF
    cat >"$T/bin/ps" <<EOF
#!/bin/sh
cat $T/ps
EOF
    # kill: TERM kills the test build unless it is stubborn; -9 always does.
    cat >"$T/bin/kill" <<EOF
#!/bin/sh
echo "\$*" >>$T/kill.log
sig=TERM; [ "\$1" = -9 ] && { sig=KILL; shift; }
for p in "\$@"; do
    if [ "\$p" = "\$(cat $T/pids/zwrt-datad.test 2>/dev/null)" ]; then
        [ -f $T/stubborn ] && [ \$sig = TERM ] || rm -f $T/pids/zwrt-datad.test
    fi
done
EOF
    cat >"$T/bin/initd" <<EOF
#!/bin/sh
echo "\$*" >>$T/initd.log
[ "\$1" = start ] && [ ! -f $T/prod-broken ] && echo 5555 >$T/pids/zwrt-datad
exit 0
EOF
    cat >"$T/bin/date" <<'EOF'
#!/bin/sh
echo "2026-09-25 12:00:00"
EOF
    cat >"$T/bin/setsid" <<EOF
#!/bin/sh
echo "\$*" >>$T/setsid.log
EOF
    chmod +x "$T"/bin/*
    # A real-looking init script and rc.local for the preflight.
    cat >"$T/zwrt-datad.init" <<'EOF'
#!/bin/sh /etc/rc.common
DIR=/data/plugins/zwrt-datad
BIN=$DIR/zwrt-datad
EOF
    cat >"$T/rc.local" <<'EOF'
# /etc/init.d/zwrt-datad.test start   (old note, commented out)
/etc/init.d/zwrt-datad start
exit 0
EOF

    export DT_INITD=$T/bin/initd DT_RC=$T/rc.local DT_LOG=$T/data/trial.log
    export DT_PIDFILE=$T/trial.pid DT_UPTIME=$T/uptime
    export DT_CURL=$T/bin/curl DT_PIDOF=$T/bin/pidof DT_PS=$T/bin/ps
    export DT_LOGREAD=$T/bin/logread DT_KILL=$T/bin/kill DT_SLEEP=$T/bin/sleep
    export DT_DATE=$T/bin/date DT_SETSID=$T/bin/setsid
    export DT_WINDOW=3600 DT_INTERVAL=10 DT_STALE=30
    unset DT_NETWATCH_CMD
}

# hook_at <uptime> <shell command>: do it once the fake clock reaches <uptime>
hook_at() {
    cat >"$T/hook" <<EOF
[ \$(cut -d. -f1 $T/uptime) -ge $1 ] && { $2; }
:
EOF
}

trial() { # run the watch loop in the foreground; RC gets its exit status
    timeout 20 sh "$TRIAL" run >"$T/out" 2>&1
    RC=$?
}

starts() { grep -c '^start' "$T/initd.log"; }
logged() { grep -q "$1" "$T/data/trial.log"; }
up() { cut -d. -f1 "$T/uptime"; }

# every abort must: log the reason, kill the test build, start production
# exactly once, report it back, and stop the loop.
aborted() { # aborted <case> <reason text>
    check "$1: exit 1" '[ "$RC" = 1 ]'
    check "$1: reason logged" "logged '中止：$2'"
    check "$1: test build killed" "grep -q 4242 '$T/kill.log' && [ ! -f '$T/pids/zwrt-datad.test' ]"
    check "$1: production started once" '[ "$(starts)" = 1 ]'
    check "$1: production confirmed" "logged '正式版已恢复（pid 5555）'"
    check "$1: one outcome only" '[ "$(grep -c "中止：\|通过：" "$T/data/trial.log")" = 1 ]'
    check "$1: pidfile removed" "[ ! -f '$T/trial.pid' ]"
}

echo "== datad-trial =="

# ── 1. ts stops ─────────────────────────────────────────────────────────────
setup
hook_at 1040 "touch $T/frozen"
trial
aborted "ts stops" "屏幕数据停了"
check "ts stops: not before 30 s of stillness" '[ "$(up)" -ge 1070 ] && [ "$(up)" -le 1085 ]'
rm -rf "$T"

# failed fetches count as "ts not changing", never as netwatch errors
setup
hook_at 1030 "touch $T/curl-fail"
trial
aborted "fetch fails" "屏幕数据停了"
check "fetch fails: not blamed on netwatch" "! logged netwatch"
rm -rf "$T"

# ── 2. test build crashes ───────────────────────────────────────────────────
setup
hook_at 1030 "rm -f $T/pids/zwrt-datad.test"
trial
check "crash: exit 1" '[ "$RC" = 1 ]'
check "crash: reason logged" "logged '中止：测试版崩溃'"
check "crash: production started once" '[ "$(starts)" = 1 ]'
check "crash: production confirmed" "logged '正式版已恢复'"
check "crash: aborted on the first check after" '[ "$(up)" -le 1040 ]'
rm -rf "$T"

# ── 3a. u60-uid gives the screen away ───────────────────────────────────────
setup
# older lines are the baseline, not a reason
echo "Thu Sep 25 09:00:00 2026 daemon.err u60-uid[77]: u60-uid: vendor UI on screen: corner long-press listener on" >"$T/logread"
hook_at 1030 "echo 'Thu Sep 25 12:00:30 2026 daemon.err u60-uid[77]: u60-uid: giving up: 2 launches did not stay up; vendor UI on screen. Corner long-press' >>$T/logread"
trial
aborted "uid gives up" "界面异常（u60-uid：.*giving up"
rm -rf "$T"

setup
hook_at 1030 "echo 'daemon.err u60-uid[77]: u60-uid: u60pro-devui pid 3131 ended: killed by signal 11' >>$T/logread"
trial
aborted "devui crash in uid log" "界面异常（u60-uid：.*ended: killed by signal 11"
rm -rf "$T"

# ── 3b. u60pro-devui disappears ─────────────────────────────────────────────
setup
hook_at 1030 "rm -f $T/pids/u60pro-devui"
trial
aborted "devui gone" "界面异常（u60pro-devui 进程消失）"
rm -rf "$T"

# ── 4. netwatch errors go up ────────────────────────────────────────────────
setup
hook_at 1030 "touch $T/no-wan"
trial
aborted "netwatch (own count)" "zte-agent netwatch 报错上升（0 → 1"
rm -rf "$T"

setup
echo 7 >"$T/nwcount"
printf '#!/bin/sh\ncat %s/nwcount\n' "$T" >"$T/bin/nw"
chmod +x "$T/bin/nw"
export DT_NETWATCH_CMD=$T/bin/nw
hook_at 1050 "echo 8 >$T/nwcount"
trial
aborted "netwatch (injected counter)" "zte-agent netwatch 报错上升（7 → 8"
rm -rf "$T"

# ── window ends: pass and restore ───────────────────────────────────────────
setup
export DT_WINDOW=120
# a requested devui stop and an old baseline line are not trouble
echo "daemon.err u60-uid[77]: u60-uid: giving up: old" >"$T/logread"
hook_at 1030 "grep -q requested $T/logread || echo 'daemon.err u60-uid[77]: u60-uid: u60pro-devui pid 3131 ended: exit 0 (requested)' >>$T/logread"
trial
check "window: exit 0" '[ "$RC" = 0 ]'
check "window: PASS logged" "logged '通过：120s'"
check "window: no abort" "! logged '中止'"
check "window: test build killed" "[ ! -f '$T/pids/zwrt-datad.test' ]"
check "window: production started once" '[ "$(starts)" = 1 ]'
check "window: production confirmed" "logged '正式版已恢复'"
check "window: ran the whole window" '[ "$(up)" -ge 1120 ] && [ "$(up)" -le 1135 ]'
rm -rf "$T"

# ── restore details ─────────────────────────────────────────────────────────
setup
touch "$T/stubborn"
echo '  900 root  1200 S    /bin/sh /data/plugins/zwrt-datad/start.test.sh' >>"$T/ps"
echo '  901 root  1200 S    sh /data/u60-guard/datad-trial.sh run start.test.sh' >>"$T/ps"
hook_at 1030 "rm -f $T/pids/u60pro-devui"
trial
check "stubborn: TERM then -9" "grep -qx '4242' '$T/kill.log' && grep -qx -- '-9 4242' '$T/kill.log'"
check "stubborn: wrapper killed first" '[ "$(head -n 1 "$T/kill.log")" = 900 ]'
check "stubborn: watcher itself spared" "! grep -q 901 '$T/kill.log'"
check "stubborn: test build gone" "logged '测试版已停止'"
rm -rf "$T"

setup
touch "$T/prod-broken"
hook_at 1030 "rm -f $T/pids/zwrt-datad.test"
trial
check "prod fails: retried once" '[ "$(starts)" = 2 ]'
check "prod fails: loud log line" "logged '正式版没起来'"
rm -rf "$T"

# TERM to the watcher (what `abort` sends) restores production
setup
printf '#!/bin/sh\n/bin/sleep 0.05\n' >"$T/bin/sleep"
sh "$TRIAL" run >"$T/out" 2>&1 &
W=$!
/bin/sleep 1
kill "$W"
wait "$W"
RC=$?
aborted "abort signal" "守护被停止"
rm -rf "$T"

# ── preflight ───────────────────────────────────────────────────────────────
refused() { # refused <case> <text in the message>
    sh "$TRIAL" start >"$T/out" 2>&1
    RC=$?
    check "$1: refused" '[ "$RC" = 1 ] && grep -q "拒绝开始" "$T/out"'
    check "$1: says why" "grep -q '$2' '$T/out'"
    check "$1: nothing launched or restarted" '[ ! -s "$T/setsid.log" ] && [ ! -s "$T/initd.log" ] && [ ! -s "$T/kill.log" ]'
}

setup
echo 'BIN=$DIR/zwrt-datad.test' >>"$T/zwrt-datad.init"
export DT_INITD=$T/zwrt-datad.init
refused "init.d points at .test" "里有 .test"
rm -rf "$T"

setup
echo '/data/plugins/zwrt-datad/zwrt-datad.test &' >>"$T/rc.local"
refused "rc.local points at .test" "rc.local 里 zwrt-datad"
rm -rf "$T"

setup
echo 5555 >"$T/pids/zwrt-datad"
refused "production still running" "正式版 zwrt-datad 还在跑"
rm -rf "$T"

setup
rm -f "$T/pids/zwrt-datad.test"
refused "test build not running" "测试版 zwrt-datad.test 没在跑"
rm -rf "$T"

setup
echo $$ >"$T/trial.pid"
refused "second watcher" "已经有一个守护在跑"
rm -rf "$T"

setup
sh "$TRIAL" start >"$T/out" 2>&1
RC=$?
check "preflight ok: starts" '[ "$RC" = 0 ] && grep -q "已开始" "$T/out"'
check "preflight ok: detached with setsid nohup" "grep -q '^nohup sh .*datad-trial.sh run' '$T/setsid.log'"
check "preflight ok: commented .test line ignored" '[ ! -s "$T/initd.log" ]'
rm -rf "$T"

echo "datad-trial: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
