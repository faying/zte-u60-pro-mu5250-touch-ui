#!/bin/sh
# ─────────────────────────────────────────────────────────────────────────────
# Tests for the phase-two components of u60-ship.sh (docs/SHIP.md「第二期」):
# touch (T12), uid (T13), web (T14), guard (T15), uid-restart (R8),
# install-recover, the v=2 transaction log, and what u60-recover.sh (new and
# the phase-one copy on the device) makes of every power cut in between.
# Every device command is stubbed, every device path lives under $T; after
# each case the container's real /data, /tmp and /etc/init.d are untouched.
#
#   docker run --rm -v "$PWD/scripts":/scripts:ro u60-device-busybox:20260928 \
#       sh /scripts/test/u60-ship-p2/run.sh
#
# Time is fake: $T/uptime is /proc/uptime; the stub `sleep` advances it, then
# runs $T/hook. /proc is $T/proc: <pid>/exe is a copy of the program at the
# moment the stub started it (like the kernel's view of a running binary),
# <pid>/cmdline as the kernel writes it.
# SPDX-License-Identifier: MIT
# ─────────────────────────────────────────────────────────────────────────────

SCRIPTS=${SCRIPTS:-/scripts}
SHIP=$SCRIPTS/u60-ship.sh
RECOVER=$SCRIPTS/u60-recover.sh
OLDREC=$SCRIPTS/test/fixtures/u60-recover.v1-2bb17b1.sh
PASS=0
FAIL=0

ok() { PASS=$((PASS + 1)); echo "  ok   $1"; }
bad() { FAIL=$((FAIL + 1)); echo "  FAIL $1"; }
check() { # check <description> <shell test…>
    _d=$1
    shift
    if eval "$@"; then ok "$_d"; else bad "$_d  [$*]"; fi
}

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
fp() { sh "$SHIP" tree-fp "$1"; }

leak_snapshot() { ls -la /data /tmp/u60-ship /tmp/u60-guard /tmp/u60-uid.log /etc/init.d 2>&1 | md5sum; }
LEAK0=$(leak_snapshot)

setup() {
    T=$(mktemp -d)
    R=$T/root
    DV=$R/data/plugins/u60pro-devui
    GD=$R/data/u60-guard
    mkdir -p "$T/bin" "$T/pids" "$T/tmp" "$T/proc" "$DV" "$GD" "$R/data/u60-ship/stage" "$R/etc/init.d" "$R/data/u60-uid"
    echo 1000 >"$T/uptime"
    echo aaaa-1111 >"$T/boot_id"
    : >"$T/uidlog"
    : >"$T/kill.log"
    : >"$T/setsid.log"
    : >"$T/uid-initd.log"
    : >"$T/guard-initd.log"
    printf ':\n' >"$T/hook"
    cp "$RECOVER" "$R/data/u60-ship/u60-recover.sh"

    cat >"$T/bin/sleep" <<EOF
#!/bin/sh
echo \$(( \$(cut -d. -f1 $T/uptime | tr -dc 0-9) + \${1%%.*} )) >$T/uptime
. $T/hook
EOF
    cat >"$T/bin/pidof" <<EOF
#!/bin/sh
cat $T/pids/"\$1" 2>/dev/null | grep . || exit 1
EOF
    # proc_new <pid> <program file> <cmdline words…>
    cat >"$T/bin/proc_new" <<EOF
#!/bin/sh
p=\$1; f=\$2; shift 2
mkdir -p $T/proc/\$p
cp "\$f" $T/proc/\$p/exe 2>/dev/null
printf '%s\0' "\$@" >$T/proc/\$p/cmdline
EOF
    cat >"$T/bin/kill" <<EOF
#!/bin/sh
echo "\$*" >>$T/kill.log
[ "\$1" = -9 ] && shift
for p in "\$@"; do
    for n in u60pro-devui.test u60pro-devui; do
        [ "\$p" = "\$(cat $T/pids/\$n 2>/dev/null)" ] || continue
        [ \$n = u60pro-devui.test ] && [ -f $T/unkillable ] && continue
        [ \$n = u60pro-devui ] && [ -f $T/ui-unkillable ] && continue
        rm -f $T/pids/\$n; rm -rf $T/proc/\$p
        echo "kill \$n \$(cut -d. -f1 $T/uptime)" >>$T/events
    done
done
exit 0
EOF
    # u60-uid under procd: stop leaves the UI on screen; start adopts a
    # running UI or launches one, counting launches like src/uid.c
    # (UID_MAX_ATTEMPTS 2: the third unsteady launch gives up)
    cat >"$T/bin/uid-initd" <<EOF
#!/bin/sh
echo "\$1 \$(cut -d. -f1 $T/uptime)" >>$T/uid-initd.log
case "\$1" in
    stop) rm -f $T/pids/u60-uid ;;
    start)
        [ -s $T/pids/u60-uid ] && exit 0
        # 9-26: u60-uid came up and was gone a second later
        [ -f $T/uid-dies-once ] && { rm -f $T/uid-dies-once; exit 0; }
        [ -f $T/uid-broken ] && grep -q NEW $DV/u60-uid && exit 0
        echo 8100 >$T/pids/u60-uid
        $T/bin/proc_new 8100 $DV/u60-uid $DV/u60-uid
        [ -s $T/pids/u60pro-devui ] && { echo "adopted running u60pro-devui pid \$(cat $T/pids/u60pro-devui)" >>$T/uidlog; exit 0; }
        [ -f $R/data/u60-uid/gave-up ] && exit 0
        a=\$(cat $R/data/u60-uid/attempts 2>/dev/null); a=\$(( \${a:-0} + 1 ))
        echo \$a >$R/data/u60-uid/attempts
        if [ \$a -gt 2 ]; then
            echo "giving up: \$a launches did not stay up; vendor UI on screen" >>$T/uidlog
            echo 1 >$R/data/u60-uid/gave-up
            exit 0
        fi
        if [ -f $T/ui-broken ] && grep -q NEW $DV/u60pro-devui; then
            echo "u60pro-devui pid 9999 ended: exit 1" >>$T/uidlog
            exit 0
        fi
        n=\$(( \$(cat $T/nextpid 2>/dev/null || echo 8200) + 1 )); echo \$n >$T/nextpid
        echo \$n >$T/pids/u60pro-devui
        $T/bin/proc_new \$n $DV/u60pro-devui $DV/u60pro-devui
        echo "launched u60pro-devui pid \$n (attempt \$a of 2 before giving up)" >>$T/uidlog
        echo "ui \$n \$(cut -d. -f1 $T/uptime)" >>$T/events
        ;;
esac
exit 0
EOF
    cat >"$T/bin/guard-initd" <<EOF
#!/bin/sh
echo "\$1 \$(cut -d. -f1 $T/uptime)" >>$T/guard-initd.log
case "\$1" in
    stop) rm -rf $T/proc/7300; rm -f $T/pids/guard ;;
    start)
        [ -d $T/proc/7300 ] && exit 0
        grep -q BROKEN $GD/u60-guard.sh && exit 0
        $T/bin/proc_new 7300 /bin/sh /bin/sh $GD/u60-guard.sh
        echo 7300 >$T/pids/guard
        ;;
esac
exit 0
EOF
    # procd's record of the guard instance (only the main loop's pid)
    cat >"$T/bin/ubus" <<EOF
#!/bin/sh
echo "\$*" >>$T/ubus.log
if [ -d $T/proc/7300 ]; then
    printf '{\n\t"u60-guard": {\n\t\t"instances": {\n\t\t\t"instance1": {\n\t\t\t\t"running": true,\n\t\t\t\t"pid": 7300,\n\t\t\t\t"command": [ "/bin/sh" ]\n\t\t\t}\n\t\t}\n\t}\n}\n'
else
    echo '{ }'
fi
EOF
    cat >"$T/bin/setsid" <<EOF
#!/bin/sh
echo "\$*" >>$T/setsid.log
case "\$*" in
    *u60pro-devui.test*)
        # nohup sh -c 'cd "\$1" && exec "\$2"' sh <dir> <program>
        for a in "\$@"; do dir=\$bin; bin=\$a; done
        echo "cwd=\$dir bin=\$bin" >>$T/launch.log
        [ -f $T/test-broken ] && exit 0
        echo 9100 >$T/pids/u60pro-devui.test
        $T/bin/proc_new 9100 "\$bin" "\$bin"
        echo "test 9100 \$(cut -d. -f1 $T/uptime)" >>$T/events
        ;;
    *u60-ship.sh\ run\ *) "\$@" ;;
esac
exit 0
EOF
    cat >"$T/bin/date" <<'EOF'
#!/bin/sh
echo "2026-10-01 12:00:00"
EOF
    cat >"$T/bin/df" <<EOF
#!/bin/sh
echo 'Filesystem 1024-blocks Used Available Capacity Mounted on'
echo "/dev/x 3000000 1000000 1100000 40% /data"
EOF
    # zte-agent serving /data/admin on :9090
    cat >"$T/bin/curl" <<EOF
