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
echo \$(( \$(cut -d. -f1 $T/uptime | tr -dc 0-9) + \${1%%.*} )) >$T/uptime
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
if [ -f $T/frozen ]; then ts=\$(cat $T/frozen-ts); else ts=\$(cut -d. -f1 $T/uptime | tr -dc 0-9); echo \$ts >$T/frozen-ts; fi
if [ -f $T/state-body ]; then sed "s/@TS@/\$ts/" $T/state-body
elif [ -f $T/no-wan ]; then echo "{\"ts\":\$ts,\"datad\":{\"name\":\"zwrt-datad\"},\"net\":{\"wan_dns\":\"1.1.1.1\"}}"
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
        [ -f $T/unkillable ] && continue
        [ -f $T/stubborn ] && [ \$sig = TERM ] || rm -f $T/pids/zwrt-datad.test
    fi
done
EOF
    cat >"$T/bin/initd" <<EOF
#!/bin/sh
echo "\$*" >>$T/initd.log
# a started production datad answers /state (clears curl-fail)
[ "\$1" = start ] && [ ! -f $T/prod-broken ] && echo 5555 >$T/pids/zwrt-datad && rm -f $T/curl-fail
[ "\$1" = stop ] && [ ! -f $T/prod-stuck ] && rm -f $T/pids/zwrt-datad
exit 0
EOF
    cat >"$T/bin/date" <<'EOF'
#!/bin/sh
echo "2026-09-25 12:00:00"
EOF
    # setsid: logs what it would run; a launched test build comes up
    # (pid 4242) unless $T/test-broken exists.
    cat >"$T/bin/setsid" <<EOF
