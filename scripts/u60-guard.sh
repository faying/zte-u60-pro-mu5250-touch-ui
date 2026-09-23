#!/bin/sh
# ─────────────────────────────────────────────────────────────────────────────
# u60-guard — the last line of defence for Wi-Fi, and the alert SMS sender.
#
# The U60 is its owner's only uplink when out. The scenario engine inside
# zte-agent turns the APs off at home; if the agent then dies or hangs, nobody
# turns them back on and the phone has no network after leaving the house.
# This script is deliberately independent of the agent: it knows it only by
# the heartbeat file, and forces Wi-Fi on when the heartbeat stops while the
# APs are down. It also sends the alert SMS, because the agent cannot report
# its own death. Runs under procd (u60-guard.init), one round every 60 s.
#
# Contract with zte-agent (lock, heartbeat, takeover marker, /data/alerts):
# source/manager docs/RELIABILITY.md. The Wi-Fi-on path mirrors
# zte-agent/src/wifi_radio.rs `apply(true, true)` — keep the two in step.
#
#   u60-guard.sh          loop forever (procd)
#   u60-guard.sh once     one round, then exit (tests, manual check)
#
# Every external command and path can be overridden from the environment so
# scripts/test/u60-guard/ can run all of this in a container with stubs.
# SPDX-License-Identifier: MIT
# ─────────────────────────────────────────────────────────────────────────────

HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/alert-lib.sh"

# ── tunables (seconds) ──────────────────────────────────────────────────────
INTERVAL=${GUARD_INTERVAL:-60}
GRACE=${GUARD_GRACE:-300}             # after boot: judge nothing
NEVER_SEEN=${GUARD_NEVER_SEEN:-900}   # no heartbeat at all this long after boot = agent never came up
STALE=${GUARD_STALE:-300}             # heartbeat older than this = agent gone
LOCK_WAIT=${GUARD_LOCK_WAIT:-150}     # > agent's MAX_HOLD (120 s), see RELIABILITY.md §1
ON_DEADLINE=${GUARD_ON_DEADLINE:-45}  # same as wifi_radio.rs ON_DEADLINE
WAKE_GAP=${GUARD_WAKE_GAP:-$((2 * INTERVAL + 30))} # rounds this far apart = the device was asleep
BACKOFF_MAX=480                       # restore retries: 60, 120, 240, 480, 480, …
CLOCK_SANE_AFTER=1704067200           # 2024-01-01; before NTP the RTC says ~1971

# ── paths and commands ──────────────────────────────────────────────────────
HEARTBEAT=${GUARD_HEARTBEAT:-/tmp/scenario.heartbeat}
MARKER=${GUARD_MARKER:-/tmp/u60-wifiguard.took-over}
WIFI_LOCK=${GUARD_WIFI_LOCK:-/tmp/u60-wifi.lock}
STATE=${GUARD_STATE:-/tmp/u60-guard}
LOG=${GUARD_LOG:-/tmp/u60-guard.log}
UPTIME_FILE=${GUARD_UPTIME_FILE:-/proc/uptime}
PROC=${GUARD_PROC:-/proc}
RTC=${GUARD_RTC:-/sys/class/rtc/rtc0}
UCI=${GUARD_UCI:-uci}
UBUS=${GUARD_UBUS:-ubus}
PS=${GUARD_PS:-ps w}
SLEEP=${GUARD_SLEEP:-sleep}
KILL=${GUARD_KILL:-kill}
JSONFILTER=${GUARD_JSONFILTER:-jsonfilter}

TAB=$(printf '\t')

# ── helpers ─────────────────────────────────────────────────────────────────

log() {
    if [ -f "$LOG" ] && [ "$(wc -c <"$LOG")" -gt 262144 ]; then
        tail -n 500 "$LOG" >"$LOG.tmp" && mv "$LOG.tmp" "$LOG"
    fi
    echo "$(date '+%Y-%m-%dT%H:%M:%S') up=$(uptime_s) $*" >>"$LOG"
}

uptime_s() { cut -d. -f1 "$UPTIME_FILE" 2>/dev/null || echo 0; }
wall_s() { echo "${GUARD_WALL_NOW:-$(date +%s)}"; }

num() { # num <file> — the number in it, or 0
    _n=$(cat "$1" 2>/dev/null)
    case "$_n" in '' | *[!0-9]*) echo 0 ;; *) echo "$_n" ;; esac
}