#!/bin/sh
url=
for a in "\$@"; do case "\$a" in http*) url=\$a ;; esac; done
p=\${url#http://agent.test:9090}
[ -f $T/web-down ] && { printf 000; exit 7; }
[ -f $T/web-500 ] && grep -q NEW $R/data/admin/index.html 2>/dev/null && { printf 500; exit 0; }
case "\$p" in /) p=/index.html ;; esac
if [ -f "$R/data/admin\$p" ]; then printf 200; else printf 404; fi
EOF
    chmod +x "$T"/bin/*

    export U60S_ROOT=$R U60S_TMP=$T/tmp U60S_BOOT_ID=$T/boot_id U60S_DF=$T/bin/df U60S_PROC=$T/proc
    export U60S_MANIFEST=$R/data/u60-manifest.jsonl U60S_UID_INITD=$T/bin/uid-initd U60S_GUARD_INITD=$T/bin/guard-initd
    export U60S_UBUS=$T/bin/ubus
    export U60S_GUARD_STOPREQ=$T/guardtmp/stop-requested U60S_KMSG=$T/kmsg U60S_AGENT_URL=http://agent.test:9090 U60S_TO_DOCTOR=3
    unset U60S_CRASH_AT U60S_DIR U60S_LOG U60S_TEST_LOG U60S_MIN_FREE_KB U60S_TO_INITD U60S_UID_STATE
    export DT_UPTIME=$T/uptime DT_PIDFILE=$T/trial.pid DT_CURL=$T/bin/curl DT_PIDOF=$T/bin/pidof
    export DT_LOGREAD="cat $T/uidlog" DT_KILL=$T/bin/kill DT_SLEEP=$T/bin/sleep DT_DATE=$T/bin/date DT_SETSID=$T/bin/setsid
    export DT_RC=$T/rc.local DT_INITD=$T/bin/none
    export U60R_ROOT=$R
    unset U60R_DIR U60R_BOOT_ID

    # the screen as it is before every case: old u60-uid running, old UI on it
    printf 'OLD-UI\n' >"$DV/u60pro-devui"
    printf '#!/bin/sh\n# OLD start.sh\nexit 0\n' >"$DV/start.sh"
    printf 'OLD-UID\n' >"$DV/u60-uid"
    chmod 755 "$DV/u60pro-devui" "$DV/start.sh" "$DV/u60-uid"
    UI_OLD=$(md5 "$DV/u60pro-devui") SS_OLD=$(md5 "$DV/start.sh") UID_OLD=$(md5 "$DV/u60-uid")
    echo 8100 >"$T/pids/u60-uid"
    "$T/bin/proc_new" 8100 "$DV/u60-uid" "$DV/u60-uid"
    echo 8200 >"$T/pids/u60pro-devui"
    echo 8200 >"$T/nextpid"
    "$T/bin/proc_new" 8200 "$DV/u60pro-devui" "$DV/u60pro-devui"
    echo 1 >"$R/data/u60-uid/attempts"
}

teardown() {
    check "$CASE: nothing written outside the sandbox" '[ "$(leak_snapshot)" = "$LEAK0" ]'
    rm -rf "$T"
}

hook_at() { # hook_at <uptime> <shell command>: once, at that uptime or later
    cat >"$T/hook" <<EOF
[ \$(cut -d. -f1 $T/uptime) -ge $1 ] && [ ! -f $T/hook.done ] && { touch $T/hook.done; $2; }
:
EOF
}

stage() { sh "$SHIP" stage "$1" >"$T/out" 2>&1; RC=$?; }
run() { timeout 120 sh "$SHIP" run "$1" >"$T/out" 2>&1; RC=$?; }
phase() { sed -n 's/^phase=//p' "$R/data/u60-ship/txn" 2>/dev/null; }
reason() { sed -n 's/^reason=//p' "$R/data/u60-ship/txn" 2>/dev/null; }
txnv() { sed -n 's/^v=//p' "$R/data/u60-ship/txn" 2>/dev/null; }
logged() { grep -q "$1" "$R/data/u60-ship/ship.log"; }
mlines() { grep -c . "$U60S_MANIFEST" 2>/dev/null || echo 0; }
ui_md5() { md5sum <"$T/proc/$(cat "$T/pids/u60pro-devui" 2>/dev/null)/exe" 2>/dev/null | cut -d' ' -f1; }
uid_md5() { md5sum <"$T/proc/$(cat "$T/pids/u60-uid" 2>/dev/null)/exe" 2>/dev/null | cut -d' ' -f1; }
recover_newboot() { # recover_newboot [old]: rc.local's line after a power cut
    echo bbbb-2222 >"$T/newboot"
    if [ "$1" = old ]; then
        U60R_BOOT_ID=$T/newboot sh "$OLDREC" >"$T/rout" 2>&1
    else
        U60R_BOOT_ID=$T/newboot sh "$R/data/u60-ship/u60-recover.sh" >"$T/rout" 2>&1
    fi
    RRC=$?
}
# tree: the sandbox, byte for byte (recover.log aside: a recover that leaves
# things alone still writes its one line there)
tree() { find "$R" | grep -v "/u60-ship/recover\.log$" | sort; find "$R" -type f | grep -v "/u60-ship/recover\.log$" | sort | while read -r f; do md5sum "$f"; done; }
meta() { # meta <txn> <comp> [extra lines]
    {
        echo v=1
        echo "txn=$1"
        echo "comp=$2"
        echo commit=7e8d223
        echo mac_time=1790000000
        echo format=1
        [ -n "$3" ] && printf '%s\n' "$3"
    } >"$R/data/u60-ship/stage/$1/meta"
}

echo "== u60-ship phase two =="

# ═══ T12 touch ══════════════════════════════════════════════════════════════
TT=20261001-120000-touch
upload_touch() { # upload_touch [txn] [UI content]
    _t=${1:-$TT}
    mkdir -p "$R/data/u60-ship/stage/$_t"
    printf '%s\n' "${2:-NEW-UI $_t}" >"$R/data/u60-ship/stage/$_t/u60pro-devui"
    printf '#!/bin/sh\n# NEW start.sh %s\nexit 0\n' "$_t" >"$R/data/u60-ship/stage/$_t/start.sh"
    UI_NEW=$(md5 "$R/data/u60-ship/stage/$_t/u60pro-devui") SS_NEW=$(md5 "$R/data/u60-ship/stage/$_t/start.sh")
    meta "$_t" touch "file=u60pro-devui $UI_NEW${NL}file=start.sh $SS_NEW"
}
NL='
'

setup
CASE="ship touch"
upload_touch
stage $TT
check "$CASE: staged as v=1 (the phase-one u60-recover.sh reads it)" '[ "$RC" = 0 ] && [ "$(phase)" = staged ] && [ "$(txnv)" = 1 ]'
check "$CASE: both side files in place" '[ "$(md5 "$DV/u60pro-devui.test")" = "$UI_NEW" ] && [ -x "$DV/u60pro-devui.test" ] && [ "$(md5 "$DV/start.sh.test")" = "$SS_NEW" ]'
cat >"$T/hook" <<EOF
read -r hb _p _t ph <$T/tmp/heartbeat 2>/dev/null && echo "\$ph \$(( \$(cut -d. -f1 $T/uptime) - hb ))" >>$T/hb.trace
:
EOF
run $TT
check "$CASE: done" '[ "$RC" = 0 ] && [ "$(phase)" = done ]'
check "$CASE: live files are the new ones, previous kept" '[ "$(md5 "$DV/u60pro-devui")" = "$UI_NEW" ] && [ "$(md5 "$DV/start.sh")" = "$SS_NEW" ] && [ "$(md5 "$DV/u60pro-devui.prev-$TT")" = "$UI_OLD" ] && [ "$(md5 "$DV/start.sh.prev-$TT")" = "$SS_OLD" ]'
check "$CASE: the test build ran from the plugin directory, as u60-uid runs the UI" "grep -qx 'cwd=$DV bin=$DV/u60pro-devui.test' '$T/launch.log' && grep -q 'nohup sh -c cd \"\$1\" && exec \"\$2\" sh $DV $DV/u60pro-devui.test' '$T/setsid.log'"
check "$CASE: u60-uid stopped for the trial, started once after promotion" '[ "$(cut -d" " -f1 "$T/uid-initd.log" | tr "\n" " ")" = "stop start " ]'
check "$CASE: the old UI was killed by pid, the test build after the trial" "grep -qx '8200' '$T/kill.log' && grep -qx '9100' '$T/kill.log'"
check "$CASE: the new UI runs (exe md5), u60-uid runs" '[ "$(ui_md5)" = "$UI_NEW" ] && [ "$(uid_md5)" = "$UID_OLD" ] && [ ! -s "$T/pids/u60pro-devui.test" ]'
check "$CASE: launch count cleared before the start (1 = this launch)" '[ "$(cat "$R/data/u60-uid/attempts")" = 1 ] && [ ! -e "$R/data/u60-uid/gave-up" ]'
check "$CASE: trial heartbeat every second" '[ "$(grep -c "^trial " "$T/hb.trace")" -ge 60 ] && [ -z "$(awk "\$1 == \"trial\" && \$2 > 1" "$T/hb.trace")" ]'
check "$CASE: manifest line with both files" "grep -q '\"comp\":\"touch\",\"txn\":\"$TT\"' '$U60S_MANIFEST' && grep -q '\"path\":\"$DV/start.sh\",\"md5\":\"$SS_NEW\"' '$U60S_MANIFEST'"
check "$CASE: no side file left" '[ ! -e "$DV/u60pro-devui.test" ] && [ ! -e "$DV/start.sh.test" ]'
teardown

setup
CASE="touch: test build crashes 5 s in"
upload_touch
stage $TT
hook_at 1005 "rm -f $T/pids/u60pro-devui.test; rm -rf $T/proc/9100; echo \"crash \$(cut -d. -f1 $T/uptime)\" >>$T/events"
run $TT
CRASH=$(sed -n 's/^crash //p' "$T/events")
START=$(awk '$1 == "start" { print $2; exit }' "$T/uid-initd.log")
check "$CASE: aborted, says why" '[ "$RC" = 1 ] && [ "$(phase)" = aborted ] && reason | grep -q "^试跑不过：测试版崩溃"'
check "$CASE: production started ≤ 1 s after the crash (crash $CRASH, u60-uid start $START)" '[ -n "$CRASH" ] && [ -n "$START" ] && [ $((START - CRASH)) -le 1 ]'
check "$CASE: the old UI runs again, u60-uid started once" '[ "$(ui_md5)" = "$UI_OLD" ] && [ "$(grep -c "^start" "$T/uid-initd.log")" = 1 ] && [ -s "$T/pids/u60-uid" ]'
check "$CASE: live untouched, no .prev, no side files" '[ "$(md5 "$DV/u60pro-devui")" = "$UI_OLD" ] && [ ! -e "$DV/u60pro-devui.prev-$TT" ] && [ ! -e "$DV/u60pro-devui.test" ] && [ ! -e "$DV/start.sh.test" ]'
check "$CASE: manifest untouched" '[ "$(mlines)" = 0 ]'
teardown

setup
CASE="touch: test build never shows (screen dark > 20 s)"
touch "$T/test-broken"
upload_touch
stage $TT
run $TT
KILL_AT=$(awk '$1 == "kill" && $2 == "u60pro-devui" { print $3; exit }' "$T/events")
START=$(awk '$1 == "start" { print $2; exit }' "$T/uid-initd.log")
check "$CASE: aborted after 20 s of an empty screen" '[ "$(phase)" = aborted ] && reason | grep -q "屏幕空了 20s"'
check "$CASE: production back ≤ 21 s after the old UI went (gone $KILL_AT, start $START)" '[ $((START - KILL_AT)) -le 21 ] && [ "$(ui_md5)" = "$UI_OLD" ]'
teardown

setup
CASE="touch: three ships in 10 minutes"
for n in 1 2 3; do
    t=2026100${n}-120000-touch
    upload_touch "$t"
    stage "$t"
    run "$t"
    [ "$(phase)" = done ] && ok "$CASE: ship $n done" || bad "$CASE: ship $n: $(phase) $(reason)"
done
check "$CASE: all within 10 minutes of fake time ($(($(cat "$T/uptime") - 1000)) s)" '[ $(($(cat "$T/uptime") - 1000)) -lt 600 ]'
check "$CASE: u60-uid never gave up, count back to 1 each time" '! grep -q "^giving up" "$T/uidlog" && [ ! -e "$R/data/u60-uid/gave-up" ] && [ "$(cat "$R/data/u60-uid/attempts")" = 1 ]'
check "$CASE: the third build runs" '[ "$(ui_md5)" = "$UI_NEW" ]'
# the stub counts like u60-uid: without the clearing, the third launch gives up
rm -f "$T/pids/u60pro-devui" "$T/pids/u60-uid"
"$T/bin/uid-initd" start; rm -f "$T/pids/u60pro-devui" "$T/pids/u60-uid"
"$T/bin/uid-initd" start; rm -f "$T/pids/u60pro-devui" "$T/pids/u60-uid"
"$T/bin/uid-initd" start
check "$CASE: (control) the stub gives up on its third uncleared launch" 'grep -q "^giving up" "$T/uidlog"'
teardown

setup
CASE="touch: new UI restarted during the check"
upload_touch
stage $TT
cat >"$T/hook" <<EOF
if grep -qx phase=check $R/data/u60-ship/txn && [ ! -f $T/once ]; then
    touch $T/once; echo 8300 >$T/pids/u60pro-devui; $T/bin/proc_new 8300 $DV/u60pro-devui $DV/u60pro-devui
fi
:
EOF
run $TT
check "$CASE: rolled back, says why" '[ "$(phase)" = rolledback ] && reason | grep -q "^转正后检查不过：界面被重启过"'
check "$CASE: old files back, old UI on screen" '[ "$(md5 "$DV/u60pro-devui")" = "$UI_OLD" ] && [ "$(md5 "$DV/start.sh")" = "$SS_OLD" ] && [ "$(ui_md5)" = "$UI_OLD" ]'
teardown

setup
CASE="touch: u60-uid gives up during the check"
upload_touch
stage $TT
cat >"$T/hook" <<EOF
grep -qx phase=check $R/data/u60-ship/txn && [ ! -f $T/once ] && { touch $T/once; echo "giving up: 2 launches did not stay up" >>$T/uidlog; }
:
EOF
run $TT
check "$CASE: rolled back" '[ "$(phase)" = rolledback ] && reason | grep -q "界面异常（u60-uid：giving up"'
teardown

setup
CASE="touch: new UI will not stay up"
touch "$T/ui-broken"
upload_touch "" "NEW-UI but it crashes"
stage $TT
cat >"$T/hook" <<EOF
# the test build crashes too, but only after the trial window
:
EOF
run $TT
check "$CASE: rolled back (new version will not start)" '[ "$(phase)" = rolledback ] && reason | grep -q "^新版起不来：u60-uid 两次 start 都没起好"'
check "$CASE: old UI running at the end" '[ "$(md5 "$DV/u60pro-devui")" = "$UI_OLD" ] && [ "$(ui_md5)" = "$UI_OLD" ]'
teardown

setup
CASE="touch: test build unkillable"
touch "$T/unkillable"
upload_touch
stage $TT
run $TT
check "$CASE: failed; production NOT started next to it (two UIs = reboot)" '[ "$(phase)" = failed ] && ! grep -q "^start" "$T/uid-initd.log"'
check "$CASE: live untouched" '[ "$(md5 "$DV/u60pro-devui")" = "$UI_OLD" ]'
teardown

setup
CASE="touch: old UI will not exit"
touch "$T/ui-unkillable"
upload_touch
stage $TT
run $TT
check "$CASE: aborted, test build never launched, u60-uid started again" '[ "$(phase)" = aborted ] && reason | grep -q "正式界面 10s 内没有退出" && [ ! -s "$T/launch.log" ] && grep -q "^start" "$T/uid-initd.log"'
teardown

setup
CASE="touch: the test build is still running"
echo 9100 >"$T/pids/u60pro-devui.test"
upload_touch
stage $TT
check "$CASE: stage refused" '[ "$RC" = 1 ] && grep -q "测试版 u60pro-devui.test 还在跑" "$T/out" && [ ! -e "$R/data/u60-ship/txn" ]'
teardown

setup
CASE="touch: start.sh with a syntax error"
upload_touch
printf 'if then\n' >"$R/data/u60-ship/stage/$TT/start.sh"
sed -i "s/^file=start.sh .*/file=start.sh $(md5 "$R/data/u60-ship/stage/$TT/start.sh")/" "$R/data/u60-ship/stage/$TT/meta"
stage $TT
check "$CASE: stage refused" '[ "$RC" = 1 ] && grep -q "start.sh 有语法错误" "$T/out" && [ ! -e "$DV/start.sh.test" ]'
teardown

