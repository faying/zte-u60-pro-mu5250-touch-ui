#!/bin/sh
# ─────────────────────────────────────────────────────────────────────────────
# Tests for u60-ship.sh (ship transactions: stage, trial, promote, check,
# manifest, rollback) and its hand-off to u60-recover.sh after a power cut.
# Every device command is stubbed and every device path lives under $T:
# after each case the container's real /data, /tmp/u60-ship and /etc/init.d
# must still be untouched. Runs in a plain busybox container:
#
#   scripts/test/docker.sh        (runs every suite under scripts/test/)
#
# Time is fake: $T/uptime is /proc/uptime and the stub `sleep` advances it,
# then runs $T/hook (break one thing at a set time). A "power cut" is
# U60S_CRASH_AT=<point>: the executor kill -9s itself there (no traps run),
# then u60-recover.sh runs with another boot_id, as rc.local would.
# SPDX-License-Identifier: MIT
# ─────────────────────────────────────────────────────────────────────────────

SCRIPTS=${SCRIPTS:-/scripts}
SHIP=$SCRIPTS/u60-ship.sh
RECOVER=$SCRIPTS/u60-recover.sh
PASS=0
FAIL=0

ok() { PASS=$((PASS + 1)); echo "  ok   $1"; }
bad() { FAIL=$((FAIL + 1)); echo "  FAIL $1"; }
check() { # check <description> <shell test…>
    _d=$1
    shift
    if eval "$@"; then ok "$_d"; else bad "$_d  [$*]"; fi
}

# no timeout(1) on the device busybox: a watchdog with the real sleep
_real_sleep=$(command -v sleep)
if ! command -v timeout >/dev/null 2>&1; then
    timeout() {
        _to=$1
        shift
        "$@" &
        _tp=$!
        ("$_real_sleep" "$_to" && kill -9 "$_tp") </dev/null >/dev/null 2>&1 &
        _tw=$!
        wait "$_tp"
        _trc=$?
        kill "$_tw" 2>/dev/null
        return $_trc
    }
fi

md5() { md5sum "$1" 2>/dev/null | cut -d' ' -f1; }

# state of the real container paths before any case (leak check)
leak_snapshot() { ls -la /data /tmp/u60-ship /etc/init.d /tmp/datad-trial.pid 2>&1 | md5sum; }
LEAK0=$(leak_snapshot)

TXN=20260930-120000-datad
LIVE=
setup() {
    T=$(mktemp -d)
    R=$T/root
    D=$R/data/plugins/zwrt-datad
    LIVE=$D/zwrt-datad
    mkdir -p "$T/bin" "$T/pids" "$T/tmp" "$D" "$R/data/zwrt-datad" "$R/data/u60-ship/stage" "$R/etc/init.d"
    echo 1000 >"$T/uptime"
    echo aaaa-1111 >"$T/boot_id"
    echo 5555 >"$T/pids/zwrt-datad"
    echo 3131 >"$T/pids/u60pro-devui"
    : >"$T/logread"
    : >"$T/kill.log"
    : >"$T/initd.log"
    : >"$T/setsid.log"
    echo '  PID USER       VSZ STAT COMMAND' >"$T/ps"
    printf ':\n' >"$T/hook"
    printf 'OLD-DATAD\n' >"$LIVE"
    chmod 755 "$LIVE"
    OLD=$(md5 "$LIVE")
    printf 'cool-old\n' >"$R/data/zwrt-datad/cooling.conf"
    COOL_OLD=$(md5 "$R/data/zwrt-datad/cooling.conf")
    # hand-made neighbours that ship must never touch
    for f in "$D/zwrt-datad.prev-cache-20260923" "$D/zwrt-datad.orig-backup" "$R/data/zte-agent" \
        "$R/data/zte-agent.env" "$R/data/zte-agent.prev-esim-20260926" "$R/data/admin.prev-reset-20260926.tgz"; do
        echo "keep $f" >"$f"
    done

    cat >"$T/bin/sleep" <<EOF
#!/bin/sh
echo \$(( \$(cut -d. -f1 $T/uptime | tr -dc 0-9) + \${1%%.*} )) >$T/uptime
# a running zte-agent writes its scenario heartbeat (uptime seconds)
if [ ! -f $T/hb-frozen ] && { [ -s $T/pids/zte-agent ] || [ -s $T/pids/zte-agent.test ]; }; then cut -d. -f1 $T/uptime >$T/scenario.heartbeat; fi
. $T/hook
EOF
    cat >"$T/bin/pidof" <<EOF
#!/bin/sh
cat $T/pids/"\$1" 2>/dev/null | grep . || exit 1
EOF
    cat >"$T/bin/curl" <<EOF
#!/bin/sh
case "\$*" in *agent.test:9090*) exec $T/bin/agent-curl "\$@" ;; esac
[ -f $T/curl-fail ] && exit 7
if [ -f $T/frozen ]; then ts=\$(cat $T/frozen-ts); else ts=\$(cut -d. -f1 $T/uptime | tr -dc 0-9); echo \$ts >$T/frozen-ts; fi
echo "{\"epoch\":\"e\",\"seq\":3,\"blocks\":{\"live\":{\"revision\":1,\"observed_at\":\$ts,\"stale\":false,\"data\":{}},\"signal\":{\"revision\":1,\"observed_at\":\$ts,\"stale\":false,\"data\":{\"wan_status\":\"ipv4_ipv6_connected\"}}}}"
EOF
    cat >"$T/bin/logread" <<EOF
#!/bin/sh
cat $T/logread
EOF
    cat >"$T/bin/ps" <<EOF
#!/bin/sh
cat $T/ps
EOF
    cat >"$T/bin/kill" <<EOF
#!/bin/sh
echo "\$*" >>$T/kill.log
[ "\$1" = -9 ] && shift
for p in "\$@"; do
    [ "\$p" = "\$(cat $T/pids/zwrt-datad.test 2>/dev/null)" ] && [ ! -f $T/unkillable ] && rm -f $T/pids/zwrt-datad.test
    [ "\$p" = "\$(cat $T/pids/zte-agent.test 2>/dev/null)" ] && [ ! -f $T/unkillable ] && rm -f $T/pids/zte-agent.test
done
exit 0
EOF
    # zte-agent on :9090: flags in $T break one thing each
    cat >"$T/bin/agent-curl" <<EOF
#!/bin/sh
out=/dev/null; tok=; data=; url=
while [ \$# -gt 0 ]; do
    case "\$1" in
        -o) out=\$2; shift ;;
        -H) case "\$2" in "Authorization: Bearer "*) tok=\${2#Authorization: Bearer } ;; esac; shift ;;
        --data) data=\${2#@}; shift ;;
        -w | -m | -X | --noproxy) shift ;;
        http*) url=\$1 ;;
    esac
    shift
done
if [ -f $T/ag-down ] || { [ ! -s $T/pids/zte-agent ] && [ ! -s $T/pids/zte-agent.test ]; }; then printf 000; exit 7; fi
code=401; body='{"error":"unauthorized"}'
case "\$url" in
    */api/scenario)
        if [ -n "\$tok" ]; then [ "\$tok" = good-token ] && [ ! -f $T/ag-token-bad ] && { code=200; body='{"enabled":true}'; }
        elif [ -f $T/ag-open ]; then code=200; body='{"enabled":true}'; fi ;;
    */api/auth/login)
        pw=\$(sed -n 's/.*"password":"\([^"]*\)".*/\1/p' "\$data")
        if [ "\$pw" = s3cret-pw ] && [ ! -f $T/ag-login-broken ]; then code=200; body='{"token":"good-token"}'
        elif [ -z "\$pw" ] && [ -f $T/ag-empty-ok ]; then code=200; body='{"token":"empty-token"}'; fi ;;
esac
printf '%s' "\$body" >"\$out"
printf '%s' "\$code"
EOF
    cat >"$T/bin/agent-initd" <<EOF
#!/bin/sh
echo "\$*" >>$T/agent-initd.log
if [ "\$1" = start ]; then
    if [ -f $T/ag-new-broken ] && grep -q NEW $R/data/zte-agent; then exit 1; fi
    echo 7777 >$T/pids/zte-agent
fi
[ "\$1" = stop ] && rm -f $T/pids/zte-agent
exit 0
EOF
    # production datad: starts unless the live file is the new build and
    # $T/new-broken exists (a new version that cannot come up)
    cat >"$T/bin/initd" <<EOF
#!/bin/sh
echo "\$*" >>$T/initd.log
# hangs (until the step timeout kills it); meanwhile note the heartbeat file's
# inode twice: a new inode = the executor wrote a heartbeat while we hung
if [ -f $T/hang-initd-\$1 ]; then
    /bin/sleep 0.3; ls -i $T/tmp/heartbeat >>$T/hang.inodes
    /bin/sleep 1.5; ls -i $T/tmp/heartbeat >>$T/hang.inodes
    /bin/sleep 100