# Same test as wifi_radio.rs hostapd_count: hostapd emits the beacons, so zero
# processes is positive proof nothing is broadcasting.
hostapd_count() {
    $PS 2>/dev/null | awk '/\/hostapd( |$)/ && !/awk/ {n++} END {print n+0}'
}

# ── Wi-Fi lock (RELIABILITY.md §1) ──────────────────────────────────────────
# busybox flock has no -w, so poll -n. If the agent has held the lock longer
# than it ever legitimately does, it is wedged: kill it (procd restarts it)
# and take the lock. Anything else holding it is left alone.

lock_wifi() {
    exec 9>>"$WIFI_LOCK"
    _waited=0
    while ! flock -n 9; do
        if [ "$_waited" -ge "$LOCK_WAIT" ]; then
            _pid=$(num "$WIFI_LOCK")
            _comm=$(cat "$PROC/$_pid/comm" 2>/dev/null)
            case "$_comm" in
                zte-agent*)
                    log "Wi-Fi lock held ${_waited}s by $_comm ($_pid): killing it"
                    alert_add agent-hung "held the Wi-Fi lock over ${LOCK_WAIT}s; killed"
                    $KILL -9 "$_pid" 2>/dev/null
                    $SLEEP 1
                    flock -n 9 && break
                    ;;
            esac
            log "Wi-Fi lock still held (pid=$_pid comm=${_comm:-?}); giving up this round"
            exec 9>&-
            return 1
        fi
        $SLEEP 2
        _waited=$((_waited + 2))
    done
    echo $$ >"$WIFI_LOCK"
    return 0
}

unlock_wifi() { exec 9>&-; }

# Radios AND AP interfaces: the agent's scenario engine only toggles the APs,
# but the admin UI can switch a whole radio off, and turning the APs on under
# a disabled radio would be retried forever to no effect.
restore_wifi() {
    lock_wifi || return 1
    $UCI set wireless.wifi0.disabled=0
    $UCI set wireless.wifi1.disabled=0
    $UCI set wireless.main_2g.disabled=0
    $UCI set wireless.main_5g.disabled=0
    $UCI commit wireless
    $UBUS call zwrt_wlan reload >/dev/null 2>&1
    # Poll, never assume: the same reload has taken 8 s once and done nothing
    # for a full minute another time.
    _t=0
    _ok=1
    while :; do
        if [ "$(hostapd_count)" -gt 0 ]; then
            _ok=0
            break
        fi
        [ "$_t" -ge "$ON_DEADLINE" ] && break
        $SLEEP 3
        _t=$((_t + 3))
    done
    unlock_wifi
    return $_ok
}

# ── sleep survival ──────────────────────────────────────────────────────────

set_autosleep() { # set_autosleep true|false
    $UBUS call zwrt_zte_sleep_faw.wakelock enableAutoSleep "{\"switch\":$1}" >/dev/null 2>&1
}

# While the APs are down, make sure the RTC will wake the device within five
# minutes so this script gets to run. Never pushes an earlier alarm out — the
# engine arms its own, and whichever is sooner should win.
arm_rtc() {
    _base=$(num "$RTC/since_epoch")
    [ "$_base" -gt 0 ] || return 0
    _want=$((_base + 300))
    _cur=$(num "$RTC/wakealarm")
    if [ "$_cur" -le "$_base" ] || [ "$_cur" -gt "$_want" ]; then
        echo 0 >"$RTC/wakealarm" 2>/dev/null
        echo "$_want" >"$RTC/wakealarm" 2>/dev/null
    fi
}

# ── the watchdog round ──────────────────────────────────────────────────────
#
# State in $STATE (tmpfs, so a reboot starts clean):
#   seen        a heartbeat has been observed this boot
#   episode     uptime when the current outage was first noticed
#   took-over   the marker has been written for this episode (write it ONCE:
#               the agent times its 10-minute hand-back from the content)
#   next-try    uptime before which no restore is attempted (backoff)
#   backoff     seconds to wait after the next failure
#   sleep-held  we switched auto-sleep off and owe it back
#   last-round  uptime of the previous round
#   woke        uptime at which we noticed the device had been asleep
#   alerted-*   one-per-episode alert flags

end_episode() {
    log "agent heartbeat is back; outage over"
    # We held sleep off. The agent now sits in away until it releases the
    # takeover, and away lets the device sleep — so give that back.
    [ -f "$STATE/sleep-held" ] && set_autosleep true
    rm -f "$STATE/sleep-held" "$STATE/episode" "$STATE/took-over" "$STATE/next-try" "$STATE/backoff" "$STATE"/alerted-*
}

