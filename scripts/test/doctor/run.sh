#!/bin/sh
# doctor.sh with every device command stubbed (busybox container).
# SPDX-License-Identifier: MIT
SCRIPTS=${SCRIPTS:-/scripts}
PASS=0; FAIL=0
ok() { PASS=$((PASS + 1)); echo "  ok   $1"; }
bad() { FAIL=$((FAIL + 1)); echo "  FAIL $1"; }
check() { _d=$1; shift; if eval "$@"; then ok "$_d"; else bad "$_d  [$*]"; fi; }

setup() {
    T=$(mktemp -d); mkdir -p "$T/bin" "$T/initd" "$T/alerts" "$T/crash" "$T/uid" "$T/rcd" "$T/proc/100" "$T/proc/101"
    echo u60pro-devui >"$T/proc/100/comm"; echo zte-agent >"$T/proc/101/comm"
    echo "1000.5 1" >"$T/uptime"; echo 995 >"$T/hb"
    echo "sync success" >"$T/sync"; echo 0 >"$T/mode"; echo 1 >"$T/poll"; echo 8 >"$T/aps"
    for s in zte-agent zwrt-datad u60-guard u60-uid; do printf '#!/bin/sh\n' >"$T/initd/$s"; chmod +x "$T/initd/$s"; done
    { echo "#!/bin/sh"; for s in zte-agent zwrt-datad u60-guard u60-uid; do echo "$T/initd/$s start"; done; echo "exit 0"; } >"$T/rc"
    printf 'sync    ALL        zte_topsw_devui\nsync    ALL        zte_topsw_wlan\n' >"$T/daemon.conf"
    echo "+8612300000000" >"$T/alerts/sms-to"
    cat >"$T/bin/ubus" <<X
#!/bin/sh
case "\$2 \$3" in
  "zwrt_topsw_daemon.sync get_sync_info") echo "{ \"noSyncModuleName\": \"\$(cat $T/sync)\" }" ;;
  "service list") [ -f $T/down-\$(echo "\$4" | sed 's/.*"name":"\([^"]*\)".*/\1/') ] && echo '{}' || echo '{ "x": { "instances": { "i": { "running": true } } } }' ;;
esac
X
    cat >"$T/bin/uci" <<X
#!/bin/sh
case "\$*" in *dm_update_mode*) cat $T/mode ;; *TURNOFFPOLLING*) cat $T/poll ;; esac
X
    cat >"$T/bin/ps" <<X