#!/bin/sh
echo "\$*" >>$T/setsid.log
case "\$*" in *zwrt-datad.test\ *) [ -f $T/test-broken ] || echo 4242 >$T/pids/zwrt-datad.test ;; esac
# the watcher: a real process whose cmdline names datad-trial.sh writes the
# pidfile, unless $T/watcher-broken exists
case "\$*" in *datad-trial.sh\ run*) [ -f $T/watcher-broken ] || { sh $T/fake/datad-trial.sh </dev/null >/dev/null 2>&1 & echo \$! >$T/trial.pid; } ;; esac
exit 0
EOF
    chmod +x "$T"/bin/*
    mkdir -p "$T/fake"
    printf '/bin/sleep 30\n' >"$T/fake/datad-trial.sh"
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
    export DT_NETWATCH_FILE=$T/netwatch.errors
    export DT_TEST_BIN=$T/zwrt-datad.test DT_TEST_LOG=$T/data/test.log
    printf '#!/bin/sh\n' >"$T/zwrt-datad.test"
    chmod +x "$T/zwrt-datad.test"
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
    check "$1: production confirmed" "logged '正式版已恢复（pid 5555，'"
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
check "fetch fails: not blamed on netwatch" "! logged '中止：zte-agent netwatch'"
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
hook_at 1030 "echo '2026-09-25T12:00:30 u60pro-devui pid 3131 ended: killed by signal 11' >>$T/logread"
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
check "netwatch (own count): signal logged once" '[ "$(grep -c "netwatch 信号：自己读 /state" "$T/data/trial.log")" = 1 ]'
rm -rf "$T"

# zte-agent's counter file is the default signal
setup
echo 7 >"$T/netwatch.errors"
hook_at 1050 "echo 9 >$T/netwatch.errors"
trial
aborted "netwatch file rises" "zte-agent netwatch 报错上升（7 → 9；来源：zte-agent 计数文件"
check "netwatch file rises: signal logged" "logged 'netwatch 信号：zte-agent 计数文件 .*（基线 7）'"
rm -rf "$T"

# the file wins over our own count: no wan_status alone is not an abort
setup
echo 0 >"$T/netwatch.errors"
export DT_WINDOW=60
hook_at 1020 "touch $T/no-wan"
trial
check "netwatch file preferred: own count ignored" '[ "$RC" = 0 ] && ! logged "中止"'
rm -rf "$T"

# a smaller count = agent restarted: new baseline, no abort ...
setup
echo 5 >"$T/netwatch.errors"
export DT_WINDOW=120
hook_at 1030 "echo 2 >$T/netwatch.errors"
trial
check "netwatch file drops: no abort" '[ "$RC" = 0 ] && logged "通过：" && ! logged "中止"'
check "netwatch file drops: rebaseline logged" "logged 'netwatch 计数变小（5 → 2），当作 zte-agent 重启'"
rm -rf "$T"

# ... and rises are measured from the new baseline
setup
echo 5 >"$T/netwatch.errors"
hook_at 1030 "echo 2 >$T/netwatch.errors; [ \$(cut -d. -f1 $T/uptime) -ge 1060 ] && echo 3 >$T/netwatch.errors"
trial
aborted "netwatch file drops then rises" "zte-agent netwatch 报错上升（2 → 3"
rm -rf "$T"

# garbage in the file: fall back to our own count, say so
setup
echo 'oops' >"$T/netwatch.errors"
hook_at 1030 "touch $T/no-wan"
trial
aborted "netwatch file garbage" "zte-agent netwatch 报错上升（0 → 1；来源：自己读 /state"
check "netwatch file garbage: fallback logged" "logged 'netwatch 信号：自己读 /state'"
rm -rf "$T"

# the file appears mid-trial: switch signal, baseline from it, no abort
setup
export DT_WINDOW=60
hook_at 1030 "[ -f $T/netwatch.errors ] || echo 4 >$T/netwatch.errors"
trial
check "netwatch file appears: no abort" '[ "$RC" = 0 ] && ! logged "中止"'
check "netwatch file appears: switch logged" "logged 'netwatch 信号改为：zte-agent 计数文件 .*（基线 4）'"
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

# the watcher dies of its own error (bad arithmetic): EXIT trap restores
setup
hook_at 1040 "echo 1040x >$T/uptime"
trial
check "script error: exit non-0" '[ "$RC" != 0 ]'
check "script error: reason logged" "logged '中止：守护意外退出'"
check "script error: test build killed" "[ ! -f '$T/pids/zwrt-datad.test' ]"
check "script error: production started once, confirmed" '[ "$(starts)" = 1 ] && logged "正式版已恢复（pid 5555，"'
check "script error: one outcome" '[ "$(grep -c "中止：\|通过：" "$T/data/trial.log")" = 1 ]'
rm -rf "$T"

# the test build cannot be killed: never start production next to it
setup
touch "$T/unkillable"
hook_at 1030 "rm -f $T/pids/u60pro-devui"
trial
check "unkillable: exit non-0" '[ "$RC" != 0 ]'
check "unkillable: -9 tried" "grep -q -- '-9 4242' '$T/kill.log'"
check "unkillable: production NOT started" '[ "$(starts)" = 0 ]'
check "unkillable: loud log line" "logged '！！测试版杀不掉.*没有启动正式版'"
check "unkillable: pidfile removed" "[ ! -f '$T/trial.pid' ]"
rm -rf "$T"

# leading zeros in the counter file ("08" is bad octal in ash)
setup
echo 08 >"$T/netwatch.errors"
hook_at 1030 "echo 09 >$T/netwatch.errors"
trial
aborted "leading zeros" "zte-agent netwatch 报错上升（8 → 9"
rm -rf "$T"

# "ts" not first in /state, spaces around ":"; wan_status with spaces
setup
export DT_WINDOW=100
echo '{"datad":{"name":"zwrt-datad"},"ts" : @TS@,"net":{"wan_status" : "connected"}}' >"$T/state-body"
trial
check "ts not first: passes" '[ "$RC" = 0 ] && logged "通过："'
check "ts not first: ts read" "logged 'ts=1000'"
check "wan_status spaced: no netwatch abort" "! logged '中止：'"
rm -rf "$T"

# wan_status null (no SIM / dialling) is not a netwatch error
setup
export DT_WINDOW=100
echo '{"ts":@TS@,"net":{"wan_status":null}}' >"$T/state-body"
trial
check "wan_status null: passes" '[ "$RC" = 0 ] && logged "通过：" && ! logged "中止："'
rm -rf "$T"
# but a reply without wan_status still counts
setup
echo '{"ts":@TS@,"net":{"wan_dns":"1.1.1.1"}}' >"$T/state-body"
trial
aborted "wan_status missing" "zte-agent netwatch 报错上升"
rm -rf "$T"
export DT_WINDOW=3600

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

# ── launch ──────────────────────────────────────────────────────────────────
# production running, test build pushed but not running: the normal start.
launch_setup() {
    setup
    echo 5555 >"$T/pids/zwrt-datad"
    rm -f "$T/pids/zwrt-datad.test"
}
launch() { timeout 20 sh "$TRIAL" launch "$@" >"$T/out" 2>&1; RC=$?; }

launch_setup
launch ZWRT_DATAD_UBUS=cli
EXPECT="env $(sh "$TRIAL" print-launch ZWRT_DATAD_UBUS=cli | sed 's/^env //')"
check "launch: exit 0" '[ "$RC" = 0 ]'
check "launch: production stopped, not restarted" '[ "$(cat "$T/initd.log")" = stop ]'
check "launch: test build started as print-launch says" '[ "$(sed -n 1p "$T/setsid.log")" = "$EXPECT" ]'
check "launch: extra env passed" "sed -n 1p '$T/setsid.log' | grep -q 'ZWRT_DATAD_OTA_DISABLE_AUTO=1 ZWRT_DATAD_UBUS=cli nohup $T/zwrt-datad.test -i 1000'"
check "launch: no supervise.sh" "! grep -q supervise '$T/setsid.log'"
check "launch: then the watcher, detached" "sed -n 2p '$T/setsid.log' | grep -q '^nohup sh .*datad-trial.sh run'"
check "launch: test output header" "grep -q '^=== .*zwrt-datad.test -i 1000' '$T/data/test.log'"
check "launch: says so" 'grep -q "测试版已起来" "$T/out" && grep -q "已开始守护" "$T/out"'
check "launch: logged" "logged 'launch：测试版已起来（pid 4242'"
rm -rf "$T"

# the test build never comes up: production back at once, no watcher
launch_setup
touch "$T/test-broken"
launch
check "launch broken: exit 1" '[ "$RC" = 1 ]'
check "launch broken: stop then start" '[ "$(tr "\n" " " <"$T/initd.log")" = "stop start " ]'
check "launch broken: production confirmed" "logged '正式版已恢复（pid 5555，'"
check "launch broken: reason logged" "logged '启动失败：测试版 20s 内没起来（进程没出现）'"
check "launch broken: no watcher" "! grep -q 'datad-trial.sh run' '$T/setsid.log'"
check "launch broken: tells the user" 'grep -q "已恢复正式版" "$T/out"'
rm -rf "$T"

# up but /state never answers: same
launch_setup
touch "$T/curl-fail"
launch
check "launch no /state: exit 1, production back" '[ "$RC" = 1 ] && logged "正式版已恢复" && logged "（/state 没有 ts）"'
check "launch no /state: test build killed" "grep -q 4242 '$T/kill.log' && [ ! -f '$T/pids/zwrt-datad.test' ]"
check "launch no /state: no watcher" "! grep -q 'datad-trial.sh run' '$T/setsid.log'"
rm -rf "$T"

# production will not stop: never start the test build
launch_setup
touch "$T/prod-stuck"
launch
check "launch prod stuck: exit 1, test build not started" '[ "$RC" = 1 ] && [ ! -s "$T/setsid.log" ]'
check "launch prod stuck: logged" "logged '启动失败：正式版 15s 内没有退出'"
rm -rf "$T"

# the watcher never comes up: restore and fail
launch_setup
touch "$T/watcher-broken"
launch
check "launch no watcher: exit 1" '[ "$RC" = 1 ]'
check "launch no watcher: stop then start" '[ "$(tr "\n" " " <"$T/initd.log")" = "stop start " ]'
check "launch no watcher: test build killed" "[ ! -f '$T/pids/zwrt-datad.test' ]"
check "launch no watcher: reason logged" "logged '启动失败：守护 5s 内没起来'"
check "launch no watcher: absolute script path" "grep -q '^nohup sh /.*/datad-trial.sh run' '$T/setsid.log'"
rm -rf "$T"

