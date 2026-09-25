#!/bin/sh
# ─────────────────────────────────────────────────────────────────────────────
# doctor.sh — read-only health check of the U60 Pro (MU5250) setup.
#
#   sh /data/u60-guard/doctor.sh          human-readable (install.sh doctor)
#   sh /data/u60-guard/doctor.sh --tsv    one check per line for zte-agent:
#                                          <ok|warn|bad>\t<id>\t<label>\t<detail>
#   sh /data/u60-guard/doctor.sh --calibrate-standby
#                                          set the standby sentinel's baseline from
#                                          u60-guard's screen-off records (≥30 lines;
#                                          run after 30-60 min with the screen off)
#
# One implementation of "is this device healthy", used by the install kit over
# SSH (works with the agent dead) and by zte-agent's /api/health (health page,
# touch-screen summary). Changes nothing, ever — safe to run any time.
# Every command and path can be overridden from the environment for tests.
# SPDX-License-Identifier: MIT
# ─────────────────────────────────────────────────────────────────────────────

UBUS=${DOC_UBUS:-ubus}
UCI=${DOC_UCI:-uci}
PS=${DOC_PS:-ps w}
RC=${DOC_RC:-/etc/rc.local}
INITD=${DOC_INITD:-/etc/init.d}
UPTIME_FILE=${DOC_UPTIME:-/proc/uptime}
HEARTBEAT=${DOC_HEARTBEAT:-/tmp/scenario.heartbeat}
MARKER=${DOC_MARKER:-/tmp/u60-wifiguard.took-over}
ALERTS=${DOC_ALERTS:-/data/alerts}
CRASH=${DOC_CRASH:-/data/crashlog}
UID_STATE=${DOC_UID_STATE:-/data/u60-uid}
OVERLAY_RCD=${DOC_OVERLAY_RCD:-/zteoverlay/etc-upper_a/rc.d}
DAEMON_CONF=${DOC_DAEMON_CONF:-/etc/config/zte_topsw_daemon.conf}
DATA_DIR=${DOC_DATA:-/data}
WGET=${DOC_WGET:-wget}
PROC=${DOC_PROC:-/proc}
STANDBY_STAT=${DOC_STANDBY_STAT:-/tmp/standby.stat}
STANDBY_BASE=${DOC_STANDBY_BASE:-/data/u60-guard/standby.baseline}
CLOCK_SANE_AFTER=1704067200

# Standby sentinel. u60-guard writes one line per screen-off minute:
#   <uptime> <cellular pkts/min> <tunnel pkts/min> <wakeups/s ×5 programs | ->
# Columns 2-8 are judged against a baseline of median and MAD (median
# absolute deviation) taken from those same records.
standby_labels='- 蜂窝包/分 Tailscale隧道包/分 tailscaled唤醒/秒 mihomo唤醒/秒 触屏唤醒/秒 数据服务唤醒/秒 后台唤醒/秒'

calibrate_standby() {
    n=$(wc -l <"$STANDBY_STAT" 2>/dev/null || echo 0)
    if [ "${n:-0}" -lt 30 ]; then
        echo "只有 ${n:-0} 行息屏记录，至少要 30 行（屏幕熄灭约 30 分钟后再试）"; return 1
    fi
    awk '
        function sortv(a, c,   i, j, v) { for (i = 2; i <= c; i++) { v = a[i]; j = i - 1; while (j > 0 && a[j] > v) { a[j + 1] = a[j]; j-- } a[j + 1] = v } }
        function med(a, c) { sortv(a, c); return (c % 2) ? a[(c + 1) / 2] : (a[c / 2] + a[c / 2 + 1]) / 2 }
        { for (k = 2; k <= 8; k++) if ($k != "-" && $k != "") v[k, ++cnt[k]] = $k }
        END {
            for (k = 2; k <= 8; k++) {
                c = cnt[k] + 0; if (c == 0) { print k, "-", "-"; continue }
                for (i = 1; i <= c; i++) x[i] = v[k, i]
                m = med(x, c)
                for (i = 1; i <= c; i++) { d = v[k, i] - m; x[i] = (d < 0 ? -d : d) }   # busybox awk: ?: must be parenthesised
                printf "%d %.1f %.1f\n", k, m, med(x, c)
            }
        }' "$STANDBY_STAT" >"$STANDBY_BASE.tmp" && mv -f "$STANDBY_BASE.tmp" "$STANDBY_BASE" || return 1
    echo "基线已写入 $STANDBY_BASE（$n 行记录）"
}