fi
if [ "\$1" = start ]; then
    if [ -f $T/new-broken ] && grep -q NEW $LIVE; then exit 1; fi
    echo 5555 >$T/pids/zwrt-datad
fi
[ "\$1" = stop ] && rm -f $T/pids/zwrt-datad
exit 0
EOF
    cat >"$T/bin/date" <<'EOF'
#!/bin/sh
echo "2026-09-30 12:00:00"
EOF
    cat >"$T/bin/setsid" <<EOF
#!/bin/sh
echo "\$*" >>$T/setsid.log
case "\$*" in *zwrt-datad.test\ *) [ -f $T/test-broken ] || echo 4242 >$T/pids/zwrt-datad.test ;; esac
case "\$*" in *zte-agent.test*) [ -f $T/test-broken ] || echo 6262 >$T/pids/zte-agent.test ;; esac
case "\$*" in *u60-ship.sh\ run\ *) [ -f $T/exec-broken ] || "\$@" ;; esac
exit 0
EOF
    cat >"$T/bin/df" <<EOF
#!/bin/sh
echo 'Filesystem 1024-blocks Used Available Capacity Mounted on'
echo "/dev/x 3000000 1000000 \$(cat $T/free-kb 2>/dev/null || echo 1100000) 40% /data"
EOF
    chmod +x "$T"/bin/*
    mkdir -p "$T/fake"
    printf '/bin/sleep 30\n' >"$T/fake/datad-trial.sh"
    printf '/bin/sleep 30\n' >"$T/fake/u60-ship.sh"

    export U60S_ROOT=$R U60S_TMP=$T/tmp U60S_BOOT_ID=$T/boot_id U60S_DF=$T/bin/df
    export U60S_MANIFEST=$R/data/u60-manifest.jsonl
    unset U60S_CRASH_AT U60S_DIR U60S_LOG U60S_TEST_LOG U60S_MIN_FREE_KB
    export DT_INITD=$T/bin/initd DT_UPTIME=$T/uptime DT_PIDFILE=$T/trial.pid
    export DT_CURL=$T/bin/curl DT_PIDOF=$T/bin/pidof DT_PS=$T/bin/ps
    export DT_LOGREAD=$T/bin/logread DT_KILL=$T/bin/kill DT_SLEEP=$T/bin/sleep
    export DT_DATE=$T/bin/date DT_SETSID=$T/bin/setsid DT_NETWATCH_FILE=$T/netwatch.errors
    export DT_RC=$T/rc.local
    unset DT_NETWATCH_CMD DT_TEST_BIN DT_LOG DT_TEST_LOG DT_WINDOW
    export U60R_ROOT=$R
    unset U60R_DIR U60R_BOOT_ID
    export U60S_AGENT_INITD=$T/bin/agent-initd U60S_AGENT_URL=http://agent.test:9090 U60S_AGENT_HB=$T/scenario.heartbeat
    unset U60S_TO_INITD
    printf 'ZTE_AGENT_PASSWORD=s3cret-pw\n' >"$R/data/zte-agent.env"
    printf 'OLD-AGENT\n' >"$R/data/zte-agent"
    chmod 755 "$R/data/zte-agent"
    AOLD=$(md5 "$R/data/zte-agent")
    mkdir -p "$R/data/scenario"
    printf '{"pin":null}\n' >"$R/data/scenario/state.json"
    SC_OLD=$(md5 "$R/data/scenario/state.json")
}

teardown() {
    check "$CASE: nothing written outside the sandbox" '[ "$(leak_snapshot)" = "$LEAK0" ]'
    rm -rf "$T"
}

hook_at() { # hook_at <uptime> <shell command>
    cat >"$T/hook" <<EOF
[ \$(cut -d. -f1 $T/uptime) -ge $1 ] && { $2; }
:
EOF
}

# upload [txn] [extra meta lines]: what tools/u60 puts into stage/<txn>/
upload() {
    _t=${1:-$TXN}
    _c=${_t##*-}
    _f=zwrt-datad
    [ "$_c" = agent ] && _f=zte-agent
    mkdir -p "$R/data/u60-ship/stage/$_t"
    printf 'NEW-%s %s\n' "$_c" "$_t" >"$R/data/u60-ship/stage/$_t/$_f"
    NEW=$(md5 "$R/data/u60-ship/stage/$_t/$_f")
    {
        echo v=1
        echo "txn=$_t"
        echo "comp=$_c"
        echo commit=7e8d223
        echo mac_time=1790000000
        echo format=1
        echo trial=60
        echo "file=$_f $NEW"
        [ -n "$2" ] && printf '%s\n' "$2"
    } >"$R/data/u60-ship/stage/$_t/meta"
}

stage() { sh "$SHIP" stage "${1:-$TXN}" >"$T/out" 2>&1; RC=$?; }
run() { timeout 60 sh "$SHIP" run "${1:-$TXN}" >"$T/out" 2>&1; RC=$?; }
phase() { sed -n 's/^phase=//p' "$R/data/u60-ship/txn" 2>/dev/null; }
logged() { grep -q "$1" "$R/data/u60-ship/ship.log"; }
mlines() { grep -c . "$U60S_MANIFEST" 2>/dev/null || echo 0; }
recover_newboot() {
    echo bbbb-2222 >"$T/newboot"
    U60R_BOOT_ID=$T/newboot sh "$RECOVER" >"$T/rout" 2>&1
    RRC=$?
}
tree() { find "$R" | sort; find "$R" -type f | sort | while read -r f; do md5sum "$f"; done; }

echo "== u60-ship =="

# ── stage refuses ───────────────────────────────────────────────────────────
refused() { # refused <case> <text in the message>
    CASE=$1
    check "$1: refused" '[ "$RC" = 1 ] && grep -q "拒绝暂存" "$T/out"'
    check "$1: says why" "grep -q -- '$2' '$T/out'"
    check "$1: live untouched, no side file" '[ "$(md5 "$LIVE")" = "$OLD" ] && [ ! -e "$LIVE.test" ]'
    check "$1: no service touched" '[ ! -s "$T/initd.log" ] && [ ! -s "$T/kill.log" ]'
}
nolog() { check "$CASE: no transaction written" '[ ! -f "$R/data/u60-ship/txn" ]'; }

setup
stage "../etc"
refused "bad txn id" "格式不对"
nolog
teardown

setup
upload 20260930-120000-foo
stage 20260930-120000-foo
refused "unknown component" "不认识"
nolog
teardown


setup
upload 20260930-120000-touch
stage 20260930-120000-touch
refused "touch with another component's file" "file=zwrt-datad 不是组件 touch 的文件"
nolog
teardown

setup
upload
sed -i "s/^file=zwrt-datad .*/file=zwrt-datad 0123456789abcdef0123456789abcdef/" "$R/data/u60-ship/stage/$TXN/meta"
stage
refused "md5 of the upload wrong" "md5 不对"
nolog
teardown

setup
upload
rm "$R/data/u60-ship/stage/$TXN/zwrt-datad"
stage
refused "product missing" "暂存目录里没有 zwrt-datad"
nolog
teardown

setup
upload
sed -i '/^file=/d' "$R/data/u60-ship/stage/$TXN/meta"
stage
refused "file= missing" "没有 file=zwrt-datad"
nolog
teardown

setup
upload "" "file=evil 0123456789abcdef0123456789abcdef"
stage
refused "extra file" "file=evil 不是组件"
nolog
teardown

setup
upload
sed -i 's/^commit=.*/commit=xyz; rm -rf \//' "$R/data/u60-ship/stage/$TXN/meta"
stage
refused "bad commit" "提交号"
nolog
teardown

for bad in 'format=a' 'format=' 'mac_time=12a' 'trial=10' 'trial=08' 'trial=99999999'; do
    setup
    upload
    k=${bad%%=*}
    sed -i "s/^$k=.*/$bad/" "$R/data/u60-ship/stage/$TXN/meta"
    stage
    refused "bad $bad" "$k"
    nolog
    teardown
done

setup
upload "" "evil=1"
stage
refused "unknown meta key" "看不懂的一行"
nolog
teardown

setup
upload
sed -i 's/^v=1/v=2/' "$R/data/u60-ship/stage/$TXN/meta"
stage
refused "meta version" "v=1"
nolog
teardown

setup
upload 20260930-120000-datad
sed -i 's/^txn=.*/txn=20260930-120001-datad/' "$R/data/u60-ship/stage/$TXN/meta"
stage
refused "meta txn differs from dir" "和目录名"
nolog
teardown