alert_once() { # alert_once <flag> <kind> <text…>
    _flag=$1
    shift
    [ -f "$STATE/alerted-$_flag" ] && return 0
    touch "$STATE/alerted-$_flag"
    alert_add "$@"
}

guard_round() {
    mkdir -p "$STATE"
    _now=$(uptime_s)

    # /proc/uptime keeps counting through suspend, but neither our sleep nor
    # the agent's does. After a 20-minute sleep the last heartbeat looks 20
    # minutes old although the agent is fine and simply has not had its turn
    # yet. Rounds much further apart than INTERVAL mean we were asleep: from
    # then on the agent gets a full STALE window before it counts as gone.
    _prev=$(num "$STATE/last-round")
    echo "$_now" >"$STATE/last-round"
    if [ "$_prev" -gt 0 ] && [ $((_now - _prev)) -gt "$WAKE_GAP" ]; then
        echo "$_now" >"$STATE/woke"
        log "device was asleep ~$((_now - _prev))s; giving the agent ${STALE}s to check in"
    fi

    [ "$_now" -lt "$GRACE" ] && return 0

    _hb=$(num "$HEARTBEAT")
    [ "$_hb" -gt 0 ] && touch "$STATE/seen"

    if [ -f "$STATE/seen" ]; then
        if [ "$_hb" -gt 0 ] && [ $((_now - _hb)) -le "$STALE" ]; then
            [ -f "$STATE/episode" ] && end_episode
            return 0
        fi
        # Just woke and no outage was already under way: wait, don't judge.
        _woke=$(num "$STATE/woke")
        if [ ! -f "$STATE/episode" ] && [ "$_woke" -gt 0 ] && [ $((_now - _woke)) -le "$STALE" ]; then
            return 0
        fi
        _why="no heartbeat for $((_now - _hb))s"
        [ "$_hb" -gt 0 ] || _why="heartbeat file gone"
    else
        # Never seen one. Early on that is normal (the agent is still coming
        # up); well after boot it means the agent never started at all.
        [ "$_now" -lt "$NEVER_SEEN" ] && return 0
        _why="no heartbeat since boot"
    fi

    if [ ! -f "$STATE/episode" ]; then
        echo "$_now" >"$STATE/episode"
        log "agent looks gone: $_why"
    fi
    alert_once silent agent-silent "$_why"

    _aps=$(hostapd_count)
    if [ "$_aps" -gt 0 ]; then
        return 0 # Wi-Fi is up; nothing to rescue
    fi

    # Agent gone and nothing broadcasting. From here the device must not
    # suspend, and Wi-Fi comes back on — over any "AP off" the user may have
    # set by hand: with the agent dead there is no way to tell the two apart,
    # and a phone stranded without network is the worse mistake.
    set_autosleep false
    touch "$STATE/sleep-held"
    arm_rtc
    if [ ! -f "$STATE/took-over" ]; then
        echo "$_now" >"$MARKER"
        touch "$STATE/took-over"
        log "taking over Wi-Fi"
        alert_add wifi-takeover "agent gone ($_why), Wi-Fi was off; turning it on"
    fi

    [ "$_now" -lt "$(num "$STATE/next-try")" ] && return 0

    if restore_wifi; then
        log "Wi-Fi restored"
        rm -f "$STATE/next-try" "$STATE/backoff"
        return 0
    fi
    _b=$(num "$STATE/backoff")
    [ "$_b" -gt 0 ] || _b=60
    echo $(($(uptime_s) + _b)) >"$STATE/next-try"
    _nb=$((_b * 2))
    [ "$_nb" -gt "$BACKOFF_MAX" ] && _nb=$BACKOFF_MAX
    echo "$_nb" >"$STATE/backoff"
    log "Wi-Fi restore failed; next try in ${_b}s"
    alert_once restore wifi-restore-failed "could not turn Wi-Fi on; retrying"
}

# Keep an eye on the RTC whenever the APs are down, even with the agent
# alive: if the agent dies while the device is suspended, only a wake gets
# this script running again.
rtc_round() {
    [ "$(hostapd_count)" -eq 0 ] && arm_rtc
}

# ── alert SMS (RELIABILITY.md §4, 短信) ─────────────────────────────────────
# Only this script sends alert SMS. It is the only writer of sms-log and
# sms-done, so neither needs the directory lock; the queue is replaced whole
# when trimmed, so reading it unlocked is safe too.