# power cuts: touch logs are v=1, so the new and the old recover agree
for p in trial:running promote:moved:u60pro-devui promote:moved:start.sh check:begin; do
    for which in new old; do
        setup
        CASE="touch: power cut at $p, $which u60-recover.sh"
        upload_touch
        stage $TT
        U60S_CRASH_AT=$p run $TT
        recover_newboot $which
        case "$p" in
            trial:*) check "$CASE: aborted, live old" '[ "$(phase)" = aborted ] && [ "$(md5 "$DV/u60pro-devui")" = "$UI_OLD" ]' ;;
            *) check "$CASE: rolled back, both files old" '[ "$(phase)" = rolledback ] && [ "$(md5 "$DV/u60pro-devui")" = "$UI_OLD" ] && [ "$(md5 "$DV/start.sh")" = "$SS_OLD" ]' ;;
        esac
        teardown
    done
done

setup
CASE="touch: recover-live after kill -9 in the check"
upload_touch
stage $TT
U60S_CRASH_AT=check:begin run $TT
: >"$T/uid-initd.log"
sh "$SHIP" recover-live >"$T/out" 2>&1
check "$CASE: rolled back, u60-uid restarted on the old UI" '[ "$(phase)" = rolledback ] && [ "$(md5 "$DV/u60pro-devui")" = "$UI_OLD" ] && grep -q "^start" "$T/uid-initd.log" && [ -s "$T/pids/u60-uid" ]'
teardown