for sp in /etc/passwd "$R/data/zwrt-datad/../u60-ship/txn" "$R/data/scenario/log" "$R/data/zwrt-datad/wifi" "$R/data/zwrt-datad/cooling.conf x"; do
    setup
    upload "" "state=$sp"
    stage
    refused "state out of bounds: ${sp#"$R"}" "不在组件 datad 的清单里"
    nolog
    teardown
done

setup
rm "$R/data/zwrt-datad/cooling.conf"
mkdir "$R/data/zwrt-datad/cooling.conf"
upload
stage
refused "state file is a directory" "不是普通文件"
nolog
teardown

setup
echo 100000 >"$T/free-kb"
upload
stage
refused "/data under 200 MB" "可用只有 97 MB"
nolog
teardown

setup
rm "$LIVE"
OLD=
upload
stage
refused "live file missing" "不存在（第一次安装走装机包）"
nolog
teardown

setup
echo 4242 >"$T/pids/zwrt-datad.test"
upload
stage
refused "test build still running" "还在跑"
teardown

setup
"$T/fake/datad-trial.sh" &
W=$!
sh "$T/fake/datad-trial.sh" &
W=$!
echo $W >"$T/trial.pid"
upload
stage
refused "datad-trial watcher running" "datad-trial 的守护在跑"
nolog
kill $W 2>/dev/null
teardown

setup
printf 'v=1\ntxn=20260929-100000-datad\ncomp=datad\nphase=check\nend=1\n' >"$R/data/u60-ship/txn"
upload
stage
refused "second transaction" "还在进行：check"
teardown

setup
printf 'v=1\ntxn=20260929-100000-datad\ncomp=datad\nphase=failed\nreason=x\nend=1\n' >"$R/data/u60-ship/txn"
upload
stage
refused "failed transaction blocks" "停在 failed"
teardown

setup
printf 'garbage\n' >"$R/data/u60-ship/txn"
upload
stage
refused "unreadable transaction log blocks" "看不懂"
teardown

setup
echo '{"v":1,"kind":"ship","comp":"datad","txn":"20260901-000000-datad","format":1}' >"$U60S_MANIFEST"
upload
sed -i 's/^format=.*/format=2/' "$R/data/u60-ship/stage/$TXN/meta"
stage
refused "data format goes up" "数据格式版本升高（设备 1 → 新版 2）"
nolog
teardown

setup
echo '{"v":1,"kind":"ship","comp":"datad","txn":"20260901-000000-datad","format":3}' >"$U60S_MANIFEST"
upload
sed -i 's/^format=.*/format=2/' "$R/data/u60-ship/stage/$TXN/meta"
stage
refused "data format goes down" "数据格式版本降低"
nolog
teardown

setup
upload
( flock -x 9; "$_real_sleep" 3 ) 9>"$R/data/u60-ship/lock" &
L=$!
"$_real_sleep" 0.5
stage
refused "another stage holds the lock" "另一个 stage"
nolog
wait $L
teardown

setup
upload 20260930-120000-foo
sed -i 's/^comp=.*/comp=datad/' "$R/data/u60-ship/stage/20260930-120000-foo/meta"
stage 20260930-120000-foo
refused "txn not ending in the component" "不是以组件名"
nolog
teardown

setup
CASE="usage"
sh "$SHIP" stage >"$T/out" 2>&1
RC=$?
check "usage: stage without txn exits 2" '[ $RC = 2 ]'
sh "$SHIP" frobnicate >"$T/out" 2>&1
RC=$?
check "usage: unknown command exits 2" '[ $RC = 2 ] && grep -q "usage:" "$T/out"'
teardown

# ── the normal ship ─────────────────────────────────────────────────────────
setup
CASE="ship datad"
upload
stage
check "$CASE: staged" '[ "$RC" = 0 ] && [ "$(phase)" = staged ] && grep -q "已暂存" "$T/out"'
check "$CASE: side file in place, upload copy gone" '[ "$(md5 "$LIVE.test")" = "$NEW" ] && [ -x "$LIVE.test" ] && [ ! -e "$R/data/u60-ship/stage/$TXN/zwrt-datad" ]'
check "$CASE: staging touches nothing live" '[ "$(md5 "$LIVE")" = "$OLD" ] && [ ! -s "$T/initd.log" ]'
check "$CASE: boot id and md5s logged" "grep -qx 'boot_id=aaaa-1111' '$R/data/u60-ship/txn' && grep -qx 'file=$LIVE|$OLD|$NEW' '$R/data/u60-ship/txn'"
# the test build writes its state during the trial
hook_at 1020 "echo cool-by-test >$R/data/zwrt-datad/cooling.conf"
run
check "$CASE: run exit 0, done" '[ "$RC" = 0 ] && [ "$(phase)" = done ]'
check "$CASE: live is the new build" '[ "$(md5 "$LIVE")" = "$NEW" ] && [ -x "$LIVE" ]'
check "$CASE: previous version kept" '[ "$(md5 "$LIVE.prev-$TXN")" = "$OLD" ]'
check "$CASE: side file gone" '[ ! -e "$LIVE.test" ]'
check "$CASE: state snapshot kept" '[ "$(md5 "$R/data/zwrt-datad/cooling.conf.prev-$TXN")" = "$COOL_OLD" ]'
check "$CASE: state lines (present + absent)" "grep -qx 'state=$R/data/zwrt-datad/cooling.conf|$COOL_OLD' '$R/data/u60-ship/txn' && grep -qx 'state=$R/data/zwrt-datad/neighbor.json|-' '$R/data/u60-ship/txn'"
check "$CASE: stop for the trial, start after promotion" '[ "$(tr "\n" " " <"$T/initd.log")" = "stop start " ]'
check "$CASE: test build started without supervise, then killed" "grep -q 'zwrt-datad.test -i 1000' '$T/setsid.log' && ! grep -q supervise '$T/setsid.log' && grep -q 4242 '$T/kill.log'"
check "$CASE: trial ran the whole window" "logged '试跑通过（60s）'"
check "$CASE: manifest one line" '[ "$(mlines)" = 1 ]'
check "$CASE: manifest fields" "grep -q '\"kind\":\"ship\",\"comp\":\"datad\",\"txn\":\"$TXN\",\"commit\":\"7e8d223\",\"format\":1,\"mac_time\":1790000000,\"boot_id\":\"aaaa-1111\"' '$U60S_MANIFEST' && grep -q '\"path\":\"$LIVE\",\"md5\":\"$NEW\",\"prev\":\"$LIVE.prev-$TXN\"' '$U60S_MANIFEST'"
check "$CASE: stage dir removed" '[ ! -e "$R/data/u60-ship/stage/$TXN" ]'
check "$CASE: heartbeat names the transaction" "grep -q ' $TXN done\$' '$T/tmp/heartbeat'"
check "$CASE: hand-made neighbours untouched" '[ "$(cat "$D/zwrt-datad.prev-cache-20260923")" = "keep $D/zwrt-datad.prev-cache-20260923" ] && [ -f "$D/zwrt-datad.orig-backup" ] && [ -f "$R/data/zte-agent.prev-esim-20260926" ]'
sh "$SHIP" status >"$T/st"
check "$CASE: status: done, observing" "grep -qx 'phase=done' '$T/st' && grep -q '^observe_left=3[0-9][0-9][0-9]\$' '$T/st'"
echo bbbb-2222 >"$T/boot_id"
sh "$SHIP" status >"$T/st"
check "$CASE: status: no observation after a reboot" "! grep -q observe_left '$T/st'"
sh "$SHIP" wait 10 >"$T/st"
RC=$?
check "$CASE: wait returns at once on a terminal phase" '[ $RC = 0 ] && grep -qx "phase=done" "$T/st"'
teardown

# heartbeat: every fake sleep records the heartbeat's age and phase
setup
CASE="heartbeat"
upload
stage
cat >"$T/hook" <<EOF
read -r hb _p _t ph <$T/tmp/heartbeat 2>/dev/null && echo "\$ph \$(( \$(cut -d. -f1 $T/uptime) - hb ))" >>$T/hb.trace
:
EOF
run
check "$CASE: run done" '[ "$(phase)" = done ]'
check "$CASE: trial beats every second" '[ "$(grep -c "^trial " "$T/hb.trace")" -ge 60 ] && [ -z "$(awk "\$1 == \"trial\" && \$2 > 1" "$T/hb.trace")" ]'
check "$CASE: other phases at most every 5 s" '[ "$(grep -c "^check " "$T/hb.trace")" -ge 20 ] && [ -z "$(awk "\$1 != \"trial\" && \$2 > 5" "$T/hb.trace")" ]'
teardown