# prints "<ok|warn>\t<detail>"
standby_check() {
    [ -f "$STANDBY_BASE" ] || { printf 'ok\t未校准（屏幕熄灭 30~60 分钟后执行 doctor.sh --calibrate-standby）\n'; return; }
    awk -v now="$(uptime_s)" -v labels="$standby_labels" -v base="$STANDBY_BASE" '
        function sortv(a, c,   i, j, v) { for (i = 2; i <= c; i++) { v = a[i]; j = i - 1; while (j > 0 && a[j] > v) { a[j + 1] = a[j]; j-- } a[j + 1] = v } }
        function med(a, c) { sortv(a, c); return (c % 2) ? a[(c + 1) / 2] : (a[c / 2] + a[c / 2 + 1]) / 2 }
        BEGIN { split(labels, lab, " "); while ((getline l < base) > 0) { split(l, f, " "); bm[f[1]] = f[2]; bd[f[1]] = f[3] } }
        $1 >= now - 900 { rows++; for (k = 2; k <= 8; k++) if ($k != "-" && $k != "") v[k, ++cnt[k]] = $k }
        END {
            if (rows < 8) { printf "ok\t最近 15 分钟息屏记录只有 %d 行，不判定\n", rows; exit }
            out = ""
            for (k = 2; k <= 8; k++) {
                c = cnt[k] + 0; if (c == 0 || bm[k] == "-" || bm[k] == "") continue
                for (i = 1; i <= c; i++) x[i] = v[k, i]
                m = med(x, c)
                if (k == 2) cell = m
                if (m > bm[k] + 3 * bd[k] && m > 1.5 * bm[k] && m - bm[k] >= 1)
                    out = out (out == "" ? "" : "；") sprintf("%s %.0f（基线 %.0f）", lab[k], m, bm[k])
            }
            if (out != "") printf "warn\t%s\n", out
            else printf "ok\t正常（蜂窝每分钟 %.0f 个包）\n", cell
        }' "$STANDBY_STAT" 2>/dev/null || printf 'ok\t没有息屏记录\n'
}

if [ "$1" = "--calibrate-standby" ]; then
    uptime_s() { cut -d. -f1 "$UPTIME_FILE" 2>/dev/null || echo 0; }
    calibrate_standby; exit $?
fi

TSV=0
[ "$1" = "--tsv" ] && TSV=1
NBAD=0
NWARN=0

# report <ok|warn|bad> <id> <label> <detail>
report() {
    case "$1" in bad) NBAD=$((NBAD + 1)) ;; warn) NWARN=$((NWARN + 1)) ;; esac
    if [ "$TSV" = 1 ]; then
        printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$(printf '%s' "$4" | tr '\t\n' '  ')"
    else
        case "$1" in ok) s='●' ;; warn) s='▲' ;; *) s='■' ;; esac
        printf '  %s %s：%s\n' "$s" "$3" "$4"
    fi
}

uptime_s() { cut -d. -f1 "$UPTIME_FILE" 2>/dev/null || echo 0; }
count_comm() { # processes whose /proc comm starts with $1
    _n=0
    for _d in "$PROC"/[0-9]*; do
        { read -r _c <"$_d/comm"; } 2>/dev/null || continue   # the process may be gone by now
        case "$_c" in "$1"*) _n=$((_n + 1)) ;; esac
    done
    echo "$_n"
}
svc_running() {
    $UBUS call service list "{\"name\":\"$1\"}" 2>/dev/null | grep -q '"running": *true'
}

# ── boot and firmware ───────────────────────────────────────────────────────
sync=$($UBUS call zwrt_topsw_daemon.sync get_sync_info '{}' 2>/dev/null | sed -n 's/.*"noSyncModuleName": *"\([^"]*\)".*/\1/p')
if [ "$sync" = "sync success" ]; then
    report ok boot-sync "开机同步" "sync success"
else
    report bad boot-sync "开机同步" "${sync:-读不到}（有原厂服务没注册：屏幕卡 logo / 不拨号的根源）"
fi

