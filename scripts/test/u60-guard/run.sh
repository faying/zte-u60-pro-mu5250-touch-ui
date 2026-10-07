#!/bin/sh
# ─────────────────────────────────────────────────────────────────────────────
# Branch tests for u60-guard.sh and alert-lib.sh, with every device command
# stubbed. Runs in a plain busybox container (same applets as the device):
#
#   scripts/test/docker.sh        (runs every suite under scripts/test/)
#
# Time is fake: $T/uptime is /proc/uptime, and the stub `sleep` advances it
# instead of sleeping, so the 150 s lock wait and the backoff run instantly.
# SPDX-License-Identifier: MIT
# ─────────────────────────────────────────────────────────────────────────────

SCRIPTS=${SCRIPTS:-/scripts}
GUARD=$SCRIPTS/u60-guard.sh
PASS=0
FAIL=0
TAB=$(printf '\t')

ok() { PASS=$((PASS + 1)); echo "  ok   $1"; }
bad() { FAIL=$((FAIL + 1)); echo "  FAIL $1"; }
check() { # check <description> <shell test…>
    _d=$1
    shift
    if eval "$@"; then ok "$_d"; else bad "$_d  [$*]"; fi
}

# ── fresh world per case ────────────────────────────────────────────────────
setup() {
    T=$(mktemp -d)
    mkdir -p "$T/bin" "$T/proc" "$T/rtc" "$T/alerts"
    echo 1000 >"$T/uptime"
    echo 0 >"$T/hostapd"
    : >"$T/ubus.log"
    : >"$T/uci.log"
    : >"$T/kill.log"

    cat >"$T/bin/sleep" <<EOF
#!/bin/sh
echo \$(( \$(cut -d. -f1 $T/uptime) + \${1%%.*} )) >$T/uptime
EOF
    cat >"$T/bin/ps" <<EOF
#!/bin/sh
echo "  PID USER       VSZ STAT COMMAND"
echo "    1 root      1000 S    /sbin/procd"
n=\$(cat $T/hostapd); i=0
while [ \$i -lt \$n ]; do echo "  1\$i root  2000 S    /usr/sbin/hostapd -g /var/run/hostapd/global"; i=\$((i+1)); done
EOF
    cat >"$T/bin/uci" <<EOF
#!/bin/sh
echo "\$*" >>$T/uci.log
case "\$*" in
    *time_from_utc*) cat $T/tz 2>/dev/null ;;
    *"get wireless.zte_mbb.wifi_onoff") cat $T/onoff 2>/dev/null ;; # absent = key missing
    *"get wireless.zte_mbb.lbd") cat $T/lbd 2>/dev/null ;;
esac
EOF
    cat >"$T/bin/ubus" <<EOF
#!/bin/sh
echo "\$*" >>$T/ubus.log
[ "\$1" = -t ] && shift 2
case "\$2 \$3" in
    "zwrt_web device_info") cat $T/device_info 2>/dev/null ;;
    "zwrt_bsp.pm list") cat $T/pm 2>/dev/null ;;
    # the APs only come up while the vendor master switch (\$T/onoff) is not "0"
    "zwrt_wlan reload") [ -f $T/reload-works ] && [ "\$(cat $T/onoff 2>/dev/null)" != 0 ] && echo 8 >$T/hostapd ;;
    "zwrt_wlan set") case "\$4" in *'"zte_mbb":{"wifi_onoff":"1"'*) echo 1 >$T/onoff ;; esac ;;
    "zwrt_wms zte_libwms_send_sms") cat $T/sms-resp 2>/dev/null || echo '{"result":3}' ;;
    "zwrt_wms zte_libwms_get_sms_data") case "\$4" in *'"mem_store":1'*) cat $T/sent-box 2>/dev/null ;; esac ;;
esac
exit 0
EOF
    cat >"$T/bin/kill" <<EOF
#!/bin/sh
echo "\$*" >>$T/kill.log
/bin/kill "\$@"
EOF
    # The device has jsonfilter; busybox does not. Enough for '@.result' and
    # "@.messages[@.number='N'].id" (fixtures put one message per line).
    cat >"$T/bin/jsonfilter" <<'EOF'
#!/bin/sh
case "$2" in
  *messages*) n=$(echo "$2" | sed -n "s/.*number='\([^']*\)'.*/\1/p")
              grep "\"number\":\"$n\"" | sed -n 's/.*"id":\([0-9]*\).*/\1/p'; exit ;;
  @.result) sed -n 's/.*"result"[^0-9]*\([0-9][0-9]*\).*/\1/p'; exit ;;
esac
# Anything else: plain '@.a.b' paths and 'VAR=@.a.b' exports, looked up in
# the JSON on stdin by a small parser, so the paths guard uses are checked
# against a real /state document.
exprs=
while [ $# -gt 0 ]; do
    [ "$1" = -e ] && { exprs="$exprs $2"; shift; }
    shift
done
awk -v exprs="$exprs" '
function ws() { while (i <= n && substr(s, i, 1) ~ /[ \t\r\n]/) i++ }
function str(   c, o) {
    i++
    o = ""
    while (i <= n) {
        c = substr(s, i, 1)
        if (c == "\\") { o = o substr(s, i + 1, 1); i += 2; continue }
        if (c == "\"") { i++; return o }
        o = o c
        i++
    }
    return o
}
function val(p,   c, k, x) {
    ws()
    c = substr(s, i, 1)
    if (c == "{" || c == "[") {
        i++
        x = 0
        ws()
        if (substr(s, i, 1) == "}" || substr(s, i, 1) == "]") { i++; return }
        while (i <= n) {
            ws()
            if (c == "{") { k = str(); ws(); i++; val(((p == "") ? k : (p "." k))) }
            else { val(p "[" x "]"); x++ }
            ws()
            k = substr(s, i, 1)
            i++
            if (k != ",") return
        }
        return
    }
    if (c == "\"") { f[p] = str(); return }
    x = ""
    while (i <= n && substr(s, i, 1) !~ /[],} \t\r\n]/) { x = x substr(s, i, 1); i++ }
    if (x == "true") x = 1          # the device jsonfilter prints booleans as 1 / 0
    else if (x == "false") x = 0
    f[p] = x
}
{ s = s $0 "\n" }
END {
    n = length(s)
    i = 1
    val("")
    m = split(exprs, e, " ")
    for (j = 1; j <= m; j++) {
        v = e[j]
        name = ""
        if (index(v, "=")) { name = substr(v, 1, index(v, "=") - 1); v = substr(v, index(v, "=") + 1) }
        sub(/^@\./, "", v)
        if (!(v in f)) continue
        if (name == "") { print f[v]; continue }
        x = f[v]
        gsub(/\047/, "\047\\\047\047", x)
        printf "export %s=\047%s\047; ", name, x
    }
}'
EOF
    cat >"$T/bin/wget" <<EOF
#!/bin/sh
out=-
t=
while [ \$# -gt 0 ]; do
    case "\$1" in -O) out=\$2; shift ;; -T) t=\$2; shift ;; esac
    shift
done
if [ -f $T/datad-hang ]; then # no answer: its -T runs out on the fake clock
    echo \$((\$(cut -d. -f1 $T/uptime) + t)) >$T/uptime
    exit 1
fi
[ -f $T/state.json ] && [ ! -f $T/datad-down ] || exit 1
if [ "\$out" = - ]; then cat $T/state.json; else cat $T/state.json >"\$out"; fi
EOF
    # tailscaled's LocalAPI through curl: answers \$T/ts.json with HTTP
    # \$T/ts-code (200), or exits \$T/ts-rc; \$T/ts-hang: no answer in its -m
    cat >"$T/bin/curl" <<EOF
#!/bin/sh
case "\$*" in
    *9460/control*)
        # datad's /control: absent unless \$T/datad-control; answers \$T/control-reply
        # (default ok), or exits \$T/control-rc (28 = no answer in time)
        [ -f $T/datad-control ] || exit 7
        while [ \$# -gt 0 ]; do [ "\$1" = --data-binary ] && { printf '%s\n' "\$2" >>$T/control.log; break; }; shift; done
        rc=\$(cat $T/control-rc 2>/dev/null)
        [ -n "\$rc" ] && exit "\$rc"
        cat $T/control-reply 2>/dev/null || printf '{"action":"x","ok":true,"result":{"result":"3"}}'
        exit 0
        ;;
esac
echo "\$*" >>$T/curl.log
out=
m=
while [ \$# -gt 0 ]; do
    case "\$1" in -o) out=\$2; shift ;; -m) m=\$2; shift ;; esac
    shift
done
if [ -f $T/ts-hang ]; then
    echo \$((\$(cut -d. -f1 $T/uptime) + m)) >$T/uptime
    printf 000
    exit 28