# ── the trial fails: the live slot was never touched ────────────────────────
setup
CASE="trial crash"
upload
stage
hook_at 1030 "echo cool-by-test >$R/data/zwrt-datad/cooling.conf; echo n >$R/data/zwrt-datad/neighbor.json; rm -f $T/pids/zwrt-datad.test"
run
check "$CASE: exit 1, aborted" '[ "$RC" = 1 ] && [ "$(phase)" = aborted ]'
check "$CASE: reason" "grep -q '^reason=试跑不过：测试版崩溃' '$R/data/u60-ship/txn'"
check "$CASE: live is the old build, no .prev" '[ "$(md5 "$LIVE")" = "$OLD" ] && [ ! -e "$LIVE.prev-$TXN" ]'
check "$CASE: state restored, new file removed" '[ "$(md5 "$R/data/zwrt-datad/cooling.conf")" = "$COOL_OLD" ] && [ ! -e "$R/data/zwrt-datad/neighbor.json" ]'
check "$CASE: production back" '[ "$(tr "\n" " " <"$T/initd.log")" = "stop start " ] && [ -f "$T/pids/zwrt-datad" ]'
check "$CASE: manifest untouched" '[ "$(mlines)" = 0 ]'
check "$CASE: side file removed" '[ ! -e "$LIVE.test" ]'
teardown

setup
CASE="test build never up"
touch "$T/test-broken"
upload
stage
run
check "$CASE: aborted" '[ "$RC" = 1 ] && [ "$(phase)" = aborted ] && grep -q "^reason=测试版没起来（进程没出现）" "$R/data/u60-ship/txn"'
check "$CASE: live old, production back" '[ "$(md5 "$LIVE")" = "$OLD" ] && [ "$(tr "\n" " " <"$T/initd.log")" = "stop start " ]'
teardown

setup
CASE="test build unkillable"
touch "$T/unkillable"
upload
stage
hook_at 1030 "rm -f $T/pids/u60pro-devui"
run
check "$CASE: failed, production not started next to it" '[ "$RC" = 1 ] && [ "$(phase)" = failed ] && [ "$(tr "\n" " " <"$T/initd.log")" = "stop " ]'
check "$CASE: live old" '[ "$(md5 "$LIVE")" = "$OLD" ]'
upload 20260930-130000-datad
stage 20260930-130000-datad
check "$CASE: blocks the next stage" '[ "$RC" = 1 ] && grep -q "停在 failed" "$T/out"'
teardown

setup
CASE="rebooted between stage and run"
upload
stage
echo bbbb-2222 >"$T/boot_id"
run
check "$CASE: aborted, nothing touched" '[ "$(phase)" = aborted ] && [ "$(md5 "$LIVE")" = "$OLD" ] && [ ! -s "$T/initd.log" ] && [ ! -e "$LIVE.test" ]'
teardown

# ── the check after promotion fails: back to the old version ────────────────
setup
CASE="check fails"
upload
stage
hook_at 1020 "echo cool-by-test >$R/data/zwrt-datad/cooling.conf"
cat >>"$T/hook" <<EOF
[ \$(cut -d. -f1 $T/uptime) -ge 1100 ] && [ ! -f $T/frozen ] && { touch $T/frozen; echo cool-by-new >$R/data/zwrt-datad/cooling.conf; }
:
EOF
run
check "$CASE: exit 1, rolled back" '[ "$RC" = 1 ] && [ "$(phase)" = rolledback ]'
check "$CASE: reason" "grep -q '^reason=转正后检查不过：屏幕数据停了' '$R/data/u60-ship/txn'"
check "$CASE: live is the old build again" '[ "$(md5 "$LIVE")" = "$OLD" ]'
check "$CASE: state restored" '[ "$(md5 "$R/data/zwrt-datad/cooling.conf")" = "$COOL_OLD" ]'
check "$CASE: stop, start new, stop, start old" '[ "$(tr "\n" " " <"$T/initd.log")" = "stop start stop start " ]'
check "$CASE: manifest untouched" '[ "$(mlines)" = 0 ]'
teardown

setup
CASE="new version will not start"
touch "$T/new-broken"
upload
stage
run
check "$CASE: rolled back" '[ "$RC" = 1 ] && [ "$(phase)" = rolledback ] && grep -q "^reason=新版起不来" "$R/data/u60-ship/txn"'
check "$CASE: live old and running" '[ "$(md5 "$LIVE")" = "$OLD" ] && [ -f "$T/pids/zwrt-datad" ]'
teardown

# a script error in the executor (bad arithmetic) still rolls back
setup
CASE="executor script error"
upload
stage
hook_at 1100 "echo 1100x >$T/uptime"
run
check "$CASE: rolled back by the EXIT trap" '[ "$(phase)" = rolledback ] && grep -q "^reason=执行器意外退出" "$R/data/u60-ship/txn"'
check "$CASE: live old" '[ "$(md5 "$LIVE")" = "$OLD" ]'
teardown

# ── manifest cannot be written ──────────────────────────────────────────────
setup
CASE="manifest write fails"
: >"$T/notadir"
export U60S_MANIFEST=$T/notadir/u60-manifest.jsonl
upload
stage
run
check "$CASE: exit 2, manifest_pending" '[ "$RC" = 2 ] && [ "$(phase)" = manifest_pending ]'
check "$CASE: live stays new" '[ "$(md5 "$LIVE")" = "$NEW" ]'
rm "$T/notadir"
mkdir "$T/notadir"
sh "$SHIP" manifest-finish >"$T/out" 2>&1
RC=$?
check "$CASE: manifest-finish completes it" '[ $RC = 0 ] && [ "$(phase)" = done ] && [ "$(mlines)" = 1 ]'
sh "$SHIP" manifest-finish >"$T/out" 2>&1
check "$CASE: manifest-finish again: no second line" '[ "$(mlines)" = 1 ]'
teardown

setup
CASE="manifest finished by the next stage"
: >"$T/notadir"
export U60S_MANIFEST=$T/notadir/u60-manifest.jsonl
upload
stage
run
rm "$T/notadir"
mkdir "$T/notadir"
upload 20260930-130000-datad
stage 20260930-130000-datad
check "$CASE: next stage ok" '[ "$RC" = 0 ] && [ "$(phase)" = staged ]'
check "$CASE: owed line written first" "grep -q '\"txn\":\"$TXN\"' '$U60S_MANIFEST' && [ \"\$(mlines)\" = 1 ]"
teardown

# ── .prev housekeeping ──────────────────────────────────────────────────────
setup
CASE="keep 3 ship .prev"
for d in 01 02 03 04; do
    echo "old $d" >"$LIVE.prev-202609${d}-000000-datad"
    echo "cool $d" >"$R/data/zwrt-datad/cooling.conf.prev-202609${d}-000000-datad"
done
upload
stage
run
check "$CASE: done" '[ "$(phase)" = done ]'
check "$CASE: three newest program .prev left" '[ "$(ls "$D" | grep -c "^zwrt-datad\.prev-2026")" = 3 ] && [ -f "$LIVE.prev-20260903-000000-datad" ] && [ -f "$LIVE.prev-20260904-000000-datad" ] && [ -f "$LIVE.prev-$TXN" ]'
check "$CASE: three newest state .prev left" '[ "$(ls "$R/data/zwrt-datad" | grep -c "^cooling\.conf\.prev-")" = 3 ] && [ ! -e "$R/data/zwrt-datad/cooling.conf.prev-20260902-000000-datad" ]'
check "$CASE: hand-made backups untouched" '[ -f "$D/zwrt-datad.prev-cache-20260923" ] && [ -f "$D/zwrt-datad.orig-backup" ] && [ -f "$R/data/zte-agent.prev-esim-20260926" ] && [ -f "$R/data/admin.prev-reset-20260926.tgz" ] && [ -f "$R/data/zte-agent.env" ]'
check "$CASE: pruning logged" "logged '删掉较早的上一版 .*zwrt-datad.prev-20260901-000000-datad'"
teardown

# ── power cuts at every point, then the next boot ───────────────────────────
# crash <point>: stage, run with U60S_CRASH_AT; the test build rewrites the
# state during the trial; a check failure is arranged for rollback:* points.
crash() {
    setup
    CASE="power cut at $1"
    upload
    stage
    hook_at 1020 "[ -f $R/data/zwrt-datad/cooling.conf.prev-$TXN ] && echo cool-by-test >$R/data/zwrt-datad/cooling.conf"
    case "$1" in rollback:*)
        cat >>"$T/hook" <<EOF