setup
CASE="touch: recover-live after kill -9 in the trial"
upload_touch
stage $TT
U60S_CRASH_AT=trial:running run $TT
sh "$SHIP" recover-live >"$T/out" 2>&1
check "$CASE: aborted, test build killed, the old UI back" '[ "$(phase)" = aborted ] && [ ! -s "$T/pids/u60pro-devui.test" ] && [ "$(ui_md5)" = "$UI_OLD" ]'
teardown

# (the touch launch vs src/uid.c: scripts/test/cross-repo, on the host)

# pidof: the two names do not see each other (real busybox pidof, real
# processes: comm u60pro-devui vs u60pro-devui.te, argv[1] …/u60pro-devui.test)
CASE="pidof names"
P=$(mktemp -d)
printf '#!/bin/sh\nwhile :; do %s 1; done\n' "$_real_sleep" >"$P/u60pro-devui"
cp "$P/u60pro-devui" "$P/u60pro-devui.test"
chmod 755 "$P/u60pro-devui" "$P/u60pro-devui.test"
"$P/u60pro-devui" &
A=$!
"$P/u60pro-devui.test" &
B=$!
"$_real_sleep" 0.3
check "$CASE: pidof u60pro-devui = the live one only" '[ "$(pidof u60pro-devui)" = "$A" ]'
check "$CASE: pidof u60pro-devui.test = the test build only" '[ "$(pidof u60pro-devui.test)" = "$B" ]'
check "$CASE: the test build's comm is cut to 15 characters" '[ "$(cat /proc/$B/comm)" = u60pro-devui.te ]'
kill $A $B 2>/dev/null
wait $A $B 2>/dev/null
rm -rf "$P"

# ═══ uid-restart (R8; the install kit calls it too) ═════════════════════════
setup
CASE="uid-restart"
sh "$SHIP" uid-restart >"$T/out" 2>&1
RC=$?
check "$CASE: exit 0, stop then start, versions as on disk" '[ $RC = 0 ] && [ "$(cut -d" " -f1 "$T/uid-initd.log" | tr "\n" " ")" = "stop start " ] && grep -q "版本对" "$T/out"'
check "$CASE: launch count and give-up cleared" '[ ! -e "$R/data/u60-uid/attempts" ] && [ ! -e "$R/data/u60-uid/gave-up" ]'
check "$CASE: the UI on screen was adopted, not restarted" '[ "$(cat "$T/pids/u60pro-devui")" = 8200 ]'
sh "$SHIP" uid-restart "$UI_OLD" "$UID_OLD" >"$T/out" 2>&1
RC=$?
check "$CASE: explicit md5s: exit 0" '[ $RC = 0 ]'
: >"$T/uid-initd.log"
sh "$SHIP" uid-restart 0123456789abcdef0123456789abcdef >"$T/out" 2>&1
RC=$?
check "$CASE: another UI version wanted: start twice, exit 1, says why" '[ $RC = 1 ] && [ "$(grep -c "^start" "$T/uid-initd.log")" = 2 ] && grep -q "版本不对" "$T/out"'
sh "$SHIP" uid-restart nothex >"$T/out" 2>&1
RC=$?
check "$CASE: a bad md5 refused" '[ $RC = 1 ] && grep -q "不是 md5" "$T/out"'
rm -f "$T/pids/u60pro-devui" "$T/pids/u60-uid"
echo 5 >"$R/data/u60-uid/attempts"
echo 1 >"$R/data/u60-uid/gave-up"
sh "$SHIP" uid-restart >"$T/out" 2>&1
RC=$?
check "$CASE: after a give-up: cleared, UI launched" '[ $RC = 0 ] && [ -s "$T/pids/u60pro-devui" ] && [ "$(cat "$R/data/u60-uid/attempts")" = 1 ]'
printf 'v=1\ntxn=%s\ncomp=datad\nphase=check\nend=1\n' 20261001-110000-datad >"$R/data/u60-ship/txn"
sh "$SHIP" uid-restart >"$T/out" 2>&1
RC=$?
check "$CASE: refused while a transaction is in progress" '[ $RC = 1 ] && grep -q "还在进行" "$T/out"'
teardown

setup
CASE="uid-restart: u60-uid comes up but leaves again"
touch "$T/uid-dies-once"
rm -f "$T/pids/u60-uid"
sh "$SHIP" uid-restart >"$T/out" 2>&1
RC=$?
check "$CASE: started again, exit 0" '[ $RC = 0 ] && [ "$(grep -c "^start" "$T/uid-initd.log")" = 2 ] && [ -s "$T/pids/u60-uid" ]'
teardown

# ═══ T13 uid ════════════════════════════════════════════════════════════════
TU=20261001-120000-uid
upload_uid() {
    mkdir -p "$R/data/u60-ship/stage/$TU"
    printf 'NEW-UID %s\n' "$1" >"$R/data/u60-ship/stage/$TU/u60-uid"
    UID_NEW=$(md5 "$R/data/u60-ship/stage/$TU/u60-uid")
    meta "$TU" uid "file=u60-uid $UID_NEW"
}

setup
CASE="ship uid"
upload_uid
stage $TU
run $TU
check "$CASE: done, v=1" '[ "$RC" = 0 ] && [ "$(phase)" = done ] && [ "$(txnv)" = 1 ]'
check "$CASE: the new u60-uid runs, the UI was adopted (not restarted)" '[ "$(uid_md5)" = "$UID_NEW" ] && [ "$(cat "$T/pids/u60pro-devui")" = 8200 ] && [ "$(ui_md5)" = "$UI_OLD" ]'
check "$CASE: stop, then start" '[ "$(cut -d" " -f1 "$T/uid-initd.log" | tr "\n" " ")" = "stop start " ]'
check "$CASE: previous kept" '[ "$(md5 "$DV/u60-uid.prev-$TU")" = "$UID_OLD" ]'
teardown

setup
CASE="uid: new u60-uid will not start"
touch "$T/uid-broken"
upload_uid
stage $TU
run $TU
check "$CASE: rolled back, the old u60-uid runs" '[ "$(phase)" = rolledback ] && reason | grep -q "^新版起不来" && [ "$(md5 "$DV/u60-uid")" = "$UID_OLD" ] && [ "$(uid_md5)" = "$UID_OLD" ]'
check "$CASE: the UI stayed on screen all along" '[ "$(cat "$T/pids/u60pro-devui")" = 8200 ]'
teardown

setup
CASE="uid: u60-uid gone during the check"
upload_uid
stage $TU
cat >"$T/hook" <<EOF
grep -qx phase=check $R/data/u60-ship/txn && [ ! -f $T/once ] && { touch $T/once; rm -f $T/pids/u60-uid; }
:
EOF
run $TU
check "$CASE: rolled back, the old u60-uid runs" '[ "$(phase)" = rolledback ] && reason | grep -q "u60-uid 不在了" && [ "$(uid_md5)" = "$UID_OLD" ]'
teardown

for p in promote:moved:u60-uid check:begin; do
    setup
    CASE="uid: power cut at $p"
    upload_uid
    stage $TU
    U60S_CRASH_AT=$p run $TU
    recover_newboot
    check "$CASE: rolled back, old file" '[ "$(phase)" = rolledback ] && [ "$(md5 "$DV/u60-uid")" = "$UID_OLD" ]'
    teardown
done

setup
CASE="uid: prepare-rollback"
upload_uid
stage $TU
run $TU
sh "$SHIP" prepare-rollback uid 20261001-130000-uid 1790000100 >"$T/out" 2>&1
stage 20261001-130000-uid
run 20261001-130000-uid
check "$CASE: back to the version before, by an ordinary transaction" '[ "$(phase)" = done ] && [ "$(md5 "$DV/u60-uid")" = "$UID_OLD" ] && [ "$(uid_md5)" = "$UID_OLD" ]'
teardown