fi
rc=\$(cat $T/ts-rc 2>/dev/null)
if [ -n "\$rc" ]; then printf 000; exit "\$rc"; fi
[ -n "\$out" ] && cat $T/ts.json >"\$out"
printf '%s' "\$(cat $T/ts-code 2>/dev/null || echo 200)"
EOF
    # a healthy node, as LocalAPI's /status puts it (made up; peers carry their own Online)
    cat >"$T/ts.json" <<'EOF'
{
	"Version": "1.102.4-t0",
	"BackendState": "Running",
	"Self": {
		"ID": "n0",
		"HostName": "u60pro",
		"TailscaleIPs": ["100.64.0.1", "fd7a:115c:a1e0::1"],
		"Online": true
	},
	"Peer": {
		"nodekey:0001": {"HostName": "mac", "Online": false}
	}
}
EOF
    printf '#!/bin/sh\necho "Filesystem 1K-blocks Used Available Use%% Mounted on"\necho "/dev/ubi0 4000000 2800000 1200000 70%% /data"\n' >"$T/bin/df"
    echo feedc0de-0000-4000-8000-000000000001 >"$T/bootid"
    chmod +x "$T"/bin/*

    export GUARD_UPTIME_FILE=$T/uptime GUARD_HEARTBEAT=$T/heartbeat GUARD_MARKER=$T/marker
    export GUARD_WIFI_LOCK=$T/wifi.lock GUARD_STATE=$T/state GUARD_LOG=$T/guard.log
    export GUARD_PROC=$T/proc GUARD_RTC=$T/rtc
    export GUARD_UCI=$T/bin/uci GUARD_UBUS=$T/bin/ubus GUARD_PS=$T/bin/ps
    export GUARD_SLEEP=$T/bin/sleep GUARD_KILL=$T/bin/kill GUARD_JSONFILTER=$T/bin/jsonfilter
    export ALERT_DIR=$T/alerts ALERT_UPTIME_FILE=$T/uptime
    export GUARD_DATAD_MARKER=$T/datad-degraded
    export GUARD_DEVUI_CONF=$T/devui.conf # absent unless a case writes it: zh
    export GUARD_WALL_NOW=1790500000 GUARD_WALL_LAST=$T/wall.last GUARD_LEDGER_FG=1 \
        GUARD_LEDGER_DIR=$T/ledger GUARD_SPOOL_TMP=$T/spool-tmp GUARD_BOOT_ID_FILE=$T/bootid GUARD_DF=$T/bin/df \
        GUARD_WGET=$T/bin/wget GUARD_CURL=$T/bin/curl GUARD_TS_SOCK=$T/tailscaled.sock # a plain file stands in for the socket
    unset GUARD_LOCK_WAIT
    export GUARD_SHIP_TXN=$T/u60-ship/txn GUARD_SHIP_HB=$T/u60-ship/heartbeat GUARD_SHIP=$T/u60-ship/u60-ship.sh
    # The cases below jump the clock between rounds freely; sleep detection
    # would read every jump as a suspend. Its own case turns it back on.
    export GUARD_WAKE_GAP=100000000
    # The crash watcher is a long-lived child: only its own cases start one.
    export GUARD_WATCHER=0
}
# Background work fails quietly by design (its errors only reach guard.log),
# so every case ends by checking that log for shell errors.
SHELL_ERRORS='arithmetic|syntax error|can.t open|: not found|bad number|Illegal number|unexpected|Bad substitution|No such file'
teardown() {
    if [ -z "$EXPECT_SHELL_ERRORS" ] && grep -q -E "$SHELL_ERRORS" "$T/guard.log" 2>/dev/null; then
        bad "shell errors in guard.log: $(grep -E "$SHELL_ERRORS" "$T/guard.log" | head -n 3 | tr '\n' '|')"
    fi
    unset EXPECT_SHELL_ERRORS
    rm -rf "$T"
}

round() { sh "$GUARD" once; }
up() { echo "$1" >"$T/uptime"; }
hb() { echo "$1" >"$T/heartbeat"; }
kinds() { awk -F'\t' '{print $4}' "$T/alerts/queue" 2>/dev/null | tr '\n' ' '; }
restores() { grep -c 'zwrt_wlan reload' "$T/ubus.log"; }

# ── cases ───────────────────────────────────────────────────────────────────

echo "grace period and first heartbeat"
setup
up 100
round
check "inside the 5-minute grace: nothing happens" '[ "$(restores)" = 0 ] && [ ! -f $T/marker ]'
up 400
round
check "never seen a heartbeat, 400 s after boot: still waiting" '[ "$(restores)" = 0 ] && [ -z "$(kinds)" ]'
hb 390
round
check "fresh heartbeat: nothing to do" '[ "$(restores)" = 0 ] && [ -f $T/state/seen ]'
teardown

echo "agent never came up"
setup
up 1000
touch "$T/reload-works"
round
check "no heartbeat since boot + AP off after 15 min: takes over" '[ -f $T/marker ] && [ "$(restores)" = 1 ]'
check "turns radios AND APs on" 'grep -q "set wireless.wifi0.disabled=0" $T/uci.log && grep -q "set wireless.wifi1.disabled=0" $T/uci.log && grep -q "set wireless.main_2g.disabled=0" $T/uci.log && grep -q "set wireless.main_5g.disabled=0" $T/uci.log && grep -q "commit wireless" $T/uci.log'
check "alerts: silent + takeover" '[ "$(kinds)" = "agent-silent wifi-takeover " ]'
check "holds sleep off while rescuing" 'grep -q "enableAutoSleep {\"switch\":false}" $T/ubus.log'
check "lock released afterwards" 'flock -n $T/wifi.lock true'
teardown

echo "datad is there: the guard writes through it (E4 T7c)"
setup
up 1000
touch "$T/reload-works" "$T/datad-control"
round
check "Wi-Fi restore through datad as guard: wifi.apply, all four switches, reload" 'grep -q "\"action\":\"wifi.apply\",\"source\":\"guard\"" $T/control.log && grep -q "\"wireless.wifi0.disabled\":\"0\",\"wireless.wifi1.disabled\":\"0\",\"wireless.main_2g.disabled\":\"0\",\"wireless.main_5g.disabled\":\"0\"},\"reload\":true" $T/control.log'
check "no direct uci write" '! grep -q "set wireless" $T/uci.log 2>/dev/null'
check "sleep hold through datad too, not ubus" 'grep -q "enableAutoSleep\",\"args\":{\"switch\":false}" $T/control.log && ! grep -q enableAutoSleep $T/ubus.log 2>/dev/null'
teardown
setup
up 1000
touch "$T/reload-works" "$T/datad-control"
echo 28 >"$T/control-rc"
round
check "datad alive but not answering: no direct write (D18), restore counted as failed" '! grep -q "set wireless" $T/uci.log 2>/dev/null && grep -q "did not answer the Wi-Fi restore" $T/guard.log'
teardown
setup
up 1000
touch "$T/reload-works" "$T/datad-control"
printf '{"action":"wifi.apply","error":{"code":"failed"},"ok":false}' >"$T/control-reply"
round
check "datad says no: no direct write either" '! grep -q "set wireless" $T/uci.log 2>/dev/null'
teardown
setup
up 1000
touch "$T/reload-works" "$T/datad-control"
printf '{"action":"wifi.apply","error":{"code":"unknown_action","message":"unsupported control action"},"ok":false}' >"$T/control-reply"
round
check "datad from before T7 (unknown action): writes directly as before" 'grep -q "set wireless.main_2g.disabled=0" $T/uci.log && [ "$(restores)" = 1 ]'
teardown

echo "vendor master switch off (stock web/touch screen turned Wi-Fi off)"
VSET='zwrt_wlan set {"zte_mbb":{"wifi_onoff":"1"}}'
setup
up 1000
touch "$T/reload-works"
echo 0 >"$T/onoff"
round
check "agent gone, wifi_onoff=0, no datad: vendor switch on directly, then the APs" 'grep -q -F "call $VSET" $T/ubus.log && [ "$(grep -n -F "$VSET" $T/ubus.log | cut -d: -f1)" -lt "$(grep -n "zwrt_wlan reload" $T/ubus.log | cut -d: -f1)" ]'
check "…and Wi-Fi really comes back" '[ "$(cat $T/hostapd)" = 8 ] && [ "$(cat $T/onoff)" = 1 ] && grep -q "Wi-Fi restored" $T/guard.log'
check "…the APs' disabled restore still runs" 'grep -q "set wireless.main_2g.disabled=0" $T/uci.log'
teardown
setup
up 1000
touch "$T/reload-works"
echo 0 >"$T/onoff"
echo 1 >"$T/lbd"
round
check "direct: band steering sent along as it stands (stock body)" 'grep -q -x -F "call zwrt_wlan set {\"zte_mbb\":{\"wifi_onoff\":\"1\",\"lbd\":\"1\"}}" $T/ubus.log && [ "$(cat $T/hostapd)" = 8 ]'
teardown
setup
up 1000
touch "$T/reload-works"
echo 0 >"$T/onoff"
printf '1"}}; x\n' >"$T/lbd"
round
check "direct: lbd not 0/1: left out, nothing of it in the call" 'grep -q -x -F "call $VSET" $T/ubus.log && ! grep -q lbd $T/ubus.log'
teardown
setup
up 1000
touch "$T/reload-works"
round
check "wifi_onoff key missing (= on): no vendor call, Wi-Fi comes up as before" '[ "$(cat $T/hostapd)" = 8 ] && ! grep -q "zwrt_wlan set" $T/ubus.log'
teardown
setup
up 1000
touch "$T/reload-works" "$T/datad-control"
echo 0 >"$T/onoff"
round
check "through datad: wifi.set_module enabled 1 as guard, before wifi.apply" 'grep -q -x -F "{\"action\":\"wifi.set_module\",\"source\":\"guard\",\"params\":{\"enabled\":1}}" $T/control.log && [ "$(grep -n "wifi.set_module" $T/control.log | cut -d: -f1)" -lt "$(grep -n "wifi.apply" $T/control.log | cut -d: -f1)" ]'
check "…and no direct ubus set" '! grep -q "zwrt_wlan set" $T/ubus.log'
teardown
setup
up 1000
touch "$T/reload-works" "$T/datad-control"
echo 0 >"$T/onoff"
printf '{"action":"wifi.set_module","error":{"code":"failed","message":"zwrt_wlan set: timeout"},"ok":false}' >"$T/control-reply"
round
check "datad refuses the vendor switch: logged, no direct write, the restore is still asked" 'grep -q "refused or did not answer the vendor Wi-Fi switch" $T/guard.log && ! grep -q "zwrt_wlan set" $T/ubus.log && grep -q "\"action\":\"wifi.apply\"" $T/control.log'
teardown
setup
up 1000
touch "$T/reload-works" "$T/datad-control"
echo 0 >"$T/onoff"
printf '{"action":"x","error":{"code":"unknown_action","message":"unsupported control action"},"ok":false}' >"$T/control-reply"
round
check "datad too old for both: vendor switch and APs written directly" 'grep -q -F "call $VSET" $T/ubus.log && grep -q "set wireless.main_5g.disabled=0" $T/uci.log && [ "$(cat $T/hostapd)" = 8 ]'
teardown
setup
up 1000
touch "$T/reload-works" "$T/datad-control"
echo 1 >"$T/onoff"
round
check "wifi_onoff=1: no vendor call at all" '! grep -q "wifi.set_module" $T/control.log && ! grep -q "zwrt_wlan set" $T/ubus.log && ! grep -q "vendor Wi-Fi switch" $T/guard.log'
teardown
setup
up 2000
hb 1990
echo 0 >"$T/onoff"
round
check "wifi_onoff=0 but the agent is alive: left alone (deliberate off is the agent's/user's)" '! grep -q "zwrt_wlan" $T/ubus.log && [ ! -f $T/marker ] && ! grep -q "set wireless" $T/uci.log'
teardown
setup
up 1000
echo 0 >"$T/onoff"
echo 8 >"$T/hostapd"
round
check "wifi_onoff=0, agent gone, but something beacons: no rescue" '! grep -q "zwrt_wlan" $T/ubus.log && [ ! -f $T/marker ]'
teardown

echo "agent dies while Wi-Fi is up"
setup
up 2000
hb 1600
echo 8 >"$T/hostapd"
round
round
check "stale but beaconing: no restore" '[ "$(restores)" = 0 ] && [ ! -f $T/marker ]'
check "agent-silent alerted once" '[ "$(kinds)" = "agent-silent " ]'
teardown

echo "restore fails, backs off, alerts once"
setup
up 2000
hb 1600
round
check "first attempt made" '[ "$(restores)" = 1 ]'
check "restore failure alerted" '[ "$(kinds)" = "agent-silent wifi-takeover wifi-restore-failed " ]'
m1=$(cat "$T/marker")
nt=$(cat "$T/state/next-try")
check "next try 60 s after the failed attempt" '[ "$nt" = $(( $(cat $T/uptime) + 60 )) ]'
up $((nt - 1))
round
check "no attempt before next-try" '[ "$(restores)" = 1 ]'
up "$nt"
round
check "retried at next-try" '[ "$(restores)" = 2 ]'
check "backoff doubled to 240 for the next wait" '[ "$(cat $T/state/backoff)" = 240 ]'
check "failure not re-alerted" '[ "$(kinds)" = "agent-silent wifi-takeover wifi-restore-failed " ]'
check "marker written once (content unchanged)" '[ "$(cat $T/marker)" = "$m1" ]'
touch "$T/reload-works"
up $(cat "$T/state/next-try")
round
check "succeeds eventually" '[ "$(cat $T/hostapd)" = 8 ] && [ ! -f $T/state/next-try ]'
hb "$(cat "$T/uptime")"
round
check "heartbeat back: episode over, sleep handed back" '[ ! -f $T/state/episode ] && grep -q "enableAutoSleep {\"switch\":true}" $T/ubus.log'
check "marker left for the agent to remove" '[ -f $T/marker ]'
teardown

echo "backoff caps at 480"
setup
up 2000
hb 1600
for i in 1 2 3 4 5 6; do up "$(cat "$T/state/next-try" 2>/dev/null || echo 2000)"; round; done
check "backoff never exceeds 480" '[ "$(cat $T/state/backoff)" = 480 ]'
teardown

echo "wedged agent holding the Wi-Fi lock"
setup
export GUARD_LOCK_WAIT=10
up 2000
hb 1600
touch "$T/reload-works"
(flock 9 && exec /bin/sleep 60) 9>>"$T/wifi.lock" &
holder=$!
/bin/sleep 0.3
echo "$holder" >"$T/wifi.lock"
mkdir -p "$T/proc/$holder"
echo zte-agent >"$T/proc/$holder/comm"
round
check "killed the agent holding the lock" 'grep -q "^-9 $holder" $T/kill.log'
check "then restored Wi-Fi" '[ "$(restores)" = 1 ] && [ "$(cat $T/hostapd)" = 8 ]'
check "agent-hung alerted" 'kinds | grep -q agent-hung'
wait "$holder" 2>/dev/null
teardown

echo "something else holding the Wi-Fi lock"
setup
export GUARD_LOCK_WAIT=10
up 2000
hb 1600
(flock 9 && exec /bin/sleep 60) 9>>"$T/wifi.lock" &
holder=$!
/bin/sleep 0.3
echo "$holder" >"$T/wifi.lock"
mkdir -p "$T/proc/$holder"
echo homemode.sh >"$T/proc/$holder/comm"
round
check "does not kill a non-agent holder" '[ ! -s $T/kill.log ]'
check "gives up this round without writing uci" '[ "$(restores)" = 0 ] && ! grep -q "^\(set\|commit\|delete\) " $T/uci.log'
check "schedules a retry" '[ -f $T/state/next-try ]'
/bin/kill "$holder" 2>/dev/null
teardown

echo "device suspended (uptime jumps, heartbeat does not)"
setup
unset GUARD_WAKE_GAP
up 1600; hb 1590; round
up 1660; hb 1650; round
up 2900; round
check "woke 20 min later, heartbeat from before the sleep: no alert, no action" '[ -z "$(kinds)" ] && [ "$(restores)" = 0 ] && [ ! -f $T/marker ]'
up 2960; hb 2955; round
check "agent checks in after waking: still nothing" '[ -z "$(kinds)" ] && [ "$(restores)" = 0 ]'
up 3020; round
up 3080; round
up 3140; round
up 3200; round
up 3260; round
check "then silent past STALE: judged normally" '[ "$(kinds)" = "agent-silent wifi-takeover wifi-restore-failed " ]'
teardown

setup
unset GUARD_WAKE_GAP
up 2000; hb 1600; round
check "outage already under way before the sleep…" '[ -f $T/state/episode ] && [ "$(restores)" = 1 ]'
up "$(( $(cat "$T/state/next-try") + 1200 ))"; round
check "…keeps being handled after waking" '[ "$(restores)" = 2 ]'
teardown

echo "RTC wake while APs are down"
setup
up 100
echo 1000 >"$T/rtc/since_epoch"
: >"$T/rtc/wakealarm"
round
check "arms an alarm when none is pending" '[ "$(cat $T/rtc/wakealarm)" = 1300 ]'
echo 1100 >"$T/rtc/wakealarm"
round
check "keeps an earlier alarm" '[ "$(cat $T/rtc/wakealarm)" = 1100 ]'
echo 5000 >"$T/rtc/wakealarm"
round
check "pulls a later alarm in" '[ "$(cat $T/rtc/wakealarm)" = 1300 ]'
echo 8 >"$T/hostapd"
echo 5000 >"$T/rtc/wakealarm"
round
check "leaves the RTC alone while beaconing" '[ "$(cat $T/rtc/wakealarm)" = 5000 ]'
teardown

echo "alert-lib"
setup
. "$SCRIPTS/alert-lib.sh"
alert_add agent-crash "$(printf 'exit 139\tsegv\nnext line ✗ done')"
check "one clean line, 5 fields" '[ "$(wc -l <$T/alerts/queue)" = 1 ] && [ "$(awk -F"\t" "{print NF}" $T/alerts/queue)" = 5 ]'
check "tabs/newlines/non-ASCII removed" 'grep -q "exit 139 segv next line  done$" $T/alerts/queue'
check "rejects a bad kind" '! alert_add Bad_Kind x && [ "$(wc -l <$T/alerts/queue)" = 1 ]'
long=$(printf 'x%.0s' $(seq 1 200))
alert_add datad-crash "$long"
check "text cut to 120 chars" '[ "$(tail -n1 $T/alerts/queue | cut -f5 | wc -c)" = 121 ]'
i=0
while [ $i -lt 300 ]; do alert_add agent-crash "n$i"; i=$((i + 1)); done
check "trimmed to 200 once past 300" '[ "$(wc -l <$T/alerts/queue)" -le 300 ] && [ "$(wc -l <$T/alerts/queue)" -ge 200 ]'
check "sequence keeps counting through trims" '[ "$(cat $T/alerts/seq)" = 302 ] && [ "$(tail -n1 $T/alerts/queue | cut -f1)" = 302 ]'
teardown

echo "SMS"
setup
echo "8.00" >"$T/tz"
printf '%s\n' '{"id":77,"number":"+8612300000000"}' '{"id":76,"number":"+8612300000000"}' >"$T/sent-box"
up 100 # inside grace: only the SMS part does anything
. "$SCRIPTS/alert-lib.sh"
alert_add agent-crash one
round
check "no number: logged as no-number, cursor advanced" 'grep -q "${TAB}1${TAB}agent-crash${TAB}no-number" $T/alerts/sms-log && [ "$(cat $T/alerts/sms-done)" = 1 ]'
echo "+8612300000000" >"$T/alerts/sms-to"
alert_add agent-crash two
alert_add agent-crash three
round
check "sent the first" 'grep -q "${TAB}2${TAB}agent-crash${TAB}sent" $T/alerts/sms-log'
check "same kind within the hour: suppressed" 'grep -q "${TAB}3${TAB}agent-crash${TAB}suppressed-rate" $T/alerts/sms-log'
check "body: plain Chinese for agent-crash, UCS-2 hex (UTF-8 decoded)" 'grep -q "\"message_body\":\"301000550036003030117BA17406540E53F0610F5916900051FAFF0C5DF281EA52A891CD542F30024E0D75287BA13002FF08" $T/ubus.log'
check "number and fields as sms_forward sends them" 'grep -q "\"number\":\"+8612300000000\",.*\"encode_type\":\"UNICODE\",\"sms_time\":\"[0-9][0-9];[0-9][0-9];[0-9][0-9];[0-9][0-9];[0-9][0-9];[0-9][0-9];[+-][0-9]*\",\"id\":\"-1\"" $T/ubus.log'
check "sent copy deleted (newest to that number)" 'grep -q "zwrt_wms zwrt_wms_delete_sms {\"id\":\"77\"}" $T/ubus.log'
check "SMS time zone from ZTE SNTP (+8), not the UTC system zone" 'grep -q "\"sms_time\":\"[0-9;]*;+8\"" $T/ubus.log'
for k in k-a k-b k-c k-d k-e; do alert_add "$k" x; done
round
check "at most 5 sent per day" '[ "$(grep -c "${TAB}sent$" $T/alerts/sms-log)" = 5 ] && grep -q "k-e${TAB}suppressed-rate" $T/alerts/sms-log'
teardown

setup
up 100
. "$SCRIPTS/alert-lib.sh"
echo "12300000000" >"$T/alerts/sms-to"
touch "$T/alerts/abroad"
alert_add agent-crash x
round
check "abroad: suppressed" 'grep -q "suppressed-abroad" $T/alerts/sms-log && ! grep -q send_sms $T/ubus.log'
touch "$T/alerts/sms-abroad"
alert_add datad-crash x
round
check "abroad with sms-abroad on: sent" 'grep -q "datad-crash${TAB}sent" $T/alerts/sms-log'
teardown

setup
up 100
. "$SCRIPTS/alert-lib.sh"
echo "12300000000" >"$T/alerts/sms-to"
alert_add devui-theme-paused x
round
check "theme-paused alert: no SMS, cursor advanced" '! grep -q send_sms $T/ubus.log && [ "$(cat $T/alerts/sms-done)" = 1 ]'
alert_add agent-crash x
round
check "and it does not use up the day's budget" 'grep -q "agent-crash${TAB}sent" $T/alerts/sms-log'
teardown

setup
up 100
. "$SCRIPTS/alert-lib.sh"
echo "12300000000" >"$T/alerts/sms-to"
alert_add agent-crash x
GUARD_WALL_NOW=35392498 round
check "clock not set: nothing sent, left pending" '[ ! -f $T/alerts/sms-done ] && ! grep -q send_sms $T/ubus.log'
round
check "sent once the clock is set" 'grep -q "agent-crash${TAB}sent" $T/alerts/sms-log'
teardown

setup
up 100
. "$SCRIPTS/alert-lib.sh"
echo "12300000000" >"$T/alerts/sms-to"
echo '{"result":5}' >"$T/sms-resp"
alert_add agent-crash x
round
check "device rejects: logged failed + sms-failed event" 'grep -q "agent-crash${TAB}failed" $T/alerts/sms-log && kinds | grep -q sms-failed'
round
check "no SMS about the failed SMS" '[ "$(grep -c send_sms $T/ubus.log)" = 1 ]'
check "failure does not use up the hourly slot" '! grep -q suppressed-rate $T/alerts/sms-log'
teardown

setup
up 100
. "$SCRIPTS/alert-lib.sh"
echo "12300000000" >"$T/alerts/sms-to"
printf '%s\t9\tagent-crash\tsent\n' $((GUARD_WALL_NOW + 999999)) >"$T/alerts/sms-log"
alert_add agent-crash x
round
check "a future-stamped send counts as just sent" 'grep -q "agent-crash${TAB}suppressed-rate" $T/alerts/sms-log'
teardown

setup
up 100
. "$SCRIPTS/alert-lib.sh"
echo '138"},{"x' >"$T/alerts/sms-to"
alert_add agent-crash x
round
check "malformed number never reaches ubus" '! grep -q send_sms $T/ubus.log && grep -q no-number $T/alerts/sms-log'
teardown

echo "## alert SMS language (devui.conf lang=, docs/ui-glossary.md §9)"
# The Chinese texts exactly as they were before English existed: zh must not
# move by one byte. The English ones are the glossary's, final.
SMS_KINDS='wifi-takeover wifi-restore-failed agent-silent agent-hung agent-crash datad-crash datad-degraded devui-crash devui-gave-up sms-test devui-theme-paused'
sms_zh() {
    case "$1" in
        wifi-takeover) m="管理后台没反应了。为了让你能上网，已自动打开U60的Wi-Fi。不用管。" ;;
        wifi-restore-failed) m="想自动打开U60的Wi-Fi没成功，还在重试。如果手机连不上U60，请重启它。" ;;
        agent-silent) m="管理后台超过5分钟没反应。上网一般不受影响；Wi-Fi要是关着，会自动打开。" ;;
        agent-hung) m="管理后台卡住了，已强制重启。不用管。" ;;
        agent-crash) m="管理后台意外退出，已自动重启。不用管。" ;;
        datad-crash) m="屏幕的数据服务意外退出，已自动重启。不用管。" ;;
        datad-degraded) m="屏幕的数据服务超过5分钟不正常，管理后台已改用备用方式读数据。上网不受影响。" ;;
        devui-crash) m="屏幕界面闪退了，已自动重新打开。不用管。" ;;
        devui-gave-up) m="屏幕界面连续打不开，已换成原厂界面，上网不受影响。长按屏幕右下角3秒可换回。" ;;
        sms-test) m="这是测试短信。收到了，说明告警短信能正常发到你手机。" ;;
        *) m="有一条新告警（$1），请到管理网页「系统→告警」查看。" ;;
    esac
    printf '【U60】%s（%s）' "$m" "$2"
}
sms_en() {
    case "$1" in
        wifi-takeover) m="Admin down; Wi-Fi turned on; no action needed" ;;
        wifi-restore-failed) m="Wi-Fi didn't start; retrying; stuck? Restart U60" ;;
        agent-silent) m="Admin silent 5+ min; internet usually fine" ;;
        agent-hung) m="Admin hung; force-restarted; no action needed" ;;
        agent-crash) m="Admin crashed; restarted; no action needed" ;;
        datad-crash) m="Data service crashed; restarted; no action needed" ;;
        datad-degraded) m="Data service down 5+ min; using fallback; net OK" ;;
        devui-crash) m="Screen UI crashed; reopened; no action needed" ;;
        devui-gave-up) m="Stock UI on; hold bottom-right 3s to switch back" ;;
        sms-test) m="Test SMS: alert texts reach your phone" ;;
        *) m="New alert $1; see Alerts on web" ;;
    esac
    printf '[U60] %s (%s)' "$m" "$2"
}
# sms_is <kind> <zh|en>: guard's text for <kind> equals the expected one, with
# the time taken just before or just after (a minute may turn over between)
sms_is() {
    if [ "$2" = en ]; then
        _b=$(LC_ALL=C date '+%d %b %H:%M'); SMS_GOT=$(sh "$GUARD" sms-text "$1"); _a=$(LC_ALL=C date '+%d %b %H:%M')
        [ "$SMS_GOT" = "$(sms_en "$1" "$_b")" ] || [ "$SMS_GOT" = "$(sms_en "$1" "$_a")" ]
    else
        _b=$(date '+%m-%d %H:%M'); SMS_GOT=$(sh "$GUARD" sms-text "$1"); _a=$(date '+%m-%d %H:%M')
        [ "$SMS_GOT" = "$(sms_zh "$1" "$_b")" ] || [ "$SMS_GOT" = "$(sms_zh "$1" "$_a")" ]
    fi
}
# sms_all <zh|en>: every kind; prints the kinds that differ
sms_all() { for k in $SMS_KINDS; do sms_is "$k" "$1" || printf '%s ' "$k"; done; }

setup
check "the reference texts: the English default with the longest kind is 68 chars" '[ "$(sms_en devui-theme-paused "01 Oct 14:32" | wc -c)" = 68 ]'
check "the ASCII test below does catch Chinese" '[ -n "$(sms_zh agent-crash "10-01 14:32" | tr -d " -~")" ]'
for k in $SMS_KINDS; do
    printf 'theme=1\nlang=en\nbright=80\n' >"$T/devui.conf"
    sms_is "$k" en; r=$?
    check "lang=en, $k: the glossary text" '[ $r = 0 ]'
    check "lang=en, $k: printable ASCII, <= 70 chars, no closing full stop (${#SMS_GOT})" \
        '[ -z "$(printf "%s" "$SMS_GOT" | tr -d " -~")" ] && [ "$(printf "%s" "$SMS_GOT" | wc -c)" -le 70 ] && case "$SMS_GOT" in *". ("* | *.) false ;; *) true ;; esac'
    check "lang=en, $k: time as 01 Oct 14:32" 'printf "%s" "$SMS_GOT" | grep -q " ([0-3][0-9] [A-Z][a-z][a-z] [0-2][0-9]:[0-5][0-9])$"'
done
rm -f "$T/devui.conf"
check "no devui.conf: every kind in Chinese, byte for byte as before" '[ -z "$(sms_all zh)" ]'
printf 'lang=zh\n' >"$T/devui.conf"
check "lang=zh: Chinese" '[ -z "$(sms_all zh)" ]'
: >"$T/devui.conf"
check "empty devui.conf: Chinese" '[ -z "$(sms_all zh)" ]'
for v in '' fr 1 'EN' 'en ' ' en' 'en_US' '$((x+1))' '`reboot`' 'zh;en'; do
    printf 'theme=0\nlang=%s\n' "$v" >"$T/devui.conf"
    check "lang=\"$v\" (not exactly en): Chinese" '[ -z "$(sms_all zh)" ]'
done
printf 'lang=en\r\n' >"$T/devui.conf"
check "lang=en with a CR: Chinese" '[ -z "$(sms_all zh)" ]'
printf 'lang=zh\nlang=en\n' >"$T/devui.conf"
check "two lang= lines: the last wins, as on the screen (en)" '[ -z "$(sms_all en)" ]'
printf 'lang=en\nlang=zh\n' >"$T/devui.conf"
check "two lang= lines: the last wins (zh)" '[ -z "$(sms_all zh)" ]'
printf 'xlang=en\n' >"$T/devui.conf"
check "a key that only ends in lang=: Chinese" '[ -z "$(sms_all zh)" ]'
mkdir -p "$T/dir.conf"; GUARD_DEVUI_CONF=$T/dir.conf
check "devui.conf a directory (unreadable): Chinese" '[ -z "$(sms_all zh)" ]'
export GUARD_DEVUI_CONF=$T/devui.conf
printf 'lang=en\n' >"$T/devui.conf"
check "an unknown kind is cut to 20 safe chars, still one SMS" 'g=$(sh "$GUARD" sms-text "a-very-long-kind-name-that-goes-on/\"x"); case "$g" in "[U60] New alert a-very-long-kind-nam; see Alerts on web ("*) [ "$(printf "%s" "$g" | wc -c)" -le 70 ] ;; *) false ;; esac'
teardown

setup
echo "8.00" >"$T/tz"
up 100
. "$SCRIPTS/alert-lib.sh"
echo "+8612300000000" >"$T/alerts/sms-to"
printf 'lang=en\n' >"$T/devui.conf"
alert_add agent-crash x
round
check "lang=en end to end: sent, English body in UCS-2 hex, still UNICODE" 'grep -q "agent-crash${TAB}sent" $T/alerts/sms-log && grep -q "\"message_body\":\"005B005500360030005D002000410064006D0069006E0020006300720061007300680065006400" $T/ubus.log && grep -q "\"encode_type\":\"UNICODE\"" $T/ubus.log'
printf 'lang=zh\n' >"$T/devui.conf"
alert_add datad-crash x
round
check "switched back to zh: the next SMS is Chinese, no restart" 'grep -q "datad-crash${TAB}sent" $T/alerts/sms-log && grep -q "\"message_body\":\"30100055003600303011" $T/ubus.log && [ "$(grep -c "\"message_body\":\"3010" $T/ubus.log)" = 1 ]'
teardown

echo "## log cap"
setup
export GUARD_CAP_LOGS="$T/ts.log $T/missing.log" GUARD_CAP_BYTES=100
head -c 150 /dev/zero | tr '\0' 'a' >"$T/ts.log"
round
check "over the cap: previous copy kept as .old" '[ "$(wc -c <"$T/ts.log.old")" -eq 150 ]'
check "over the cap: log started over" '[ ! -s "$T/ts.log" ]'
check "over the cap: logged" 'grep -q "capped $T/ts.log at 150 bytes" "$T/guard.log"'
head -c 50 /dev/zero | tr '\0' 'b' >"$T/ts.log"
round
check "under the cap: untouched" '[ "$(wc -c <"$T/ts.log")" -eq 50 ] && [ "$(wc -c <"$T/ts.log.old")" -eq 150 ]'
# a writer that did not open the log for append: leave it alone
head -c 150 /dev/zero | tr '\0' 'c' >"$T/ts.log"
mkdir -p "$T/proc/700/fd" "$T/proc/700/fdinfo"; ln -s "$T/ts.log" "$T/proc/700/fd/1"
printf 'pos:\t150\nflags:\t0100001\n' >"$T/proc/700/fdinfo/1"
round
check "non-append writer: not truncated" '[ "$(wc -c <"$T/ts.log")" -eq 150 ]'
check "non-append writer: logged once" '[ "$(grep -c "not capping" "$T/guard.log")" = 1 ]'
round
check "non-append writer: still logged once" '[ "$(grep -c "not capping" "$T/guard.log")" = 1 ]'
printf 'pos:\t150\nflags:\t0102001\n' >"$T/proc/700/fdinfo/1"
round
check "within the hour after a refusal: not rescanned" '[ "$(wc -c <"$T/ts.log")" -eq 150 ]'
up $(( $(cut -d. -f1 "$T/uptime") + 3700 ))
round
check "an hour later, append writer: capped" '[ ! -s "$T/ts.log" ]'
rm -rf "$T/proc/700"
unset GUARD_CAP_LOGS GUARD_CAP_BYTES
teardown

echo "## log cap: the /tmp list has its own, smaller cap"
setup
export GUARD_CAP_LOGS="$T/ts.log" GUARD_CAP_BYTES=100 GUARD_CAP_TMP_LOGS="$T/agent.log $T/devui.log" GUARD_CAP_TMP_BYTES=60
head -c 80 /dev/zero | tr '\0' 'a' >"$T/ts.log"
head -c 80 /dev/zero | tr '\0' 'b' >"$T/agent.log"
round
check "tmp list over its cap: capped, .old kept" '[ ! -s "$T/agent.log" ] && [ "$(wc -c <"$T/agent.log.old")" -eq 80 ]'
check "same size under the other list's cap: untouched" '[ "$(wc -c <"$T/ts.log")" -eq 80 ] && [ ! -e "$T/ts.log.old" ]'
# one log refused (a non-append writer) does not hold the others back
head -c 150 /dev/zero | tr '\0' 'c' >"$T/ts.log"
mkdir -p "$T/proc/701/fd" "$T/proc/701/fdinfo"; ln -s "$T/ts.log" "$T/proc/701/fd/1"
printf 'pos:\t150\nflags:\t0100001\n' >"$T/proc/701/fdinfo/1"
round
check "refused log: not truncated" '[ "$(wc -c <"$T/ts.log")" -eq 150 ]'
head -c 70 /dev/zero | tr '\0' 'd' >"$T/devui.log"
# an append writer on devui.log (O_APPEND 02000 set)
mkdir -p "$T/proc/702/fd" "$T/proc/702/fdinfo"; ln -s "$T/devui.log" "$T/proc/702/fd/1"
printf 'pos:\t70\nflags:\t0102001\n' >"$T/proc/702/fdinfo/1"
round
check "within the hour of another log's refusal: this one still capped" '[ ! -s "$T/devui.log" ] && [ "$(wc -c <"$T/devui.log.old")" -eq 70 ]'
check "refused log: logged once" '[ "$(grep -c "not capping" "$T/guard.log")" = 1 ]'
rm -rf "$T/proc/701" "$T/proc/702"
unset GUARD_CAP_LOGS GUARD_CAP_BYTES GUARD_CAP_TMP_LOGS GUARD_CAP_TMP_BYTES
teardown

echo "## standby sentinel records"
setup
export GUARD_NETDEV=$T/netdev GUARD_BACKLIGHT=$T/bl GUARD_STANDBY_STAT=$T/standby.stat
netdev() { printf 'rmnet_data0: 0 %s 0 0 0 0 0 0 0 0 0 0 0 0 0 0\ntailscale0: 0 %s 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n' "$1" "$2" >"$T/netdev"; }
tsproc() { # tsproc <pid> <switches>
    rm -rf "$T/proc/5"[0-9][0-9]; mkdir -p "$T/proc/$1/task/$1"; echo tailscaled >"$T/proc/$1/comm"
    printf 'voluntary_ctxt_switches:\t%s\nnonvoluntary_ctxt_switches:\t0\n' "$2" >"$T/proc/$1/task/$1/status"
}
# keep the agent's heartbeat fresh, so the Wi-Fi watchdog stays out of it
# (its stub sleep would move the fake clock and skew the rates)
sround() { hb "$(cut -d. -f1 "$T/uptime")"; round; }
echo 0 >"$T/bl"; netdev 1000 100; tsproc 500 1000; up 1000
sround
check "first round: nothing recorded yet" '[ ! -s "$T/standby.stat" ]'
netdev 1600 160; tsproc 500 1600; up 1060
sround
check "dark round: rates recorded" '[ "$(cat "$T/standby.stat")" = "1060 600 60 10.0 - - -" ]'
echo 255 >"$T/bl"; netdev 2000 200; up 1120
sround
check "lit screen: not recorded" '[ "$(wc -l <"$T/standby.stat")" = 1 ]'
echo 0 >"$T/bl"; netdev 2600 260; tsproc 501 50; up 1180
sround
check "restarted program: -" '[ "$(tail -n 1 "$T/standby.stat")" = "1180 600 60 - - - -" ]'
GUARD_WAKE_GAP=150; export GUARD_WAKE_GAP; netdev 3000 300; up 1500
sround
check "rounds too far apart (slept): not recorded" '[ "$(wc -l <"$T/standby.stat")" = 2 ]'
i=0; while [ $i -lt 70 ]; do up $((1560 + i * 60)); sround; i=$((i + 1)); done
check "keeps the last 60 lines" '[ "$(wc -l <"$T/standby.stat")" = 60 ]'
unset GUARD_NETDEV GUARD_BACKLIGHT GUARD_STANDBY_STAT; export GUARD_WAKE_GAP=100000000
teardown

echo "## LAN IPv6 off"
setup
export GUARD_LAN_V6_FLAG=$T/lan-ipv6-off GUARD_LAN_V6_SYSCTL=$T/disable_ipv6
echo 0 >"$T/disable_ipv6"; round
check "no flag: left alone" '[ "$(cat "$T/disable_ipv6")" = 0 ]'
touch "$T/lan-ipv6-off"; round
check "flag: IPv6 disabled on the bridge" '[ "$(cat "$T/disable_ipv6")" = 1 ]'
check "flag: logged" 'grep -q "disabled it on br-lan" "$T/guard.log"'
round
check "already off: not logged again" '[ "$(grep -c "disabled it on br-lan" "$T/guard.log")" = 1 ]'
echo 0 >"$T/disable_ipv6"; round
check "turned back on (firmware): disabled again" '[ "$(cat "$T/disable_ipv6")" = 1 ]'
unset GUARD_LAN_V6_FLAG GUARD_LAN_V6_SYSCTL
teardown

echo "## modem crash recovery"
setup
export GUARD_MSS_RECOVERY=$T/recovery
echo disabled >"$T/recovery"; round
check "disabled: enabled" '[ "$(cat "$T/recovery")" = enabled ]'
check "disabled: logged" 'grep -q "modem crash recovery was disabled" "$T/guard.log"'
round
check "already enabled: not logged again" '[ "$(grep -c "modem crash recovery" "$T/guard.log")" = 1 ]'
rm -f "$T/recovery"; round
check "no remoteproc: left alone" '[ ! -e "$T/recovery" ]'
unset GUARD_MSS_RECOVERY
teardown

echo "## data service degraded marker (datad_feed.rs writes, we only read)"
# The marker's mtime is the container's real clock; GUARD_WALL_NOW is set
# relative to it. A fresh heartbeat keeps the agent alerts out of the queue.
dmark() { # dmark <start-offset-from-mtime> — (re)write the marker, set M = its mtime
    printf '%s\n%s\n' "$(( $(date +%s) + $1 ))" "datad v2 unreachable" >"$T/datad-degraded"
    M=$(date -r "$T/datad-degraded" +%s)
}
dcount() { cat "$T/alerts/queue" 2>/dev/null | grep -c "${TAB}datad-degraded${TAB}"; }
setup
up 2000; hb 1990
dmark -100; GUARD_WALL_NOW=$((M + 10)) round
check "fresh marker, degraded < 5 min: no alert" '[ "$(dcount)" = 0 ]'
dmark -400; GUARD_WALL_NOW=$((M + 120)) round
check "fresh marker, started > 5 min ago: alerts" '[ "$(dcount)" = 1 ]'
check "alert text carries the reason" 'grep -q "datad v2 unreachable" "$T/alerts/queue"'
GUARD_WALL_NOW=$((M + 150)) round
check "next round, same episode: not again" '[ "$(dcount)" = 1 ]'
up 2400; rm -f "$T/heartbeat"; GUARD_STALE=100 GUARD_WALL_NOW=$((M + 160)) round
up 2410; hb 2405; GUARD_WALL_NOW=$((M + 170)) round
check "agent outage ended meanwhile (end_episode): still not again" '[ "$(dcount)" = 1 ] && [ ! -f $T/state/episode ]'
rm -f "$T/datad-degraded"; GUARD_WALL_NOW=$((M + 180)) round
check "recovered (agent deleted marker): no alert, dedup flag cleared" '[ "$(dcount)" = 1 ] && ! ls $T/state/alerted-datad-* >/dev/null 2>&1'
dmark -400; GUARD_WALL_NOW=$((M + 120)) round
check "new episode (new start time): alerts again" '[ "$(dcount)" = 2 ]'
teardown

setup
up 100
dmark -900; GUARD_WALL_NOW=$((M + 10)) round
check "inside the boot grace (marker may be last boot's): no alert" '[ "$(dcount)" = 0 ]'
teardown

setup
up 2000; hb 1990
dmark -900; GUARD_WALL_NOW=$((M + 200)) round
check "stale marker (mtime > 3 min): no alert, marker left in place" '[ "$(dcount)" = 0 ] && [ -f $T/datad-degraded ]'
teardown

setup
up 2000; hb 1990
GUARD_WALL_NOW=$(( $(date +%s) + 60 )) round
check "no marker (recovered / never degraded): no alert" '[ "$(dcount)" = 0 ]'
printf 'garbage\n' >"$T/datad-degraded"; GUARD_WALL_NOW=$(( $(date +%s) + 60 )) round
check "unparsable start: no alert, marker not touched" '[ "$(dcount)" = 0 ] && [ "$(cat $T/datad-degraded)" = garbage ]'
teardown

echo "## sentinel fingerprints (docs/LEDGER.md §12)"
fpl() { awk -v n="$1" '$1 == n { print $2, $3 }' "$T/state/ledger/fp"; }
setup
mkdir -p "$T/proc/501" "$T/proc/502"
echo tailscaled >"$T/proc/501/comm"; echo A >"$T/proc/501/exe"
echo zte-agent >"$T/proc/502/comm"; echo B >"$T/proc/502/exe"
up 1000; hb 995; round
check "fp: pid and md5 of each sentinel program" '[ "$(fpl tailscaled)" = "501 $(echo A | md5sum | cut -c1-8)" ] && [ "$(fpl zte-agent)" = "502 $(echo B | md5sum | cut -c1-8)" ]'
check "fp: a program not running is '- -'" '[ "$(fpl zwrt-datad)" = "- -" ]'
check "fp: first sight is not a change" '[ ! -f $T/state/ledger/fp-changed ]'
rm -rf "$T/proc/502"; mkdir -p "$T/proc/503"; echo zte-agent >"$T/proc/503/comm"; echo B >"$T/proc/503/exe"
up 1060; hb 1055; round
check "restart on the same binary: pid follows, no change recorded" '[ "$(fpl zte-agent)" = "503 $(echo B | md5sum | cut -c1-8)" ] && [ ! -f $T/state/ledger/fp-changed ]'
rm -rf "$T/proc/503"; mkdir -p "$T/proc/504"; echo zte-agent >"$T/proc/504/comm"; echo C >"$T/proc/504/exe"
up 1120; hb 1115; round
check "new binary: fp-changed = that round's uptime" '[ "$(cat $T/state/ledger/fp-changed)" = 1120 ]'
teardown

setup
unset GUARD_LEDGER_FG
mkdir -p "$T/proc/601"; echo tailscaled >"$T/proc/601/comm"; mkfifo "$T/proc/601/exe"  # md5sum blocks on it
up 1000; hb 995; round
j1=$(cut -d' ' -f1 "$T/state/ledger/fp.pid")
up 1060; hb 1055; round
j2=$(cut -d' ' -f1 "$T/state/ledger/fp.pid")
check "a hung background job: the round goes on and does not start a second one" '[ -n "$j1" ] && [ "$j1" = "$j2" ] && kill -0 "$j1"'
echo x >"$T/proc/601/exe"; sleep 1   # let it finish before teardown
export GUARD_LEDGER_FG=1
teardown

echo "## ledger core (docs/LEDGER.md §2-6)"
B=feedc0de-0000-4000-8000-000000000001
lines() { cat "$T"/ledger/boot-*.jsonl 2>/dev/null; }
jsonok() { # every ledger line is flat JSON in the documented shape (docs/LEDGER.md §3)
    lines | awk '!/^[{]"v":1,"seq":[0-9]+,"n":[0-9]+,"up":[0-9]+(\.[0-9])?,"t":(null|[0-9]+),"k":"[a-z_]+"(,"id":"[A-Za-z0-9._-]+")?(,"[a-z_0-9]+":(null|-?[0-9]+(\.[0-9]+)?|"[^"\\]*"))*[}]$/ { n++; print "  bad json: " $0 } END { exit n > 0 }'
}
ev() { # ev <dir> <id> <boot> <up> <k> <fragment> — a spool file as a producer writes it
    mkdir -p "$1"
    printf '%s\t%s\t%s\t%s\n%s\n' "$2" "$3" "$4" "$5" "$6" >"$1/$2.ev"
}
setup
up 1000; hb 995; round
seg1=$(ls "$T"/ledger/boot-000001-* 2>/dev/null)
check "first boot: sequence 1, a bootmap line, one segment" '[ "$(cat $T/ledger/seq)" = 1 ] && [ "$(cut -d" " -f1,2 $T/ledger/bootmap)" = "1 $B" ] && [ -n "$seg1" ]'
check "the boot line: fixed key order; clock trusted, so t is set" 'head -n 1 "$seg1" | grep -q "^{\"v\":1,\"seq\":1,\"n\":1,\"up\":1000,\"t\":[0-9][0-9]*,\"k\":\"boot\",\"boot\":\"$B\",\"code\":"'
up 1060; hb 1055; round
check "guard restarted in the same boot: same sequence, no second boot line" '[ "$(cat $T/ledger/seq)" = 1 ] && [ "$(lines | grep -c "\"k\":\"boot\"")" = 1 ]'
echo feedc0de-0000-4000-8000-000000000002 >"$T/bootid"; rm -rf "$T/state"; up 100; round
check "next boot: sequence 2 in a segment of its own" '[ "$(cat $T/ledger/seq)" = 2 ] && [ -f $T/ledger/boot-000002-feedc0de-001.jsonl ]'
echo junk >"$T/ledger/seq"; echo feedc0de-0000-4000-8000-000000000003 >"$T/bootid"; rm -rf "$T/state"; up 100; round
check "seq file damaged: recovered from bootmap and segment names" '[ "$(cat $T/ledger/seq)" = 3 ]'
teardown

setup
up 1000; hb 995; round
ev "$T/ledger/spool" ssr-feedc0de-7 "$B" 990.5 ssr ',"kseq":7,"line":"fatal error received"'
ev "$T/spool-tmp" link-feedc0de-000001 "$B" 991.0 link ',"state":"down","cause":"ssr","res":2'
round
check "spool events go in with their id and their own time, then are deleted" 'lines | grep -q "\"up\":990.5,\"t\":[0-9]*,\"k\":\"ssr\",\"id\":\"ssr-feedc0de-7\",\"kseq\":7" && lines | grep -q "\"k\":\"link\",\"id\":\"link-feedc0de-000001\"" && [ -z "$(ls $T/ledger/spool/*.ev $T/spool-tmp/*.ev 2>/dev/null)" ]'
ev "$T/ledger/spool" ssr-feedc0de-7 "$B" 990.5 ssr ',"kseq":7,"line":"fatal error received"'
round
check "same id again (killed after writing, before deleting): written once" '[ "$(lines | grep -c ssr-feedc0de-7)" = 1 ] && [ ! -f $T/ledger/spool/ssr-feedc0de-7.ev ]'
ev "$T/ledger/spool" res-x feedc0de-0000-4000-8000-00000000000f 12.0 ssr_result ',"result":"ok"'
round
check "an event of a boot missing from bootmap goes in with seq 0" 'lines | grep -q "^{\"v\":1,\"seq\":0,.*\"id\":\"res-x\""'
printf 'garbage\n,"a":1\n' >"$T/ledger/spool/bad1.ev"
ev "$T/ledger/spool" inj-1 "$B" 1.0 'k"x' ',"a":1'
round
check "malformed spool files are set aside, not written" '[ -f $T/ledger/spool/bad/bad1.ev ] && [ -f $T/ledger/spool/bad/inj-1.ev ] && ! lines | grep -q inj-1'
seg=$(cat "$T/state/ledger/seg"); printf '{"v":1,"seq":1,"n":9' >>"$seg"   # torn by a power cut
ev "$T/ledger/spool" oom-feedc0de-9 "$B" 995.0 oom ',"line":"Out of memory"'
round
check "after a torn tail the next line starts on a line of its own" 'grep -q "^{\"v\":1,\"seq\":1,\"n\":9$" "$seg" && grep -q "^{\"v\":1,\"seq\":1,\"n\":[0-9]*,.*oom-feedc0de-9" "$seg"'
teardown

setup
export GUARD_LEDGER_SEG_MAX=400
up 1000; hb 995; round
i=0; while [ $i -lt 6 ]; do ev "$T/spool-tmp" "link-feedc0de-00000$i" "$B" 1000.0 link ',"state":"up","cause":"other","res":60'; i=$((i + 1)); done
round
check "a full segment rolls over to the next part; n keeps counting" '[ "$(ls $T/ledger/boot-000001-* | wc -l)" -ge 2 ] && cat $T/ledger/boot-000001-* | sed -n "s/.*\"n\":\([0-9]*\).*/\1/p" | awk "\$1 != NR { bad = 1 } END { exit bad || NR < 7 }"'
unset GUARD_LEDGER_SEG_MAX
teardown