mode=$($UCI -q get zwrt_zte_dm.dm_update.dm_update_mode)
poll=$($UCI -q get zwrt_zte_dm.dm_update.TURNOFFPOLLING)
if [ "$mode" = 0 ] && [ "$poll" = 1 ]; then
    report ok fota "ZTE 自动升级" "已关闭"
else
    report bad fota "ZTE 自动升级" "没有完全关闭（dm_update_mode=${mode:-?} TURNOFFPOLLING=${poll:-?}）；升级会覆盖 rc.local"
fi

# Whiteouts in the /etc overlay's rc.d silently delete boot links. On a daemon
# the boot barrier waits for, that means a device stuck on the logo.
if [ -d "$OVERLAY_RCD" ]; then
    wo=$(ls -la "$OVERLAY_RCD" 2>/dev/null | awk '/^c/ {print $NF}')
    hit=
    for w in $wo; do
        n=$(echo "$w" | sed 's/^[SK][0-9]*//')
        grep -qE "^[^#]*[[:space:]]$n\$" "$DAEMON_CONF" 2>/dev/null && hit="$hit $w"
    done
    if [ -n "$hit" ]; then
        report bad whiteout "开机链接" "被屏蔽的原厂服务:$hit（删掉 $OVERLAY_RCD 里对应的字符设备）"
    else
        report ok whiteout "开机链接" "开机同步名单里的服务没有被屏蔽${wo:+（另有 $(echo $wo | wc -w) 个无关的：$(echo $wo | tr '\n' ' ')）}"
    fi
fi

t=$(date +%s)
if [ "$t" -ge "$CLOCK_SANE_AFTER" ]; then
    report ok clock "时钟" "已对时（$(date '+%Y-%m-%d %H:%M') 设备当地时间）"
else
    report warn clock "时钟" "还没对时，告警短信在对时前不发"
fi

# ── our services ────────────────────────────────────────────────────────────
for s in zte-agent zwrt-datad u60-guard u60-uid; do
    case "$s" in
        zte-agent) label="高级后台" ;;
        zwrt-datad) label="数据服务" ;;
        u60-guard) label="Wi-Fi 兜底看门狗" ;;
        u60-uid) label="屏幕守护进程" ;;
    esac
    if [ ! -x "$INITD/$s" ]; then
        report warn "svc-$s" "$label" "没装 procd 服务（旧装法：崩了没人拉起）"
    elif svc_running "$s"; then
        if grep -q "^[^#]*$INITD/$s start" "$RC" 2>/dev/null; then
            report ok "svc-$s" "$label" "procd 监督中"
        else
            report warn "svc-$s" "$label" "在跑，但 rc.local 里没有启动它：重启后不会起来"
        fi
    else
        report bad "svc-$s" "$label" "procd 服务没在运行（logread 看原因；/etc/init.d/$s start）"
    fi
done

for p in zte-agent zwrt-datad u60pro-devui; do
    n=$(count_comm "$p")
    if [ "$n" -gt 1 ]; then
        report bad "dup-$p" "$p 实例数" "$n 份在跑（应当只有 1 份）"
    fi
done

if $WGET -q -T 5 -O /dev/null http://127.0.0.1:9090/ 2>/dev/null; then
    report ok agent-http "管理网页 :9090" "能访问"
else
    report bad agent-http "管理网页 :9090" "打不开"
fi
if $WGET -q -T 5 -O /dev/null http://127.0.0.1:9460/state 2>/dev/null; then
    report ok datad-http "数据服务 :9460" "能访问"
else
    report bad datad-http "数据服务 :9460" "读不到 /state（屏幕会没有数据）"
fi

# ── Wi-Fi safety net ────────────────────────────────────────────────────────
up=$(uptime_s)
hb=$(cat "$HEARTBEAT" 2>/dev/null)
case "$hb" in '' | *[!0-9]*) hb= ;; esac
if [ -z "$hb" ]; then
    report bad heartbeat "后台心跳" "没有心跳文件（后台没在运行？）"
elif [ $((up - hb)) -le 120 ]; then
    report ok heartbeat "后台心跳" "$((up - hb)) 秒前"
else
    report bad heartbeat "后台心跳" "$((up - hb)) 秒没更新（情景引擎卡住了？看门狗 5 分钟后会接管 Wi-Fi）"
fi
if [ -f "$MARKER" ]; then
    report warn takeover "Wi-Fi 看门狗" "接管中：情景固定暂停，后台稳定 10 分钟后自动交回"