# ═══ T14 web ════════════════════════════════════════════════════════════════
TW=20261001-120000-web
web_old() {
    mkdir -p "$R/data/admin/_next/static/chunks"
    printf 'OLD index\n' >"$R/data/admin/index.html"
    printf 'old js\n' >"$R/data/admin/_next/static/chunks/a.js"
    printf 'old page\n' >"$R/data/admin/esim page.html"
    WEB_OLD=$(fp "$R/data/admin")
    # hand-made backups that must stay
    echo keep >"$R/data/admin.prev-reset-20260926.tgz"
    mkdir -p "$R/data/admin.old-main"
    echo keep >"$R/data/admin.old-main/index.html"
}
upload_web() { # upload_web [txn] [variant: ok|nojs|link|dotdot]
    _t=${1:-$TW}
    _w=$T/webnew
    rm -rf "$_w"
    mkdir -p "$_w/_next/static/chunks" "$R/data/u60-ship/stage/$_t"
    printf 'NEW index %s\n' "$_t" >"$_w/index.html"
    printf 'new js\n' >"$_w/_next/static/chunks/b c.js"
    printf 'new page\n' >"$_w/esim page.html"
    case "$2" in
        nojs) rm -f "$_w/_next/static/chunks/b c.js" ;;
        link) ln -s index.html "$_w/link.html" ;;
    esac
    WEB_NEW=$(fp "$_w")
    [ -n "$WEB_NEW" ] || WEB_NEW=0123456789abcdef0123456789abcdef
    if [ "$2" = dotdot ]; then
        # busybox tar drops a leading ../ when it creates an archive: write
        # "zzevil" and patch the name to "../evi" (and the header checksum)
        mkdir -p "$T/dd"
        printf 'x\n' >"$T/dd/zzevil"
        (cd "$T/dd" && tar -cf "$T/dd.tar" zzevil)
        printf '../evi' | dd of="$T/dd.tar" bs=1 seek=0 conv=notrunc 2>/dev/null
        printf '        ' | dd of="$T/dd.tar" bs=1 seek=148 conv=notrunc 2>/dev/null
        _cs=$(od -An -tu1 -v -N512 "$T/dd.tar" | awk '{ for (i = 1; i <= NF; i++) s += $i } END { printf "%06o", s }')
        printf '%s\000 ' "$_cs" | dd of="$T/dd.tar" bs=1 seek=148 conv=notrunc 2>/dev/null
        gzip -c "$T/dd.tar" >"$R/data/u60-ship/stage/$_t/admin.tgz"
    else
        (cd "$_w" && tar -czf "$R/data/u60-ship/stage/$_t/admin.tgz" .)
    fi
    meta "$_t" web "dir=admin $WEB_NEW $(md5 "$R/data/u60-ship/stage/$_t/admin.tgz")"
}

setup
CASE="ship web"
web_old
upload_web
stage $TW
check "$CASE: staged as v=2 with a dir= line" '[ "$RC" = 0 ] && [ "$(txnv)" = 2 ] && grep -qx "dir=$R/data/admin|$WEB_OLD|$WEB_NEW" "$R/data/u60-ship/txn"'
check "$CASE: unpacked next to the live one, the upload removed" '[ "$(fp "$R/data/admin.test")" = "$WEB_NEW" ] && [ ! -e "$R/data/u60-ship/stage/$TW/admin.tgz" ]'
run $TW
check "$CASE: done" '[ "$RC" = 0 ] && [ "$(phase)" = done ]'
check "$CASE: live is the new tree, the old one is the .prev" '[ "$(fp "$R/data/admin")" = "$WEB_NEW" ] && [ "$(fp "$R/data/admin.prev-$TW")" = "$WEB_OLD" ] && [ ! -e "$R/data/admin.test" ]'
check "$CASE: checked / and the first _next/static js" "logged '网页检查（/ 和 /_next/static/chunks/b c.js）'"
check "$CASE: zte-agent not restarted (nothing stopped or started)" '[ ! -s "$T/uid-initd.log" ] && [ ! -s "$T/guard-initd.log" ]'
check "$CASE: manifest directory entry" "grep -q '\"files\":\[{\"path\":\"$R/data/admin\",\"tree\":\"$WEB_NEW\",\"prev\":\"$R/data/admin.prev-$TW\"}\]' '$U60S_MANIFEST'"
check "$CASE: hand-made backups untouched" '[ -f "$R/data/admin.prev-reset-20260926.tgz" ] && [ -f "$R/data/admin.old-main/index.html" ]'
export DOC_MANIFEST=$U60S_MANIFEST DOC_SHIP_TXN=$R/data/u60-ship/txn DOC_SHIP_HB=$T/tmp/heartbeat DOC_MD5_CACHE=$T/md5cache DOC_BOOT_ID_FILE=$T/boot_id DOC_UPTIME=$T/uptime DOC_DEVUI_DIR=$DV
sh "$SCRIPTS/doctor.sh" --manifest >"$T/man"
check "$CASE: doctor: the directory matches" "grep -qx \"same	comp:web	$R/data/admin	$WEB_NEW	$WEB_NEW\" '$T/man'"
unset DOC_MANIFEST DOC_SHIP_TXN DOC_SHIP_HB DOC_MD5_CACHE DOC_BOOT_ID_FILE DOC_UPTIME DOC_DEVUI_DIR
teardown

setup
CASE="web: the phase-one u60-recover.sh on the device"
web_old
cp "$OLDREC" "$R/data/u60-ship/u60-recover.sh"
upload_web
stage $TW
check "$CASE: stage refused, asks for install-recover" '[ "$RC" = 1 ] && grep -q "先装新版 u60-recover.sh（u60 install-recover，要用户同意）" "$T/out" && [ ! -e "$R/data/admin.test" ] && [ ! -e "$R/data/u60-ship/txn" ]'
rm -f "$R/data/u60-ship/u60-recover.sh"
stage $TW
check "$CASE: no u60-recover.sh at all: refused too" '[ "$RC" = 1 ] && grep -q "先装新版 u60-recover.sh" "$T/out"'
teardown

wrefused() { # wrefused <case> <text>
    CASE=$1
    check "$1: refused, says why" "[ \"\$RC\" = 1 ] && grep -q -- '$2' '$T/out'"
    check "$1: live untouched, no side directory, no log" '[ "$(fp "$R/data/admin")" = "$WEB_OLD" ] && [ ! -e "$R/data/admin.test" ] && [ ! -e "$R/data/u60-ship/txn" ]'
}
setup; web_old; upload_web; sed -i 's/^dir=admin \([0-9a-f]*\) .*/dir=admin \1 0123456789abcdef0123456789abcdef/' "$R/data/u60-ship/stage/$TW/meta"; stage $TW
wrefused "web: tgz md5 wrong" "admin.tgz 的 md5 不对"; teardown
setup; web_old; upload_web; sed -i 's/^dir=admin [0-9a-f]* /dir=admin 0123456789abcdef0123456789abcdef /' "$R/data/u60-ship/stage/$TW/meta"; stage $TW
wrefused "web: fingerprint after unpacking wrong" "解开以后指纹不对"; teardown
setup; web_old; upload_web "" link; stage $TW
wrefused "web: a symlink in the archive" "里面有链接或特殊文件"; teardown
setup; web_old; upload_web "" dotdot; stage $TW
wrefused "web: .. in the archive" "绝对路径或 .."
check "$CASE: nothing written next to the stage directory" '[ ! -e "$R/data/u60-ship/evil" ] && [ ! -e "$R/data/u60-ship/stage/evil" ]'
teardown
setup; web_old; upload_web; sed -i '/^dir=/d' "$R/data/u60-ship/stage/$TW/meta"; stage $TW
wrefused "web: dir= missing" "没有 dir=admin"; teardown
setup; web_old; upload_web; echo "dir=other 0123456789abcdef0123456789abcdef 0123456789abcdef0123456789abcdef" >>"$R/data/u60-ship/stage/$TW/meta"; stage $TW
wrefused "web: another directory" "dir=other 不是组件 web 的目录"; teardown
setup; web_old; ln -s index.html "$R/data/admin/l"; WEB_OLD=$(fp "$R/data/admin"); upload_web; stage $TW
CASE="web: a symlink in the live directory"
check "$CASE: refused" '[ "$RC" = 1 ] && grep -q "算不了指纹" "$T/out"'
teardown
setup; upload_web; stage $TW
CASE="web: no live directory (first install)"
check "$CASE: refused" '[ "$RC" = 1 ] && grep -q "第一次安装走装机包" "$T/out"'
teardown

setup
CASE="web: the new pages have no _next js"
web_old
upload_web "" nojs
stage $TW
run $TW
check "$CASE: rolled back, the old tree live again" '[ "$(phase)" = rolledback ] && reason | grep -q "没有 _next/static/\*.js" && [ "$(fp "$R/data/admin")" = "$WEB_OLD" ]'
check "$CASE: no .rec-tmp, .bad or side directory left" '[ ! -e "$R/data/admin.rec-tmp" ] && [ ! -e "$R/data/admin.bad-$TW" ] && [ ! -e "$R/data/admin.test" ]'
teardown

setup
CASE="web: the new pages answer 500"
web_old
touch "$T/web-500"
upload_web "" ""
sed -i 's/^NEW/NEW/' "$R/data/u60-ship/stage/$TW/meta"
stage $TW
run $TW
check "$CASE: rolled back" '[ "$(phase)" = rolledback ] && reason | grep -q "网页首页 / 返回 500" && [ "$(fp "$R/data/admin")" = "$WEB_OLD" ]'
teardown

setup
CASE="web: :9090 stops answering in the check window"
web_old
upload_web
stage $TW
hook_at 1050 "touch $T/web-down"
run $TW
check "$CASE: rolled back" '[ "$(phase)" = rolledback ] && reason | grep -q "返回 000" && [ "$(fp "$R/data/admin")" = "$WEB_OLD" ]'
teardown