setup
export GUARD_LEDGER_DAY_MAX=10
up 1000; hb 995; round
ev "$T/spool-tmp" link-a "$B" 1000.0 link ',"state":"down","cause":"ssr","res":2'
ev "$T/ledger/spool" ssr-b "$B" 1000.0 ssr ',"kseq":1'
round
check "over the day's budget: link dropped, ssr (critical) still written" '! lines | grep -q link-a && lines | grep -q ssr-b && [ ! -f $T/spool-tmp/link-a.ev ]'
unset GUARD_LEDGER_DAY_MAX
teardown

setup
printf '#!/bin/sh\necho "Filesystem 1K-blocks Used Available Use%% Mounted on"\necho "/dev/ubi0 4000000 3950000 50000 99%% /data"\n' >"$T/bin/df"
up 1000; hb 995; round
ev "$T/ledger/spool" ssr-c "$B" 1000.0 ssr ',"kseq":2'
round
check "under 100 MB free: the boot line goes in, the ssr waits in the spool" 'lines | grep -q "\"k\":\"boot\"" && ! lines | grep -q ssr-c && [ -f $T/ledger/spool/ssr-c.ev ]'
teardown

setup
up 1000; hb 995; round
(flock 8 && exec /bin/sleep 3) 8>>"$T/state/ledger/writer.lock" &
lk=$!
/bin/sleep 0.3
ev "$T/ledger/spool" ssr-d "$B" 1000.0 ssr ',"kseq":3'
round
check "another writer holds the lock: this job writes nothing" '! lines | grep -q ssr-d && [ -f $T/ledger/spool/ssr-d.ev ]'
wait "$lk"
round
check "once it is gone, the event goes in" 'lines | grep -q ssr-d'
teardown

