#!/bin/sh
# doctor.sh with every device command stubbed (busybox container).
# SPDX-License-Identifier: MIT
SCRIPTS=${SCRIPTS:-/scripts}
PASS=0; FAIL=0
ok() { PASS=$((PASS + 1)); echo "  ok   $1"; }
bad() { FAIL=$((FAIL + 1)); echo "  FAIL $1"; }
check() { _d=$1; shift; if eval "$@"; then ok "$_d"; else bad "$_d  [$*]"; fi; }
MCOL=

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
    # fixed clock and disk, so the outputs can be compared with the snapshots below
    printf '#!/bin/sh\ncase "$1" in +%%s) cat %s/now ;; *) echo "2026-09-27 10:00" ;; esac\n' "$T" >"$T/bin/date"
    printf '#!/bin/sh\necho "Filesystem 1K-blocks Used Available Use%% Mounted on"\necho "/dev/ubi0 4000000 2800000 1200000 70%% /data"\n' >"$T/bin/df"
    echo 1790500000 >"$T/now"
    chmod +x "$T"/bin/*
    export DOC_UBUS=$T/bin/ubus DOC_UCI=$T/bin/uci DOC_PS=$T/bin/ps DOC_WGET=$T/bin/wget DOC_RC=$T/rc \
        DOC_INITD=$T/initd DOC_UPTIME=$T/uptime DOC_HEARTBEAT=$T/hb DOC_MARKER=$T/marker DOC_ALERTS=$T/alerts \
        DOC_CRASH=$T/crash DOC_UID_STATE=$T/uid DOC_OVERLAY_RCD=$T/rcd DOC_DAEMON_CONF=$T/daemon.conf DOC_DATA=/ DOC_PROC=$T/proc \
        DOC_DATE=$T/bin/date DOC_DF=$T/bin/df DOC_UID_WANT=$T/uid.want DOC_CLOCK_OK=$T/clock-ok DOC_WALL_LAST=$T/wall.last \
        DOC_FP=$T/fp DOC_FP_CHANGED=$T/fp-changed DOC_TS_TUNING=$T/tuning.env
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

echo "clock (docs/LEDGER.md §7)"
setup
echo 1735948800 >"$T/now"; run # 2025-01-04: the device before NTP
check "before NTP, up 1000 s: warn" '[ "$(level clock)" = warn ]'
echo "100.0 1" >"$T/uptime"; run
check "before NTP in the first 5 min: ok, still syncing" '[ "$(level clock)" = ok ] && grep -q "刚开机" $T/out'
echo "1000.5 1" >"$T/uptime"; echo 1790500000 >"$T/now"
echo "0 1790499000 990 none" >"$T/clock-ok"; run
check "u60-guard says not trusted (fresh): warn even in 2026" '[ "$(level clock)" = warn ]'
echo "0 1790499000 100 none" >"$T/clock-ok"; run
check "its verdict is stale: decide here (2026, no last clock): ok" '[ "$(level clock)" = ok ]'
rm -f "$T/clock-ok"; echo $((1790500000 + 3 * 86400)) >"$T/wall.last"; run
check "earlier than the last trusted clock minus a day: warn" '[ "$(level clock)" = warn ]'
rm -rf "$T"

fpfix() { # u60-guard's fingerprint cache: "<program> <pid> <md5>"
    printf 'tailscaled 501 aaaaaaaa\n'
    printf 'u60pro-devui 503 cccccccc\nzwrt-datad 504 dddddddd\nzte-agent 505 eeeeeeee\n'
}
echo "standby sentinel"
setup; export DOC_STANDBY_STAT=$T/sb DOC_STANDBY_BASE=$T/sb.base   # uptime is 1000: recent = 100 and later
fpfix >"$T/fp" # u60-guard's fingerprint cache
detail() { awk -F'\t' -v id="$1" '$2 == id {print $4}' "$T/out"; }
rows() { # rows <n> <first uptime> <wan pkts/min> [tailscaled wakeups/s]
    _i=0; while [ $_i -lt "$1" ]; do echo "$(($2 + _i * 60)) $3 20 ${4:-30.0}$MCOL 2.0 1.0 0.5"; _i=$((_i + 1)); done
}
run
check "no baseline: ok, says not calibrated" '[ "$(level standby)" = ok ] && detail standby | grep -q 未校准'
rows 10 100 60 >"$T/sb"
sh "$SCRIPTS/doctor.sh" --calibrate-standby >"$T/cal"; RCC=$?
check "calibration refuses fewer than 30 lines" '[ $RCC != 0 ] && [ ! -f "$T/sb.base" ]'
: >"$T/sb"; i=0; while [ $i -lt 40 ]; do echo "$((100 + i * 20)) $((50 + i)) 20 30.0$MCOL 2.0 1.0 0.5" >>"$T/sb"; i=$((i + 1)); done
sh "$SCRIPTS/doctor.sh" --calibrate-standby >"$T/cal"; RCC=$?
check "calibration: median 69.5, MAD 10.0 for 50..89" '[ $RCC = 0 ] && grep -qx "2 69.5 10.0" "$T/sb.base"'
check "calibration: constant column has MAD 0" 'grep -qx "3 20.0 0.0" "$T/sb.base"'
rows 5 700 300 >"$T/sb"; run
check "too few recent rows: no verdict" '[ "$(level standby)" = ok ] && detail standby | grep -q 不判定'
rows 10 300 72 >"$T/sb"; run
check "within the baseline: ok" '[ "$(level standby)" = ok ] && detail standby | grep -q "正常（蜂窝每分钟 72"'
rows 10 300 200 >"$T/sb"; run
check "cellular traffic well above the idle baseline: someone is using it, not judged" '[ "$(level standby)" = ok ] && detail standby | grep -q "有流量在用，不判待机"'
rows 10 300 70 90.0 >"$T/sb"; run
check "idle, one program waking far more: warn names it" '[ "$(level standby)" = warn ] && detail standby | grep -q "tailscaled唤醒/秒 90（基线 30）"'
{ rows 4 300 70; echo "600 70 20 - - - - -"; rows 5 660 70; } >"$T/sb"; run
check "missing values (-) are skipped" '[ "$(level standby)" = ok ]'
: >"$T/sb"; i=1; while [ $i -le 10 ]; do echo "$((i * 9)) 500 20 30.0$MCOL 2.0 1.0 0.5" >>"$T/sb"; i=$((i + 1)); done; run   # all before uptime 100
check "old rows (over 15 min) are ignored" '[ "$(level standby)" = ok ] && detail standby | grep -q 不判定'
unset DOC_STANDBY_STAT DOC_STANDBY_BASE
rm -rf "$T"

echo "standby fingerprints (docs/LEDGER.md §12)"
setup; export DOC_STANDBY_STAT=$T/sb DOC_STANDBY_BASE=$T/sb.base
fpfix >"$T/fp"
echo "TS_NO_LOGS_NO_SUPPORT=true" >"$T/tuning.env"
: >"$T/sb"; i=0; while [ $i -lt 40 ]; do echo "$((100 + i * 20)) $((50 + i)) 20 30.0$MCOL 2.0 1.0 0.5" >>"$T/sb"; i=$((i + 1)); done
sh "$SCRIPTS/doctor.sh" --calibrate-standby >/dev/null
check "calibration writes version 2 with a fingerprint per column" '[ "$(head -n 1 $T/sb.base)" = v2 ] && grep -q "^fp 4 aaaaaaaa+" $T/sb.base'
rows 10 300 70 90.0 >"$T/sb"
sed -i 's/^tailscaled 501 aaaaaaaa/tailscaled 601 ffffffff/' "$T/fp"; run
check "new tailscaled binary: its columns are stale, not judged" '[ "$(level standby)" = ok ] && detail standby | grep -q "tailscaled唤醒/秒 基线过期"'
sed -i 's/^tailscaled 601 ffffffff/tailscaled 601 aaaaaaaa/' "$T/fp"; run
check "same binary again: judged (warn)" '[ "$(level standby)" = warn ]'
echo "TS_DEBUG_FIREWALL_MODE=auto" >>"$T/tuning.env"; run
check "Tailscale tuning changed: stale, not judged" '[ "$(level standby)" = ok ] && detail standby | grep -q "基线过期"'
echo "TS_NO_LOGS_NO_SUPPORT=true" >"$T/tuning.env"; echo 800 >"$T/fp-changed"; run
check "only records after the last binary change are judged" '[ "$(level standby)" = ok ] && detail standby | grep -q "只有 1 行"'
rm -f "$T/fp-changed" "$T/fp"; run
check "no fingerprint cache from u60-guard: program columns are stale" '[ "$(level standby)" = ok ] && detail standby | grep -q "基线过期"'
printf '2 69.5 10.0\n3 20.0 0.0\n4 30.0 0.0\n' >"$T/sb.base"; run
check "old-format baseline: not judged, asks for a recalibration" '[ "$(level standby)" = ok ] && detail standby | grep -q "基线格式旧"'
unset DOC_STANDBY_STAT DOC_STANDBY_BASE
rm -rf "$T"

# ── snapshots ───────────────────────────────────────────────────────────────
# The --tsv rows go verbatim onto the touch screen and the web health page, so
# every word is pinned here. A deliberate wording change regenerates them:
#   docker run --rm -v "$PWD/scripts":/scripts busybox:latest \
#       sh -c 'DOCTOR_GOLDEN_UPDATE=1 sh /scripts/test/doctor/run.sh'
# and the diff of golden/ shows exactly which rows moved.
echo "the scorecard: --report (docs/LEDGER.md §11)"
# A made-up ledger: boot 1 from wall W0, hours of lines written by an awk
# generator, events added per case. Every hour at home, fully covered.
W0=1790500000
lsetup() {
    setup
    export DOC_LEDGER_DIR=$T/ledger DOC_CRASHCAP_DIR=$T/crashcap
    mkdir -p "$T/ledger" "$T/crashcap"
    SEG=$T/ledger/boot-000001-aaaaaaaa-001.jsonl
    : >"$SEG"
}
# hours <seq> <wall at uptime 0> <first from> <count> [<awk: per-hour overrides>]
XF=
hours() {
    awk -v s="$1" -v w0="$2" -v u0="$3" -v cnt="$4" -v xf="$XF" "BEGIN {
        for (i = 0; i < cnt; i++) {
            f = u0 + i * 3600; t = f + 3600; gw = 0; ab = 0; net = \"460-00\"; cpu = 10; rss = 20000; thr = 0; e = 900.0
            ton = 3600; tck = 3600; tok = 3600; old = 0 # Tailscale on, checked, healthy; old: a line from before the check
            $5
            tsx = (old ? \",\\\"ts_ok_s\\\":null\" : sprintf(\",\\\"ts_chk_s\\\":%d,\\\"ts_ok_s\\\":%d\", tck, tok))
            printf \"{\\\"v\\\":1,\\\"seq\\\":%d,\\\"n\\\":1,\\\"up\\\":%d,\\\"t\\\":%d,\\\"k\\\":\\\"hour\\\",\\\"id\\\":\\\"h-%d-%d\\\",\\\"from\\\":%d,\\\"to\\\":%d,\\\"ft\\\":%d,\\\"tt\\\":%d,\\\"awake\\\":3600,\\\"asleep\\\":0,\\\"rounds\\\":60,\\\"skipped\\\":0,\\\"max_gap\\\":60,\\\"net\\\":\\\"%s\\\",\\\"home\\\":\\\"460-00\\\",\\\"abroad\\\":%d,\\\"free_mb\\\":900,\\\"dump_mb\\\":1,\\\"budget\\\":0,\\\"g_guard\\\":0,\\\"g_job\\\":0,\\\"g_watcher\\\":%d,\\\"g_capture\\\":0,\\\"g_uidlog\\\":0,\\\"g_crashlog\\\":0,\\\"g_datad\\\":0}\n\", s, t, w0 + t, s, f, f, t, w0 + f, w0 + t, net, ab, gw
            printf \"{\\\"v\\\":1,\\\"seq\\\":%d,\\\"n\\\":2,\\\"up\\\":%d,\\\"t\\\":%d,\\\"k\\\":\\\"hour_power\\\",\\\"id\\\":\\\"hp-%d-%d\\\",\\\"src\\\":\\\"counter\\\",\\\"e\\\":%.1f,\\\"on_home_s\\\":600,\\\"on_home_e\\\":%.1f,\\\"on_abroad_s\\\":0,\\\"on_abroad_e\\\":0.0,\\\"off_home_s\\\":3000,\\\"off_home_e\\\":%.1f,\\\"off_abroad_s\\\":0,\\\"off_abroad_e\\\":0.0,\\\"chg_s\\\":0,\\\"batt_from\\\":80,\\\"batt_to\\\":79,\\\"batt_tmax\\\":350,\\\"zone_tmax\\\":45000,\\\"throttle_s\\\":%d,\\\"e_partial\\\":0}\n\", s, t, w0 + t, s, f, e, e / 3, e * 2 / 3, thr
            printf \"{\\\"v\\\":1,\\\"seq\\\":%d,\\\"n\\\":3,\\\"up\\\":%d,\\\"t\\\":%d,\\\"k\\\":\\\"hour_proc\\\",\\\"id\\\":\\\"hq-%d-%d\\\",\\\"rss_agent\\\":%d,\\\"rss_datad\\\":%d,\\\"rss_devui\\\":8000,\\\"rss_ts\\\":45000,\\\"cpu_datad\\\":%d,\\\"idle_rows\\\":40,\\\"idle_wan\\\":3%s,\\\"ts_on_s\\\":%d%s}\n\", s, t, w0 + t, s, f, rss, rss, cpu, xf, ton, tsx
        } }" >>"$SEG"
}
ev() { printf '{"v":1,"seq":%s,"n":9,"up":%s,"t":%s,"k":"%s"%s}\n' "$1" "$2" "$3" "$4" "$5" >>"$SEG"; } # ev <seq> <up> <t> <k> <,fields>
boot1() { ev 1 50 null boot ',"boot":"aaaaaaaa-0000-4000-8000-000000000001","code":1150,"mode":"mode_power_on","fw":null,"net_select":null,"rb_weekly":null,"rb_cutoff":null,"rb_connfail":null'
          ev 1 50 null guard_start ',"id":"gs-aaaaaaaa-50","requested":0,"by":null,"why":null'
          ev 1 60 null ver ',"prog":"zwrt-datad","md5":"d1d1d1d1","how":"exe","pid":700,"extra":"ubus=cli"'; }
rep() { sh "$SCRIPTS/doctor.sh" --report "$1" >"$T/rep" 2>&1; RC=$?; }
line() { grep "^  $1 " "$T/rep"; }

lsetup
boot1
hours 1 "$W0" 3600 168
rep 7d
check "a clean week: runs, says how far it goes and how much ledger it read" '[ $RC = 0 ] && grep -q "^设备成绩单（7 天，截至" $T/rep && grep -q "账本：1 次开机，168 小时的记录" $T/rep'
check "S1: no unexpected reboot over a whole week, covered: 达标" 'line S1 | grep -q "0 次 · 达标 · 7 天 / 7 天 · 覆盖完整"'
check "S2b, S3: no crash, covered: 达标" 'line S2b | grep -q "在家 没有崩溃；国外 没有崩溃 · 达标" && line S3 | grep -q "在家 0 次；国外 0 次 · 达标"'
check "S6 looks at one day" 'line S6 | grep -q "0 次 · 达标 · 1 天 / 1 天"'
check "S8: Tailscale healthy all week, checked throughout: 达标" 'line S8 | grep -q "Tailscale 100.0% · 达标 · 7 天 / 7 天 · 覆盖完整 · 自动"'
check "U1-U5 by known facts, U6 for a person" 'line U1 | grep -q "不达标 · 第二步前靠已知事实" && line U6 | grep -q "· 人工 ·"'
check "P1: average power from the hours; target not set yet" 'line P1 | grep -q "平均 900 mW（7 天 的样本） · 没测（目标待定）"'
check "P2: each cell over 6 h has a figure, the rest too few" 'line P2 | grep -q "亮屏在家 1800 mW；亮屏国外 样本不足；息屏在家 720 mW；息屏国外 样本不足"'
check "P4a: datad at 1.0% of a core: 达标; P4b: cli backend: 不达标" 'line P4a | grep -q "平均 1.0%（24 小时） · 达标" && line P4b | grep -q "cli 后端.* · 不达标"'
check "P5: a boot that ran 24 h, RSS flat: 达标" 'line P5 | grep -q "RSS 增长都在 10% 以内；OOM 0 次 · 达标"'
check "P6: no throttling, no thermal rise: 达标" 'line P6 | grep -q "降频 0 分；过热等级上升 0 次 · 达标"'
rep 24h
check "--report 24h: 7-day items show 1 day of 7: 注意" 'line S1 | grep -q "0 次 · 注意 · 1 天 / 7 天"'
check "summary: three lines for the default output" 'sh "$SCRIPTS/doctor.sh" --report 7d --summary >$T/sum && [ "$(wc -l <$T/sum)" = 3 ] && grep -q "意外重启 0 次（达标）" $T/sum && grep -q "耗电：平均 900 mW" $T/sum'
cp "$T/sum" "$T/ledger/summary"
check "the default output ends with them" 'sh "$SCRIPTS/doctor.sh" >$T/out2; tail -n 3 $T/out2 | cmp -s - $T/sum'
rm -rf "$T"

lsetup # two days only, and a boot that ended in a modem crash with guard up
boot1
hours 1 "$W0" 3600 40
ev 1 20000 $((W0 + 20000)) ssr ',"id":"ssr-aaaaaaaa-500","kseq":500,"line":"x","rat":"SA","band":"n78","nrband":"78","net":"460-00","home":"460-00","net_age":10,"wan_ppm":3'
SEG=$T/ledger/boot-000002-bbbbbbbb-001.jsonl
ev 2 50 null boot ',"boot":"bbbbbbbb-0000-4000-8000-000000000002","code":1182,"mode":"mode_power_on","fw":null,"net_select":null,"rb_weekly":null,"rb_cutoff":null,"rb_connfail":null'
hours 2 $((W0 + 40 * 3600 + 3600 + 120)) 60 8
rep 7d
check "1182 after a crash with no result: the modem, guard was up: S1 and S2a 不达标" 'line S1 | grep -q "1 次 · 不达标" && line S2a | grep -q "1 次（开机空档 0 次） · 不达标"'
check "the crash that rebooted counts as 600 s or more in S2b" 'line S2b | grep -q "在家 P95 600 秒（1 次）.* · 不达标"'
check "under a week of ledger, no violation elsewhere: 注意 with how long" 'line S5a | grep -q "0 次 · 注意 · 2 天"'
rm -rf "$T"

lsetup # recovery times at home and abroad; a watcher gap of 12 minutes
boot1
hours 1 "$W0" 3600 168 'if (i == 100) gw = 720; if (i >= 120) { ab = 3600; net = "440-10" }'
for k in 1 2 3; do
    ev 1 $((100000 + k * 1000)) null ssr ',"id":"ssr-aaaaaaaa-'$k'","kseq":'$k',"line":"x","rat":"SA","band":"n78","nrband":"78","net":"460-00","home":"460-00","net_age":10,"wan_ppm":3'
    ev 1 $((100030 + k * 1000)) null ssr_result ',"id":"res-ssr-aaaaaaaa-'$k'","ssr":"ssr-aaaaaaaa-'$k'","result":"ok","recovered_s":'$((10 + k))',"rx_seen_s":null,"down_seen":1,"resumed":0,"gap_s":0,"slept":0'
done
ev 1 500000 null ssr ',"id":"ssr-aaaaaaaa-9","kseq":9,"line":"rf_nr5g_sub6_tx.c:5428","rat":"SA","band":"n77","nrband":"77","net":"440-10","home":"460-00","net_age":10,"wan_ppm":90'
ev 1 500100 null ssr_result ',"id":"res-ssr-aaaaaaaa-9","ssr":"ssr-aaaaaaaa-9","result":"ok","recovered_s":80,"rx_seen_s":84,"down_seen":1,"resumed":0,"gap_s":0,"slept":0'
rep 7d
check "one watcher gap over 10 minutes: S2b and S3 not measured" 'line S2b | grep -q "· 没测 ·.*覆盖不完整（.*有一段超过 10 分钟）" && line S3 | grep -q "· 没测 ·"'
check "S2b still shows the figures: home P95 13 s of 3, abroad 80 s" 'line S2b | grep -q "在家 P95 13 秒（3 次）；国外 P95 80 秒（1 次）"'
check "S3 lists abroad crashes by network, RAT and band for a person" 'line S3 | grep -q "在家 3 次；国外 1 次" && grep -q "国外：440-10 SA n77/n77 1 次" $T/rep'
check "S1 is not about the watcher: still covered" 'line S1 | grep -q "覆盖完整"'
rm -rf "$T"


# S8, the Tailscale half (docs/LEDGER.md §8 and §11): healthy over checked, its own coverage
s8() { # s8 <awk: per-hour overrides>: a clean week, then the S8 row
    lsetup; boot1; hours 1 "$W0" 3600 168 "$1"; rep 7d; line S8 >"$T/s8"; S8=$(cat "$T/s8"); rm -rf "$T"
}
s8 'old = 1'
check "S8: hour lines from before the check: never checked, not measured (not 0%)" 'echo "$S8" | grep -q "Tailscale 没查到过 · 没测 · .*覆盖不完整（Tailscale 开着 7 天，查到 0 分，有一小时里超过 10 分钟没查到）"'
s8 'tok = 3500'
check "S8: Tailscale healthy 97.2% of the checked time: 不达标" 'echo "$S8" | grep -q "Tailscale 97.2% · 不达标 · .*覆盖完整"'
s8 'tck = 3240; tok = 3240'
check "S8: 10% of the on time unchecked: not measured, whatever the checked part says" 'echo "$S8" | grep -q "Tailscale 100.0% · 没测 · .*覆盖不完整（Tailscale 开着 7 天，查到 6 天 7 小时）"'
s8 'if (i == 50) { tck = 2940; tok = 2940 }'
check "S8: one hour with 11 minutes unchecked: not measured" 'echo "$S8" | grep -q "Tailscale 100.0% · 没测 · .*有一小时里超过 10 分钟没查到"'
s8 'if (i == 50) { tck = 3060; tok = 3060 }'
check "S8: one hour with 9 minutes unchecked: still covered" 'echo "$S8" | grep -q "Tailscale 100.0% · 达标 · .*覆盖完整"'
s8 'tck = 3000; tok = 2000'
check "S8: under 99% even if every unchecked second was healthy: 不达标 despite the coverage" 'echo "$S8" | grep -q "Tailscale 66.7% · 不达标 · .*覆盖不完整"'
s8 'ton = 0; tck = 0; tok = 0'
check "S8: Tailscale never meant on: nothing to judge" 'echo "$S8" | grep -q "Tailscale 没开过 · 没测"'

lsetup # datad degraded, datad hungry, memory growing, throttled
boot1
hours 1 "$W0" 3600 168 'if (i >= 140) cpu = 30; if (i >= 100) rss = 26000; if (i == 160) thr = 120'
ev 1 600000 null datad_degraded ',"id":"dds-aaaaaaaa-1790000000","state":"start","since":1790000000,"reason":"x","dur_s":null,"how":null'
ev 1 600400 null datad_degraded ',"id":"dde-aaaaaaaa-1790000000","state":"end","since":1790000000,"reason":null,"dur_s":400,"how":"gone"'
ev 1 500000 null thermal ',"id":"th-aaaaaaaa-1","level":"0x3","line":"x"'
ev 1 550000 null thermal ',"id":"th-aaaaaaaa-2","level":"0x5","line":"x"'
rep 7d
check "S6: one episode today, 6 min 40 s: 不达标" 'line S6 | grep -q "1 次，共 6 分 · 不达标"'
check "P4a: datad over 2% of a core today: 不达标" 'line P4a | grep -q "平均 3.0%.* · 不达标"'
check "P5: RSS up 30% over a 24 h boot: 不达标, named" 'line P5 | grep -q "agent +30%；datad +30%；.* · 不达标"'
check "P6: throttled 2 min and one thermal rise above the boot's first level: 不达标" 'line P6 | grep -q "降频 2 分；过热等级上升 1 次 · 不达标"'
rm -rf "$T"

lsetup # a hole between boots that only ends inside the window: only its part inside counts
boot1
hours 1 "$W0" 3600 30
B2=$((W0 + 31 * 3600 + 3900)) # boot 2's first hour starts 65 min after boot 1's last one ended
SEG=$T/ledger/boot-000002-bbbbbbbb-001.jsonl
ev 2 50 null boot ',"boot":"bbbbbbbb-0000-4000-8000-000000000002","code":1134,"mode":"mode_power_on","fw":null,"net_select":null,"rb_weekly":null,"rb_cutoff":null,"rb_connfail":null'
hours 2 $((B2 - 60)) 60 23
printf '{"v":1,"seq":2,"n":9,"up":86160,"t":%s,"k":"hour","id":"h-2-82860","from":82860,"to":86160,"ft":%s,"tt":%s,"awake":3300,"asleep":0,"rounds":55,"skipped":0,"max_gap":60,"net":"460-00","home":"460-00","abroad":0,"free_mb":900,"dump_mb":1,"budget":0,"g_guard":0,"g_job":0,"g_watcher":0,"g_capture":0,"g_uidlog":0,"g_crashlog":0,"g_datad":0}\n' \
    $((B2 - 60 + 86160)) $((B2 - 60 + 82860)) $((B2 - 60 + 86160)) >>"$SEG" # the last hour ends 5 min short
rep 24h
check "a 65-minute hole that reaches 5 minutes into the window: 5 minutes counted, no 10-minute gap" 'line S2b | grep -q "· 覆盖完整 ·"'
rm -rf "$T"

lsetup # a merged chain that slept in its first part: kept apart like any slept sample
boot1
hours 1 "$W0" 3600 168
ev 1 100000 null ssr ',"id":"ssr-aaaaaaaa-1","kseq":1,"line":"x","rat":"SA","band":"n78","nrband":"78","net":"460-00","home":"460-00","net_age":10,"wan_ppm":3'
ev 1 100070 null ssr_result ',"id":"res-ssr-aaaaaaaa-1","ssr":"ssr-aaaaaaaa-1","result":"merged","recovered_s":null,"rx_seen_s":null,"down_seen":1,"resumed":0,"gap_s":40,"slept":1'
ev 1 100070 null ssr ',"id":"ssr-aaaaaaaa-2","kseq":2,"line":"x","rat":"SA","band":"n78","nrband":"78","net":"460-00","home":"460-00","net_age":10,"wan_ppm":3'
ev 1 100100 null ssr_result ',"id":"res-ssr-aaaaaaaa-2","ssr":"ssr-aaaaaaaa-2","result":"ok","recovered_s":20,"rx_seen_s":null,"down_seen":1,"resumed":0,"gap_s":0,"slept":0'
rep 7d
check "S2b: the chain is a slept sample, not a 90 s recovery; nothing to judge 达标 on" 'line S2b | grep -q "在家 没有崩溃；国外 没有崩溃；跟随中睡过 1 次另列 · 注意"'
rm -rf "$T"

lsetup # alerts in the window, a trimmed queue
boot1
hours 1 "$W0" 3600 168
printf '150\t%s\t100\tagent-crash\tx\n151\t%s\t200\tsms-test\tx\n' $((W0 + 400000)) $((W0 + 400100)) >"$T/alerts/queue"
rep 7d
check "U6: alerts in the window listed (sms-test left out), queue trimmed after the window began: 没测" 'line U6 | grep -q "窗口内告警 1 条.* · 没测 ·.*队列剪过" && grep -q "告警：.* agent-crash；" $T/rep'
rm -rf "$T"

lsetup # bad lines and newer formats are skipped and counted
boot1
hours 1 "$W0" 3600 3
echo 'not json' >>"$SEG"
printf '{"v":2,"seq":1,"n":1,"up":1,"t":null,"k":"future"}\n' >>"$SEG"
printf '{"v":1,"seq":1,"n":9,"up":5,"t":null,"k":"hour","id":"h-1-3600","from":3600}\n' >>"$SEG"
rep 7d
check "unreadable lines and a newer version are skipped and counted; a repeated id is taken once" '[ $RC = 0 ] && grep -q "账本：1 次开机，3 小时的记录" $T/rep && grep -q "1 行读不懂，已跳过；1 行是更新的格式" $T/rep'
rm -rf "$T"

lsetup # 16 MB of ledger (the retention cap): how long the report takes (F24; recorded, no limit)
boot1
hours 1 "$W0" 3600 720
awk -v w0="$W0" 'BEGIN { for (i = 0; i < 36000; i++) printf "{\"v\":1,\"seq\":1,\"n\":%d,\"up\":%d,\"t\":%d,\"k\":\"link\",\"id\":\"link-aaaaaaaa-%d\",\"state\":\"up\",\"cause\":\"other\",\"res\":2,\"pad\":\"%s\"}\n", i, 4000 + i * 60, w0 + 4000 + i * 60, i, sprintf("%300s", "") }' >>"$SEG"
sz=$(wc -c <"$SEG")
t0=$(date +%s)
rep 7d
t1=$(date +%s)
rep 24h
t2=$(date +%s)
echo "  (16 MB ledger: $((sz / 1048576)) MB; --report 7d $((t1 - t0)) s, --report 24h $((t2 - t1)) s in this container)"
check "a 16 MB ledger is read to the end" '[ $RC = 0 ] && grep -q "720 小时的记录" $T/rep'
rm -rf "$T"

lsetup # hours on record, but the clock never trusted: nothing can be placed in a window
boot1
printf '{"v":1,"seq":1,"n":5,"up":7200,"t":null,"k":"hour","id":"h-1-3600","from":3600,"to":7200,"ft":null,"tt":null,"awake":3600,"asleep":0,"rounds":60,"skipped":0,"max_gap":60,"net":null,"home":null,"abroad":0,"free_mb":null,"dump_mb":null,"budget":0,"g_guard":0,"g_job":0,"g_watcher":0,"g_capture":0,"g_uidlog":0,"g_crashlog":0,"g_datad":0}\n' >>"$SEG"
rep 7d
check "hours on record but the clock never set: says that, not 'no records'" '[ $RC = 1 ] && grep -q "账本有 1 小时的记录，但设备时钟一直没对上" $T/rep'
rm -rf "$T"

lsetup
rm -f "$SEG"
rep 7d
check "no ledger yet: says so" '[ $RC = 1 ] && grep -q "账本还没有记录" $T/rep'
rm -rf "$T"

echo "--ledger-selftest (docs/LEDGER.md §13)"
ssetup() {
    setup
    export DOC_LEDGER_DIR=$T/ledger DOC_CRASHCAP_DIR=$T/crashcap DOC_KEYLOG=$T/key.log DOC_MSS_RECOVERY=$T/recovery \
        DOC_BOOT_ID_FILE=$T/bootid DOC_JSONFILTER=$T/bin/jsonfilter DOC_LEDGER_TMP=$T/lt DOC_SELFTEST_WAIT=2
    mkdir -p "$T/ledger" "$T/crashcap" "$T/lt"
    echo aaaaaaaa-0000-4000-8000-000000000001 >"$T/bootid"
    export DOC_KMSG=$T/crashcap/kmsg-aaaaaaaa.log # the capture, as if the kernel log were copied at once
    printf '6,1,1000,-;boot\n' >"$DOC_KMSG"
    printf '2026-09-28 10:00:00 : reboot_reason_code=1150!!! \n' >"$T/key.log"
    echo enabled >"$T/recovery"
    printf '#!/bin/sh\necho "export X='"'"'1'"'"'; export Z='"'"'1'"'"'; "\n' >"$T/bin/jsonfilter"
    printf '#!/bin/sh\ncase "$*" in *9460/state*) echo "{\\"ts\\":1}" ;; *public/status*) cat %s/public 2>/dev/null ;; esac\nexit 0\n' "$T" >"$T/bin/wget"
    printf '#!/bin/sh\necho "9: rmnet_data0    inet 10.1.2.3/30 scope global rmnet_data0"\n' >"$T/bin/ip"
    printf 'Iface\tDestination\tGateway\tFlags\nrmnet_data0\t00000000\t00000000\t0001\n' >"$T/route"
    chmod +x "$T/bin/jsonfilter" "$T/bin/wget" "$T/bin/ip"
    export DOC_IP=$T/bin/ip DOC_ROUTE=$T/route
}
self() { sh "$SCRIPTS/doctor.sh" --ledger-selftest >"$T/st" 2>&1; RC=$?; }
ssetup
self
check "everything in place: every item PASS, exit 0" '[ $RC = 0 ] && ! grep -q "^FAIL" $T/st && grep -q "^PASS kmsg 落盘" $T/st && grep -q "全部通过" $T/st'
check "the link as the watcher sees it: an address and the default route" 'grep -q "^PASS ip -4 -o addr 看得到 rmnet_data0 的地址" $T/st && grep -q "^PASS 默认路由走 rmnet_data0" $T/st'
check "it names the device tools u60-guard leans on" 'grep -q "^PASS dd iflag=skip_bytes" $T/st && grep -q "^PASS awk 的 mktime" $T/st && grep -q "^PASS flock -n" $T/st && grep -q "^PASS jsonfilter 多个 -e" $T/st && grep -q "^PASS wget -T 2" $T/st'
check "the ledger is left as it was: no test file, nothing else written" '[ -z "$(ls -A $T/ledger)" ]'
echo "10 1 0" >"$T/lt/w.pos" # a watcher that stays behind the test line
self
check "watcher running but not past the test line: FAIL" '[ $RC != 0 ] && grep -q "^FAIL 观察循环读过了测试行" $T/st'
rm -f "$T/lt/w.pos"
: >"$T/key.log"
rm -f "$T/crashcap/kmsg-aaaaaaaa.log"
export DOC_KMSG=$T/no/such/kmsg
self
printf 'Iface\tDestination\tGateway\tFlags\n' >"$T/route" # no default route
self
check "no reason code, no capture file, no default route: each a FAIL, the rest still tried" 'grep -q "^FAIL key.log 的原因码" $T/st && grep -q "^FAIL kmsg 落盘：没有这次开机的落盘文件" $T/st && grep -q "^FAIL 默认路由走 rmnet_data0" $T/st && grep -q "^PASS 账本目录可写" $T/st && grep -q "3 项没通过" $T/st'
unset DOC_LEDGER_DIR DOC_CRASHCAP_DIR DOC_KEYLOG DOC_MSS_RECOVERY DOC_BOOT_ID_FILE DOC_JSONFILTER DOC_LEDGER_TMP DOC_SELFTEST_WAIT DOC_KMSG DOC_IP DOC_ROUTE
rm -rf "$T"

echo "snapshots"
GOLD=$SCRIPTS/test/doctor/golden
snap() { # snap <name>
    sh "$SCRIPTS/doctor.sh" --tsv >"$T/$1.tsv" 2>&1
    sh "$SCRIPTS/doctor.sh" >"$T/$1.txt" 2>&1
    for ext in tsv txt; do
        if [ -n "$DOCTOR_GOLDEN_UPDATE" ]; then
            mkdir -p "$GOLD" && cp "$T/$1.$ext" "$GOLD/$1.$ext" && ok "golden $1.$ext written"
        elif cmp -s "$T/$1.$ext" "$GOLD/$1.$ext"; then
            ok "snapshot $1.$ext"
        else
            bad "snapshot $1.$ext differs from golden/$1.$ext"
            diff "$GOLD/$1.$ext" "$T/$1.$ext" 2>&1 | head -20
        fi
    done
}
setup; snap healthy; rm -rf "$T"
setup
echo "zte_topsw_devui" >"$T/sync"; echo 1 >"$T/mode"; echo 0 >"$T/aps"
touch "$T/down-u60-guard"; echo 500 >"$T/hb"; touch "$T/uid/gave-up"; : >"$T/alerts/sms-to"
grep -v "u60-uid start" "$T/rc" >"$T/rc2"; mv "$T/rc2" "$T/rc"
printf '9\t1\tcrash\tx\ty\n' >"$T/alerts/queue"
snap problems; rm -rf "$T"
setup; export DOC_STANDBY_STAT=$T/sb DOC_STANDBY_BASE=$T/sb.base DOC_LEDGER_DIR=$T/ledger DOC_BOOT_ID_FILE=$T/bootid
echo aaaaaaaa-0000-4000-8000-000000000001 >"$T/bootid"
i=0; while [ $i -lt 40 ]; do echo "$((100 + i * 20)) $((50 + i)) 20 30.0$MCOL 2.0 1.0 0.5" >>"$T/sb"; i=$((i + 1)); done
sh "$SCRIPTS/doctor.sh" --calibrate-standby >/dev/null
check "calibration leaves a calib event in the /data spool, as a producer writes it (§5)" 'f=$T/ledger/spool/calib-aaaaaaaa-1000.ev; [ -f "$f" ] && [ "$(head -n 1 "$f")" = "calib-aaaaaaaa-1000	aaaaaaaa-0000-4000-8000-000000000001	1000	calib" ] && [ "$(sed -n 2p "$f")" = ",\"rows\":40" ] && [ -z "$(ls $T/ledger/spool | grep tmp)" ]'
unset DOC_LEDGER_DIR DOC_BOOT_ID_FILE
: >"$T/sb"; i=0; while [ $i -lt 10 ]; do echo "$((300 + i * 60)) 200 20 30.0$MCOL 2.0 1.0 0.5" >>"$T/sb"; i=$((i + 1)); done
snap standby; unset DOC_STANDBY_STAT DOC_STANDBY_BASE; rm -rf "$T"

echo; echo "passed $PASS, failed $FAIL"; [ "$FAIL" = 0 ]