# T14: a power cut at every step of the two renames and of the rollback;
# the new u60-recover.sh puts the old tree back, the old one leaves it alone
for p in promote:begin promote:dir-away:admin promote:dir-away-synced:admin promote:dir-moved:admin promote:dir-moved-synced:admin check:begin \
    rollback:begin rollback:dir-copied:admin rollback:dir-bad:admin rollback:dir-moved:admin; do
    setup
    CASE="web: power cut at $p"
    web_old
    case "$p" in rollback:*) upload_web "" nojs ;; *) upload_web ;; esac
    stage $TW
    U60S_CRASH_AT=$p run $TW
    check "$CASE: executor died there" '[ "$RC" = 137 ]'
    PH=$(phase)
    tree >"$T/t0"
    recover_newboot old
    tree >"$T/t1"
    check "$CASE: the phase-one u60-recover.sh touches nothing" '[ "$RRC" = 0 ] && cmp -s "$T/t0" "$T/t1" && [ "$(phase)" = "$PH" ]'
    recover_newboot
    check "$CASE: new u60-recover.sh: rolled back, the old tree by fingerprint" '[ "$(phase)" = rolledback ] && [ "$(fp "$R/data/admin")" = "$WEB_OLD" ]'
    check "$CASE: no .rec-tmp or .bad left" '[ ! -e "$R/data/admin.rec-tmp" ] && [ ! -e "$R/data/admin.bad-$TW" ]'
    tree >"$T/t2"
    recover_newboot
    tree >"$T/t3"
    check "$CASE: again: nothing changes" 'cmp -s "$T/t2" "$T/t3"'
    teardown
done

setup
CASE="web: power cut at manifest:begin"
web_old
upload_web
stage $TW
U60S_CRASH_AT=manifest:begin run $TW
recover_newboot
check "$CASE: manifest_pending, the new tree stays" '[ "$(phase)" = manifest_pending ] && [ "$(fp "$R/data/admin")" = "$WEB_NEW" ]'
teardown

setup
CASE="web: recover-live after kill -9 between the renames"
web_old
upload_web
stage $TW
U60S_CRASH_AT=promote:dir-away-synced:admin run $TW
check "$CASE: (no live directory at this point)" '[ ! -e "$R/data/admin" ]'
sh "$SHIP" recover-live >"$T/out" 2>&1
check "$CASE: rolled back, nothing restarted" '[ "$(phase)" = rolledback ] && [ "$(fp "$R/data/admin")" = "$WEB_OLD" ] && [ ! -s "$T/uid-initd.log" ] && [ ! -s "$T/guard-initd.log" ]'
teardown

setup
CASE="web: keep 3 ship .prev directories"
web_old
for d in 01 02 03; do
    mkdir -p "$R/data/admin.prev-202609${d}-000000-web"
    echo "$d" >"$R/data/admin.prev-202609${d}-000000-web/index.html"
done
upload_web
stage $TW
run $TW
check "$CASE: done" '[ "$(phase)" = done ]'
check "$CASE: three newest .prev directories left" '[ "$(ls "$R/data" | grep -c "^admin\.prev-20")" = 3 ] && [ ! -e "$R/data/admin.prev-20260901-000000-web" ] && [ -d "$R/data/admin.prev-$TW" ]'
check "$CASE: hand-made backups untouched" '[ -f "$R/data/admin.prev-reset-20260926.tgz" ] && [ -d "$R/data/admin.old-main" ]'
teardown

setup
CASE="web: prepare-rollback"
web_old
upload_web
stage $TW
run $TW
sh "$SHIP" prepare-rollback web 20261001-130000-web 1790000100 >"$T/out" 2>&1
RC=$?
check "$CASE: prepared: a tgz of the .prev and its fingerprint" "[ \$RC = 0 ] && grep -q '^dir=admin $WEB_OLD ' '$R/data/u60-ship/stage/20261001-130000-web/meta'"
stage 20261001-130000-web
run 20261001-130000-web
check "$CASE: back to the old tree by an ordinary transaction" '[ "$(phase)" = done ] && [ "$(fp "$R/data/admin")" = "$WEB_OLD" ]'
teardown

setup
CASE="web: record-kit"
web_old
sh "$SHIP" record-kit web 20261001 abc1234 1 1790000000 >"$T/out" 2>&1
RC=$?
check "$CASE: a kit line with the tree" "[ \$RC = 0 ] && grep -q '\"files\":\[{\"path\":\"$R/data/admin\",\"tree\":\"$WEB_OLD\"}\]' '$U60S_MANIFEST'"
teardown

# ═══ T15 guard ══════════════════════════════════════════════════════════════
TG=20261001-120000-guard
GFILES="alert-lib.sh u60-guard.sh supervise.sh agent-auth.sh chaos.sh doctor.sh config-backup.sh power-sample.sh wan-sources.sh wifi-ab.sh u60-fallback.sh zte-agent.init zwrt-datad.init u60-guard.init"
doctor_src() { # doctor_src <OLD|NEW>: a doctor that prints $T/tsv-<v> and passes --ledger-selftest
    cat <<EOF
#!/bin/sh
# $1 doctor
case "\$1" in
    --tsv) [ -f $T/doctor-hang-$1 ] && $_real_sleep 30; cat $T/tsv-$1 ;;
    --ledger-selftest) if [ -f $T/ledger-fail ]; then echo "FAIL key.log 的原因码"; exit 1; fi; echo "全部通过" ;;
esac
EOF
}
tsv() { # tsv <OLD|NEW> <level:id> …
    _v=$1
    shift
    : >"$T/tsv-$_v"
    for _x in "$@"; do printf '%s\t%s\t名\t说明\n' "${_x%%:*}" "${_x#*:}" >>"$T/tsv-$_v"; done
}
guard_old() {
    for f in $GFILES; do
        [ "$f" = wifi-ab.sh ] && continue # not on the device yet: a new file
        case "$f" in
            doctor.sh) doctor_src OLD >"$GD/$f" ;;
            *) printf '#!/bin/sh\n# OLD %s\n:\n' "$f" >"$GD/$f" ;;
        esac
    done
    printf '#!/bin/sh /etc/rc.common\n# OLD init\n' >"$R/etc/init.d/u60-guard"
    chmod 755 "$R/etc/init.d/u60-guard"
    for f in ledger lan-ipv6-off standby.baseline u60-guard.sh.pre-lanv6-20260924; do echo keep >"$GD/$f"; done
    GI_OLD=$(md5 "$R/etc/init.d/u60-guard") GS_OLD=$(md5 "$GD/u60-guard.sh")
    "$T/bin/guard-initd" start
    : >"$T/guard-initd.log"
    : >"$T/kmsg"
    "$T/bin/proc_new" 7400 /bin/sh cat "$T/kmsg"
    "$T/bin/proc_new" 7401 /bin/sh sh -c 'cat "$1" | awk "$3" >>"$2"' sh "$T/kmsg" /data/crashcap/x '!/ audit: /'
    tsv OLD ok:wifi ok:standby ok:clock ok:manifest warn:fota ok:sms
    tsv NEW ok:wifi ok:standby ok:clock ok:manifest warn:fota ok:sms
}
upload_guard() { # upload_guard [txn] [variant: ok|broken]
    _t=${1:-$TG}
    _s=$R/data/u60-ship/stage/$_t
    mkdir -p "$_s"
    _m=
    for f in $GFILES u60-guard; do
        case "$f" in
            doctor.sh) doctor_src NEW >"$_s/$f" ;;
            u60-guard) printf '#!/bin/sh /etc/rc.common\n# NEW init %s\n' "$_t" >"$_s/$f" ;;
            u60-guard.sh) printf '#!/bin/sh\n# NEW %s %s\n:\n' "$f" "$_t" >"$_s/$f"; [ "$2" = broken ] && echo '# BROKEN' >>"$_s/$f" ;;
            *) printf '#!/bin/sh\n# NEW %s %s\n:\n' "$f" "$_t" >"$_s/$f" ;;
        esac
        _m="$_m${_m:+$NL}file=$f $(md5 "$_s/$f")"
    done
    GI_NEW=$(md5 "$_s/u60-guard") GS_NEW=$(md5 "$_s/u60-guard.sh") GW_NEW=$(md5 "$_s/wifi-ab.sh")
    meta "$_t" guard "$_m"
}
gold() { # every guard file is the old one again, the new one gone
    check "$CASE: all files old again, wifi-ab.sh (new) deleted" '[ "$(md5 "$GD/u60-guard.sh")" = "$GS_OLD" ] && [ "$(md5 "$R/etc/init.d/u60-guard")" = "$GI_OLD" ] && [ ! -e "$GD/wifi-ab.sh" ] && grep -q OLD "$GD/doctor.sh"'
}