setup # a line already appended (visible) but maybe not on flash yet: dropping its spool copy fsyncs first
up 1000; hb 995; round
seg=$(ls "$T"/ledger/boot-000001-* | head -n 1)
printf '{"v":1,"seq":1,"n":90,"up":999,"t":null,"k":"ssr","id":"ssr-feedc0de-77","kseq":77}\n' >>"$seg"
ev "$T/ledger/spool" ssr-feedc0de-77 "$B" 999 ssr ',"kseq":77'
: >"$T/fsync.log"
up 1060; hb 1055; GUARD_FSYNC_LOG=$T/fsync.log round
check "the id is in the ledger: the segment is fsynced, then the spool file dropped, the event not doubled" '[ ! -f $T/ledger/spool/ssr-feedc0de-77.ev ] && grep -qx "$seg" $T/fsync.log && [ "$(lines | grep -c "\"id\":\"ssr-feedc0de-77\"")" = 1 ]'
teardown

echo "## boot events and backfill (docs/LEDGER.md §10)"
kl() { printf '%s : reboot_reason_code=%s!!! \n' "$1" "$2"; } # a key.log reason-code line
setup
export GUARD_KEYLOG=$T/key.log GUARD_CRASHLOG_DIR=$T/crashlog GUARD_MC_TMP=$T/mc_tmp GUARD_MSS_RECOVERY=$T/recovery
{ echo "2025-01-04 00:00:18 : usb_mode=user "; kl "2025-01-04 00:00:19" 1150; echo "x"; kl "2025-01-04 00:00:19" 1182; } >"$T/key.log"
printf "config x\n\toption mode_main_state 'mode_power_on'\n" >"$T/mc_tmp"
mkdir -p "$T/crashlog/zte-agent"; printf 'program: zte-agent\nstatus:  exit 1\n' >"$T/crashlog/zte-agent/20250104-000100-up60.log"
echo disabled >"$T/recovery"
cat >"$T/bin/uci" <<EOF
#!/bin/sh
echo "\$*" >>$T/uci.log
case "\$*" in
    *SoftwareVersion*) printf '%s\n' 'MU5250 "B27" \\ ok' ;;
    *"show zte_nwinfo"*) printf '%s\n' "zte_nwinfo.sys_info=nwinfo" "zte_nwinfo.sys_info.net_select='WCDMA_AND_LTE'" "zte_nwinfo.sys_info.net_select_mode='auto_select'" ;;
    *"show zwrt_zte_nwinfo"*) echo "zwrt_zte_nwinfo.x.net_select='NOT_THIS_PACKAGE'" ;;
    *"show zwrt_zte_mc.reboot_schedule"*) printf '%s\n' "zwrt_zte_mc.reboot_schedule=zudata_reboot_fun" "zwrt_zte_mc.reboot_schedule.reboot_schedule_enable='1'" \
        "zwrt_zte_mc.reboot_schedule.reboot_schedule_mode='1'" "zwrt_zte_mc.reboot_schedule.reboot_dow='2'" "zwrt_zte_mc.reboot_schedule.reboot_hour1='2'" ;;
    *"show zwrt_router.cutoff_protect"*) printf '%s\n' "zwrt_router.cutoff_protect=zudata_cutoff_protect" "zwrt_router.cutoff_protect.reboot_times='0'" ;;
    *"show zwrt_data_commit.wwaniface1"*) printf '%s\n' "zwrt_data_commit.wwaniface1=interface" "zwrt_data_commit.wwaniface1.proto='qmi'" \
        "zwrt_data_commit.wwaniface1.connect_fail_reboot_enable='1'" "zwrt_data_commit.wwaniface1.connect_fail_reboot_counts='60'" ;;
    *time_from_utc*) cat $T/tz 2>/dev/null ;;
esac
EOF
chmod +x "$T/bin/uci"
mkdir -p "$T/proc/701"; echo zwrt-datad >"$T/proc/701/comm"; echo D >"$T/proc/701/exe"
printf 'A=1\000ZWRT_DATAD_UBUS=socket\000' >"$T/proc/701/environ"
up 1000; hb 995; sh "$GUARD" started; round
bl=$(lines | grep '"k":"boot"')
check "boot line: this boot's reason code, mode, cleaned firmware string, net_select" 'echo "$bl" | grep -q "\"code\":1182,\"mode\":\"mode_power_on\",\"fw\":\"MU5250 B27  ok\",\"net_select\":\"WCDMA_AND_LTE\""'
check "boot line: the vendor auto-reboot settings as option=value, no uci header lines" 'echo "$bl" | grep -q "\"rb_weekly\":\"reboot_schedule_enable=1 reboot_schedule_mode=1 reboot_dow=2\",\"rb_cutoff\":\"reboot_times=0\",\"rb_connfail\":\"connect_fail_reboot_enable=1 connect_fail_reboot_counts=60\",\"pon\":null"'
check "ver: datad by its running binary, with its ubus backend" 'lines | grep -q "\"k\":\"ver\",\"prog\":\"zwrt-datad\",\"md5\":\"$(echo D | md5sum | cut -c1-8)\",\"how\":\"exe\",\"pid\":701,\"extra\":\"ubus=socket\""'
check "ver: guard itself by its script file, not by /bin/sh" 'lines | grep -q "\"k\":\"ver\",\"prog\":\"u60-guard.sh\",\"md5\":\"$(md5sum $GUARD | cut -c1-8)\",\"how\":\"file\""'
check "first go-live: no history backfilled; crash files only recorded" '! lines | grep -q boot_backfill && ! lines | grep -q svc_exit && [ -f $T/ledger/state/init-done ] && grep -q "zte-agent/20250104-000100-up60.log" $T/ledger/state/crashlog.seen'
check "guard_start from started.log; recovery seen, set by guard" 'lines | grep -q "\"k\":\"guard_start\",\"id\":\"gs-feedc0de-1000\",\"requested\":0" && lines | grep -q "\"k\":\"recovery_seen\",\"value\":\"enabled\",\"set_by_guard\":1"'
# the next boot: key.log shows one more boot in between that never got a ledger
{ kl "2025-01-04 00:00:19" 1134; echo "y"; kl "2025-01-04 00:00:19" 1182; } >>"$T/key.log"
mkdir -p "$T/crashlog/zwrt-datad"; printf 'program: zwrt-datad\nstatus:  killed by SIGSEGV\n' >"$T/crashlog/zwrt-datad/20250104-000200-up120.log"
printf '{\n\t"wa_inner_version": "BD_CNMU5250V1.0.0B27",\n\t"cr_inner_version": ""\n}\n' >"$T/device_info"
printf '{\n\t"power_on_reason": 1\n}\n' >"$T/pm"
echo feedc0de-0000-4000-8000-000000000002 >"$T/bootid"; rm -rf "$T/state"; up 100; sh "$GUARD" started; round
check "boot line: the power-on reason as zwrt_bsp.pm list gives it" 'lines | grep "\"seq\":2," | grep "\"k\":\"boot\"" | grep -q ",\"pon\":1}"'
check "boot line: firmware from device_info's wa_inner_version when it answers (the build number is in it)" 'lines | grep "\"seq\":2," | grep "\"k\":\"boot\"" | grep -q "\"fw\":\"BD_CNMU5250V1.0.0B27\""'
check "next boot: the boot in between is backfilled; this one is the boot line" '[ "$(lines | grep -c boot_backfill)" = 1 ] && lines | grep -q "\"k\":\"boot_backfill\",\"id\":\"kl-[0-9]*-5\",\"code\":1134,\"at\":\"2025-01-04 00:00:19\",\"started\":null,\"gap\":0" && lines | grep "\"seq\":2," | grep "\"k\":\"boot\"" | grep -q "\"code\":1182"'
check "a new crash file becomes svc_exit (status, crash uptime)" 'lines | grep -q "\"k\":\"svc_exit\",\"id\":\"cl-[0-9a-f]*\",\"prog\":\"zwrt-datad\",\"file\":\"20250104-000200-up120.log\",\"status\":\"killed by SIGSEGV\",\"found\":\"boot_init\",\"crash_up\":120"'
rm -f "$T/state/ledger/boot.done"; round
check "running the boot steps again doubles nothing" '[ "$(lines | grep -c boot_backfill)" = 1 ] && [ "$(lines | grep -c "\"k\":\"boot\"")" = 2 ] && [ "$(lines | grep -c svc_exit)" = 1 ]'
mv "$T/key.log" "$T/key.log.0" # the firmware's rotation keeps the inode
{ echo "z"; kl "2025-01-04 00:00:19" 1155; kl "2025-01-04 00:00:19" 1182; } >"$T/key.log"
echo feedc0de-0000-4000-8000-000000000003 >"$T/bootid"; rm -rf "$T/state"; up 100; sh "$GUARD" started; round
check "after a rotation: the anchor is found in key.log.0, the new file's earlier boot backfilled" '[ "$(lines | grep -c boot_backfill)" = 2 ] && lines | grep "boot_backfill" | grep -q "\"code\":1155,.*\"gap\":0"'
rm -f "$T/key.log" "$T/key.log.0"
{ echo "other"; kl "2025-01-04 00:00:19" 1185; kl "2025-01-04 00:00:19" 1182; } >"$T/key.log"
echo feedc0de-0000-4000-8000-000000000004 >"$T/bootid"; rm -rf "$T/state"; up 100; sh "$GUARD" started; round
check "anchor lost: what key.log shows is backfilled and marked as a possible gap" 'lines | grep "boot_backfill" | grep -q "\"code\":1185,.*\"gap\":1"'
printf 'program: zwrt-datad\nstatus:  exit 139\nlonger now\n' >"$T/crashlog/zwrt-datad/20250104-000200-up120.log"
round
check "same crash file name, new content (same second in another boot): another svc_exit" '[ "$(lines | grep -c svc_exit)" = 2 ]'
mkdir -p "$T/state"; echo "deploy ledger-step1" >"$T/state/stop-requested"; up 1200; sh "$GUARD" started; round
check "a requested restart is marked so, and the marker is used up" 'lines | grep -q "\"k\":\"guard_start\",\"id\":\"gs-feedc0de-1200\",\"requested\":1,\"by\":\"deploy\",\"why\":\"ledger-step1\"" && [ ! -f $T/state/stop-requested ]'
unset GUARD_KEYLOG GUARD_CRASHLOG_DIR GUARD_MC_TMP GUARD_MSS_RECOVERY
teardown

echo "## clock trust (docs/LEDGER.md §7)"
ck() { cut -d' ' -f1,4 "$T/state/clock-ok"; }
setup
up 1000; hb 995
. "$SCRIPTS/alert-lib.sh"
echo "12300000000" >"$T/alerts/sms-to"
alert_add agent-crash x
GUARD_WALL_NOW=1735948800 round # 2025-01-04: what the device reads before NTP
check "2025-01-04 before NTP: not trusted" '[ "$(ck)" = "0 none" ]'
check "2025-01-04 before NTP: SMS left pending, not sent" '[ ! -f $T/alerts/sms-done ] && ! grep -q send_sms $T/ubus.log'
up 1060; hb 1055; round
check "NTP sets the clock (offset leaps a day+): trusted by the jump" '[ "$(ck)" = "1 jump" ]'
check "then the pending SMS goes out" 'grep -q "agent-crash${TAB}sent" $T/alerts/sms-log'
teardown
setup
up 1000; hb 995; round
check "first deployment, 2026, no last trusted clock: trusted by the year" '[ "$(ck)" = "1 year" ]'
teardown
setup
up 1000; hb 995
echo $((GUARD_WALL_NOW + 3 * 86400)) >"$T/wall.last"
round
check "earlier than the last trusted clock minus a day: not trusted" '[ "$(ck)" = "0 none" ]'
echo $((GUARD_WALL_NOW - 3600)) >"$T/wall.last"
round
check "not earlier than it: trusted" '[ "$(ck)" = "1 last_wall" ]'
up 1060; hb 1055; GUARD_WALL_NOW=$((GUARD_WALL_NOW - 2 * 86400)) round
check "a trusted clock that falls back two days: revoked" '[ "$(ck)" = "0 revoked" ]'
teardown