#!/bin/sh
n=\$(cat $T/aps); i=0; while [ \$i -lt \$n ]; do echo "1 root 0 S /usr/sbin/hostapd -g x"; i=\$((i+1)); done
X
    printf '#!/bin/sh\ncase "$*" in *public/status*) cat %s/public 2>/dev/null ;; esac\nexit 0\n' "$T" >"$T/bin/wget"
    chmod +x "$T"/bin/*
    export DOC_UBUS=$T/bin/ubus DOC_UCI=$T/bin/uci DOC_PS=$T/bin/ps DOC_WGET=$T/bin/wget DOC_RC=$T/rc \
        DOC_INITD=$T/initd DOC_UPTIME=$T/uptime DOC_HEARTBEAT=$T/hb DOC_MARKER=$T/marker DOC_ALERTS=$T/alerts \
        DOC_CRASH=$T/crash DOC_UID_STATE=$T/uid DOC_OVERLAY_RCD=$T/rcd DOC_DAEMON_CONF=$T/daemon.conf DOC_DATA=/ DOC_PROC=$T/proc
}
run() { sh "$SCRIPTS/doctor.sh" --tsv >"$T/out"; RC=$?; }
level() { awk -F'\t' -v id="$1" '$2 == id {print $1}' "$T/out"; }

echo "healthy device"
setup; run
check "every line has 4 fields and a known level" '! awk -F"\t" "NF != 4 || (\$1 != \"ok\" && \$1 != \"warn\" && \$1 != \"bad\")" $T/out | grep -q .'
check "no bad checks, exit 0" '[ $RC = 0 ] && ! grep -q "^bad" $T/out'
check "services ok" '[ "$(level svc-zte-agent)" = ok ] && [ "$(level svc-u60-uid)" = ok ]'
check "fota ok" '[ "$(level fota)" = ok ]'
rm -rf "$T"

echo "problems are reported"
setup
echo "zte_topsw_devui" >"$T/sync"; echo 1 >"$T/mode"; echo 0 >"$T/aps"
touch "$T/down-u60-guard"; echo 500 >"$T/hb"; touch "$T/uid/gave-up"; : >"$T/alerts/sms-to"
mknod "$T/rcd/S99zte_topsw_devui" c 0 0 2>/dev/null
grep -v "u60-uid start" "$T/rc" >"$T/rc2"; mv "$T/rc2" "$T/rc"
run
check "exit non-zero" '[ $RC != 0 ]'
check "boot sync failure: bad" '[ "$(level boot-sync)" = bad ]'
check "auto-update on: bad" '[ "$(level fota)" = bad ]'
check "stopped service: bad" '[ "$(level svc-u60-guard)" = bad ]'
check "running but not in rc.local: warn" '[ "$(level svc-u60-uid)" = warn ]'
check "stale heartbeat: bad" '[ "$(level heartbeat)" = bad ]'
check "screen gave up: bad" '[ "$(level screen)" = bad ]'
check "no SMS number: warn" '[ "$(level sms)" = warn ]'
mkdir -p "$T/proc/102"; echo zte-agent >"$T/proc/102/comm"; run
check "two zte-agent processes: bad" '[ "$(level dup-zte-agent)" = bad ]'
check "Wi-Fi off, scenario did not ask: warn" '[ "$(level wifi)" = warn ]'
echo '{"ok":true,"data":{"scenario":{"current":"home","wifi_off":true}}}' >"$T/public"; run
check "Wi-Fi off because the scenario turns it off: ok" '[ "$(level wifi)" = ok ]'
touch "$T/crash/x.log"; run
check "crash log, its alert read: ok" '[ "$(level crashes)" = ok ]'
printf '9\t1\tcrash\tx\ty\n' >"$T/alerts/queue"; run
check "crash log with unread alerts: warn" '[ "$(level crashes)" = warn ]'
[ -c "$T/rcd/S99zte_topsw_devui" ] && check "whiteout on a boot-barrier daemon: bad" '[ "$(level whiteout)" = bad ]'
rm -rf "$T"

echo "old install (no procd services)"
setup; rm -f "$T"/initd/*; run
check "missing services are warnings, not errors" '[ "$(level svc-zte-agent)" = warn ]'
rm -rf "$T"

echo "standby sentinel"
setup; export DOC_STANDBY_STAT=$T/sb DOC_STANDBY_BASE=$T/sb.base   # uptime is 1000: recent = 100 and later
detail() { awk -F'\t' -v id="$1" '$2 == id {print $4}' "$T/out"; }
rows() { # rows <n> <first uptime> <wan pkts/min> [tailscaled wakeups/s]
    _i=0; while [ $_i -lt "$1" ]; do echo "$(($2 + _i * 60)) $3 20 ${4:-30.0} 5.0 2.0 1.0 0.5"; _i=$((_i + 1)); done
}
run
check "no baseline: ok, says not calibrated" '[ "$(level standby)" = ok ] && detail standby | grep -q 未校准'
rows 10 100 60 >"$T/sb"
sh "$SCRIPTS/doctor.sh" --calibrate-standby >"$T/cal"; RCC=$?
check "calibration refuses fewer than 30 lines" '[ $RCC != 0 ] && [ ! -f "$T/sb.base" ]'
: >"$T/sb"; i=0; while [ $i -lt 40 ]; do echo "$((100 + i * 20)) $((50 + i)) 20 30.0 5.0 2.0 1.0 0.5" >>"$T/sb"; i=$((i + 1)); done
sh "$SCRIPTS/doctor.sh" --calibrate-standby >"$T/cal"; RCC=$?
check "calibration: median 69.5, MAD 10.0 for 50..89" '[ $RCC = 0 ] && grep -qx "2 69.5 10.0" "$T/sb.base"'
check "calibration: constant column has MAD 0" 'grep -qx "3 20.0 0.0" "$T/sb.base"'
rows 5 700 300 >"$T/sb"; run
check "too few recent rows: no verdict" '[ "$(level standby)" = ok ] && detail standby | grep -q 不判定'
rows 10 300 72 >"$T/sb"; run
check "within the baseline: ok" '[ "$(level standby)" = ok ] && detail standby | grep -q "正常（蜂窝每分钟 72"'
rows 10 300 200 >"$T/sb"; run
check "cellular chatter well above baseline: warn" '[ "$(level standby)" = warn ] && detail standby | grep -q "蜂窝包/分 200（基线 70）"'
rows 10 300 70 90.0 >"$T/sb"; run
check "one program waking far more: warn names it" '[ "$(level standby)" = warn ] && detail standby | grep -q "tailscaled唤醒/秒 90（基线 30）"'
{ rows 4 300 70; echo "600 70 20 - - - - -"; rows 5 660 70; } >"$T/sb"; run
check "missing values (-) are skipped" '[ "$(level standby)" = ok ]'
: >"$T/sb"; i=1; while [ $i -le 10 ]; do echo "$((i * 9)) 500 20 30.0 5.0 2.0 1.0 0.5" >>"$T/sb"; i=$((i + 1)); done; run   # all before uptime 100
check "old rows (over 15 min) are ignored" '[ "$(level standby)" = ok ] && detail standby | grep -q 不判定'
unset DOC_STANDBY_STAT DOC_STANDBY_BASE
rm -rf "$T"

echo; echo "passed $PASS, failed $FAIL"; [ "$FAIL" = 0 ]