setup
CASE="ship guard"
guard_old
upload_guard
stage $TG
check "$CASE: staged as v=2 (new file, /etc path)" '[ "$RC" = 0 ] && [ "$(txnv)" = 2 ] && grep -qx "file=$GD/wifi-ab.sh|-|$GW_NEW" "$R/data/u60-ship/txn" && grep -qx "file=$R/etc/init.d/u60-guard|$GI_OLD|$GI_NEW" "$R/data/u60-ship/txn"'
run $TG
check "$CASE: done" '[ "$RC" = 0 ] && [ "$(phase)" = done ]'
check "$CASE: every file new, the new one in place" '[ "$(md5 "$GD/u60-guard.sh")" = "$GS_NEW" ] && [ "$(md5 "$R/etc/init.d/u60-guard")" = "$GI_NEW" ] && [ "$(md5 "$GD/wifi-ab.sh")" = "$GW_NEW" ] && [ -x "$GD/wifi-ab.sh" ]'
check "$CASE: the init script's .prev in u60-ship/prev, nothing extra in /etc/init.d" '[ "$(md5 "$R/data/u60-ship/prev/etc.init.d.u60-guard.prev-$TG")" = "$GI_OLD" ] && [ "$(ls "$R/etc/init.d")" = u60-guard ]'
check "$CASE: old doctor asked before, stop-requested written, stop then start" '[ -f "$T/tmp/doctor-before.tsv" ] && [ "$(cat "$T/guardtmp/stop-requested")" = "u60-ship $TG" ] && [ "$(cut -d" " -f1 "$T/guard-initd.log" | tr "\n" " ")" = "stop start " ]'
check "$CASE: the 300 s check ran" "logged 'guard 检查，窗口 300s' && logged 'doctor --ledger-selftest 通过' && logged 'doctor --tsv 和换之前比没有变坏'"
check "$CASE: logs, ledger and hand-made files untouched" '[ "$(cat "$GD/ledger")" = keep ] && [ -f "$GD/lan-ipv6-off" ] && [ -f "$GD/u60-guard.sh.pre-lanv6-20260924" ]'
check "$CASE: manifest: the new file without prev, the init with its prev" "grep -q '{\"path\":\"$GD/wifi-ab.sh\",\"md5\":\"$GW_NEW\"}' '$U60S_MANIFEST' && grep -q '\"path\":\"$R/etc/init.d/u60-guard\",\"md5\":\"$GI_NEW\",\"prev\":\"$R/data/u60-ship/prev/etc.init.d.u60-guard.prev-$TG\"' '$U60S_MANIFEST'"
teardown

# T15: doctor --tsv before / after, row by row
gtsv() { # gtsv <case> <expect done|rolledback> <NEW rows…>
    setup
    CASE="guard tsv: $1"
    _ge=$2
    shift 2
    guard_old
    tsv NEW "$@"
    upload_guard
    stage $TG
    run $TG
    check "$CASE: $_ge" '[ "$(phase)" = "$_ge" ]'
    [ "$_ge" = rolledback ] && { check "$CASE: says which row" 'reason | grep -q "doctor 变坏：wifi（ok→warn）"'; gold; }
    teardown
}
gtsv "wifi ok → warn" rolledback warn:wifi ok:standby ok:clock ok:manifest warn:fota ok:sms
gtsv "standby, clock, manifest worse: left out" done ok:wifi bad:standby warn:clock warn:manifest warn:fota ok:sms
gtsv "warn → bad: not counted (was not ok)" done ok:wifi ok:standby ok:clock ok:manifest bad:fota ok:sms
gtsv "a new row, a missing row" done ok:wifi ok:standby ok:clock ok:manifest warn:fota bad:brand-new
gtsv "rows in another order" done ok:sms warn:fota ok:manifest ok:clock ok:standby ok:wifi

setup
CASE="guard: the old doctor hangs"
guard_old
touch "$T/doctor-hang-OLD"
upload_guard
stage $TG
run $TG
check "$CASE: aborted before anything was touched" '[ "$(phase)" = aborted ] && reason | grep -q "旧 doctor.sh --tsv 跑不完" && [ ! -s "$T/guard-initd.log" ] && [ "$(md5 "$GD/u60-guard.sh")" = "$GS_OLD" ] && [ ! -e "$GD/wifi-ab.sh" ] && [ ! -e "$GD/wifi-ab.sh.test" ]'
teardown

setup
CASE="guard: ledger self-test fails"
guard_old
touch "$T/ledger-fail"
upload_guard
stage $TG
run $TG
check "$CASE: rolled back" '[ "$(phase)" = rolledback ] && reason | grep -q "ledger-selftest 没通过：FAIL key.log"'
gold
check "$CASE: guard restarted on the old files" '[ "$(cut -d" " -f1 "$T/guard-initd.log" | tr "\n" " ")" = "stop start stop start " ] && [ -d "$T/proc/7300" ]'
teardown

setup
CASE="guard: two kmsg capture pipelines"
guard_old
upload_guard
stage $TG
hook_at 1100 "$T/bin/proc_new 7402 /bin/sh cat $T/kmsg"
run $TG
check "$CASE: rolled back" '[ "$(phase)" = rolledback ] && reason | grep -q "kmsg 落盘管道有 2 条"'
teardown

setup
CASE="guard: the new guard will not start"
guard_old
upload_guard "" broken
stage $TG
run $TG
check "$CASE: rolled back, the old guard running" '[ "$(phase)" = rolledback ] && reason | grep -q "^新版起不来：guard 没起来" && [ -d "$T/proc/7300" ]'
gold
teardown

setup
CASE="guard: an old background job (same cmdline) still finishing"
guard_old
upload_guard
stage $TG
# a ( … ) & job of the old guard outlives the stop for a while
"$T/bin/proc_new" 7350 /bin/sh /bin/sh "$GD/u60-guard.sh"
run $TG
check "$CASE: neither holds up the stop nor counts as the guard: done" '[ "$(phase)" = done ] && grep -q "service list" "$T/ubus.log"'
teardown

setup
CASE="guard: the new guard will not start, an old job lingers"
guard_old
upload_guard "" broken
stage $TG
"$T/bin/proc_new" 7350 /bin/sh /bin/sh "$GD/u60-guard.sh"
run $TG
check "$CASE: the leftover job does not pass for a running guard: rolled back" '[ "$(phase)" = rolledback ] && reason | grep -q "^新版起不来：guard 没起来"'
teardown

setup
CASE="guard without ubus (cmdline fallback)"
export U60S_UBUS=$T/bin/no-such-ubus
guard_old
upload_guard
stage $TG
run $TG
check "$CASE: done" '[ "$(phase)" = done ]'
teardown

setup
CASE="guard: a new file changed during the check"
guard_old
upload_guard
stage $TG
hook_at 1200 "echo tampered >>$GD/alert-lib.sh"
run $TG
check "$CASE: rolled back" '[ "$(phase)" = rolledback ] && reason | grep -q "alert-lib.sh 不是这一版了"'
teardown

setup
CASE="guard: the phase-one u60-recover.sh"
guard_old
cp "$OLDREC" "$R/data/u60-ship/u60-recover.sh"
upload_guard
stage $TG
check "$CASE: stage refused" '[ "$RC" = 1 ] && grep -q "先装新版 u60-recover.sh" "$T/out" && [ ! -e "$GD/wifi-ab.sh.test" ] && [ ! -e "$R/etc/init.d/u60-guard.test" ]'
teardown

setup
CASE="guard: a script with a syntax error"
guard_old
upload_guard
printf 'case x in\n' >"$R/data/u60-ship/stage/$TG/chaos.sh"
sed -i "s/^file=chaos.sh .*/file=chaos.sh $(md5 "$R/data/u60-ship/stage/$TG/chaos.sh")/" "$R/data/u60-ship/stage/$TG/meta"
stage $TG
check "$CASE: stage refused" '[ "$RC" = 1 ] && grep -q "chaos.sh 有语法错误" "$T/out"'
teardown

setup
CASE="guard: the meta lacks one file"
guard_old
upload_guard
sed -i '/^file=wifi-ab.sh /d' "$R/data/u60-ship/stage/$TG/meta"
stage $TG
check "$CASE: stage refused" '[ "$RC" = 1 ] && grep -q "没有 file=wifi-ab.sh" "$T/out"'
teardown

for p in promote:moved:alert-lib.sh promote:moved:wifi-ab.sh promote:prev-synced:u60-guard promote:moved:u60-guard check:begin rollback:files:u60-guard; do
    setup
    CASE="guard: power cut at $p"
    guard_old
    upload_guard
    [ "${p%%:*}" = rollback ] && touch "$T/ledger-fail"
    stage $TG
    U60S_CRASH_AT=$p run $TG
    check "$CASE: executor died there" '[ "$RC" = 137 ]'
    PH=$(phase)
    tree >"$T/t0"
    recover_newboot old
    tree >"$T/t1"
    check "$CASE: the phase-one u60-recover.sh touches nothing" 'cmp -s "$T/t0" "$T/t1" && [ "$(phase)" = "$PH" ]'
    recover_newboot
    check "$CASE: rolled back" '[ "$(phase)" = rolledback ]'
    gold
    teardown
done

setup
CASE="guard: recover-live (u60 status) after kill -9 in the check"
guard_old
upload_guard
stage $TG
U60S_CRASH_AT=check:begin run $TG
: >"$T/guard-initd.log"
sh "$SHIP" recover-live >"$T/out" 2>&1
check "$CASE: rolled back, guard restarted" '[ "$(phase)" = rolledback ] && [ "$(cut -d" " -f1 "$T/guard-initd.log" | tr "\n" " ")" = "stop start " ]'
gold
teardown