echo "kernel log capture"
setup
up 100
export GUARD_KMSG=$T/kmsg GUARD_CRASHCAP_DIR=$T/crashcap GUARD_BOOT_ID_FILE=$T/bootid GUARD_CRASHCAP_KEEP=3 GUARD_CRASHCAP_MAX=100
printf '6,1,100,-;boot line\n5,2,200,-;audit: type=1400 audit(1.2:3): avc:  denied\n3,3,300,-;fatal error received\n' >"$T/kmsg"
printf 'deadbeef-0000\n' >"$T/bootid"
mkdir -p "$T/crashcap"; for b in aaaa bbbb cccc dddd; do echo x >"$T/crashcap/kmsg-$b.log"; sleep 1; done
sh "$GUARD" crashcap; sleep 1
check "writes kmsg-<boot>.log without audit lines" '[ "$(grep -c . $T/crashcap/kmsg-deadbeef.log)" = 2 ] && grep -q "fatal error" $T/crashcap/kmsg-deadbeef.log && ! grep -q audit $T/crashcap/kmsg-deadbeef.log'
check "keeps only the newest KEEP boots incl. this one" '[ "$(ls $T/crashcap | tr "\n" " ")" = "kmsg-cccc.log kmsg-dddd.log kmsg-deadbeef.log " ]'
check "remembers the reader pid" '[ -s $T/state/crashcap-pid ]'
head -c 150 /dev/zero | tr "\0" a >>"$T/crashcap/kmsg-deadbeef.log"; round
check "round cuts a file over MAX to its second half" '[ "$(wc -c <$T/crashcap/kmsg-deadbeef.log)" -le 50 ] && grep -q "cut " $T/guard.log'
# the reader ends (cat /dev/kmsg gets EPIPE once records it had not read are
# overwritten): the main loop starts it again, without what the file already has
C=$T/crashcap/kmsg-deadbeef.log
capwait() { _i=0; while [ $_i -lt 25 ] && ! grep -q "$1" "$C"; do busybox sleep 0.2; _i=$((_i + 1)); done; }
check "a round soon after the start: the ended reader is left alone (CRASHCAP_RESPAWN)" '[ "$(cat $T/state/crashcap-starts)" = 1 ] && ! grep -q "starting it again" $T/guard.log'
printf '6,1,100,-;boot line\n3,3,300,-;fatal error received\n6,9876,900,-;seen record\n' >"$C"
printf '6,9000,800,-;old record\n SUBSYSTEM=old\n4,10000,1000,-;usb 1-1: new device\n SUBSYSTEM=usb\n' >>"$T/kmsg"
up 500; round; capwait "new device"
check "ended reader started again: only records newer than the file's last (numbers, not text: 9000 < 9876 < 10000), continuation lines with their record" '[ "$(grep -c "boot line" $C)" = 1 ] && [ "$(grep -c "fatal error" $C)" = 1 ] && ! grep -q "old" $C && grep -q "usb 1-1: new device" $C && grep -q "^ SUBSYSTEM=usb" $C && grep -q "capture (pid [0-9]*) ended: starting it again" $T/guard.log && [ "$(cat $T/state/crashcap-starts)" = 2 ]'
up 600; round
check "not again within CRASHCAP_RESPAWN of that start" '[ "$(cat $T/state/crashcap-starts)" = 2 ]'
export GUARD_CRASHCAP_RESTARTS=1
up 900; round; up 1300; round
check "at most CRASHCAP_RESTARTS restarts a boot; the give-up logged once" '[ "$(cat $T/state/crashcap-starts)" = 2 ] && [ "$(grep -c "not again" $T/guard.log)" = 1 ]'
unset GUARD_CRASHCAP_RESTARTS
unset GUARD_KMSG GUARD_CRASHCAP_DIR GUARD_BOOT_ID_FILE GUARD_CRASHCAP_KEEP GUARD_CRASHCAP_MAX
teardown

setup
up 100
export GUARD_KMSG=$T/kmsg GUARD_CRASHCAP_DIR=$T/crashcap GUARD_BOOT_ID_FILE=$T/bootid GUARD_CRASHCAP_KEEP=3
printf '6,1,100,-;boot line\n' >"$T/kmsg"
mkdir -p "$T/crashcap"; for b in old1 old2; do echo x >"$T/crashcap/kmsg-$b.log"; sleep 1; done # from before the list
cboot() { # cboot <boot8>: a new boot whose guard starts the capture
    kill "$(cat "$T/state/crashcap-pid" 2>/dev/null)" 2>/dev/null; rm -f "$T/state/crashcap-pid"
    printf '%s-0000\n' "$1" >"$T/bootid"; sh "$GUARD" crashcap; sleep 1
}
# a reboot loop (9-28): a long boot, a short one that died before NTP, one
# whose guard started but that died before its file was written (on the list,
# no file; with "keep the previous boot" this one took the only protected
# slot and the short boot before it was deleted), then a normal one
cboot long0001
cboot shrt0002; touch -d @1735948800 "$T/crashcap/kmsg-shrt0002.log"
cboot crsh0003; rm -f "$T/crashcap/kmsg-crsh0003.log"
cboot norm0004
check "reboot loop: the short boot's file stays although its mtime is the oldest; the oldest by boot order go" '[ -f $T/crashcap/kmsg-long0001.log ] && [ -f $T/crashcap/kmsg-shrt0002.log ] && [ -f $T/crashcap/kmsg-norm0004.log ] && [ ! -f $T/crashcap/kmsg-old1.log ] && [ ! -f $T/crashcap/kmsg-old2.log ]'
check "the list is in boot order, each boot once" '[ "$(sed "s|.*/kmsg-||" $T/crashcap/.order | tr "\n" " ")" = "long0001.log shrt0002.log crsh0003.log norm0004.log " ]'
kill "$(cat "$T/state/crashcap-pid")" 2>/dev/null; rm -f "$T/state/crashcap-pid"
sh "$GUARD" crashcap; sleep 1
check "a second start in the same boot adds nothing and removes nothing" '[ "$(grep -c . $T/crashcap/.order)" = 4 ] && [ -f $T/crashcap/kmsg-long0001.log ] && [ -f $T/crashcap/kmsg-shrt0002.log ]'
cboot next0005
check "one more boot: the oldest by boot order goes, not the one with the 2025 mtime" '[ ! -f $T/crashcap/kmsg-long0001.log ] && [ -f $T/crashcap/kmsg-shrt0002.log ] && [ -f $T/crashcap/kmsg-norm0004.log ] && [ -f $T/crashcap/kmsg-next0005.log ]'
seq 1 60 | sed "s|^|$T/crashcap/kmsg-x|; s|\$|.log|" >"$T/crashcap/.order"
cboot last0006
check "the list stays short" '[ "$(grep -c . $T/crashcap/.order)" -le 50 ] && grep -q kmsg-last0006 $T/crashcap/.order'
unset GUARD_KMSG GUARD_CRASHCAP_DIR GUARD_BOOT_ID_FILE GUARD_CRASHCAP_KEEP
teardown

echo "## what the ledger job samples each round (docs/LEDGER.md §4, §7, §11)"
fakeproc() { # fakeproc <pid> <comm> <start time> [<exe content>]
    mkdir -p "$T/proc/$1"
    echo "$2" >"$T/proc/$1/comm"
    echo "$1 ($2) S 1 $1 $1 0 -1 4194560 100 0 0 0 1 2 0 0 20 0 1 0 $3 1000 50" >"$T/proc/$1/stat"
    printf '%s' "${4:-$2 build 1}" >"$T/proc/$1/exe"
}
lround() { up "$1"; hb $(($1 - 5)); round; } # a round at uptime <s>, agent heartbeat fresh
setup
export GUARD_CRASHLOG_DIR=$T/crashlog GUARD_UID_LOG=$T/uid.log
fakeproc 700 zwrt-datad 5000
fakeproc 701 zte-agent 5001
fakeproc 702 u60-uid 5002 "uid build 1"
lround 1000
check "first sight of datad, agent, u60-uid: recorded, no restart" '! lines | grep -q proc_restart && grep -q "^zwrt-datad 700 5000\$" $T/state/ledger/procs'
rm -rf "$T/proc/700"
fakeproc 710 zwrt-datad 9000
lround 1060
check "datad under a new pid: proc_restart from 700 to 710, no crash file" 'lines | grep -q "\"k\":\"proc_restart\",\"id\":\"pr-feedc0de-710-9000\",\"prog\":\"zwrt-datad\",\"old\":700,\"new\":710,\"crashlog\":0"'
check "the same build: no second ver line for it" '[ "$(lines | grep -c "\"k\":\"ver\",\"prog\":\"zwrt-datad\"")" = 1 ]'
rm -rf "$T/proc/710"
fakeproc 720 zwrt-datad 9500 "zwrt-datad build 2"
mkdir -p "$T/crashlog/zwrt-datad"
printf 'program: zwrt-datad\nstatus: 139\n' >"$T/crashlog/zwrt-datad/20260928-120000-up1100.log"
lround 1120
check "a crash file for it in the same round: crashlog=1" 'lines | grep -q "\"prog\":\"zwrt-datad\",\"old\":710,\"new\":720,\"crashlog\":1"'
check "another build under the new pid: ver again, with that pid" '[ "$(lines | grep -c "\"k\":\"ver\",\"prog\":\"zwrt-datad\"")" = 2 ] && lines | grep "\"k\":\"ver\",\"prog\":\"zwrt-datad\"" | tail -n 1 | grep -q "\"pid\":720,"'
rm -rf "$T/proc/701"
lround 1180
check "agent gone: nothing yet" '! lines | grep -q "\"prog\":\"zte-agent\",\"old\""'
fakeproc 730 zte-agent 12000
lround 1240
check "and back: one restart, from the last pid seen" '[ "$(lines | grep -c "\"prog\":\"zte-agent\",\"old\":701,\"new\":730")" = 1 ]'
rm -rf "$T/proc/702"
fakeproc 740 u60-uid 13000 "uid build 2"
lround 1300
check "u60-uid restarted as another build: proc_restart and a ver line hashed for it" 'lines | grep -q "\"prog\":\"u60-uid\",\"old\":702,\"new\":740" && lines | grep "\"k\":\"ver\",\"prog\":\"u60-uid\"" | tail -n 1 | grep -q "\"pid\":740,"'
lround 1360
check "nothing changed: no new restart lines" '[ "$(lines | grep -c "\"k\":\"proc_restart\"")" = 4 ]'
check "no transaction file: every restart so far ship=0" '[ "$(lines | grep "\"k\":\"proc_restart\"" | grep -c "\"ship\":0}")" = 4 ]'
# a deployment's restarts (S4 leaves them out): the transaction of this boot,
# of the component that restarts the program, running or ended ≤ 10 min ago
txn() { mkdir -p "$T/u60-ship"; printf 'v=2\ntxn=x\ncomp=%s\nphase=%s\nboot_id=%s\nt_phase=%s\n' "$1" "$2" "${4:-$(cat $T/bootid)}" "$3" >"$T/u60-ship/txn"; }
txn touch done 1350
rm -rf "$T/proc/740"; fakeproc 750 u60-uid 14000
lround 1380
check "u60-uid restarted by a touch ship that just ended: ship=1" 'lines | grep -q "\"prog\":\"u60-uid\",\"old\":740,\"new\":750,\"crashlog\":0,\"ship\":1}"'
rm -rf "$T/proc/720"; fakeproc 760 zwrt-datad 15000
lround 1385
check "datad restarting during a touch ship is not that ship's: ship=0" 'lines | grep -q "\"prog\":\"zwrt-datad\",\"old\":720,\"new\":760,\"crashlog\":0,\"ship\":0}"'
txn datad trial 1386
rm -rf "$T/proc/760"; fakeproc 770 zwrt-datad 16000
lround 1390
check "datad in its own ship's trial: ship=1" 'lines | grep -q "\"old\":760,\"new\":770,\"crashlog\":0,\"ship\":1}"'
txn datad done 1391
rm -rf "$T/proc/770"; fakeproc 780 zwrt-datad 17000
lround 2000
check "more than 10 minutes after the ship ended: ship=0" 'lines | grep -q "\"old\":770,\"new\":780,\"crashlog\":0,\"ship\":0}"'
txn datad done 1990 feedc0de-0000-4000-8000-00000000beef
rm -rf "$T/proc/780"; fakeproc 790 zwrt-datad 18000
lround 2010
check "a transaction of another boot: ship=0" 'lines | grep -q "\"old\":780,\"new\":790,\"crashlog\":0,\"ship\":0}"'
rm -f "$T/u60-ship/txn"

printf '2026-09-28T10:00:00 starting (pid 740)\n2026-09-28T10:00:01 launched u60pro-devui pid 800 (attempt 1 of 3 before giving up)\n' >"$T/uid.log"
lround 2020
check "u60-uid launch lines are not events (its 'before giving up' does not count)" '! lines | grep -q "\"k\":\"uid\""'
printf '%s\n' "2026-09-28T10:05:00 giving up: 3 launches did not stay up; vendor UI on screen. Corner long-press or 'echo devui > /tmp/u60-uid.ctl' to retry" \
    "2026-09-28T10:05:00 starting the vendor UI" "2026-09-28T10:06:00 request: our UI (clears give-up and the attempt count)" >>"$T/uid.log"
lround 2080
check "give-up, hand-back, the owner's request: one uid line each, numbered by log line" '[ "$(lines | grep -c "\"k\":\"uid\"")" = 3 ] && lines | grep -q "\"k\":\"uid\",\"id\":\"uid-feedc0de-3\",\"what\":\"gave_up\",\"detail\":\"2026-09-28T10:05:00 giving up: 3 launches" && lines | grep -q "\"what\":\"handback\"" && lines | grep -q "\"what\":\"other\""'
lround 2140
check "read once: the next round adds none" '[ "$(lines | grep -c "\"k\":\"uid\"")" = 3 ]'
# over UID_LOG_CAP once read: renamed to .old, and the numbering goes on past it
GUARD_UID_LOG_CAP=100 lround 2200
check "read and over the cap: renamed to .old; uid.pos keeps the count (0 read, 5 before)" '[ ! -e $T/uid.log ] && [ "$(grep -c . $T/uid.log.old)" = 5 ] && [ "$(cat $T/state/ledger/uid.pos)" = "0 5" ] && grep -q "u60-uid log over 100 bytes" $T/guard.log'
printf '%s\n' "2026-09-28T11:00:00 giving up: again" "2026-09-28T11:00:01 launched u60pro-devui pid 900 (attempt 1 of 3 before giving up)" \
    "2026-09-28T11:00:02 starting the vendor UI" >"$T/uid.log"
lround 2260
check "the new file numbers on from 6: no id is used twice" 'lines | grep -q "\"id\":\"uid-feedc0de-6\",\"what\":\"gave_up\"" && lines | grep -q "\"id\":\"uid-feedc0de-8\",\"what\":\"handback\"" && [ "$(lines | grep -c "\"k\":\"uid\"")" = 5 ]'
# cut under the job (the logcap backstop: copy to .old, empty the file) with a line it had not read yet
echo "2026-09-28T11:01:00 request: vendor UI" >>"$T/uid.log"
cp "$T/uid.log" "$T/uid.log.old"
echo "2026-09-28T11:02:00 corner long-press: back to our UI" >"$T/uid.log"
lround 2320
check "cut by someone else: the unread rest of .old first (9), then the new file (10)" 'lines | grep -q "\"id\":\"uid-feedc0de-9\",\"what\":\"other\",\"detail\":\"2026-09-28T11:01:00 request" && lines | grep -q "\"id\":\"uid-feedc0de-10\",\"what\":\"other\",\"detail\":\"2026-09-28T11:02:00 corner" && [ "$(cat $T/state/ledger/uid.pos)" = "1 9" ]'
lround 2380
check "and nothing twice after that" '[ "$(lines | grep -c "\"k\":\"uid\"")" = 7 ]'
check "all of it is flat JSON" 'jsonok'
teardown

setup
dmark() { # dmark <since-offset-from-now> — (re)write the marker; M = its mtime
    printf '%s\n%s\n' "$(($(date +%s) + $1))" "datad v2 unreachable" >"$T/datad-degraded"
    M=$(date -r "$T/datad-degraded" +%s)
}
dlines() { lines | grep "\"k\":\"datad_degraded\""; }
dmark -30
GUARD_WALL_NOW=$((M + 10)) lround 200
check "a fresh marker inside the first 5 min of a boot: no episode yet" '[ -z "$(dlines)" ]'
S1=$(head -n 1 "$T/datad-degraded")
GUARD_WALL_NOW=$((M + 20)) lround 400
check "after 5 min: start, with its since and reason" 'dlines | grep -q "\"id\":\"dds-feedc0de-$S1\",\"state\":\"start\",\"since\":$S1,\"reason\":\"datad v2 unreachable\""'
GUARD_WALL_NOW=$((M + 30)) lround 460
check "still degraded next round: no second start" '[ "$(dlines | grep -c start)" = 1 ]'
rm -f "$T/datad-degraded"
GUARD_WALL_NOW=$((M + 40)) lround 520
check "marker gone: end, gone, 120 s by this boot's uptime" 'dlines | grep -q "\"id\":\"dde-feedc0de-$S1\",\"state\":\"end\",\"since\":$S1,\"reason\":null,\"dur_s\":120,\"how\":\"gone\""'
dmark -5
S2=$(head -n 1 "$T/datad-degraded")
GUARD_WALL_NOW=$((M + 10)) lround 580
printf '%s\n%s\n' "$((S2 + 100))" "another reason" >"$T/datad-degraded"
M=$(date -r "$T/datad-degraded" +%s)
S3=$((S2 + 100))
GUARD_WALL_NOW=$((M + 10)) lround 640
check "since changed: the old one ends (new_episode, 60 s), the new one starts" 'dlines | grep -q "\"since\":$S2,\"reason\":null,\"dur_s\":60,\"how\":\"new_episode\"" && dlines | grep -q "\"state\":\"start\",\"since\":$S3,"'
GUARD_WALL_NOW=$((M + 400)) lround 700
check "marker stale (the agent stopped refreshing it): end, stale, length unknown" 'dlines | grep -q "\"since\":$S3,\"reason\":null,\"dur_s\":null,\"how\":\"stale\""'
GUARD_WALL_NOW=$((M + 10)) lround 760
check "the same since fresh again: not started twice" '[ "$(dlines | grep -c "\"state\":\"start\",\"since\":$S3,")" = 1 ]'
rm -f "$T/datad-degraded"
lround 820
dmark -777 # a since of its own (the case runs within one second)
S4=$(head -n 1 "$T/datad-degraded")
GUARD_WALL_NOW=$((M + 10)) lround 880
check "an episode open at a reboot is on record in /data" 'grep -q "^$S4 feedc0de\$" $T/ledger/state/datad.open'
echo 0badc0de-0000-4000-8000-000000000009 >"$T/bootid"
rm -rf "$T/state"
rm -f "$T/datad-degraded"
lround 100
check "next boot: it ends there as reboot, length unknown" 'dlines | grep -q "\"id\":\"dde-feedc0de-$S4\",\"state\":\"end\",\"since\":$S4,\"reason\":null,\"dur_s\":null,\"how\":\"reboot\"" && [ ! -f $T/ledger/state/datad.open ]'
teardown