# abort: pidfile pid reused by another process → not killed, restore directly
setup
/bin/sleep 30 &
O=$!
echo $O >"$T/trial.pid"
sh "$TRIAL" abort >"$T/out" 2>&1
RC=$?
check "abort pid reuse: other process spared" "! grep -qw $O '$T/kill.log' && [ -d /proc/$O ]"
check "abort pid reuse: logged" "logged 'pid 复用'"
check "abort pid reuse: restored directly" '[ "$RC" = 0 ] && [ "$(starts)" = 1 ] && logged "中止：手动 abort"'
kill $O 2>/dev/null
rm -rf "$T"

# abort: the real watcher gets the signal
setup
sh "$T/fake/datad-trial.sh" &
O=$!
echo $O >"$T/trial.pid"
sh "$TRIAL" abort >"$T/out" 2>&1
check "abort watcher: signalled" "grep -qx $O '$T/kill.log' && [ ! -s '$T/initd.log' ]"
kill $O 2>/dev/null
rm -rf "$T"

launch_refused() { # launch_refused <case> <text in the message> [args…]
    _c=$1 _w=$2
    shift 2
    launch "$@"
    check "$_c: refused" '[ "$RC" = 1 ] && grep -q "拒绝启动" "$T/out"'
    check "$_c: says why" "grep -q '$_w' '$T/out'"
    check "$_c: production untouched" '[ ! -s "$T/initd.log" ] && [ ! -s "$T/setsid.log" ] && [ ! -s "$T/kill.log" ] && [ -f "$T/pids/zwrt-datad" ]'
}