fi
aps=$($PS 2>/dev/null | awk '/\/hostapd( |$)/ && !/awk/ {n++} END {print n+0}')
if [ "$aps" -gt 0 ]; then
    report ok wifi "Wi-Fi" "在广播"
else
    # 情景有意关掉的（在家）不算问题：问后台当前情景是不是关 Wi-Fi 的那种
    pub=$($WGET -q -T 3 -O - http://127.0.0.1:9090/api/public/status 2>/dev/null)
    case "$pub" in
        *'"wifi_off":true'*) report ok wifi "Wi-Fi" "当前情景关着 Wi-Fi（按设定）" ;;
        *) report warn wifi "Wi-Fi" "没有在广播，当前情景也没要求关" ;;
    esac
fi

# ── screen ──────────────────────────────────────────────────────────────────
if [ -f "$UID_STATE/gave-up" ]; then
    report bad screen "触屏界面" "反复启动失败，已换回原厂界面（长按屏幕右下角 3 秒，或 echo devui > /tmp/u60-uid.ctl）"
elif [ "$(cat /tmp/u60-uid.want 2>/dev/null)" = vendor ]; then
    report ok screen "触屏界面" "按要求显示原厂界面"
elif [ "$(count_comm u60pro-devui)" -ge 1 ]; then
    a=$(cat "$UID_STATE/attempts" 2>/dev/null)
    case "$a" in '' | 0) report ok screen "触屏界面" "运行中" ;;
        *) report ok screen "触屏界面" "运行中（刚启动，稳定 10 分钟后确认）" ;; esac
else
    report bad screen "触屏界面" "没在运行"
fi

# ── alerts, crashes, storage ────────────────────────────────────────────────
unread=0
if [ -f "$ALERTS/queue" ]; then
    read_seq=$(cat "$ALERTS/read" 2>/dev/null)
    case "$read_seq" in '' | *[!0-9]*) read_seq=0 ;; esac
    unread=$(awk -F'\t' -v r="$read_seq" 'NF == 5 && $1 + 0 > r + 0' "$ALERTS/queue" | wc -l)
fi
if [ "$unread" -gt 0 ]; then
    report warn alerts "告警" "$unread 条未读（管理网页「系统 → 告警」）"
else
    report ok alerts "告警" "没有未读"
fi
if [ -s "$ALERTS/sms-to" ]; then
    report ok sms "短信告警" "已配置"
else
    report warn sms "短信告警" "没配置号码：后台挂了你不会知道"
fi

recent=$(find "$CRASH" -name '*.log' -mtime -1 2>/dev/null | wc -l)
if [ "$recent" -gt 0 ]; then
    # 每次崩溃都会记一条告警；告警都读过了，就只记一笔、不再算「注意」
    if [ "$unread" -gt 0 ]; then
        report warn crashes "崩溃记录" "最近 24 小时 $recent 份（$CRASH）"
    else
        report ok crashes "崩溃记录" "最近 24 小时 $recent 份，对应告警已读"
    fi
else
    report ok crashes "崩溃记录" "最近 24 小时没有"
fi

# Last line, counted from the end: a long device name makes df wrap its row.
free_kb=$(df -k "$DATA_DIR" 2>/dev/null | tail -n 1 | awk '{print $(NF - 2)}')
case "$free_kb" in '' | *[!0-9]*) free_kb= ;; esac
if [ -z "$free_kb" ]; then
    report warn disk "/data 空间" "读不到"
elif [ "$free_kb" -lt 20480 ]; then
    report bad disk "/data 空间" "只剩 $((free_kb / 1024)) MB"
elif [ "$free_kb" -lt 102400 ]; then
    report warn disk "/data 空间" "只剩 $((free_kb / 1024)) MB"
else
    report ok disk "/data 空间" "剩 $((free_kb / 1024)) MB"
fi

_sb=$(standby_check)
report "${_sb%%	*}" standby "待机" "${_sb#*	}"

if [ "$TSV" = 0 ]; then
    echo
    if [ "$NBAD" = 0 ] && [ "$NWARN" = 0 ]; then
        echo "  一切正常"
    else
        echo "  $NBAD 项异常，$NWARN 项需要注意"
    fi
fi
[ "$NBAD" = 0 ]