setup
GUARD_WALL_NOW=1735977600 lround 100 # 2025-01-04, before NTP: not trusted
check "clock not trusted yet: no clock line, no wall.last" '! lines | grep -q "\"k\":\"clock\"" && [ ! -f $T/wall.last ]'
GUARD_WALL_NOW=1790500100 lround 160 # NTP: the offset leaps
check "trusted by the leap: one clock line with the offset, wall.last written" 'lines | grep -q "\"k\":\"clock\",\"offset\":1790499940,\"how\":\"jump\"" && [ "$(cat $T/wall.last)" = 1790500100 ]'
GUARD_WALL_NOW=1790500160 lround 220
check "same offset next round: no second clock line; wall.last kept within the hour" '[ "$(lines | grep -c "\"k\":\"clock\"")" = 1 ] && [ "$(cat $T/wall.last)" = 1790500100 ]'
GUARD_WALL_NOW=1790503800 lround 280
check "an hour later: wall.last moves on" '[ "$(cat $T/wall.last)" = 1790503800 ]'
GUARD_WALL_NOW=1790300000 lround 340 # fell back two days
check "revoked: a clock line with a null offset" 'lines | grep -q "\"k\":\"clock\",\"offset\":null,\"how\":\"revoked\""'
teardown

setup
cp "$SCRIPTS/test/u60-guard/state-v2.json" "$T/state.json" # /v2/state built from data-service's golden /state (made-up SIM)
cat >"$T/bin/uci" <<EOF
#!/bin/sh
echo "\$*" >>$T/uci.log
case "\$*" in *"show zte_nwinfo"*) echo "zte_nwinfo.sys_info.net_select='\$(cat $T/net_select)'" ;; esac
exit 0
EOF
echo WL_AND_5G >"$T/net_select"
lround 1000
check "net cache from datad /state: rat, band, NR band, serving and home MCC-MNC" '[ "$(cut -d" " -f2- $T/state/ledger/net)" = "SA n78 78 460-00 460-00" ] && [ "$(cut -d" " -f1 $T/state/ledger/net)" = 1000 ]'
check "the IMSI is nowhere in /tmp or the ledger" '! grep -rq 460000000000001 $T/state $T/ledger'
check "first reading: net_change with net_select and both networks" 'lines | grep -q "\"k\":\"net_change\",\"net_select\":\"WL_AND_5G\",\"net\":\"460-00\",\"home\":\"460-00\""'
lround 1060
check "nothing changed: no second net_change" '[ "$(lines | grep -c "\"k\":\"net_change\"")" = 1 ]'
sed -i 's/"mcc":460/"mcc":440/; s/"mnc":0,/"mnc":10,/' "$T/state.json"
lround 1120
check "serving country changed (roaming in Japan): net_change; home stays" 'lines | grep -q "\"net_select\":\"WL_AND_5G\",\"net\":\"440-10\",\"home\":\"460-00\"" && [ "$(cut -d" " -f5,6 $T/state/ledger/net)" = "440-10 460-00" ]'
sed -i 's/"mnc":10,/"mnc":20,/' "$T/state.json"
lround 1180
check "another network in the same country: no net_change" '[ "$(lines | grep -c "\"k\":\"net_change\"")" = 2 ]'
echo TCHGWL_5G >"$T/net_select"
lround 1240
check "net_select changed: net_change" '[ "$(lines | grep -c "\"k\":\"net_change\"")" = 3 ]'
sed -i 's/"mcc":440/"mcc":310/; s/"mnc":20,/"mnc":260,/' "$T/state.json"
lround 1300
check "a country with 3-digit MNCs: 310-260" '[ "$(cut -d" " -f5 $T/state/ledger/net)" = 310-260 ]'
touch "$T/datad-down"
lround 1360
check "datad not answering: the cache stays as it was, the round goes on" '[ "$(cut -d" " -f5 $T/state/ledger/net)" = 310-260 ] && [ "$(cat $T/state/ledger/datad.read)" = 1300 ]'
rm -f "$T/datad-down"
GUARD_LEDGER_READ_BUDGET=0 lround 1420
check "no read time left this round: datad not asked, said in the log" '[ "$(cat $T/state/ledger/datad.read)" = 1300 ] && grep -q "no time left this round for datad" $T/guard.log'
cp "$T/state.json" "$T/state.fresh"
sed -i 's/"signal":{"revision":1,"observed_at":1790500000,"stale":false/"signal":{"revision":2,"observed_at":1790500000,"stale":true/' "$T/state.json"
lround 1480
check "signal block stale: read as a failed read, not its kept value (V2-29)" '[ "$(cut -d" " -f2-5 $T/state/ledger/net)" = "- - - -" ] && [ "$(lines | grep -c "\"k\":\"net_change\"")" = 4 ]'
mv -f "$T/state.fresh" "$T/state.json"
check "all of it is flat JSON" 'jsonok'
teardown

echo "## the hourly summary (docs/LEDGER.md §4: hour, hour_power, hour_proc)"
# The wall clock moves with uptime here (1790499600 is on the hour), so the
# hour turns when uptime crosses a multiple of 3600.
hsetup() {
    setup
    export GUARD_BAT=$T/bat GUARD_THERMAL=$T/thermal GUARD_CPUFREQ=$T/cpufreq GUARD_BACKLIGHT=$T/bl \
        GUARD_STANDBY_STAT=$T/standby.stat GUARD_STANDBY_BASE=$T/standby.baseline GUARD_NETDEV=$T/netdev
    mkdir -p "$T/bat" "$T/thermal/thermal_zone0" "$T/thermal/thermal_zone1" "$T/thermal/thermal_zone2" "$T/thermal/thermal_zone3" "$T/cpufreq/policy0"
    echo cpuss-0 >"$T/thermal/thermal_zone0/type"; echo 40000 >"$T/thermal/thermal_zone0/temp"
    echo sys-therm-1 >"$T/thermal/thermal_zone1/type"; echo 45000 >"$T/thermal/thermal_zone1/temp"
    echo cpuss-1 >"$T/thermal/thermal_zone2/type"; echo -40000 >"$T/thermal/thermal_zone2/temp" # below zero: not a maximum
    echo sdr0_pa >"$T/thermal/thermal_zone3/type"; echo 90000 >"$T/thermal/thermal_zone3/temp" # modem side: never read
    echo 1804800 >"$T/cpufreq/policy0/scaling_max_freq"
    echo 0 >"$T/bl"
    echo 4000000 >"$T/bat/voltage_now"
    echo -250000 >"$T/bat/current_now"
    echo 3000000 >"$T/bat/charge_counter"
    echo 80 >"$T/bat/capacity"
    echo 300 >"$T/bat/temp"
    echo Discharging >"$T/bat/status"
}
hround() { GUARD_WALL_NOW=$((1790499600 + $1)) lround "$1"; } # a round at uptime <s>, wall clock in step
hq() { echo $(($(cat "$T/bat/charge_counter") + $1)) >"$T/bat/charge_counter"; } # the counter moves by <µAh>
datadproc() { # datadproc <utime ticks>: datad as pid 700 with that much CPU used
    mkdir -p "$T/proc/700"
    echo zwrt-datad >"$T/proc/700/comm"
    echo "700 (zwrt-datad) S 1 700 700 0 -1 4194560 100 0 0 0 $1 0 0 0 20 0 1 0 5000 1000 50" >"$T/proc/700/stat"
    printf 'Name:\tzwrt-datad\nVmRSS:\t   6012 kB\n' >"$T/proc/700/status"
    printf 'datad build 1' >"$T/proc/700/exe"
}
hsetup
datadproc 0
printf 'v2\n2 4 1\n' >"$T/standby.baseline"
hround 3000
for k in 1 2 3 4; do hq -1000; hround $((3000 + 60 * k)); done
echo 1209600 >"$T/cpufreq/policy0/scaling_max_freq" # capped below this boot's highest
hq -1000; hround 3300
echo 100 >"$T/bl"                                   # screen on
hq -1000; hround 3360
echo 1804800 >"$T/cpufreq/policy0/scaling_max_freq"
echo 320 >"$T/bat/temp"
hq -1000; hround 3420
echo Charging >"$T/bat/status"
hq 500; hround 3480
echo Discharging >"$T/bat/status"
hq -1000; hround 3540
printf '2900 1 0\n3100 5 0\n3160 50 0\n3220 3 0\n3280 4 0\n' >"$T/standby.stat" # dark minutes; the 50 is traffic
datadproc 3600 # 36 s of CPU over the 600 s window
echo 79 >"$T/bat/capacity"
hq -1000; hround 3600
hl() { lines | grep "\"k\":\"$1\""; }
check "the hour turns: one of each hour line, numbered by where the window began" '[ "$(hl hour | wc -l)" = 1 ] && [ "$(hl hour_power | wc -l)" = 1 ] && [ "$(hl hour_proc | wc -l)" = 1 ] && hl hour | grep -q "\"id\":\"h-feedc0de-3000\""'
check "hour: from/to, wall clock, awake, rounds of the main loop, longest wait" 'hl hour | grep -q "\"from\":3000,\"to\":3600,\"ft\":1790502600,\"tt\":1790503200,\"awake\":600,\"asleep\":0,\"rounds\":10,\"skipped\":0,\"max_gap\":60,"'
check "hour: no network known yet, nothing abroad; free space and budget" 'hl hour | grep -q "\"net\":null,\"home\":null,\"abroad\":0,\"free_mb\":1171,\"dump_mb\":null,\"budget\":0"'
check "hour_power: charge counter x voltage; dark and lit minutes each in their cell" 'hl hour_power | grep -q "\"src\":\"counter\",\"e\":36.0,\"on_home_s\":240,\"on_home_e\":16.0,\"on_abroad_s\":0,\"on_abroad_e\":0.0,\"off_home_s\":300,\"off_home_e\":20.0,\"off_abroad_s\":0,\"off_abroad_e\":0.0"'
check "hour_power: charging minute kept out of the cells; battery, heat, capped CPU minutes" 'hl hour_power | grep -q "\"chg_s\":60,\"batt_from\":80,\"batt_to\":79,\"batt_tmax\":320,\"zone_tmax\":45000,\"throttle_s\":120,\"e_partial\":0"'
check "hour_proc: datad RSS, its CPU in per mille, the idle dark minutes and their median" 'hl hour_proc | grep -q "\"rss_datad\":6012," && hl hour_proc | grep -q "\"cpu_datad\":60,\"idle_rows\":3,\"idle_wan\":4"'
check "programs not running: RSS null, not 0" 'hl hour_proc | grep -q "\"rss_agent\":null"'
check "hour lines are flat JSON" 'jsonok'
check "doctor.sh reads what guard wrote (round trip): one boot, one hour" 'DOC_LEDGER_DIR=$T/ledger sh "$SCRIPTS/doctor.sh" --report 7d >$T/rep 2>&1; grep -q "账本：1 次开机，1 小时的记录" $T/rep && grep -q "^  S1 意外整机重启：" $T/rep'
check "and the summary lines were redone after the hour, three of them" '[ "$(wc -l <$T/ledger/summary)" = 3 ] && grep -q "账本（7 天" $T/ledger/summary && [ ! -f $T/state/ledger/summary.want ]'
teardown

hsetup
rm -f "$T/bat/charge_counter" # no fuel gauge counter: V x I
hround 3000
hround 3060
mkdir -p "$T/state/ledger"
echo "3070 3290 pm" >"$T/state/ledger/w.slept" # asleep, with evidence, for 220 s
hround 3300
hround 3600
check "without a counter: 1 W of V x I over the awake 380 s only; the hour marked partial" 'hl hour_power | grep -q "\"src\":\"vi\",\"e\":105.6,.*\"off_home_s\":380,\"off_home_e\":105.6,.*\"e_partial\":1"'
check "hour: asleep counts only the evidenced 220 s" 'hl hour | grep -q "\"awake\":380,\"asleep\":220,"'
teardown

hsetup # coverage (§8): every source working, then one of them not, then the main loop late
L=$T/state/ledger
export GUARD_CRASHLOG_DIR=$T/crashlog GUARD_UID_LOG=$T/uid.log GUARD_ROUTE=$T/route GUARD_RC_LOCAL=$T/rc.local
mkdir -p "$T/crashlog" "$L"
verdict() { :; }
cp "$SCRIPTS/test/u60-guard/state-v2.json" "$T/state.json"
echo "2026-09-28T10:00:00 starting (pid 1)" >"$T/uid.log"
printf 'Iface\tDestination\tGateway\tFlags\nrmnet_data0\t00000000\t00000000\t0001\n' >"$T/route"
echo "sh /data/tailscale/start.sh &" >"$T/rc.local"
: >"$T/tailscaled.sock"
echo "$$ 1 x" >"$L/watcher.pid" # alive: this test's own shell
echo "$$" >"$T/state/crashcap-pid"
wround() { echo "$1" >"$L/w.up"; verdict "$1"; hround "$1"; } # a round with the watcher's heartbeat fresh
acc() { awk -v k="$1" '$1 == k { print $2 }' "$L/acc"; }
wround 3600
wround 3660
wround 3720
rm -f "$T/uid.log"
wround 3780
wround 3840
echo "2026-09-28T10:30:00 request: vendor UI" >"$T/uid.log"
wround 3900
check "a source that stops: one gap event, from the last round it worked to the round it works again" 'lines | grep -q "\"k\":\"gap\",\"src\":\"uidlog\",\"from\":3720,\"to\":3900" && [ "$(lines | grep -c "\"k\":\"gap\"")" = 1 ] && [ "$(acc g_uidlog)" = 120 ]'
mv "$T/uid.log" "$T/uid.log.old" # the ledger job renamed it; u60-uid has not written since
echo "3930 skipped" >>"$L/rounds" # a round whose ledger job was skipped
wround 3960
check "u60-uid's log just renamed to .old: its last line counts, no uidlog gap" '[ "$(acc g_uidlog)" = 120 ] && [ "$(lines | grep -c "\"k\":\"gap\"")" = 1 ]'
check "a skipped ledger job: its interval counted for the job source" '[ "$(acc g_job)" = 60 ]'
wround 4380
check "the main loop 420 s late, awake: 360 s counted for the guard source" '[ "$(acc g_guard)" = 360 ]'
echo "4390 4590 pm" >>"$L/w.slept"
wround 4600
check "220 s late but 200 of them asleep with evidence: no more guard gap" '[ "$(acc g_guard)" = 360 ] && [ "$(acc asleep)" = 200 ]'
check "the other sources worked throughout" '[ -z "$(acc g_watcher)$(acc g_capture)$(acc g_crashlog)$(acc g_datad)" ] || [ "$(acc g_watcher)$(acc g_capture)$(acc g_crashlog)$(acc g_datad)" = 0000 ]'
check "Tailscale meant on, checked and healthy while awake" '[ "$(acc ts_on_s)" = 800 ] && [ "$(acc ts_chk_s)" = 800 ] && [ "$(acc ts_ok_s)" = 800 ]'
teardown


hsetup # Tailscale (S8, docs/LEDGER.md §6): tailscaled's LocalAPI, asked once a round while it is meant on
L=$T/state/ledger
export GUARD_ROUTE=$T/route GUARD_RC_LOCAL=$T/rc.local
printf 'Iface\tDestination\tGateway\tFlags\nrmnet_data0\t00000000\t00000000\t0001\n' >"$T/route"
echo "sh /data/tailscale/start.sh &" >"$T/rc.local"
: >"$T/tailscaled.sock"
cp "$T/ts.json" "$T/ts.good"
ts3() { echo $(for k in ts_on_s ts_chk_s ts_ok_s; do awk -v k=$k '$1 == k { print $2 }' "$L/acc"; done); } # on, checked, healthy
notes() { grep -c "Tailscale not checked" "$T/guard.log" 2>/dev/null; }
hround 3600
hround 3660
check "Running and online: on, checked, healthy" '[ "$(ts3)" = "60 60 60" ]'
check "the same request as the touch screen card: its socket, Host and path, at most 2 s" 'tail -n 1 $T/curl.log | grep -q -- "-m 2 --unix-socket $T/tailscaled.sock .*http://local-tailscaled.sock/localapi/v0/status"'
sed -i 's/"Online": true/"Online": false/; s/"mac", "Online": false/"mac", "Online": true/' "$T/ts.json"
hround 3720
check "the node not online (a peer online does not count): checked, not healthy" '[ "$(ts3)" = "120 120 60" ]'
sed 's/"Running"/"Starting"/' "$T/ts.good" >"$T/ts.json"
hround 3780
check "backend Starting: checked, not healthy" '[ "$(ts3)" = "180 180 60" ]'
cp "$T/ts.good" "$T/ts.json"
rm -f "$T/tailscaled.sock"
: >"$T/curl.log"
hround 3840
check "no socket (tailscaled not running): not healthy, curl not run" '[ "$(ts3)" = "240 240 60" ] && [ ! -s $T/curl.log ]'
: >"$T/tailscaled.sock"
echo 7 >"$T/ts-rc"
hround 3900
check "connection refused (a socket left behind): not healthy" '[ "$(ts3)" = "300 300 60" ]'
rm -f "$T/ts-rc"
echo 500 >"$T/ts-code"
hround 3960
check "HTTP 500 from tailscaled: not healthy" '[ "$(ts3)" = "360 360 60" ]'
echo 403 >"$T/ts-code"
hround 4020
check "HTTP 403 (our request refused): not checked, not counted as down; said in the log" '[ "$(ts3)" = "420 360 60" ] && [ "$(notes)" = 1 ] && grep -q "Tailscale not checked (HTTP 403)" $T/guard.log'
rm -f "$T/ts-code"
echo 4 >"$T/ts-rc"
hround 4080
hround 4140
check "curl without unix sockets: not checked; said once while the reason stays" '[ "$(ts3)" = "540 360 60" ] && [ "$(notes)" = 2 ] && grep -q "Tailscale not checked (curl exit 4)" $T/guard.log'
rm -f "$T/ts-rc"
hround 4200
check "checks work again: healthy" '[ "$(ts3)" = "600 420 120" ]'
echo '{"Version": "1.102.4-t0"}' >"$T/ts.json"
hround 4260
check "an answer without BackendState: not checked" '[ "$(ts3)" = "660 420 120" ] && grep -q "Tailscale not checked (no BackendState in the answer)" $T/guard.log'
cp "$T/ts.good" "$T/ts.json"
: >"$T/curl.log"
(GUARD_LEDGER_READ_BUDGET=0 hround 4320)
check "no read time left this round: curl not run, not checked; said in the log" '[ "$(ts3)" = "720 420 120" ] && [ ! -s $T/curl.log ] && grep -q "Tailscale not checked (no read time left)" $T/guard.log'
touch "$T/datad-hang" "$T/ts-hang"
hround 4380
check "datad and tailscaled both hanging: datad 2 s, Tailscale the 1 s left, 3 s in all; not healthy" 'tail -n 1 $T/curl.log | grep -q -- "-m 1 " && [ "$(cat $T/uptime)" = 4383 ] && [ "$(ts3)" = "782 482 120" ]'
rm -f "$T/datad-hang" "$T/ts-hang"
: >"$T/rc.local"
: >"$T/curl.log"
hround 4440
check "not started by rc.local: not meant on, not asked" '[ "$(ts3)" = "782 482 120" ] && [ ! -s $T/curl.log ]'
echo "sh /data/tailscale/start.sh &" >"$T/rc.local"
hround 7200
check "the hour line: meant on, checked, healthy seconds" 'hl hour_proc | grep -q "\"ts_on_s\":3542,\"ts_chk_s\":3242,\"ts_ok_s\":2880}"'
check "all of it is flat JSON" 'jsonok'
teardown