valid_number() {
    case "$1" in
        '' | +) return 1 ;;
        +*) _d=${1#+} ;;
        *) _d=$1 ;;
    esac
    case "$_d" in *[!0-9]*) return 1 ;; esac
    [ "${#1}" -le 20 ] && [ "${#_d}" -ge 3 ]
}

sms_log() { # sms_log <wall> <seq> <kind> <result>
    printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" >>"$ALERT_DIR/sms-log"
    if [ "$(wc -l <"$ALERT_DIR/sms-log")" -gt 100 ]; then
        tail -n 100 "$ALERT_DIR/sms-log" >"$ALERT_DIR/.sms-log.tmp" &&
            mv "$ALERT_DIR/.sms-log.tmp" "$ALERT_DIR/sms-log"
    fi
}

# rate_ok <kind> <now> — at most 1 per kind per hour, 5 in 24 h, counting only
# what was actually sent. A record stamped in the future (the clock jumped
# back) counts as just sent: the safe reading.
rate_ok() {
    [ -f "$ALERT_DIR/sms-log" ] || return 0
    awk -F'\t' -v k="$1" -v now="$2" '
        $4 == "sent" {
            if ($1 > now || $1 > now - 86400) day++
            if ($3 == k && ($1 > now || $1 > now - 3600)) hour++
        }
        END { exit (hour >= 1 || day >= 5) ? 1 : 0 }
    ' "$ALERT_DIR/sms-log"
}

# UTF-8 in, UCS-2 big-endian hex out (uppercase), as sms_forward.rs encodes it.
# The messages are Chinese, so this decodes UTF-8 itself: busybox has no iconv.
# Characters outside the BMP (emoji) become U+FFFD — none of ours use them.
ucs2_hex() {
    printf '%s' "$1" | od -An -tu1 -v | awk '
        { for (i = 1; i <= NF; i++) b[n++] = $i }
        END {
            i = 0
            while (i < n) {
                c = b[i]
                if (c < 128)      { cp = c; i += 1 }
                else if (c < 224) { cp = (c - 192) * 64 + (b[i + 1] - 128); i += 2 }
                else if (c < 240) { cp = (c - 224) * 4096 + (b[i + 1] - 128) * 64 + (b[i + 2] - 128); i += 3 }
                else              { cp = 65533; i += 4 }
                printf "%04X", cp
            }
        }'
}

# What the owner reads on their phone. Plain Chinese, one SMS (<= 70 chars):
# what happened, whether it affects getting online, and whether to do anything.
# The event's technical text stays in the web page's alert list.
sms_body() { # <kind> <device-local time>
    case "$1" in
        wifi-takeover) m="管理后台没反应了。为了让你能上网，已自动打开U60的Wi-Fi。不用管。" ;;
        wifi-restore-failed) m="想自动打开U60的Wi-Fi没成功，还在重试。如果手机连不上U60，请重启它。" ;;
        agent-silent) m="管理后台超过5分钟没反应。上网一般不受影响；Wi-Fi要是关着，会自动打开。" ;;
        agent-hung) m="管理后台卡住了，已强制重启。不用管。" ;;
        agent-crash) m="管理后台意外退出，已自动重启。不用管。" ;;
        datad-crash) m="屏幕的数据服务意外退出，已自动重启。不用管。" ;;
        devui-crash) m="屏幕界面闪退了，已自动重新打开。不用管。" ;;
        devui-gave-up) m="屏幕界面连续打不开，已换成原厂界面，上网不受影响。长按屏幕右下角3秒可换回。" ;;
        sms-test) m="这是测试短信。收到了，说明告警短信能正常发到你手机。" ;;
        *) m="有一条新告警（$1），请到管理网页「系统→告警」查看。" ;;
    esac
    printf '【U60】%s（%s）' "$m" "$2"
}

# ZTE's "YY;MM;DD;HH;MM;SS;+TZ", TZ in whole hours as in sms_forward.rs.
# The clock already reads local wall time but the system zone says UTC (ZTE's
# SNTP sets it that way — zte-agent clock.rs), so `date +%z` would claim +0.
# The real zone is ZTE's own SNTP setting ("8.00").
sms_time() {
    _tz=$($UCI -q get zwrt_zte_sntp.settings.time_from_utc 2>/dev/null)
    case "$_tz" in
        -*) _sign=-; _tz=${_tz#-} ;;
        *) _sign=+ ;;
    esac
    _tz=${_tz%%.*}
    case "$_tz" in '' | *[!0-9]*) _tz=0; _sign=+ ;; esac
    printf '%s;%s%s' "$(date '+%y;%m;%d;%H;%M;%S')" "$_sign" "$((_tz + 0))"
}