launch_setup
echo 'BIN=$DIR/zwrt-datad.test' >>"$T/zwrt-datad.init"
printf '#!/bin/sh\necho "$*" >>%s/initd.log\n' "$T" >>"$T/zwrt-datad.init"
cp "$T/zwrt-datad.init" "$T/bin/initd-test"
export DT_INITD=$T/bin/initd-test
launch_refused "launch: init.d points at .test" "里有 .test"
rm -rf "$T"

launch_setup
echo '/data/plugins/zwrt-datad/zwrt-datad.test &' >>"$T/rc.local"
launch_refused "launch: rc.local points at .test" "rc.local 里 zwrt-datad"
rm -rf "$T"

launch_setup
rm -f "$T/zwrt-datad.test"
launch_refused "launch: no test build" "不存在或不可执行"
rm -rf "$T"

launch_setup
echo 4242 >"$T/pids/zwrt-datad.test"
launch_refused "launch: test build already running" "已经在跑"
rm -rf "$T"

launch_setup
echo $$ >"$T/trial.pid"
launch_refused "launch: watcher already running" "已经有一个守护在跑"
rm -rf "$T"

launch_setup
launch_refused "launch: bad extra arg" "不是 VAR=value" "ZWRT_DATAD_UBUS=cli" "rm -rf"
rm -rf "$T"

# ── detach: no setsid on the device (BusyBox 1.36.1) ───────────────────────
# mode ssd: only start-stop-daemon; mode none: neither. A PATH nohup mock
# stands in for the real one so nothing is really started; the mock
# start-stop-daemon runs the real /bin/sh -c line, so its redirection is real.
detach_mode() { # detach_mode ssd|none
    export DT_SETSID=$T/bin/no-setsid
    : >"$T/ssd.log"
    : >"$T/nohup.log"
    cat >"$T/bin/nohup" <<EOF
#!/bin/sh
echo "\$*" >>$T/nohup.log
case "\$*" in *zwrt-datad.test\ *)
    [ -f $T/test-broken ] || echo 4242 >$T/pids/zwrt-datad.test
    echo "out: \$* UBUS=\$ZWRT_DATAD_UBUS OTA=\$ZWRT_DATAD_OTA_DISABLE_AUTO"
    echo "err: stderr" >&2 ;;
esac
# the watcher: a real process whose cmdline names datad-trial.sh writes the
# pidfile, unless $T/watcher-broken exists
case "\$*" in *datad-trial.sh\ run*) [ -f $T/watcher-broken ] || { sh $T/fake/datad-trial.sh </dev/null >/dev/null 2>&1 & echo \$! >$T/trial.pid; } ;; esac
exit 0
EOF
    if [ "$1" = ssd ]; then
        cat >"$T/bin/ssd" <<EOF