hsetup
rm -rf "$T/cpufreq" "$T/thermal" "$T/bat"
hround 3000
hround 3600
check "nothing to read: e, temperatures, throttle, idle all null" 'hl hour_power | grep -q "\"src\":null,\"e\":null,.*\"batt_tmax\":null,\"zone_tmax\":null,\"throttle_s\":null,\"e_partial\":1" && hl hour_proc | grep -q "\"idle_rows\":null,\"idle_wan\":null"'
teardown

hsetup
hround 9997000 # the hour turns at uptime 9997200 here
awk '{ print } END {
    print "e 99999900"; print "n_e 9"; print "n_vi 9"; print "e_partial 1"; print "chg_s 99999"
    split("on_home on_abroad off_home off_abroad", c, " ")
    for (i = 1; i <= 4; i++) { print c[i] "_s 99999"; print c[i] "_e 99999900" }
    print "batt_tmax 999"; print "zone_tmax 999999"; print "throttle_s 99999"; print "n_thr 9"; print "batt_from 100"
    print "awake 99999"; print "asleep 99999"; print "abroad 99999"
    split("guard job watcher capture uidlog crashlog datad", g, " "); for (i = 1; i <= 7; i++) print "g_" g[i], 99999
    print "ts_on_s 99999"; print "ts_chk_s 99999"; print "ts_ok_s 99999" }' \
    "$T/state/ledger/acc" >"$T/acc.big" && mv "$T/acc.big" "$T/state/ledger/acc"
echo 100 >"$T/bat/capacity"
hround 9999999
for k in hour hour_power hour_proc; do
    check "$k at its widest: under 512 bytes with room for 6-digit seq and n, not truncated" 'L=$(hl '$k'); [ -n "$L" ] && ! echo "$L" | grep -q trunc && [ $((${#L} + 12)) -le 512 ]'
done
teardown

hsetup # a reboot in the middle of an hour: the next boot writes that hour out from its copy in /data
hround 3000
hq -1000; hround 3060
hq -1000; hround 3120
check "each round leaves a copy of the hour's sums in /data" 'grep -q "^boot feedc0de 1\$" $T/ledger/state/acc.last && grep -q "^last 3120\$" $T/ledger/state/acc.last'
echo 0badc0de-0000-4000-8000-00000000000c >"$T/bootid" # rebooted; /tmp is empty again
rm -rf "$T/state"
hround 100
check "next boot: the cut hour goes out under the last boot's sequence, ending at its last round" 'lines | grep "\"k\":\"hour\"," | grep -q "\"seq\":1,.*\"id\":\"h-feedc0de-3000\",\"from\":3000,\"to\":3120,.*\"rounds\":null,.*\"cut\":1"'
check "its energy and wall clock kept; this boot's clock not applied to it" 'lines | grep "\"k\":\"hour_power\"" | grep -q "\"t\":null,.*\"src\":\"counter\",\"e\":8.0," && lines | grep "\"k\":\"hour\"," | grep -q "\"ft\":1790502600,\"tt\":1790502720,"'
check "the copy is used up; a new hour started for this boot" '! grep -q feedc0de $T/ledger/state/acc.last 2>/dev/null && grep -q "^from 100\$" $T/state/ledger/acc'
teardown

hsetup # retention: whole old boots go, oldest first, never this one
mkdir -p "$T/ledger"
old() { # old <seq> <t of its last line> <bytes>
    f=$(printf '%s/ledger/boot-%06d-0000000%s-001.jsonl' "$T" "$1" "$1")
    # 200-byte lines of x, $3 bytes in all (the device busybox has no fold)
    awk -v n="$3" 'BEGIN { s = "x"; while (length(s) < 200) s = s s; for (i = 0; i < n; i += 200) print substr(s, 1, ((n - i < 200) ? n - i : 200)) }' | sed 's/^/{"v":1,"seq":'"$1"',"n":1,"up":1,"t":null,"k":"x","pad":"/; s/$/"}/' >"$f"
    printf '{"v":1,"seq":%s,"n":2,"up":2,"t":%s,"k":"x"}\n' "$1" "$2" >>"$f"
}
old 1 1780000000 2000
old 2 1789000000 2000
old 3 null 2000
printf '1 a 1\n2 b 1\n3 c 1\n' >"$T/ledger/bootmap"
echo 3 >"$T/ledger/seq"
hround 3000
hround 3600
check "a boot that ended more than 30 days ago is removed; newer ones and this one stay" '[ ! -f $T/ledger/boot-000001-00000001-001.jsonl ] && [ -f $T/ledger/boot-000002-00000002-001.jsonl ] && [ -f $T/ledger/boot-000003-00000003-001.jsonl ] && ls $T/ledger/boot-000004-* >/dev/null'
old 2 1789000000 3000000 # boot 2 grew big
GUARD_LEDGER_TOTAL_MAX=2000000 hround 7200
check "over the size limit: the oldest boot goes, and only until under it; this boot kept" '[ ! -f $T/ledger/boot-000002-00000002-001.jsonl ] && [ -f $T/ledger/boot-000003-00000003-001.jsonl ] && ls $T/ledger/boot-000004-* >/dev/null'
teardown

hsetup # retention: up for weeks, no older boot left and still over: this boot's oldest segments go, never the one being written
mkdir -p "$T/ledger"
pad() { # pad <file> <bytes>: that many bytes more of well-formed lines
    awk -v n="$2" 'BEGIN { s = "x"; while (length(s) < 200) s = s s; for (i = 0; i < n; i += 200) print substr(s, 1, ((n - i < 200) ? n - i : 200)) }' | sed 's/^/{"v":1,"seq":1,"n":1,"up":1,"t":null,"k":"x","pad":"/; s/$/"}/' >>"$1"
}
old 1 null 2000
printf '1 a 1\n' >"$T/ledger/bootmap"
echo 1 >"$T/ledger/seq"
hround 3000
cur=$(cat $T/state/ledger/seg) # part 001 of this boot
for p in 001 002 003 004; do pad "${cur%-*}-$p.jsonl" 400000; done # four full parts, ~530 KB each (rotation itself is tested above)
cur=${cur%-*}-005.jsonl
: >"$cur"
echo "$cur" >$T/state/ledger/seg # 005 is the one being written
check "set up: five parts of this boot, the last one being written" '[ "$(ls $T/ledger/boot-000002-*.jsonl | wc -l)" = 5 ]'
GUARD_LEDGER_TOTAL_MAX=1300000 hround 3600
kb=$(du -sk $T/ledger | awk "{ print \$1 }")
check "the older boot goes first" '[ ! -f $T/ledger/boot-000001-00000001-001.jsonl ]'
check "then this boot's oldest parts, only until under the limit" '[ ! -f ${cur%-*}-001.jsonl ] && [ ! -f ${cur%-*}-002.jsonl ] && [ -f ${cur%-*}-003.jsonl ] && [ $((kb * 1024)) -le 1300000 ]'
check "the part being written stays, with this hour's lines in it" '[ "$(cat $T/state/ledger/seg)" = "$cur" ] && grep -q "\"k\":\"hour\"," $cur'
check "the trims are logged" 'grep -q "no older boot left; boot-000002-.*-001.jsonl removed" $T/guard.log'
GUARD_LEDGER_TOTAL_MAX=1 hround 7200 # a cap nothing fits under
check "a cap nothing fits under: every older part goes, never the segment being written" '[ "$(ls $T/ledger/boot-000002-*.jsonl)" = "$cur" ]'
teardown