setup
CASE="guard: prepare-rollback after a ship that added a file"
guard_old
upload_guard
stage $TG
run $TG
sh "$SHIP" prepare-rollback guard 20261001-130000-guard 1790000100 >"$T/out" 2>&1
RC=$?
check "$CASE: prepared; the added file stays as it is" "[ \$RC = 0 ] && grep -qx 'file=wifi-ab.sh $GW_NEW' '$R/data/u60-ship/stage/20261001-130000-guard/meta' && grep -qx \"file=u60-guard $GI_OLD\" '$R/data/u60-ship/stage/20261001-130000-guard/meta'"
stage 20261001-130000-guard
run 20261001-130000-guard
check "$CASE: back to the old files (init from u60-ship/prev)" '[ "$(phase)" = done ] && [ "$(md5 "$R/etc/init.d/u60-guard")" = "$GI_OLD" ] && [ "$(md5 "$GD/u60-guard.sh")" = "$GS_OLD" ]'
teardown

setup
CASE="record-kit touch and uid (the install kit's do_devui)"
sh "$SHIP" record-kit touch 20261001 abc1234 1 1790000000 >"$T/out" 2>&1
RC=$?
check "$CASE: touch: both files" "[ \$RC = 0 ] && grep -q '\"comp\":\"touch\".*\"files\":\[{\"path\":\"$DV/u60pro-devui\",\"md5\":\"$UI_OLD\"},{\"path\":\"$DV/start.sh\",\"md5\":\"$SS_OLD\"}\]' '$U60S_MANIFEST'"
sh "$SHIP" record-kit uid 20261001 abc1234 1 1790000000 dirty >"$T/out" 2>&1
RC=$?
check "$CASE: uid" "[ \$RC = 0 ] && grep -q '\"comp\":\"uid\".*\"files\":\[{\"path\":\"$DV/u60-uid\",\"md5\":\"$UID_OLD\"}\],\"state\":\[\],\"note\":\"kit-dirty\"}' '$U60S_MANIFEST'"
teardown

setup
CASE="side files are 755 whatever the upload's mode"
guard_old
chmod 644 "$R/etc/init.d/u60-guard" "$GD/u60-guard.sh"
upload_guard
chmod 600 "$R/data/u60-ship/stage/$TG/u60-guard" "$R/data/u60-ship/stage/$TG/u60-guard.sh"
stage $TG
check "$CASE: /etc/init.d/u60-guard.test and u60-guard.sh.test are 755" '[ "$(stat -c %a "$R/etc/init.d/u60-guard.test")" = 755 ] && [ "$(stat -c %a "$GD/u60-guard.sh.test")" = 755 ] && [ "$(stat -c %a "$GD/wifi-ab.sh.test")" = 755 ]'
teardown

setup
CASE="guard: record-kit with a file the kit does not install"
guard_old
sh "$SHIP" record-kit guard 20261001 abc1234 1 1790000000 >"$T/out" 2>&1
RC=$?
check "$CASE: recorded, the absent one as -" "[ \$RC = 0 ] && grep -q '{\"path\":\"$GD/wifi-ab.sh\",\"md5\":\"-\"}' '$U60S_MANIFEST'"
teardown

# ═══ install-recover ════════════════════════════════════════════════════════
TR=20261001-120000-recover
upload_recover() { # upload_recover [file to use]
    mkdir -p "$R/data/u60-ship/stage/$TR"
    cp "${1:-$RECOVER}" "$R/data/u60-ship/stage/$TR/u60-recover.sh"
    {
        echo v=1
        echo "txn=$TR"
        echo comp=recover
        echo commit=abcdef1
        echo mac_time=1790000000
        echo "file=u60-recover.sh $(md5 "$R/data/u60-ship/stage/$TR/u60-recover.sh")"
    } >"$R/data/u60-ship/stage/$TR/meta"
}
irun() { timeout 120 sh "$SHIP" install-recover "$TR" >"$T/out" 2>&1; RC=$?; }

setup
CASE="install-recover"
cp "$OLDREC" "$R/data/u60-ship/u60-recover.sh"
upload_recover
irun
check "$CASE: exit 0, the new one live, formats 1 2" '[ "$RC" = 0 ] && [ "$(md5 "$R/data/u60-ship/u60-recover.sh")" = "$(md5 "$RECOVER")" ] && [ "$(sh "$R/data/u60-ship/u60-recover.sh" formats)" = "1 2" ]'
check "$CASE: the old one kept, the stage directory gone" '[ "$(md5 "$R/data/u60-ship/u60-recover.sh.prev-$TR")" = "$(md5 "$OLDREC")" ] && [ ! -e "$R/data/u60-ship/stage/$TR" ]'
check "$CASE: a record line" "grep -q '\"kind\":\"record\",\"name\":\"u60-recover.sh\",\"path\":\"$R/data/u60-ship/u60-recover.sh\",\"md5\":\"$(md5 "$RECOVER")\".*\"why\":\"install-recover abcdef1\"' '$U60S_MANIFEST'"
web_old
upload_web
stage $TW
check "$CASE: now a v=2 stage goes through" '[ "$RC" = 0 ] && [ "$(txnv)" = 2 ]'
teardown

irefused() { # irefused <case> <text>
    CASE=$1
    check "$1: refused, says why" "[ \"\$RC\" = 1 ] && grep -q -- '$2' '$T/out'"
    check "$1: the live one untouched" '[ "$(md5 "$R/data/u60-ship/u60-recover.sh")" = "$(md5 "$OLDREC")" ] && [ "$(mlines)" = 0 ]'
}
setup; cp "$OLDREC" "$R/data/u60-ship/u60-recover.sh"; upload_recover; echo x >>"$R/data/u60-ship/stage/$TR/u60-recover.sh"; irun
irefused "install-recover: md5 wrong" "md5 不对"; teardown
setup; cp "$OLDREC" "$R/data/u60-ship/u60-recover.sh"; printf 'case x in\n' >"$T/bad.sh"; upload_recover "$T/bad.sh"; irun
irefused "install-recover: syntax error" "语法错误"; teardown
setup; cp "$OLDREC" "$R/data/u60-ship/u60-recover.sh"; sed 's/t "完成 → 不动" done "new$NL" "old$NL" done new/t "完成 → 不动" done "new$NL" "old$NL" done old/' "$RECOVER" >"$T/st.sh"; upload_recover "$T/st.sh"; irun
irefused "install-recover: its selftest fails" "自检没过"; teardown
setup; cp "$OLDREC" "$R/data/u60-ship/u60-recover.sh"; upload_recover; echo "evil=1" >>"$R/data/u60-ship/stage/$TR/meta"; irun
irefused "install-recover: unknown meta key" "看不懂的一行"; teardown
setup; cp "$OLDREC" "$R/data/u60-ship/u60-recover.sh"; upload_recover; printf 'v=1\ntxn=x\ncomp=datad\nphase=check\nend=1\n' >"$R/data/u60-ship/txn"; irun
irefused "install-recover: during a transaction" "还在进行"; teardown
setup; cp "$OLDREC" "$R/data/u60-ship/u60-recover.sh"; mkdir -p "$R/data/u60-ship/stage/20261001-120000-guard"; sh "$SHIP" install-recover 20261001-120000-guard >"$T/out" 2>&1; RC=$?
irefused "install-recover: txn not ending in -recover" "以 -recover 结尾"; teardown

# ═══ record: the directories ════════════════════════════════════════════════
setup
CASE="record directories"
mkdir -p "$DV/fonts" "$DV/operator-logos"
printf 'font\n' >"$DV/fonts/u60-cjk-fallback.ttf"
printf 'png\n' >"$DV/operator-logos/cmcc.png"
sh "$SHIP" record devui-fonts 1790000000 >"$T/out" 2>&1
RC=$?
check "$CASE: fonts recorded by fingerprint" "[ \$RC = 0 ] && grep -q '\"name\":\"devui-fonts\",\"path\":\"$DV/fonts\",\"tree\":\"$(fp "$DV/fonts")\"' '$U60S_MANIFEST' && grep -q 'u60-ship: 已记录 devui-fonts $(fp "$DV/fonts")' '$T/out'"
rm -rf "$DV/operator-logos"
sh "$SHIP" record devui-logos 1790000000 >"$T/out" 2>&1
check "$CASE: an absent directory is recorded as -" "grep -q '\"name\":\"devui-logos\",\"path\":\"$DV/operator-logos\",\"tree\":\"-\"' '$U60S_MANIFEST'"
ln -s u60-cjk-fallback.ttf "$DV/fonts/link.ttf"
sh "$SHIP" record devui-fonts 1790000000 >"$T/out" 2>&1
RC=$?
check "$CASE: a symlink inside: refused" '[ $RC = 1 ] && grep -q "算不了指纹" "$T/out" && [ "$(mlines)" = 2 ]'
teardown

echo "u60-ship phase two: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