send_sms() { # send_sms <number> <text> — 0 on success
    _json=$(printf '{"number":"%s","message_body":"%s","encode_type":"UNICODE","sms_time":"%s","id":"-1"}' \
        "$1" "$(ucs2_hex "$2")" "$(sms_time)")
    _resp=$($UBUS call zwrt_wms zte_libwms_send_sms "$_json" 2>&1) || {
        log "sms: ubus failed: $_resp"
        return 1
    }
    _res=$(printf '%s' "$_resp" | $JSONFILTER -e '@.result' 2>/dev/null)
    case "$_res" in
        '' | 3) return 0 ;;
        *)
            log "sms: device rejected (result=$_res)"
            return 1
            ;;
    esac
}

# The firmware keeps every SMS we send in its "sent" box, where it shows up in
# the device's SMS list (the touch screen, the web page). The owner does not
# want alerts piling up there: once one is sent, delete that copy — the newest
# sent message to that number, as sms_forward.rs does for forwarded SMS.
delete_sent_copy() { # <number>
    sleep 1   # let the firmware store it
    for _store in 1 0; do
        _id=$($UBUS call zwrt_wms zte_libwms_get_sms_data \
            "{\"tags\":2,\"page\":0,\"data_per_page\":5,\"mem_store\":$_store,\"order_by\":\"order by id desc\"}" 2>/dev/null |
            $JSONFILTER -e "@.messages[@.number='$1'].id" 2>/dev/null | head -n 1)
        case "$_id" in '' | *[!0-9]*) continue ;; esac
        $UBUS call zwrt_wms zwrt_wms_delete_sms "{\"id\":\"$_id\"}" >/dev/null 2>&1 &&
            log "sms: deleted the sent copy (id $_id)"
        return 0
    done
    log "sms: sent copy not found to delete"
}

sms_round() {
    [ -f "$ALERT_DIR/queue" ] || return 0
    _done=$(num "$ALERT_DIR/sms-done")
    _number=$(cat "$ALERT_DIR/sms-to" 2>/dev/null | tr -d ' \r\n')
    _pending="$STATE/sms-pending"
    mkdir -p "$STATE"
    awk -F'\t' -v d="$_done" 'NF == 5 && $1 ~ /^[0-9]+$/ && $1 + 0 > d + 0' "$ALERT_DIR/queue" >"$_pending"

    while IFS="$TAB" read -r _seq _wall _up _kind _text; do
        _w=$(wall_s)
        if [ "$_kind" = sms-failed ]; then
            : # never an SMS about a failed SMS
        elif ! valid_number "$_number"; then
            sms_log "$_w" "$_seq" "$_kind" no-number
        elif [ -f "$ALERT_DIR/abroad" ] && [ ! -f "$ALERT_DIR/sms-abroad" ]; then
            sms_log "$_w" "$_seq" "$_kind" suppressed-abroad
        elif [ "$_w" -lt "$CLOCK_SANE_AFTER" ]; then
            break # rate limits need a real clock; leave it pending, not done
        elif ! rate_ok "$_kind" "$_w"; then
            sms_log "$_w" "$_seq" "$_kind" suppressed-rate
        elif send_sms "$_number" "$(sms_body "$_kind" "$(date '+%m-%d %H:%M')")"; then
            sms_log "$_w" "$_seq" "$_kind" sent
            delete_sent_copy "$_number"
        else
            sms_log "$_w" "$_seq" "$_kind" failed
            alert_add sms-failed "alert $_seq ($_kind) not sent"
        fi
        echo "$_seq" >"$ALERT_DIR/.sms-done.tmp" && mv "$ALERT_DIR/.sms-done.tmp" "$ALERT_DIR/sms-done"
    done <"$_pending"
    rm -f "$_pending"
}

# ── main ────────────────────────────────────────────────────────────────────

round() {
    guard_round
    rtc_round
    sms_round
}

case "$1" in
    once)
        round
        ;;
    *)
        log "u60-guard starting (pid $$)"
        while :; do
            round
            $SLEEP "$INTERVAL"
        done
        ;;
esac