[ \$(cut -d. -f1 $T/uptime) -ge 1100 ] && touch $T/frozen
:
EOF
        ;;
    esac
    U60S_CRASH_AT=$1 run
    check "$CASE: executor died there" '[ "$RC" = 137 ]'
    # the test build had written its state before the power went
    case "$1" in trial:*) [ -f "$R/data/zwrt-datad/cooling.conf.prev-$TXN" ] && echo cool-by-test >"$R/data/zwrt-datad/cooling.conf" ;; esac
    check "$CASE: live is whole (old or new)" '[ "$(md5 "$LIVE")" = "$OLD" ] || [ "$(md5 "$LIVE")" = "$NEW" ]'
    recover_newboot
    check "$CASE: recover exit 0" '[ "$RRC" = 0 ]'
}
after_boot() { # after_boot <phase> old|new
    _want=$1
    check "$CASE: phase $1 after boot" '[ "$(phase)" = "$_want" ]'
    if [ "$2" = old ]; then
        check "$CASE: live is the old build" '[ "$(md5 "$LIVE")" = "$OLD" ]'
    else
        check "$CASE: live is the new build" '[ "$(md5 "$LIVE")" = "$NEW" ]'
    fi
    tree >"$T/tree1"
    recover_newboot
    tree >"$T/tree2"
    check "$CASE: recover again changes nothing" 'cmp -s "$T/tree1" "$T/tree2"'
}

for p in trial:begin trial:state trial:running; do
    crash $p
    after_boot aborted old
    check "$CASE: reason" "grep -q '^reason=开机时已中止' '$R/data/u60-ship/txn'"
    check "$CASE: state back to the one before the trial" '[ "$(md5 "$R/data/zwrt-datad/cooling.conf")" = "$COOL_OLD" ]'
    teardown
done
for p in promote:begin promote:prev-copied:zwrt-datad promote:prev-synced:zwrt-datad promote:moved:zwrt-datad check:begin rollback:begin rollback:files:zwrt-datad rollback:states:cooling.conf; do
    crash $p
    after_boot rolledback old
    check "$CASE: state back to the old one" '[ "$(md5 "$R/data/zwrt-datad/cooling.conf")" = "$COOL_OLD" ]'
    check "$CASE: reason" "grep -q '^reason=开机时已退回' '$R/data/u60-ship/txn'"
    check "$CASE: manifest untouched" '[ "$(mlines)" = 0 ]'
    teardown
done
for p in manifest:begin manifest:tmp manifest:moved; do
    crash $p
    after_boot manifest_pending new
    sh "$SHIP" manifest-finish >"$T/out" 2>&1
    check "$CASE: manifest-finish: done, exactly one line" '[ "$(phase)" = done ] && [ "$(mlines)" = 1 ]'
    teardown
done
crash manifest:written
after_boot manifest_pending new
teardown

# a power cut in the same boot is not u60-recover's business
setup
CASE="same boot"
upload
stage
U60S_CRASH_AT=check:begin run
U60R_BOOT_ID=$T/boot_id sh "$RECOVER" >"$T/rout" 2>&1
check "$CASE: recover leaves it to recover-live" '[ "$(phase)" = check ] && [ "$(md5 "$LIVE")" = "$NEW" ]'
teardown

# ── recover-live (same boot; the guard calls it in T4) ──────────────────────
setup
CASE="recover-live, executor alive"
upload
stage
U60S_CRASH_AT=check:begin run
sh "$T/fake/u60-ship.sh" &
W=$!
echo "$(cat "$T/uptime") $W $TXN check" >"$T/tmp/heartbeat"
sh "$SHIP" recover-live >"$T/out" 2>&1
RC=$?
check "$CASE: refuses with 3, nothing done" '[ $RC = 3 ] && [ "$(phase)" = check ] && [ "$(md5 "$LIVE")" = "$NEW" ]'
echo "$(($(cat "$T/uptime") - 40)) $W $TXN check" >"$T/tmp/heartbeat"
sh "$SHIP" recover-live >"$T/out" 2>&1
check "$CASE: stale heartbeat: hung executor killed, rolled back" '[ "$(phase)" = rolledback ] && [ ! -d /proc/$W ] && [ "$(md5 "$LIVE")" = "$OLD" ]'
teardown

setup
CASE="recover-live after kill -9 in check"
upload
stage
hook_at 1020 "[ -f $R/data/zwrt-datad/cooling.conf.prev-$TXN ] && echo cool-by-test >$R/data/zwrt-datad/cooling.conf"
U60S_CRASH_AT=check:begin run
: >"$T/initd.log"
sh "$SHIP" recover-live >"$T/out" 2>&1
check "$CASE: rolled back" '[ "$(phase)" = rolledback ] && [ "$(md5 "$LIVE")" = "$OLD" ] && [ "$(md5 "$R/data/zwrt-datad/cooling.conf")" = "$COOL_OLD" ]'
check "$CASE: service restarted on the old build" '[ "$(tr "\n" " " <"$T/initd.log")" = "stop start " ]'
teardown

setup
CASE="recover-live after kill -9 in trial"
upload
stage
hook_at 1020 "[ -f $R/data/zwrt-datad/cooling.conf.prev-$TXN ] && echo cool-by-test >$R/data/zwrt-datad/cooling.conf"
U60S_CRASH_AT=trial:running run
sh "$SHIP" recover-live >"$T/out" 2>&1
check "$CASE: aborted" '[ "$(phase)" = aborted ] && [ "$(md5 "$LIVE")" = "$OLD" ]'
check "$CASE: test build killed, state restored, production up" "grep -q 4242 '$T/kill.log' && [ ! -f '$T/pids/zwrt-datad.test' ] && [ \"\$(md5 '$R/data/zwrt-datad/cooling.conf')\" = '$COOL_OLD' ] && [ -f '$T/pids/zwrt-datad' ]"
teardown

setup
CASE="recover-live after kill -9 in manifest"
upload
stage
U60S_CRASH_AT=manifest:begin run
sh "$SHIP" recover-live >"$T/out" 2>&1
check "$CASE: manifest written, done" '[ "$(phase)" = done ] && [ "$(mlines)" = 1 ] && [ "$(md5 "$LIVE")" = "$NEW" ]'
teardown

setup
CASE="recover-live with nothing to do"
sh "$SHIP" recover-live >"$T/out" 2>&1
RC=$?
check "$CASE: no transaction: exit 0" '[ $RC = 0 ] && [ ! -f "$R/data/u60-ship/txn" ]'
teardown

# ── start / abort ───────────────────────────────────────────────────────────
setup
CASE="start"
upload
stage
timeout 60 sh "$SHIP" start "$TXN" >"$T/out" 2>&1
RC=$?
check "$CASE: exit 0, executor up" '[ "$RC" = 0 ] && grep -q "执行器已\(起来\|接手\)" "$T/out"'
check "$CASE: detached executor (nohup sh …u60-ship.sh run)" "grep -q '^nohup sh /.*u60-ship.sh run $TXN' '$T/setsid.log'"
_i=0
while [ "$(phase)" != done ] && [ $_i -lt 300 ]; do "$_real_sleep" 0.1; _i=$((_i + 1)); done
check "$CASE: the detached executor finishes the transaction" '[ "$(phase)" = done ] && [ "$(md5 "$LIVE")" = "$NEW" ]'
teardown

setup
CASE="start, executor never comes up"
touch "$T/exec-broken"
upload
stage
sh "$SHIP" start "$TXN" >"$T/out" 2>&1
RC=$?
check "$CASE: exit 1, aborted, live untouched" '[ $RC = 1 ] && [ "$(phase)" = aborted ] && [ "$(md5 "$LIVE")" = "$OLD" ]'
teardown

setup
CASE="abort"
upload
stage
printf '#!/bin/sh\n/bin/sleep 0.05\n' >"$T/bin/sleep"
sh "$SHIP" run "$TXN" >"$T/out" 2>&1 &
W=$!
_i=0
while [ ! -s "$T/pids/zwrt-datad.test" ] && [ $_i -lt 50 ]; do "$_real_sleep" 0.1; _i=$((_i + 1)); done
"$_real_sleep" 0.3
sh "$SHIP" abort >"$T/aout" 2>&1
wait $W
check "$CASE: executor finished the trial by itself" '[ "$(phase)" = aborted ] && grep -q "^reason=执行器被停止" "$R/data/u60-ship/txn"'
check "$CASE: live old, production back" '[ "$(md5 "$LIVE")" = "$OLD" ] && [ -f "$T/pids/zwrt-datad" ] && [ ! -f "$T/pids/zwrt-datad.test" ]'
teardown

setup
CASE="wait times out"
printf 'v=1\ntxn=%s\ncomp=datad\nphase=check\nend=1\n' "$TXN" >"$R/data/u60-ship/txn"
sh "$SHIP" wait 6 >"$T/out" 2>&1
RC=$?
check "$CASE: exit 4 with the status" '[ $RC = 4 ] && grep -qx "phase=check" "$T/out"'
teardown