#!/bin/sh
echo "\$*" >>$T/ssd.log
while [ \$# -gt 0 ] && [ "\$1" != -- ]; do shift; done
shift
exec /bin/sh "\$@"
EOF
        export DT_SSD=$T/bin/ssd
    else
        export DT_SSD=$T/bin/no-ssd
    fi
    chmod +x "$T"/bin/*
    export PATH="$T/bin:$PATH"
}

for _m in ssd none; do
    _p=$PATH
    setup
    detach_mode $_m
    sh "$TRIAL" start >"$T/out" 2>&1
    RC=$?
    check "$_m start: ok, watcher via nohup" '[ "$RC" = 0 ] && grep -q "^sh .*datad-trial.sh run" "$T/nohup.log"'
    check "$_m start: setsid not used" '[ ! -s "$T/setsid.log" ]'
    if [ $_m = ssd ]; then
        check "ssd start: -S -b, never-existing pidfile, -x /bin/sh" "grep -q '^-S -b -p $T/trial.pid.none -x /bin/sh -- -c ' '$T/ssd.log'"
    else
        check "none start: fallback logged" "logged '没有 setsid 也没有 start-stop-daemon'"
    fi
    rm -rf "$T"

    launch_setup
    detach_mode $_m
    launch ZWRT_DATAD_UBUS=cli
    EXPECT=$(sh "$TRIAL" print-launch ZWRT_DATAD_UBUS=cli | sed 's/^env //; s/ nohup .*//')
    check "$_m launch: exit 0" '[ "$RC" = 0 ]'
    check "$_m launch: test build args" "sed -n 1p '$T/nohup.log' | grep -qx '$T/zwrt-datad.test -i 1000 --lan-bind 0.0.0.0 --lan-port 9461 --auth-token-file /data/plugins/zwrt-datad/auth.token'"
    check "$_m launch: env reached the test build" "grep -q 'UBUS=cli OTA=1' '$T/data/test.log'"
    check "$_m launch: stdout and stderr in test log" "grep -q '^out: ' '$T/data/test.log' && grep -q '^err: stderr' '$T/data/test.log'"
    check "$_m launch: header kept" "sed -n 1p '$T/data/test.log' | grep -q '^=== '"
    check "$_m launch: then the watcher" "sed -n 2p '$T/nohup.log' | grep -q '^sh .*datad-trial.sh run'"
    if [ $_m = ssd ]; then
        check "ssd launch: command after the log file, as words" "sed -n 1p '$T/ssd.log' | grep -qF -- '$T/data/test.log env $EXPECT nohup $T/zwrt-datad.test -i 1000'"
    fi
    rm -rf "$T"
    PATH=$_p
done
unset DT_SSD

# ── drift: launch must run what the init script's start_service runs ────────
INIT=$SCRIPTS/zwrt-datad.init
(
    unset DT_TEST_BIN
    # the init script's own variables (DIR, BIN, TOKEN_FILE, LAN_*)
    eval "$(grep -E '^(DIR|BIN|TOKEN_FILE|LAN_BIND|LAN_PORT)=' "$INIT")"
    # procd_set_param command …: join continuation lines, keep what follows "$BIN"
    CMD=$(sed -n '/procd_set_param command/,/[^\\]$/p' "$INIT" | sed 's/\\$//' | tr '\n' ' ')
    case "$CMD" in *'"$BIN"'*) ;; *) echo "  FAIL drift: no \"\$BIN\" in the init command"; exit 1 ;; esac
    eval "set -- ${CMD#*\"\$BIN\"}"
    WANT_ARGS=$*
    WANT_ENV=$(sed -n 's/^[[:space:]]*procd_set_param env[[:space:]]*//p' "$INIT" | sed 's/[[:space:]]*#.*//' | tr -s ' \t' '  ')
    WANT="env $WANT_ENV nohup $BIN.test $WANT_ARGS"
    GOT=$(sh "$TRIAL" print-launch)
    if [ -n "$WANT_ARGS" ] && [ -n "$WANT_ENV" ] && [ "$GOT" = "$WANT" ]; then
        echo "  ok   drift: launch = init start_service (args, env, binary)"
    else
        echo "  FAIL drift: launch differs from $INIT"
        echo "       init:   $WANT"
        echo "       launch: $GOT"
        exit 1
    fi
) && PASS=$((PASS + 1)) || FAIL=$((FAIL + 1))

echo "datad-trial: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