echo "## crash watcher (docs/LEDGER.md §9)"
# The test drives the capture file, the link and the clock. The watcher's
# sleep stub advances the fake uptime, then runs $T/at/<loop> if it exists.
wsetup() {
    setup
    W=$T/state/ledger
    CAP=$T/crashcap/kmsg-feedc0de.log
    export GUARD_WATCHER=1 GUARD_KMSG=$T/kmsg GUARD_CRASHCAP_DIR=$T/crashcap GUARD_ROUTE=$T/route \
        GUARD_NETDEV=$T/netdev GUARD_IP=$T/bin/ip GUARD_DUMP_DIR=$T/dumps GUARD_SUSPEND_STATS=$T/suspend \
        GUARD_WATCH_SLEEP=$T/bin/wsleep
    unset GUARD_WATCH_INTERVAL GUARD_WATCH_GAP GUARD_WATCH_MAX
    : >"$T/kmsg"
    mkdir -p "$T/crashcap" "$T/dumps" "$T/at" "$T/ledger/state" "$W"
    : >"$T/ledger/state/watch-init" # not the first go-live (that has a case of its own)
    printf '6,1,1000000,-,caller=T1;Booting Linux\n' >"$CAP"
    echo 4 >"$T/suspend"
    echo 0 >"$T/loops"
    {
        echo "T=$T CAP=$CAP"
        cat <<'EOF'
link() { # link up|down: the address and the default route go together
    printf 'Iface\tDestination\tGateway\tFlags\n' >"$T/route"
    rm -f "$T/addr"
    [ "$1" = up ] || return 0
    printf 'rmnet_data0\t00000000\t00000000\t0001\n' >>"$T/route"
    : >"$T/addr"
}
rx() { # rx <bytes> <packets each way>
    printf 'Inter-|   Receive\n face |bytes packets\n  rmnet_data0: %s %s 0 0 0 0 0 0 5000 %s 0 0 0 0 0 0\n' "$1" "$2" "$2" >"$T/netdev"
}
kmsg() { printf '3,%s,%s000000,-,caller=T64;%s\n' "$1" "$2" "$3" >>"$CAP"; } # kmsg <seq> <kernel s> <text>
jump() { echo $(($(cat "$T/uptime") + $1)) >"$T/uptime"; } # the next loop comes $1 s late
MODEM='qcom_q6v5_pas 4080000.remoteproc-mss:'
EOF
    } >"$T/helpers.sh"
    . "$T/helpers.sh"
    cat >"$T/bin/wsleep" <<EOF
#!/bin/sh
. $T/helpers.sh
echo \$((\$(cut -d. -f1 $T/uptime) + \${1%%.*})) >$T/uptime
n=\$((\$(cat $T/loops) + 1))
echo \$n >$T/loops
[ -f $T/at/\$n ] && . $T/at/\$n
exit 0
EOF
    cat >"$T/bin/ip" <<EOF
#!/bin/sh
[ -f $T/addr ] && echo "9: rmnet_data0    inet 10.1.2.3/30 scope global rmnet_data0"
exit 0
EOF
    chmod +x "$T/bin/wsleep" "$T/bin/ip"
    link up
    rx 1000 10
    up 1000
}
wteardown() {
    unset GUARD_WATCHER GUARD_KMSG GUARD_CRASHCAP_DIR GUARD_ROUTE GUARD_NETDEV GUARD_IP GUARD_DUMP_DIR \
        GUARD_SUSPEND_STATS GUARD_WATCH_SLEEP GUARD_WATCH_INTERVAL GUARD_WATCH_GAP GUARD_WATCH_MAX
    teardown
}
at() { printf '%s\n' "$2" >>"$T/at/$1"; }      # at <loop> <shell>: runs after that loop's sleep
watch() { GUARD_WATCH_MAX=$1 sh "$GUARD" watcher; } # a watcher that stops after <n> loops
restart() { rm -rf "$T/at"; mkdir -p "$T/at"; echo 0 >"$T/loops"; } # the next watch is a new process
spool() { # spool <kind>: "<id> <up> <fragment>" for each spooled event of that kind
    for f in "$T"/ledger/spool/*.ev "$T"/spool-tmp/*.ev; do
        [ -f "$f" ] || continue
        { IFS="$TAB" read -r i b u k; IFS= read -r fr; } <"$f"
        [ "$k" = "$1" ] && echo "$i $u $fr"
    done
}
res() { cat "$T/ledger/spool/res-ssr-feedc0de-$1.ev" 2>/dev/null; } # res <kmsg seq>: that crash's result
drain() { hb "$(cut -d. -f1 "$T/uptime")"; GUARD_WATCHER=0 sh "$GUARD" once; }
alive() { [ -d "/proc/$1" ] && ! grep -q ') Z ' "/proc/$1/stat" 2>/dev/null; }

wsetup
echo "940 0" >"$W/w.pk" # a full minute of packets before the crash: wan_ppm
at 1 'kmsg 3008 484 "qcom_q6v5_pas 4080000.remoteproc-mss: fatal error received: rf_nr5g_sub6_tx.c:5428:Assertion ((rfe_instance_p->txagc.pa == \"x\\y\"))"
kmsg 3009 484 "qcom_q6v5_pas 4080000.remoteproc-mss: rproc recovery state: enabled and kick reovery process"
kmsg 3010 484 "remoteproc remoteproc0: crash detected in 4080000.remoteproc-mss: type fatal error"
kmsg 3012 484 "remoteproc remoteproc0: recovering 4080000.remoteproc-mss"
link down'
at 4 'link up'
at 6 'rx 5000 40'
watch 8
check "four kernel lines of one crash: one ssr, spooled on /data" '[ "$(spool ssr | wc -l)" = 1 ] && [ -f $T/ledger/spool/ssr-feedc0de-3008.ev ]'
check "ssr: the modem assertion, cleaned; packets a minute before it" 'spool ssr | grep -q "^ssr-feedc0de-3008 1002.0 ,\"kseq\":3008,\"line\":\"rf_nr5g_sub6_tx.c:5428:Assertion ((rfe_instance_p->txagc.pa == xy))\",.*\"wan_ppm\":20\$"'
check "down, back 6 s after it was found, traffic at 10 s: ok" 'res 3008 | grep -q "\"result\":\"ok\",\"recovered_s\":6,\"rx_seen_s\":10,\"down_seen\":1,\"resumed\":0,\"gap_s\":0,\"slept\":0"'
check "link events for the drop and the return, caused by the crash" '[ "$(spool link | grep -c "\"cause\":\"ssr\",\"res\":2")" = 2 ] && spool link | grep -q "\"state\":\"down\"" && spool link | grep -q "\"state\":\"up\""'
check "nothing left open; heartbeat at the last loop" '[ ! -f $W/w.ssr ] && [ "$(cat $W/w.up)" = 1014 ]'
drain
check "the ledger job takes them in: ssr, ssr_result, two link lines; spool empty" '[ "$(lines | grep -c "\"k\":\"ssr\",\"id\":\"ssr-feedc0de-3008\"")" = 1 ] && [ "$(lines | grep -c "\"k\":\"ssr_result\"")" = 1 ] && [ "$(lines | grep -c "\"k\":\"link\"")" = 2 ] && [ -z "$(ls $T/ledger/spool $T/spool-tmp | grep "\.ev")" ]'
check "every ledger line is flat JSON" 'jsonok'
wteardown

wsetup
at 1 'kmsg 500 600 "$MODEM fatal error received: a.c:1:boom"'
at 4 'rx 3000 30'
watch 20
check "no down in 30 s: no_drop, measured by the first new traffic" 'res 500 | grep -q "\"result\":\"no_drop\",\"recovered_s\":null,\"rx_seen_s\":6,\"down_seen\":0"'
wteardown

wsetup
at 1 'kmsg 500 600 "$MODEM fatal error received: a.c:1:boom"'
watch 50
check "no down and no traffic: no_drop at 90 s, rx_seen_s null" 'res 500 | grep -q "\"result\":\"no_drop\",\"recovered_s\":null,\"rx_seen_s\":null,\"down_seen\":0" && [ "$(spool ssr_result | cut -d" " -f2)" = 1092.0 ]'
wteardown

wsetup
export GUARD_WATCH_INTERVAL=30 GUARD_WATCH_GAP=100
at 1 'kmsg 500 600 "remoteproc remoteproc0: crash detected in 4080000.remoteproc-mss: type watchdog"; link down'
watch 25
check "down and not back: timeout 600 s after it was found" 'res 500 | grep -q "\"result\":\"timeout\",\"recovered_s\":null,\"rx_seen_s\":null,\"down_seen\":1" && [ "$(spool ssr_result | cut -d" " -f2)" = 1630.0 ]'
check "a watchdog crash: its own line kept whole" 'spool ssr | grep -q "\"line\":\"remoteproc remoteproc0: crash detected in 4080000.remoteproc-mss: type watchdog\""'
wteardown

wsetup
at 1 'kmsg 500 600 "$MODEM fatal error received: a.c:1:first"; link down'
at 10 'kmsg 520 640 "$MODEM fatal error received: a.c:1:again"'
at 40 'kmsg 540 700 "$MODEM fatal error received: a.c:1:second"'
watch 42
check "a crash line 40 s later by kernel time: the same crash" '[ ! -f $T/ledger/spool/ssr-feedc0de-520.ev ]'
check "100 s later and still down: the first is merged, the second opens" 'res 500 | grep -q "\"result\":\"merged\",\"recovered_s\":null" && [ -f $T/ledger/spool/ssr-feedc0de-540.ev ] && grep -q "^ssr-feedc0de-540 " $W/w.ssr'
wteardown

wsetup
at 1 'kmsg 500 600 "$MODEM fatal error received: a.c:1:first"; link down'
at 3 'link up'
at 6 'kmsg 540 700 "$MODEM fatal error received: a.c:1:second"'
watch 8
check "back but no traffic yet, then another crash: the first ends ok, the second opens" 'res 500 | grep -q "\"result\":\"ok\",\"recovered_s\":4,\"rx_seen_s\":null" && [ -f $T/ledger/spool/ssr-feedc0de-540.ev ]'
wteardown

echo "  capture cut in place"
wsetup
at 1 'kmsg 500 600 "$MODEM fatal error received: a.c:1:first"'
watch 3
drain # the first crash is in the ledger now, its spool file gone
tail -c 60 "$CAP" >"$T/cut" && cat "$T/cut" >"$CAP" && echo 1 >"$T/state/crashcap-cuts"
kmsg 560 900 "$MODEM fatal error received: a.c:1:after the cut"
restart
watch 2
check "cut and counted: read again; the old crash not opened twice, the new one found" '[ ! -f $T/ledger/spool/ssr-feedc0de-500.ev ] && [ -f $T/ledger/spool/ssr-feedc0de-560.ev ] && [ "$(cut -d" " -f3 $W/w.pos)" = 1 ]'
printf '6,1,1000000,-,caller=T1;x\n' >"$CAP"
kmsg 580 1000 "$MODEM fatal error received: a.c:1:shorter"
restart
watch 1
check "shorter than the position, no cut counted: read again all the same" '[ -f $T/ledger/spool/ssr-feedc0de-580.ev ]'
wteardown

wsetup
at 1 'kmsg 500 600 "$MODEM fatal error received: a.c:1:x"; link down'
watch 3
up 1034
link up
restart
at 1 'rx 9000 90'
watch 3
check "restarted while following: carries on, resumed, 28 s without samples" 'res 500 | grep -q "\"result\":\"ok\",\"recovered_s\":32,\"rx_seen_s\":34,\"down_seen\":1,\"resumed\":1,\"gap_s\":28,\"slept\":0"'
check "the time it was not running is not taken for sleep" '[ -z "$(spool sleep)" ]'
wteardown

wsetup
watch 1
cp "$W/w.pos" "$T/pos.before"
kmsg 500 600 "$MODEM fatal error received: a.c:1:x"
restart
watch 1
drain
cp "$T/pos.before" "$W/w.pos"
rm -f "$W/w.ssrlast" "$W/w.ssr" # killed right after the ssr was spooled
restart
watch 1
drain
check "killed before moving on, the crash read twice: one ssr in the ledger" '[ "$(lines | grep -c "\"id\":\"ssr-feedc0de-500\"")" = 1 ]'
cp "$T/pos.before" "$W/w.pos"
restart
watch 1
check "with w.ssrlast kept, a re-read does not even spool it again" '[ ! -f $T/ledger/spool/ssr-feedc0de-500.ev ]'
wteardown

wsetup
at 1 "printf '3,700,800000000,-,caller=T64;$MODEM fatal err' >>\"\$CAP\""
at 2 "printf 'or received: late.c:9:tail\n' >>\"\$CAP\""
watch 3
check "a line still missing its newline waits for it: one crash, whole line, loop 3" '[ "$(spool ssr | wc -l)" = 1 ] && spool ssr | grep -q "^ssr-feedc0de-700 1004.0 ,\"kseq\":700,\"line\":\"late.c:9:tail\""'
wteardown

wsetup
at 1 'kmsg 800 900 "Out of memory: Killed process 1234 (zte-agent) total-vm:1kB, anon-rss:1kB"
kmsg 801 900 "zte_thermal_monitor soc:zte_thermal_monitor: zte_thermal_update_throttling_level() to 0x3"'
watch 2
check "oom kill on /data, thermal level in /tmp" '[ -f $T/ledger/spool/oom-feedc0de-800.ev ] && [ -f $T/spool-tmp/th-feedc0de-801.ev ] && spool thermal | grep -q "throttling_level() to 0x3"'
drain
check "both reach the ledger as flat JSON" 'lines | grep -q "\"k\":\"oom\"" && lines | grep -q "\"k\":\"thermal\"" && jsonok'
wteardown

echo "  pauses: sleep only with evidence (docs/LEDGER.md §8)"
wsetup
at 3 'kmsg 900 950 "PM: suspend entry (deep)"'
at 4 'jump 40; kmsg 901 951 "PM: suspend exit"'
at 6 'jump 30; echo 5 >"$T/suspend"'
at 8 'jump 30'
watch 13
check "suspend lines around a pause: ev=pm, from the missed loop to the late one" 'spool sleep | grep -q "\"from\":1008,\"to\":1048,\"ev\":\"pm\""'
check "the suspend counter went up: ev=stats" 'spool sleep | grep -q "\"from\":1052,\"to\":1082,\"ev\":\"stats\""'
check "no evidence: ev=inferred, settled three loops later" 'spool sleep | grep -q "^sleep-feedc0de-3 1116 ,\"from\":1086,\"to\":1116,\"ev\":\"inferred\""'
check "three pauses, three sleep events" '[ "$(spool sleep | wc -l)" = 3 ]'
drain
check "sleep lines are flat JSON" 'lines | grep -q "\"k\":\"sleep\"" && jsonok'
wteardown

wsetup
at 1 'kmsg 500 600 "$MODEM fatal error received: a.c:1:x"; link down'
at 3 'jump 40; kmsg 501 601 "PM: suspend exit"; link up'
at 5 'rx 9000 90'
watch 7
check "asleep while following, with evidence: slept=1, its seconds in gap_s" 'res 500 | grep -q "\"resumed\":0,\"gap_s\":40,\"slept\":1"'
wteardown

wsetup
head -c 10 /dev/zero >"$T/dumps/old_1.elf" # there before: not news
at 1 'kmsg 500 600 "$MODEM fatal error received: a.c:1:x"'
at 2 'head -c 100 /dev/zero >"$T/dumps/ipa_driver_1.elf"'
at 3 'head -c 200 /dev/zero >"$T/dumps/ipa_driver_1.elf"'
at 6 'head -c 50 /dev/zero >"$T/dumps/x.elf"'
at 48 'head -c 70 /dev/zero >"$T/dumps/late.elf"'
watch 50
check "a new dump is reported once, after its size held still" '[ "$(spool ssr_dump | grep -c ipa_driver_1.elf)" = 1 ] && spool ssr_dump | grep -q "\"ssr\":\"ssr-feedc0de-500\",\"file\":\"ipa_driver_1.elf\",\"size\":200"'
check "a second one in the window too; the one there before, never" 'spool ssr_dump | grep -q "\"file\":\"x.elf\",\"size\":50" && ! spool ssr_dump | grep -q old_1'
check "the window closes 90 s after the crash: a later file is not this crash's" '! spool ssr_dump | grep -q late.elf && [ ! -f $W/w.dump ]'
drain
check "dump lines are flat JSON" 'lines | grep -q "\"k\":\"ssr_dump\"" && jsonok'
wteardown

wsetup
rm -f "$T/ledger/state/watch-init"
kmsg 500 600 "$MODEM fatal error received: a.c:1:before go-live"
kmsg 501 700 "zte_thermal_monitor soc:zte_thermal_monitor: zte_thermal_update_throttling_level() to 0x3"
watch 2
check "first go-live: what the capture held is taken as seen; nothing spooled" '[ -z "$(spool ssr)$(spool thermal)" ] && [ -f $T/ledger/state/watch-init ] && [ "$(cut -d" " -f2 $W/w.pos)" = 501 ]'
kmsg 520 800 "$MODEM fatal error received: a.c:1:after"
restart
watch 1
check "after that, crashes count" '[ -f $T/ledger/spool/ssr-feedc0de-520.ev ]'
wteardown

wsetup
kmsg 500 600 "$MODEM fatal error received: a.c:1:early in this boot"
at 1 'rx 5000 50'
watch 20
check "a later boot: a crash before the watcher started counts, resumed, gap_s up to boot" 'res 500 | grep -q "\"resumed\":1,\"gap_s\":1000,"'
wteardown

wsetup
echo "996 NR5G-SA-$(head -c 300 /dev/zero | tr '\0' x) n77 n77 440-10 460-11" >"$W/net" # a damaged cache must not blow the 400-byte fragment
at 1 'kmsg 500 600 "$MODEM fatal error received: $(head -c 400 /dev/zero | tr "\0" y)"'
watch 2
check "ssr with the network cache: fields cut short, the fragment still under 400 bytes" 'spool ssr | grep -q "\"rat\":\"NR5G-SA-xxxxxxxx\",\"band\":\"n77\",\"nrband\":\"n77\",\"net\":\"440-10\",\"home\":\"460-11\",\"net_age\":6," && [ "$(sed -n 2p $T/ledger/spool/ssr-feedc0de-500.ev | wc -c)" -le 401 ]'
drain
check "and it reaches the ledger" 'lines | grep -q "\"k\":\"ssr\",\"id\":\"ssr-feedc0de-500\"" && jsonok'
wteardown

wsetup
touch "$W/lowspace"
at 1 'kmsg 500 600 "$MODEM fatal error received: a.c:1:x"'
watch 2
check "/data nearly full: the ssr goes to /tmp, marked lowspace" '[ -f $T/spool-tmp/ssr-feedc0de-500.ev ] && grep -q "\"lowspace\":1" $T/spool-tmp/ssr-feedc0de-500.ev && [ ! -f $T/ledger/spool/ssr-feedc0de-500.ev ]'
wteardown

wsetup
drain # boot A is in the ledger: sequence 1
at 1 'kmsg 500 600 "$MODEM fatal error received: a.c:1:x"; link down'
at 3 'link up'
at 4 'rx 9000 90'
watch 6
echo feedc0de-0000-4000-8000-00000000000b >"$T/bootid" # rebooted before the ledger job took them in
rm -rf "$T/state" "$T/spool-tmp"
up 100
drain
check "crash and result from the last boot land under its sequence (C15)" 'lines | grep "\"k\":\"ssr\"," | grep -q "\"seq\":1," && lines | grep "\"k\":\"ssr_result\"" | grep -q "\"seq\":1," && lines | grep -q "\"seq\":2,.*\"k\":\"boot\""'
check "and without this boot's clock: t null (the reader works it out from that boot's own)" 'lines | grep "\"k\":\"ssr\"," | grep -q "\"t\":null,"'
wteardown

echo "  the main loop keeps one watcher, at its version"
wsetup
export GUARD_WATCH_SLEEP=sleep GUARD_WATCH_MAX=3 # a real one, living about 4 s
hb 995
exec 9>>"$T/wifi.lock"
flock -n 9 # held by this test while the round runs...
sh "$GUARD" once
exec 9>&- # ...and let go
sleep 1
wp=$(cut -d" " -f1 "$W/watcher.pid" 2>/dev/null)
check "started when there was none" '[ -n "$wp" ] && alive "$wp" && grep -q "watcher started" $T/guard.log'
check "it did not inherit the Wi-Fi lock" 'flock -n $T/wifi.lock true'
sh "$GUARD" once
sleep 1
check "the next round leaves a running one of this version alone" '[ "$(grep -c "watcher started" $T/guard.log)" = 1 ]'
i=0; while alive "$wp" && [ $i -lt 10 ]; do sleep 1; i=$((i + 1)); done
sleep 30 &
dummy=$!
echo "$dummy $(sed 's/^.*) //' /proc/$dummy/stat | awk '{ print $20 }') deadbeef" >"$W/watcher.pid"
sh "$GUARD" once
wait "$dummy" 2>/dev/null
sleep 1
check "another version running: killed and replaced by one at this version" 'grep -q "^$dummy\$" $T/kill.log && [ "$(cut -d" " -f3 $W/watcher.pid)" = "$(md5sum $GUARD | cut -c1-8)" ]'
wp=$(cut -d" " -f1 "$W/watcher.pid" 2>/dev/null)
i=0; while alive "$wp" && [ $i -lt 10 ]; do sleep 1; i=$((i + 1)); done
rm -f "$W/watcher.pid"
GUARD_WATCHER=0 sh "$GUARD" once
sleep 1
check "GUARD_WATCHER=0: none started" '[ ! -f $W/watcher.pid ]'
wteardown

wsetup
mkdir -p "$T/s"
cp "$SCRIPTS/u60-guard.sh" "$SCRIPTS/alert-lib.sh" "$T/s/"
at 5 'echo "# another version" >>"$T/s/u60-guard.sh"' # e.g. rolled back to an older guard
GUARD_WATCH_MAX=40 sh "$T/s/u60-guard.sh" watcher
check "u60-guard.sh changed on disk: the watcher leaves by itself within 30 loops" 'grep -q "watcher: u60-guard.sh is now" $T/guard.log && [ "$(cat $W/w.up)" = 1058 ]'
wteardown

echo "## soak: the real loop, background jobs and all, for three hours of fake time"
setup
unset GUARD_LEDGER_FG # background jobs as on the device
export GUARD_WATCHER=1 GUARD_KMSG=$T/kmsg GUARD_CRASHCAP_DIR=$T/crashcap GUARD_WATCH_GAP=100000000 GUARD_WATCH_SLEEP=sleep
unset GUARD_WALL_NOW
export GUARD_WALL_FILE=$T/wall # a trusted clock that moves with uptime (1790499600 is on the hour)
echo 1790500600 >"$T/wall"
printf '6,1,1000000,-;boot\n' >"$T/kmsg"
echo 8 >"$T/hostapd"
cat >"$T/bin/soaksleep" <<EOF
#!/bin/sh
u=\$((\$(cut -d. -f1 $T/uptime) + \${1%%.*}))
echo \$u >$T/uptime.tmp && mv $T/uptime.tmp $T/uptime # whole files: background jobs read them meanwhile
echo \$u >$T/heartbeat.tmp && mv $T/heartbeat.tmp $T/heartbeat
echo \$((1790499600 + u)) >$T/wall.tmp && mv $T/wall.tmp $T/wall
[ \$u -ge 12000 ] && kill -TERM \$PPID
exit 0
EOF
chmod +x "$T/bin/soaksleep"
up 1000
hb 995
GUARD_SLEEP=$T/bin/soaksleep sh "$GUARD" >/dev/null 2>&1 &
soak=$!
i=0
while kill -0 "$soak" 2>/dev/null && [ $i -lt 240 ]; do sleep 1; i=$((i + 1)); done
check "the loop ran its three hours and stopped" '! kill -0 $soak 2>/dev/null && [ "$(cat $T/uptime)" -ge 12000 ]'
sleep 3 # background jobs still finishing
L=$T/state/ledger
watchers() { # this run's crash watchers (earlier cases may have left their own)
    for p in /proc/[0-9]*; do
        tr '\0' ' ' 2>/dev/null <"$p/cmdline" | grep -q "u60-guard.sh watcher" &&
            tr '\0' '\n' 2>/dev/null <"$p/environ" | grep -q -x "GUARD_STATE=$T/state" && echo "$p"
    done | grep -c .
}
check "hour lines were written in the background, three of each kind at least" '[ "$(lines | grep -c "\"k\":\"hour\",")" -ge 2 ] && [ "$(lines | grep -c "\"k\":\"hour_power\"")" -ge 2 ] && [ "$(lines | grep -c "\"k\":\"hour_proc\"")" -ge 2 ]'
check "the summary lines were redone by their own background job" '[ "$(wc -l <$T/ledger/summary)" = 3 ]'
check "the round marks stay trimmed (at most 180)" '[ "$(wc -l <$L/rounds)" -le 180 ] && [ "$(wc -l <$L/rounds)" -ge 60 ]'
check "exactly one crash watcher" '[ "$(watchers)" = 1 ]'
kill "$(cut -d" " -f1 "$L/watcher.pid")" 2>/dev/null
sleep 3
check "nothing of this run left behind: watcher gone, the ledger and summary jobs finished" '[ "$(watchers)" = 0 ] && ! kill -0 "$(cut -d" " -f1 $L/ledger.pid)" 2>/dev/null && ! kill -0 "$(cut -d" " -f1 $L/summary.pid 2>/dev/null)" 2>/dev/null'
check "every ledger line of the soak is flat JSON" 'jsonok'
unset GUARD_WATCHER GUARD_KMSG GUARD_CRASHCAP_DIR GUARD_WATCH_GAP GUARD_WATCH_SLEEP GUARD_WALL_FILE
export GUARD_WALL_NOW=1790500000
teardown

# ── u60 ship: a transaction whose executor died (docs/SHIP.md, T4) ─────────
shsetup() {
    setup
    mkdir -p "$T/u60-ship"
    printf '#!/bin/sh\necho "$*" >>%s/ship.calls\n' "$T" >"$T/u60-ship/u60-ship.sh"
    : >"$T/ship.calls"
    up 2000
    hb 1995
}
txn() { # txn <phase> [comp] [t_phase]
    printf 'v=1\ntxn=20260930-120000-%s\ncomp=%s\nphase=%s\nt_phase=%s\nend=1\n' "${2:-datad}" "${2:-datad}" "$1" "${3:-1900}" >"$T/u60-ship/txn"
}
executor() { # executor <heartbeat uptime> alive|dead
    echo "$1 4711 20260930-120000-datad check" >"$T/u60-ship/heartbeat"
    rm -rf "$T/proc/4711"
    if [ "$2" = alive ]; then
        mkdir -p "$T/proc/4711"
        printf 'sh\000/data/u60-guard/u60-ship.sh\000run\000' >"$T/proc/4711/cmdline"
    fi
}
calls() { sleep 0.3; grep -c recover-live "$T/ship.calls"; }

shsetup
round
check "ship: no transaction, nothing called" '[ "$(calls)" = 0 ]'
teardown

for ph in done rolledback aborted manifest_pending failed; do
    shsetup
    txn $ph
    executor 1500 dead
    round
    check "ship: $ph is finished, not touched" '[ "$(calls)" = 0 ]'
    teardown
done

shsetup
txn check
executor 1990 alive
round
check "ship: executor alive, heartbeat 10 s old: left alone" '[ "$(calls)" = 0 ]'
teardown

shsetup
txn check
executor 1969 alive
round
check "ship: heartbeat 31 s old (stuck): recover-live" '[ "$(calls)" = 1 ] && grep -q "u60-ship: transaction (datad, check) has no live executor (heartbeat 31s old)" "$T/guard.log"'
teardown

shsetup
txn trial
executor 1999 dead
round
check "ship: fresh heartbeat but the executor is gone: recover-live" '[ "$(calls)" = 1 ]'
teardown

shsetup
txn promote
round
check "ship: no heartbeat at all: recover-live" '[ "$(calls)" = 1 ] && grep -q "heartbeat missing" "$T/guard.log"'
teardown

shsetup
txn check guard
executor 1500 dead
round
check "ship: a guard transaction is not the guard's to finish" '[ "$(calls)" = 0 ]'
teardown

shsetup
txn staged datad 1900
round
check "ship: staged 100 s ago, not started yet: left alone" '[ "$(calls)" = 0 ]'
txn staged datad 1600
round
check "ship: staged 400 s ago and never started: recover-live" '[ "$(calls)" = 1 ]'
teardown

shsetup
txn check
executor 1500 dead
rm "$T/u60-ship/u60-ship.sh"
round
check "ship: no u60-ship.sh: nothing to call" '! grep -q u60-ship "$T/guard.log"'
teardown

shsetup
head -c 2000 /dev/urandom >"$T/u60-ship/txn"
round
check "ship: garbage transaction log: nothing called, no shell error" '[ "$(calls)" = 0 ]'
teardown

# killed right after a heartbeat at 2000; rounds 60 s apart: taken over ≤ 90 s
shsetup
txn check
executor 2000 dead
mkdir -p "$T/proc/4711"
printf 'sh\000/data/u60-guard/u60-ship.sh\000run\000' >"$T/proc/4711/cmdline"
up 2029
round
check "ship: 29 s after the last heartbeat, alive: not yet" '[ "$(calls)" = 0 ]'
rm -rf "$T/proc/4711" # the OOM killer
up 2089
round
check "ship: executor killed: recover-live by the next round (≤ 90 s)" '[ "$(calls)" = 1 ]'
teardown

echo
echo "passed $PASS, failed $FAIL"
[ "$FAIL" = 0 ]