# ── datad-trial and ship do not run at the same time ────────────────────────
setup
CASE="datad-trial during a transaction"
printf 'v=1\ntxn=%s\ncomp=datad\nphase=trial\nend=1\n' "$TXN" >"$R/data/u60-ship/txn"
printf '#!/bin/sh\n' >"$T/zwrt-datad.test"
chmod +x "$T/zwrt-datad.test"
printf '#!/bin/sh /etc/rc.common\n' >"$T/zwrt-datad.init"
DT_TEST_BIN=$T/zwrt-datad.test DT_LOG=$T/trial.log sh "$SCRIPTS/datad-trial.sh" launch >"$T/out" 2>&1
RC=$?
check "$CASE: launch refused" '[ $RC = 1 ] && grep -q "拒绝启动" "$T/out" && grep -q "u60-ship 事务 $TXN（datad）还在进行" "$T/out"'
check "$CASE: production untouched" '[ ! -s "$T/initd.log" ] && [ ! -s "$T/setsid.log" ]'
teardown

setup
CASE="datad-trial without its engine"
mkdir "$T/alone"
cp "$SCRIPTS/datad-trial.sh" "$T/alone/"
sh "$T/alone/datad-trial.sh" status >"$T/out" 2>&1
RC=$?
check "$CASE: exit 1, says what is missing" '[ $RC = 1 ] && grep -q "找不到 .*u60-ship.sh" "$T/out"'
teardown

# ── T4: every blocking step has a timeout and beats meanwhile ───────────────
setup
CASE="init.d stop hangs"
export U60S_TO_INITD=2
touch "$T/hang-initd-stop"
upload
stage
run
check "$CASE: given up after the timeout, trial never started" '[ "$(phase)" = aborted ] && grep -q "^reason=试跑没开始：正式版 15s 内没有退出" "$R/data/u60-ship/txn"'
check "$CASE: logged the timeout" "logged '超时（2s），已放弃'"
check "$CASE: heartbeat written while the step hung" '[ "$(sort -u "$T/hang.inodes" | wc -l)" = 2 ]'
check "$CASE: live untouched" '[ "$(md5 "$LIVE")" = "$OLD" ]'
teardown

# ── T5 datad: every check, in the trial and after promotion ────────────────
# dcheck <name> <trial action> <check action> <reason in the trial> <reason after promotion>
dcheck() {
    _r4=$4
    _r5=$5
    setup
    CASE="datad trial: $1"
    echo 0 >"$T/netwatch.errors"
    upload
    stage
    _a=$(eval "printf '%s' \"$2\"")
    hook_at 1030 "[ -f $T/once ] || { touch $T/once; $_a; }"
    run
    check "$CASE: aborted" '[ "$RC" = 1 ] && [ "$(phase)" = aborted ] && grep -q "^reason=试跑不过：$_r4" "$R/data/u60-ship/txn"'
    check "$CASE: live old, production back" '[ "$(md5 "$LIVE")" = "$OLD" ] && [ -f "$T/pids/zwrt-datad" ] && [ "$(mlines)" = 0 ]'
    teardown

    setup
    CASE="datad check: $1"
    echo 0 >"$T/netwatch.errors"
    upload
    stage
    _a=$(eval "printf '%s' \"$3\"")
    cat >"$T/hook" <<EOF
grep -qx phase=check $R/data/u60-ship/txn && [ ! -f $T/once ] && { touch $T/once; $_a; }
:
EOF
    run
    check "$CASE: rolled back" '[ "$RC" = 1 ] && [ "$(phase)" = rolledback ] && grep -q "^reason=转正后检查不过：$_r5" "$R/data/u60-ship/txn"'
    check "$CASE: live old, production back" '[ "$(md5 "$LIVE")" = "$OLD" ] && [ -f "$T/pids/zwrt-datad" ] && [ "$(mlines)" = 0 ]'
    teardown
}
dcheck "ts stops" 'touch $T/frozen' 'touch $T/frozen' "屏幕数据停了" "屏幕数据停了"
dcheck "process gone" 'rm -f $T/pids/zwrt-datad.test' 'rm -f $T/pids/zwrt-datad' "测试版崩溃" "正式版消失"
dcheck "u60-uid gives up" 'echo u60-uid: giving up: 2 launches >>$T/logread' 'echo u60-uid: giving up: 2 launches >>$T/logread' "界面异常（u60-uid" "界面异常（u60-uid"
dcheck "screen UI gone" 'rm -f $T/pids/u60pro-devui' 'rm -f $T/pids/u60pro-devui' "界面异常（u60pro-devui 进程消失）" "界面异常（u60pro-devui 进程消失）"
dcheck "netwatch rises" 'echo 2 >$T/netwatch.errors' 'echo 2 >$T/netwatch.errors' "zte-agent netwatch 报错上升（0 → 2" "zte-agent netwatch 报错上升（0 → 2"

# a normal u60-uid launch line ("… before giving up)") is not trouble
setup
CASE="datad: a u60-uid launch line in the trial"
upload
stage
hook_at 1030 "echo 'launched u60pro-devui pid 812 (attempt 1 of 2 before giving up)' >>$T/logread"
run
check "$CASE: done, not aborted" '[ "$RC" = 0 ] && [ "$(phase)" = done ]'
teardown

# ── T5 agent ────────────────────────────────────────────────────────────────
ATXN=20260930-120000-agent
ALIVE=
agent_setup() {
    setup
    ALIVE=$R/data/zte-agent
    echo 7777 >"$T/pids/zte-agent"
    echo 3 >"$T/netwatch.errors"
    upload $ATXN
    # the test build writes the scenario state during the trial
    hook_at 1020 "[ -f $R/data/scenario/state.json.prev-$ATXN ] && [ ! -f $T/state-written ] && touch $T/state-written && echo '{\"pin\":\"home\"}' >$R/data/scenario/state.json"
}
arun() { timeout 60 sh "$SHIP" run $ATXN >"$T/out" 2>&1; RC=$?; }

agent_setup
CASE="ship agent"
stage $ATXN
check "$CASE: staged (agent is open now)" '[ "$RC" = 0 ] && [ "$(phase)" = staged ]'
arun
check "$CASE: done" '[ "$RC" = 0 ] && [ "$(phase)" = done ]'
check "$CASE: live is the new build, previous kept" '[ "$(md5 "$ALIVE")" = "$NEW" ] && [ "$(md5 "$ALIVE.prev-$ATXN")" = "$AOLD" ] && [ ! -e "$ALIVE.test" ]'
check "$CASE: started like the init script, without supervise" "grep -qx 'env ZTE_AGENT_PASSWORD_FILE=$R/data/zte-agent.env ZTE_AGENT_SUPERVISED=1 nohup $ALIVE.test' '$T/setsid.log'"
check "$CASE: stop for the trial, start after promotion" '[ "$(tr "\n" " " <"$T/agent-initd.log")" = "stop start " ]'
check "$CASE: test build killed" "grep -q 6262 '$T/kill.log' && [ ! -f '$T/pids/zte-agent.test' ]"
check "$CASE: login checks passed twice (trial, check)" '[ "$(grep -c "鉴权三项通过" "$R/data/u60-ship/ship.log")" = 2 ]'
check "$CASE: the scenario state snapshot kept" '[ "$(md5 "$R/data/scenario/state.json.prev-$ATXN")" = "$SC_OLD" ]'
check "$CASE: manifest line" "grep -q '\"comp\":\"agent\",\"txn\":\"$ATXN\"' '$U60S_MANIFEST'"
check "$CASE: password in no file but the env file" '[ -z "$(grep -rl s3cret-pw "$T" | grep -v -e "/data/zte-agent.env$" -e "/bin/agent-curl$")" ]'
check "$CASE: no curl work files left" '[ -z "$(ls "$T/tmp" | grep agent-check)" ]'
teardown

agent_setup
CASE="agent heartbeat"
stage $ATXN
cat >>"$T/hook" <<EOF
read -r hb _p _t ph <$T/tmp/heartbeat 2>/dev/null && echo "\$ph \$(( \$(cut -d. -f1 $T/uptime) - hb ))" >>$T/hb.trace
:
EOF
arun
check "$CASE: done" '[ "$(phase)" = done ]'
check "$CASE: trial beats every second, the rest at most every 5 s" '[ -z "$(awk "(\$1 == \"trial\" && \$2 > 1) || \$2 > 5" "$T/hb.trace")" ] && [ "$(grep -c "^check " "$T/hb.trace")" -ge 20 ]'
teardown

