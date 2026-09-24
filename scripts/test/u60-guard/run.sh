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
case "\$*" in *time_from_utc*) cat $T/tz 2>/dev/null ;; esac
EOF
    cat >"$T/bin/ubus" <<EOF
#!/bin/sh
echo "\$*" >>$T/ubus.log
case "\$2 \$3" in
    "zwrt_wlan reload") [ -f $T/reload-works ] && echo 8 >$T/hostapd ;;
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
              grep "\"number\":\"$n\"" | sed -n 's/.*"id":\([0-9]*\).*/\1/p' ;;
  *) sed -n 's/.*"result"[^0-9]*\([0-9][0-9]*\).*/\1/p' ;;
esac
EOF
    chmod +x "$T"/bin/*

    export GUARD_UPTIME_FILE=$T/uptime GUARD_HEARTBEAT=$T/heartbeat GUARD_MARKER=$T/marker
    export GUARD_WIFI_LOCK=$T/wifi.lock GUARD_STATE=$T/state GUARD_LOG=$T/guard.log
    export GUARD_PROC=$T/proc GUARD_RTC=$T/rtc
    export GUARD_UCI=$T/bin/uci GUARD_UBUS=$T/bin/ubus GUARD_PS=$T/bin/ps
    export GUARD_SLEEP=$T/bin/sleep GUARD_KILL=$T/bin/kill GUARD_JSONFILTER=$T/bin/jsonfilter
    export ALERT_DIR=$T/alerts ALERT_UPTIME_FILE=$T/uptime
    export GUARD_WALL_NOW=1758600000
    unset GUARD_LOCK_WAIT
    # The cases below jump the clock between rounds freely; sleep detection
    # would read every jump as a suspend. Its own case turns it back on.
    export GUARD_WAKE_GAP=100000000
}
teardown() { rm -rf "$T"; }

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
check "gives up this round without writing uci" '[ "$(restores)" = 0 ] && [ ! -s $T/uci.log ]'
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
check "dark round: rates recorded" '[ "$(cat "$T/standby.stat")" = "1060 600 60 10.0 - - - -" ]'
echo 255 >"$T/bl"; netdev 2000 200; up 1120
sround
check "lit screen: not recorded" '[ "$(wc -l <"$T/standby.stat")" = 1 ]'
echo 0 >"$T/bl"; netdev 2600 260; tsproc 501 50; up 1180
sround
check "restarted program: -" '[ "$(tail -n 1 "$T/standby.stat")" = "1180 600 60 - - - - -" ]'
GUARD_WAKE_GAP=150; export GUARD_WAKE_GAP; netdev 3000 300; up 1500
sround
check "rounds too far apart (slept): not recorded" '[ "$(wc -l <"$T/standby.stat")" = 2 ]'
i=0; while [ $i -lt 70 ]; do up $((1560 + i * 60)); sround; i=$((i + 1)); done
check "keeps the last 60 lines" '[ "$(wc -l <"$T/standby.stat")" = 60 ]'
unset GUARD_NETDEV GUARD_BACKLIGHT GUARD_STANDBY_STAT; export GUARD_WAKE_GAP=100000000
teardown

echo
echo "passed $PASS, failed $FAIL"
[ "$FAIL" = 0 ]