# acheck <name> <setup action, before the run> <live action> <reason>
#   setup action: a flag that holds from the start (login checks);
#   live action: a flag set while the build under watch runs, cleared
#   whenever it does not (so restarting the old one still works).
acheck() {
    _r5=$5
    _r6=$6
    for _w in trial check; do
        agent_setup
        CASE="agent $_w: $1"
        [ "$_w" = trial ] || sed -i 's/^trial=.*/trial=60/' "$R/data/u60-ship/stage/$ATXN/meta"
        [ "$1" = "heartbeat stops" ] && sed -i 's/^trial=.*/trial=180/' "$R/data/u60-ship/stage/$ATXN/meta"
        stage $ATXN
        _x2=$(eval "printf '%s' \"$2\"")
        _x3=$(eval "printf '%s' \"$3\"")
        _x4=$(eval "printf '%s' \"$4\"")
        if [ "$_w" = trial ]; then
            _on="[ -s $T/pids/zte-agent.test ] && [ \$(cut -d. -f1 $T/uptime) -ge 1030 ]"
        else
            _on="grep -qx phase=check $R/data/u60-ship/txn"
        fi
        [ -n "$_x2" ] && {
            # login flags only from the window under test on
            if [ "$_w" = trial ]; then eval "$_x2"; else
                cat >>"$T/hook" <<EOF
if grep -q start $T/agent-initd.log; then $_x2; fi
:
EOF
            fi
        }
        [ -n "$_x3" ] && cat >>"$T/hook" <<EOF
if $_on; then $_x3; else $_x4; fi
:
EOF
        arun
        if [ "$_w" = trial ]; then
            check "$CASE: aborted" '[ "$RC" = 1 ] && [ "$(phase)" = aborted ] && grep -q "^reason=试跑不过：$_r5" "$R/data/u60-ship/txn"'
        else
            check "$CASE: rolled back" '[ "$RC" = 1 ] && [ "$(phase)" = rolledback ] && grep -q "^reason=转正后检查不过：$_r6" "$R/data/u60-ship/txn"'
        fi
        check "$CASE: live old, state restored, old agent running" '[ "$(md5 "$ALIVE")" = "$AOLD" ] && [ "$(md5 "$R/data/scenario/state.json")" = "$SC_OLD" ] && [ -s "$T/pids/zte-agent" ] && [ "$(mlines)" = 0 ]'
        teardown
    done
}
acheck "process gone" "" 'rm -f $T/pids/zte-agent.test $T/pids/zte-agent' ":" "测试版不在了" "正式版不在了"
acheck ":9090 silent" "" 'touch $T/ag-down' 'rm -f $T/ag-down' ":9090 没有回答" ":9090 没有回答"
acheck "admin API open" "" 'touch $T/ag-open' 'rm -f $T/ag-open' ":9090 未登录也能访问" ":9090 未登录也能访问"
acheck "password login fails" 'touch $T/ag-login-broken' "" "" "鉴权：用 zte-agent.env 的密码登录失败" "鉴权：用 zte-agent.env 的密码登录失败"
acheck "token does not work" 'touch $T/ag-token-bad' "" "" "鉴权：登录拿到的 token 访问不了" "鉴权：登录拿到的 token 访问不了"
acheck "empty password logs in" 'touch $T/ag-empty-ok' "" "" "鉴权：空密码也能登录" "鉴权：空密码也能登录"
acheck "heartbeat stops" "" 'touch $T/hb-frozen' 'rm -f $T/hb-frozen' "情景心跳停了" "情景心跳停了"
acheck "netwatch rises" "" 'echo 5 >$T/netwatch.errors' ":" "netwatch 报错上升（3 → 5）" "netwatch 报错上升（3 → 5）"

agent_setup
CASE="agent: no password file"
rm "$R/data/zte-agent.env"
stage $ATXN
arun
check "$CASE: aborted, says why" '[ "$(phase)" = aborted ] && grep -q "^reason=试跑不过：读不到 zte-agent.env 里的密码" "$R/data/u60-ship/txn"'
teardown

agent_setup
CASE="agent: password with a quote"
printf 'ZTE_AGENT_PASSWORD=ab"cd\n' >"$R/data/zte-agent.env"
stage $ATXN
arun
check "$CASE: aborted, not sent anywhere" '[ "$(phase)" = aborted ] && grep -q "^reason=试跑不过：密码里有引号" "$R/data/u60-ship/txn"'
teardown

agent_setup
CASE="agent: new build will not start"
touch "$T/ag-new-broken"
stage $ATXN
arun
check "$CASE: rolled back, old agent running" '[ "$(phase)" = rolledback ] && grep -q "^reason=新版起不来" "$R/data/u60-ship/txn" && [ "$(md5 "$ALIVE")" = "$AOLD" ] && [ -s "$T/pids/zte-agent" ]'
teardown

agent_setup
CASE="agent: test build never answers"
touch "$T/test-broken"
stage $ATXN
arun
check "$CASE: aborted" '[ "$(phase)" = aborted ] && grep -q "^reason=测试版没起来（进程没出现）" "$R/data/u60-ship/txn"'
teardown

# drift: the trial must start what /etc/init.d/zte-agent's start_service starts
CASE="agent drift"
(
    unset U60S_ROOT
    INIT=$SCRIPTS/zte-agent.init
    eval "$(grep -E '^(BIN|ENV_FILE)=' "$INIT")"
    WANT_ENV=$(sed -n 's/^[[:space:]]*procd_set_param env[[:space:]]*//p' "$INIT" | sed 's/[[:space:]]*#.*//; s/"//g' | tr -s ' \t' '  ')
    WANT_ENV=$(eval "echo $WANT_ENV")
    case "$(sed -n '/procd_set_param command/p' "$INIT")" in *'"$BIN"') ;; *) echo "  FAIL agent drift: the init command has arguments after \$BIN"; exit 1 ;; esac
    WANT="env $WANT_ENV nohup $BIN.test"
    GOT=$(sh "$SHIP" print-launch agent)
    if [ "$GOT" = "$WANT" ]; then
        echo "  ok   agent drift: trial launch = init start_service (env, binary)"
    else
        echo "  FAIL agent drift: init: $WANT"
        echo "                    trial: $GOT"
        exit 1
    fi
) && PASS=$((PASS + 1)) || FAIL=$((FAIL + 1))

# ── record: files the manifest only records (T6), and doctor agreeing ─────
undocenv() {
    unset DOC_MANIFEST DOC_SHIP_TXN DOC_SHIP_HB DOC_MD5_CACHE DOC_TS_DIR DOC_TS_TUNING DOC_INITD DOC_RC DOC_BOOT_ID_FILE DOC_UPTIME DOC_DEVUI_DIR
}
docenv() { # doctor.sh reading this sandbox
    export DOC_MANIFEST=$U60S_MANIFEST DOC_SHIP_TXN=$R/data/u60-ship/txn DOC_SHIP_HB=$T/tmp/heartbeat \
        DOC_MD5_CACHE=$T/md5cache DOC_TS_DIR=$R/data/tailscale DOC_TS_TUNING=$R/data/tailscale/tuning.env \
        DOC_INITD=$R/etc/init.d DOC_RC=$R/etc/rc.local DOC_BOOT_ID_FILE=$T/boot_id DOC_UPTIME=$T/uptime \
        DOC_DEVUI_DIR=$R/data/plugins/u60pro-devui
}
setup
CASE="record"
mkdir -p "$R/data/tailscale"
printf '#!/bin/sh\nexit 0\n' >"$R/etc/rc.local"
echo 'TS_X=1' >"$R/data/tailscale/tuning.env"
RCM=$(md5 "$R/etc/rc.local")
sh "$SHIP" record rc.local 1790000000 'edited by hand "on purpose"' >"$T/out" 2>&1
RC=$?
check "$CASE: one line, current md5" "[ \$RC = 0 ] && [ \"\$(mlines)\" = 1 ] && grep -q '\"kind\":\"record\",\"name\":\"rc.local\",\"path\":\"$R/etc/rc.local\",\"md5\":\"$RCM\",\"mac_time\":1790000000' '$U60S_MANIFEST'"
check "$CASE: the reason kept, quotes dropped" "grep -q '\"why\":\"edited by hand on purpose\"}' '$U60S_MANIFEST'"
sh "$SHIP" record nonsense 1790000000 >"$T/out" 2>&1
RC=$?
check "$CASE: unknown name refused" '[ $RC = 1 ] && grep -q "不认识" "$T/out" && [ "$(mlines)" = 1 ]'
sh "$SHIP" record rc.local 12a >"$T/out" 2>&1
RC=$?
check "$CASE: bad time refused" '[ $RC = 1 ] && [ "$(mlines)" = 1 ]'
sh "$SHIP" record all 1790000001 >"$T/out" 2>&1
RC=$?
NREC=$(sh "$SHIP" record all 1790000002 2>/dev/null | grep -c 已记录)
check "$CASE: all = one line per listed file, absent ones as -" "[ \$RC = 0 ] && [ \"\$NREC\" -ge 9 ] && grep -q '\"name\":\"tailscaled\",\"path\":\"$R/data/tailscale/tailscaled\",\"md5\":\"-\"' '$U60S_MANIFEST'"
docenv
sh "$SCRIPTS/doctor.sh" --manifest >"$T/man"
check "$CASE: doctor knows exactly the names ship records" '[ -z "$(grep -c unrecorded "$T/man" | grep -v "^0$")" ] && [ "$(grep -c "	record:" "$T/man")" = "$NREC" ]'
check "$CASE: doctor: consistent" '[ "$(tail -n 1 "$T/man")" = "state	ok	一致（$NREC 项）" ]'
echo '# changed' >>"$R/etc/rc.local"
check "$CASE: doctor sees the edit" 'sh "$SCRIPTS/doctor.sh" --manifest | tail -n 1 | grep -q "^state	warn	不一致的是 rc.local"'
sh "$SHIP" record rc.local 1790000003 accepted >/dev/null 2>&1
check "$CASE: recorded again: consistent again" 'sh "$SCRIPTS/doctor.sh" --manifest | tail -n 1 | grep -q "^state	ok	一致"'
printf 'v=1\ntxn=%s\ncomp=datad\nphase=check\nend=1\n' "$TXN" >"$R/data/u60-ship/txn"
sh "$SHIP" record rc.local 1790000004 >"$T/out" 2>&1
RC=$?
check "$CASE: refused during a transaction" '[ $RC = 1 ] && grep -q 还在进行 "$T/out"'
undocenv
teardown

setup
CASE="ship, then doctor"
upload
stage
run
docenv
sh "$SCRIPTS/doctor.sh" --manifest >"$T/man"
check "$CASE: the shipped file is in the manifest and matches" "grep -qx \"same	comp:datad	$LIVE	$NEW	$NEW\" '$T/man'"
check "$CASE: observing for an hour" 'tail -n 1 "$T/man" | grep -q "^state	ok	观察中（还剩 60 分钟）：datad 刚上机；一致（1 项）"'
undocenv
teardown

# ── prepare-rollback (u60 rollback) and the note in the manifest ──────────
setup
CASE="rollback"
T1=20260930-120000-datad T2=20260930-130000-datad T3=20260930-140000-datad
upload $T1; stage $T1; run $T1; A=$NEW
upload $T2 "note=requires-ignored"; sed -i 's/^commit=.*/commit=abcdef1/' "$R/data/u60-ship/stage/$T2/meta"; stage $T2; run $T2; B=$NEW
check "$CASE: two ships done, the second noted" '[ "$(phase)" = done ] && [ "$(md5 "$LIVE")" = "$B" ] && tail -n 1 "$U60S_MANIFEST" | grep -q "\"state\":\[.*\],\"note\":\"requires-ignored\"}$"'
sh "$SHIP" prepare-rollback datad $T3 1790000100 >"$T/out" 2>&1
RC=$?
check "$CASE: prepared from the last ship's .prev" "[ \$RC = 0 ] && [ \"\$(md5 '$R/data/u60-ship/stage/$T3/zwrt-datad')\" = '$A' ] && grep -qx 'commit=7e8d223' '$R/data/u60-ship/stage/$T3/meta' && grep -qx 'note=rollback' '$R/data/u60-ship/stage/$T3/meta'"
stage $T3
run $T3
check "$CASE: an ordinary transaction back to the first build" '[ "$(phase)" = done ] && [ "$(md5 "$LIVE")" = "$A" ] && tail -n 1 "$U60S_MANIFEST" | grep -q "\"commit\":\"7e8d223\".*\"note\":\"rollback\"}$"'
teardown

setup
CASE="rollback after one ship"
upload; stage; run
sh "$SHIP" prepare-rollback datad 20260930-140000-datad 1790000100 >"$T/out" 2>&1
check "$CASE: the hand-installed version, commit unknown" "[ \"\$(md5 '$R/data/u60-ship/stage/20260930-140000-datad/zwrt-datad')\" = '$OLD' ] && grep -qx 'commit=0000000' '$R/data/u60-ship/stage/20260930-140000-datad/meta'"
rm -f "$LIVE.prev-$TXN"
sh "$SHIP" prepare-rollback datad 20260930-150000-datad 1790000100 >"$T/out" 2>&1
RC=$?
check "$CASE: .prev gone: refused, nothing staged" '[ $RC = 1 ] && grep -q "不在了" "$T/out" && [ ! -e "$R/data/u60-ship/stage/20260930-150000-datad" ]'
teardown

setup
CASE="rollback refusals"
sh "$SHIP" prepare-rollback datad 20260930-140000-datad 1790000100 >"$T/out" 2>&1
RC=$?
check "$CASE: never shipped: refused" '[ $RC = 1 ] && grep -q "没有 datad 上机的记录" "$T/out"'
sh "$SHIP" prepare-rollback datad 20260930-140000-agent 1790000100 >"$T/out" 2>&1
RC=$?
check "$CASE: txn of another component: refused" '[ $RC = 1 ]'
upload "" "note=please"
stage
refused "bad note" "note “please” 看不懂"
teardown

# ── record-kit: the install kit's own line (T9) ────────────────────────────
setup
CASE="record-kit"
AM=$(md5 "$R/data/zte-agent")
sh "$SHIP" record-kit agent 20261001 abc1234def 1 1790000000 >"$T/out" 2>&1
RC=$?
check "$CASE: a kit line with the live md5" "[ \$RC = 0 ] && grep -q '^{\"v\":1,\"kind\":\"kit\",\"comp\":\"agent\",\"kit\":\"20261001\",\"commit\":\"abc1234def\",\"format\":1,\"mac_time\":1790000000,' '$U60S_MANIFEST' && grep -q '\"files\":\[{\"path\":\"$R/data/zte-agent\",\"md5\":\"$AM\"}\],\"state\":\[\]}\$' '$U60S_MANIFEST'"
sh "$SHIP" record-kit agent 20261001 abc1234def 1 1790000000 dirty >"$T/out" 2>&1
check "$CASE: dirty kit noted" 'tail -n 1 "$U60S_MANIFEST" | grep -q ",\"note\":\"kit-dirty\"}$"'
for badargs in "nope 20261001 abc1234 1 1790000000" "agent 2026 abc1234 1 1790000000" "agent 20261001 xyz 1 1790000000" "agent 20261001 abc1234 a 1790000000" "agent 20261001 abc1234 1 12" "agent 20261001 abc1234 1 1790000000 clean"; do
    sh "$SHIP" record-kit $badargs >"$T/out" 2>&1
    RC=$?
    check "$CASE: refused: $badargs" '[ $RC = 1 ] && [ "$(mlines)" = 2 ]'
done
docenv
check "$CASE: doctor reads kit lines" 'sh "$SCRIPTS/doctor.sh" --manifest | grep -q "^same	comp:agent	$R/data/zte-agent	$AM	$AM"'
undocenv
echo '{"v":1,"kind":"ship","comp":"agent","txn":"20260929-100000-agent","commit":"1111111","format":3,"files":[],"state":[]}' >"$U60S_MANIFEST"
sh "$SHIP" record-kit agent 20261001 abc1234 2 1790000000 >/dev/null 2>&1
upload 20260930-120000-agent
sed -i 's/^format=.*/format=2/' "$R/data/u60-ship/stage/20260930-120000-agent/meta"
stage 20260930-120000-agent
check "$CASE: the device's format is the kit's when it came last" '[ "$RC" = 0 ]'
rm -f "$R/data/u60-ship/txn"
sh "$SHIP" prepare-rollback agent 20260930-130000-agent 1790000100 >"$T/out" 2>&1
RC=$?
check "$CASE: rollback refused when the kit came last (no ship .prev)" '[ $RC = 1 ] && grep -q "装机包装的" "$T/out"'
printf 'v=1\ntxn=%s\ncomp=datad\nphase=check\nend=1\n' "$TXN" >"$R/data/u60-ship/txn"
sh "$SHIP" record-kit agent 20261001 abc1234 1 1790000000 >"$T/out" 2>&1
RC=$?
check "$CASE: refused during a transaction" '[ $RC = 1 ] && grep -q 还在进行 "$T/out"'
teardown

# ── selftest (what runs on the device before a new u60-ship.sh goes live) ───
setup
CASE="selftest"
unset U60S_ROOT U60S_TMP U60S_BOOT_ID U60S_DF U60S_MANIFEST DT_SLEEP DT_SETSID
timeout 60 sh "$SHIP" selftest >"$T/out" 2>&1
RC=$?
check "$CASE: PASS" '[ $RC = 0 ] && grep -q "selftest: PASS" "$T/out"'
teardown

echo "u60-ship: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
