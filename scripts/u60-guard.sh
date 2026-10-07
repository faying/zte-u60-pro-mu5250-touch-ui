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
# zte-agent/src/wifi_radio.rs `apply(true, true)` — keep the two in step. Only
# the guard also turns the vendor master switch back on (vendor_wifi_on).
#
#   u60-guard.sh          loop forever (procd)
#   u60-guard.sh once     one round, then exit (tests only: runs real round actions)
#   u60-guard.sh crashcap start this boot's kernel-log capture only
#   u60-guard.sh started  write the "guard came up" ledger mark only (tests)
#   u60-guard.sh watcher  the crash watcher loop (the main loop starts it)
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
# Clock trust (docs/LEDGER.md §7). Before NTP the device clock reads 2025-01-04,
# so a fixed "after 2024" threshold proved nothing: the year must be 2026 or
# later AND there must be evidence the clock was set.
CLOCK_YEAR_MIN=${GUARD_CLOCK_YEAR_MIN:-1767225600} # 2026-01-01 00:00
SNTP_OK_CMD=${GUARD_SNTP_OK_CMD:-}    # vendor "time synced" probe; empty until verified on the device
WALL_LAST=${GUARD_WALL_LAST:-/data/ledger/state/wall.last}

# ── paths and commands ──────────────────────────────────────────────────────
HEARTBEAT=${GUARD_HEARTBEAT:-/tmp/scenario.heartbeat}
MARKER=${GUARD_MARKER:-/tmp/u60-wifiguard.took-over}
WIFI_LOCK=${GUARD_WIFI_LOCK:-/tmp/u60-wifi.lock}
STATE=${GUARD_STATE:-/tmp/u60-guard}
LTMP=${GUARD_LEDGER_TMP:-$STATE/ledger} # this boot's ledger/sentinel state (docs/LEDGER.md §2)
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
# Logs that nothing else rotates. tailscaled writes ~7.5 MB/day to its log and
# runs for weeks; start.sh opens it for append, so copy-then-truncate is safe.
CAP_LOGS=${GUARD_CAP_LOGS:-/data/tailscaled.log}
CAP_BYTES=${GUARD_CAP_BYTES:-1048576}
# The same for logs in /tmp (RAM), trimmed otherwise only when their writer
# starts: agent and datad output (supervise.sh opens it with >>) and the touch
# UI's (u60-uid opens it O_APPEND). Smaller cap: the .old copy is RAM too.
# /tmp/u60-uid.log (u60-uid opens it O_APPEND for each line) is only the
# backstop here: the ledger job reads it by line and renames it at
# UID_LOG_CAP (ledger_uid), so this cap is reached only when the ledger is off.
CAP_TMP_LOGS=${GUARD_CAP_TMP_LOGS:-/tmp/zte-agent.log /tmp/zwrt-datad.log /tmp/u60pro-devui.log /tmp/u60-uid.log}
CAP_TMP_BYTES=${GUARD_CAP_TMP_BYTES:-262144}
# Standby sentinel records (read by doctor.sh; see docs/RELIABILITY.md §8)
STANDBY_STAT=${GUARD_STANDBY_STAT:-/tmp/standby.stat}
NETDEV=${GUARD_NETDEV:-/proc/net/dev}
BACKLIGHT=${GUARD_BACKLIGHT:-/sys/class/leds/led:lcd/brightness}
WAN_IF=${GUARD_WAN_IF:-rmnet_data0}
TS_IF=${GUARD_TS_IF:-tailscale0}
STANDBY_PROGS="tailscaled u60pro-devui zwrt-datad zte-agent"
# LAN IPv6 off (owner's choice, 2026-09-24): while this flag file exists,
# IPv6 stays disabled on the LAN bridge. With no IPv6 on br-lan, odhcpd has
# nothing to advertise and clients get no IPv6 at all — the only way that
# survives zte_router, which rewrites dhcp.lan's RA settings whenever the
# cellular IPv6 comes up. Delete the flag file to give the LAN IPv6 back
# (then: echo 0 > /proc/sys/net/ipv6/conf/br-lan/disable_ipv6).
LAN_V6_FLAG=${GUARD_LAN_V6_FLAG:-/data/u60-guard/lan-ipv6-off}
LAN_V6_SYSCTL=${GUARD_LAN_V6_SYSCTL:-/proc/sys/net/ipv6/conf/br-lan/disable_ipv6}
# The touch screen's settings file (src/ui.c DEVUI_CONF_FILE). Only its
# lang= line is read here: lang=en sends the alert SMS in English.
DEVUI_CONF=${GUARD_DEVUI_CONF:-/data/plugins/u60pro-devui/devui.conf}
# Modem crash recovery (owner's choice, 2026-09-26): the firmware boots with
# remoteproc recovery disabled, so any modem assert (e.g. the n77/n78 PA-cal
# assert rf_nr5g_sub6_tx.c:5428 seen in Japan) reboots the whole device.
# Enabled, only the modem restarts (~5 s without data). Checked every round.
MSS_RECOVERY=${GUARD_MSS_RECOVERY:-/sys/class/remoteproc/remoteproc0/recovery}
# Kernel log to flash (2026-09-26). The firmware keeps no persistent kernel
# log, so a crash-reboot erases the one line that says why (the modem assert
# above was only found by copying /dev/kmsg to flash by hand). One reader per
# boot appends /dev/kmsg, minus the audit spam, to $CRASHCAP_DIR/kmsg-<boot>.log
# and fsyncs that file every 2 s (when it ends, crashcap_keep starts it again,
# a bounded number of times); the newest $CRASHCAP_KEEP boots (in boot
# order, crashcap_prune) are kept and
# a file over $CRASHCAP_MAX is cut to its second half. Nothing happens when
# $KMSG does not exist (tests).
CRASHCAP_DIR=${GUARD_CRASHCAP_DIR:-/data/crashcap}
CRASHCAP_KEEP=${GUARD_CRASHCAP_KEEP:-5}
CRASHCAP_MAX=${GUARD_CRASHCAP_MAX:-8388608}
KMSG=${GUARD_KMSG:-/dev/kmsg}
CRASHCAP_RESTARTS=${GUARD_CRASHCAP_RESTARTS:-5}  # reader ended: started again at most this many times a boot (crashcap_keep)
CRASHCAP_RESPAWN=${GUARD_CRASHCAP_RESPAWN:-300} # and not sooner than this after its last start
BOOT_ID_FILE=${GUARD_BOOT_ID_FILE:-/proc/sys/kernel/random/boot_id}
# Crash watcher (docs/LEDGER.md §9): a second loop that reads the capture file
# above every WATCH_INTERVAL s for modem crashes and how the link came back,
# dump files, out-of-memory kills, thermal throttling and suspend lines.
WATCHER=${GUARD_WATCHER:-1}              # 0 = do not run it
WATCH_INTERVAL=${GUARD_WATCH_INTERVAL:-2}
WATCH_GAP=${GUARD_WATCH_GAP:-10}         # loops further apart than this: a pause (asleep, or stalled)
WATCH_SLEEP=${GUARD_WATCH_SLEEP:-sleep}
WATCH_MAX=${GUARD_WATCH_MAX:-}           # tests: stop after this many loops
DUMP_DIR=${GUARD_DUMP_DIR:-/data/vendor/ramdump}
ROUTE=${GUARD_ROUTE:-/proc/net/route}
IP=${GUARD_IP:-ip}
SUSPEND_STATS=${GUARD_SUSPEND_STATS:-/sys/power/suspend_stats/success}
# Data-service degraded marker (zte-agent datad_feed.rs is its only writer and
# remover; we only read it). Line 1 = epoch when the agent fell back, line 2 =
# reason. The agent rewrites it every 60 s while degraded, so an old mtime
# means the agent itself stopped — that is the heartbeat alert's job, not ours.
DATAD_MARKER=${GUARD_DATAD_MARKER:-/data/u60-guard/datad-degraded}
DATAD_FRESH=${GUARD_DATAD_FRESH:-180}  # marker mtime younger than this = agent still vouching for it
DATAD_AFTER=${GUARD_DATAD_AFTER:-300}  # degraded this long before we text the owner
# The ledger (docs/LEDGER.md): an append-only event log that survives reboots.
# Only the background ledger job writes it; everything else leaves spool files.
LEDGER_DIR=${GUARD_LEDGER_DIR:-/data/ledger}
SPOOL_TMP=${GUARD_SPOOL_TMP:-/tmp/ledger-spool}
LEDGER_SEG_MAX=${GUARD_LEDGER_SEG_MAX:-262144}     # bytes per segment file
LEDGER_TOTAL_MAX=${GUARD_LEDGER_TOTAL_MAX:-16777216} # all segments + spool + state
LEDGER_DAY_MAX=${GUARD_LEDGER_DAY_MAX:-1048576}   # bytes written per day before only critical events go in
LEDGER_FREE_MIN=${GUARD_LEDGER_FREE_MIN:-102400}  # KB free on /data below which only boot lines go in
DF=${GUARD_DF:-df}
KEYLOG=${GUARD_KEYLOG:-/data/logfs/key.log}       # the firmware's own log: one reboot_reason_code= per boot
CRASHLOG_DIR=${GUARD_CRASHLOG_DIR:-/data/crashlog} # supervise.sh and u60-uid leave <program>/<time>-up<N>.log
MC_TMP=${GUARD_MC_TMP:-/etc/config/zwrt_zte_mc_tmp} # boot mode (power on / charger), as u60-guard.init reads it
STOP_REQ=${GUARD_STOP_REQ:-$STATE/stop-requested}  # "<who> <why>": the next guard start was asked for
FSYNC_LOG=${GUARD_FSYNC_LOG:-}
UID_LOG=${GUARD_UID_LOG:-/tmp/u60-uid.log}         # u60-uid's own log: hand-backs and give-ups
UID_LOG_CAP=${GUARD_UID_LOG_CAP:-65536}             # read and over this: renamed to .old by the ledger job (ledger_uid)
DATAD_URL=${GUARD_DATAD_URL:-http://127.0.0.1:9460/v2/state} # the loopback listener needs no token
DATAD_CONTROL=${GUARD_DATAD_CONTROL:-http://127.0.0.1:9460/control} # writes go through datad when it is there (E4 T7c)
WGET=${GUARD_WGET:-wget}
LEDGER_READ_BUDGET=${GUARD_LEDGER_READ_BUDGET:-3} # seconds of outside reads per ledger round (§6)
# What the hourly summary reads (docs/LEDGER.md §4 hour, hour_power, hour_proc)
BAT=${GUARD_BAT:-/sys/class/power_supply/battery}
THERMAL=${GUARD_THERMAL:-/sys/class/thermal}
CPUFREQ=${GUARD_CPUFREQ:-/sys/devices/system/cpu/cpufreq}
STANDBY_BASE=${GUARD_STANDBY_BASE:-/data/u60-guard/standby.baseline} # doctor.sh --calibrate-standby writes it
RC_LOCAL=${GUARD_RC_LOCAL:-/etc/rc.local}
TS_START=${GUARD_TS_START:-/data/tailscale/start.sh} # Tailscale is meant to run when rc.local starts it
TS_SOCK=${GUARD_TS_SOCK:-/tmp/tailscaled.sock}      # tailscaled's LocalAPI, where start.sh puts it
CURL=${GUARD_CURL:-curl}                            # the only tool here that speaks HTTP over a unix socket

# u60 ship (docs/SHIP.md): a transaction whose executor died (OOM kill -9) or
# hangs is finished by `u60-ship.sh recover-live`; this guard only notices.
SHIP=${GUARD_SHIP:-$HERE/u60-ship.sh}
SHIP_TXN=${GUARD_SHIP_TXN:-/data/u60-ship/txn}
SHIP_HB=${GUARD_SHIP_HB:-/tmp/u60-ship/heartbeat}
SHIP_STALE=${GUARD_SHIP_STALE:-30}      # executor heartbeat older than this = dead or stuck
SHIP_STAGED=${GUARD_SHIP_STAGED:-300}   # staged and never started for this long = the Mac went away

TAB=$(printf '\t')

# ── helpers ─────────────────────────────────────────────────────────────────

log() {
    if [ -f "$LOG" ] && [ "$(wc -c <"$LOG")" -gt 262144 ]; then
        tail -n 500 "$LOG" >"$LOG.tmp" && mv "$LOG.tmp" "$LOG"
    fi
    echo "$(date '+%Y-%m-%dT%H:%M:%S') up=$(uptime_s) $*" >>"$LOG"
}

uptime_s() { # whole seconds; 0 rather than nothing, so no arithmetic ever sees an empty value
    _us=$(cut -d. -f1 "$UPTIME_FILE" 2>/dev/null)
    case "$_us" in '' | *[!0-9]*) echo 0 ;; *) echo "$_us" ;; esac
}
wall_s() { # the device clock; tests fix it (GUARD_WALL_NOW) or move it along with a file (GUARD_WALL_FILE)
    if [ -n "$GUARD_WALL_FILE" ]; then cat "$GUARD_WALL_FILE"; else echo "${GUARD_WALL_NOW:-$(date +%s)}"; fi
}

num() { # num <file> — the number in it, or 0
    _n=$(cat "$1" 2>/dev/null)
    case "$_n" in '' | *[!0-9]*) echo 0 ;; *) echo "$_n" ;; esac
}

# Same test as wifi_radio.rs hostapd_count: hostapd emits the beacons, so zero
# processes is positive proof nothing is broadcasting.
hostapd_count() {
    $PS 2>/dev/null | awk '/\/hostapd( |$)/ && !/awk/ {n++} END {print n+0}'
}

# ── clock trust (docs/LEDGER.md §7) ─────────────────────────────────────────
# One answer to "can we believe the wall clock", shared by the SMS rate limits,
# the datad-degraded timer, the ledger and doctor.sh (which reads the file):
#   $STATE/clock-ok = <1|0> <wall - uptime> <uptime when written> <how>
# how: vendor | jump (the offset leapt forward over a day this boot) |
#      last_wall (not earlier than the last trusted wall clock minus a day) |
#      year (first deployment, no last trusted wall clock yet) | revoked | none
# A trusted clock that later falls back over a day is revoked until the
# evidence holds again. Runs in a subshell: it only leaves the file behind.
clock_round() {
    mkdir -p "$STATE"
    _up=$(uptime_s)
    case "$_up" in '' | *[!0-9]*) _up=0 ;; esac
    _w=$(wall_s)
    case "$_w" in '' | *[!0-9]*) _w=0 ;; esac
    _off=$((_w - _up))
    _first=$(num "$STATE/clock.first")
    if [ "$_first" -eq 0 ]; then
        echo "$_off" >"$STATE/clock.first"
        _first=$_off
    fi
    [ $((_off - _first)) -gt 86400 ] && touch "$STATE/clock.jump"
    _was=
    _woff=
    { read -r _was _woff _rest <"$STATE/clock-ok"; } 2>/dev/null
    case "$_woff" in '' | *[!0-9]*) _woff= ;; esac
    _ok=0
    _how=none
    if [ "$_was" = 1 ] && [ -n "$_woff" ] && [ "$_off" -lt $((_woff - 86400)) ]; then
        echo "$_off" >"$STATE/clock.first" # a new forward leap is needed now
        rm -f "$STATE/clock.jump"
        _how=revoked
    elif [ "$_w" -ge "$CLOCK_YEAR_MIN" ]; then
        _last=$(num "$WALL_LAST")
        if [ -n "$SNTP_OK_CMD" ] && $SNTP_OK_CMD >/dev/null 2>&1; then
            _ok=1
            _how=vendor
        elif [ -f "$STATE/clock.jump" ]; then
            _ok=1
            _how=jump
        elif [ "$_last" -gt 0 ]; then
            [ "$_w" -ge $((_last - 86400)) ] && _ok=1 && _how=last_wall
        else
            _ok=1
            _how=year
        fi
    fi
    printf '%s %s %s %s\n' "$_ok" "$_off" "$_up" "$_how" >"$STATE/clock-ok.tmp" &&
        mv -f "$STATE/clock-ok.tmp" "$STATE/clock-ok"
}

clock_ok() {
    _c=
    { read -r _c _rest <"$STATE/clock-ok"; } 2>/dev/null
    [ "$_c" = 1 ]
}

# ── Wi-Fi lock (RELIABILITY.md §1) ──────────────────────────────────────────
# busybox flock has no -w, so poll -n. If the agent has held the lock longer
# than it ever legitimately does, it is wedged: kill it (procd restarts it)
# and take the lock. Anything else holding it is left alone.

# via_datad <request-json>: the write through datad (source "guard"; datad is
# the one writer, write-op-layer.md E4). 0 = done, 1 = datad answered no or did
# not answer in time (alive but stuck: no direct write either, D18; its
# watchdog restarts it), 2 = nothing listening, or a datad too old to know the
# action: the caller writes directly, as before E4 (the guard is the rescue
# when everything else is gone).
# datad's reply on stdout.
via_datad() {
    _vd=$($CURL -s -m 30 -H 'Content-Type: application/json' --data-binary "$1" "$DATAD_CONTROL" 2>/dev/null)
    _vrc=$?
    [ "$_vrc" = 7 ] && return 2
    [ "$_vrc" = 0 ] || return 1
    printf '%s' "$_vd"
    case $_vd in
        *'"ok":true'*) return 0 ;;
        # a datad from before E4 T7 does not know these actions: write directly
        # as before (the guard may be shipped ahead of datad)
        *'"unknown_action"'*) return 2 ;;
    esac
    return 1
}

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

# The vendor's own master switch, `wireless.zte_mbb.wifi_onoff` (B31; what the
# stock web page and the stock touch screen turn off). While it reads "0" the
# APs stay down whatever their `disabled` says, so the restore below would be
# retried forever. Only "0" is off (missing = on, as zte-agent wifi.rs reads
# it). Switched on the stock way: `zwrt_wlan set {"zte_mbb":{"wifi_onoff":"1",
# "lbd":<as it stands>}}` (the stock web page sends the band-steering flag
# along so switching on keeps it; left out when it doesn't read 0 or 1).
# Through datad's `wifi.set_module` (journaled, like every guard write);
# directly only when no datad is listening — that body is data-service
# control.rs wifi_module_args, keep the two in step.
# DEPLOY ORDER: the guard must not go on a device whose datad predates the
# stock-format wifi.set_module (that one sends a flat {"SwitchOption":"1"}).
# A refusal or failure here is logged and the restore goes on: the hostapd
# poll is the judge. Called with the Wi-Fi lock held (it writes `wireless`).
vendor_wifi_on() {
    _onoff=$($UCI -q get wireless.zte_mbb.wifi_onoff 2>/dev/null)
    [ "$_onoff" = 0 ] || return 0
    log "vendor Wi-Fi switch (wireless.zte_mbb.wifi_onoff) is off; turning it on"
    via_datad '{"action":"wifi.set_module","source":"guard","params":{"enabled":1}}' >/dev/null
    case $? in
        0) log "vendor Wi-Fi switch on asked through datad" ;;
        1) log "datad refused or did not answer the vendor Wi-Fi switch; restoring the APs anyway" ;;
        *)
            _lbd=$($UCI -q get wireless.zte_mbb.lbd 2>/dev/null)
            case "$_lbd" in
                0 | 1) _lbd=",\"lbd\":\"$_lbd\"" ;;
                *) _lbd= ;;
            esac
            $UBUS call zwrt_wlan set "{\"zte_mbb\":{\"wifi_onoff\":\"1\"$_lbd}}" >/dev/null 2>&1
            ;;
    esac
    return 0
}

# Radios AND AP interfaces: the agent's scenario engine only toggles the APs,
# but the admin UI can switch a whole radio off, and turning the APs on under
# a disabled radio would be retried forever to no effect. The vendor master
# switch first (vendor_wifi_on): with the agent gone there is no telling a
# stock-UI "Wi-Fi off" from any other, and the takeover overrides them all.
restore_wifi() {
    lock_wifi || return 1
    vendor_wifi_on
    via_datad '{"action":"wifi.apply","source":"guard","params":{"set":{"wireless.wifi0.disabled":"0","wireless.wifi1.disabled":"0","wireless.main_2g.disabled":"0","wireless.main_5g.disabled":"0"},"reload":true}}' >/dev/null
    case $? in
        0) log "Wi-Fi on asked through datad" ;;
        1)
            log "datad refused or did not answer the Wi-Fi restore; next round"
            unlock_wifi
            return 1
            ;;
        *)
            $UCI set wireless.wifi0.disabled=0
            $UCI set wireless.wifi1.disabled=0
            $UCI set wireless.main_2g.disabled=0
            $UCI set wireless.main_5g.disabled=0
            $UCI commit wireless
            $UBUS call zwrt_wlan reload >/dev/null 2>&1
            ;;
    esac
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
    via_datad "{\"action\":\"vendor.call\",\"source\":\"guard\",\"params\":{\"object\":\"zwrt_zte_sleep_faw.wakelock\",\"method\":\"enableAutoSleep\",\"args\":{\"switch\":$1}}}" >/dev/null
    [ $? = 2 ] && $UBUS call zwrt_zte_sleep_faw.wakelock enableAutoSleep "{\"switch\":$1}" >/dev/null 2>&1
    return 0
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
    # Only this episode's flags: alerted-datad-* belongs to datad_round.
    rm -f "$STATE/sleep-held" "$STATE/episode" "$STATE/took-over" "$STATE/next-try" "$STATE/backoff" \
        "$STATE/alerted-silent" "$STATE/alerted-restore"
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

# What the owner reads on their phone (sms_text picks Chinese or English by
# the screen's language). Plain Chinese, one SMS (<= 70 chars):
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
        datad-degraded) m="屏幕的数据服务超过5分钟不正常，管理后台已改用备用方式读数据。上网不受影响。" ;;
        devui-crash) m="屏幕界面闪退了，已自动重新打开。不用管。" ;;
        devui-gave-up) m="屏幕界面连续打不开，已换成原厂界面，上网不受影响。长按屏幕右下角3秒可换回。" ;;
        sms-test) m="这是测试短信。收到了，说明告警短信能正常发到你手机。" ;;
        *) m="有一条新告警（$1），请到管理网页「系统→告警」查看。" ;;
    esac
    printf '【U60】%s（%s）' "$m" "$2"
}

# The same in English (docs/ui-glossary.md §9): "[U60] <body> (01 Oct 14:32)",
# printable ASCII only, the whole SMS <= 70 chars, no closing full stop.
sms_body_en() { # <kind> <time as date '+%d %b %H:%M' in the C locale>
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
        # a kind is ASCII by alert-lib's rules; kept to 20 safe chars so the
        # SMS stays one message whatever comes in
        *) m="New alert $(printf '%s' "$1" | tr -cd 'A-Za-z0-9_.-' | cut -c1-20); see Alerts on web" ;;
    esac
    printf '[U60] %s (%s)' "$m" "$2"
}

# The screen's language: en only for exactly lang=en (the last lang= line, as
# the screen reads it); a missing file, an empty or unknown value is zh.
# A plain string match, no arithmetic: a bad value must not stop guard.
sms_lang() {
    _lg=$(sed -n 's/^lang=//p' "$DEVUI_CONF" 2>/dev/null | tail -n 1)
    case "$_lg" in
        en) echo en ;;
        *) echo zh ;;
    esac
}

# sms_text <kind>: the SMS in the screen's language, with the time now.
# Read on every send, so a language switch needs no guard restart.
sms_text() {
    if [ "$(sms_lang)" = en ]; then
        sms_body_en "$1" "$(LC_ALL=C date '+%d %b %H:%M')"
    else
        sms_body "$1" "$(date '+%m-%d %H:%M')"
    fi
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
    _resp=$(via_datad "{\"action\":\"vendor.call\",\"source\":\"guard\",\"params\":{\"object\":\"zwrt_wms\",\"method\":\"zte_libwms_send_sms\",\"args\":$_json}}")
    case $? in
        0) _res=$(printf '%s' "$_resp" | $JSONFILTER -e '@.result.result' 2>/dev/null) ;;
        1)
            log "sms: datad refused or did not answer: $_resp"
            return 1
            ;;
        *)
            _resp=$($UBUS call zwrt_wms zte_libwms_send_sms "$_json" 2>&1) || {
                log "sms: ubus failed: $_resp"
                return 1
            }
            _res=$(printf '%s' "$_resp" | $JSONFILTER -e '@.result' 2>/dev/null)
            ;;
    esac
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
        via_datad "{\"action\":\"sms.delete\",\"source\":\"guard\",\"params\":{\"ids\":\"$_id\"}}" >/dev/null
        _vrc=$?
        { [ "$_vrc" = 0 ] || { [ "$_vrc" = 2 ] && $UBUS call zwrt_wms zwrt_wms_delete_sms "{\"id\":\"$_id\"}" >/dev/null 2>&1; }; } &&
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
        if [ "$_kind" = sms-failed ] || [ "$_kind" = devui-theme-paused ]; then
            : # never an SMS about a failed SMS, or about the screen's colours
        elif ! valid_number "$_number"; then
            sms_log "$_w" "$_seq" "$_kind" no-number
        elif [ -f "$ALERT_DIR/abroad" ] && [ ! -f "$ALERT_DIR/sms-abroad" ]; then
            sms_log "$_w" "$_seq" "$_kind" suppressed-abroad
        elif ! clock_ok; then
            break # rate limits need a real clock; leave it pending, not done
        elif ! rate_ok "$_kind" "$_w"; then
            sms_log "$_w" "$_seq" "$_kind" suppressed-rate
        elif send_sms "$_number" "$(sms_text "$_kind")"; then
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

# ── log caps ────────────────────────────────────────────────────────────────
# Over CAP_BYTES: keep one previous copy as <log>.old and start the log over.
# Only for writers that opened the file with O_APPEND (>>); a plain > writer
# would keep its offset and leave a hole the size of the old log.
# Every process holding <file> open must have it in O_APPEND mode (02000).
# Only checked once a log is over the cap, so the fd scan is rare.
appenders_only() { # <file>
    for _fd in "$PROC"/[0-9]*/fd/*; do
        [ "$(readlink "$_fd" 2>/dev/null)" = "$1" ] || continue
        _fl=$(awk '/^flags:/ {print $2}' "${_fd%/fd/*}/fdinfo/${_fd##*/}" 2>/dev/null)
        [ -n "$_fl" ] && [ $(( 0$_fl & 1024 )) -ne 0 ] || return 1
    done
    return 0
}
logcap_round() {
    logcap_list "$CAP_BYTES" $CAP_LOGS
    logcap_list "$CAP_TMP_BYTES" $CAP_TMP_LOGS
}
logcap_list() { # <cap bytes> <log>...
    _cap=$1
    shift
    for _f; do
        [ -f "$_f" ] || continue
        _sz=$(wc -c 2>/dev/null <"$_f") || continue
        [ "${_sz:-0}" -gt "$_cap" ] || continue
        # The fd scan below starts a process per open file on the device
        # (~2200), so after a refusal it is not repeated for an hour (per log:
        # one refused log does not hold the others back).
        _sk=$STATE/logcap-skip.${_f##*/}
        _skip=$(num "$_sk")
        [ "$_skip" -gt 0 ] && [ $(( $(uptime_s) - _skip )) -lt 3600 ] && continue
        if ! appenders_only "$_f"; then
            # truncating under a non-append writer leaves a hole the size of the old log
            [ "$_skip" -gt 0 ] || log "not capping $_f: a writer did not open it for append"
            mkdir -p "$STATE"; uptime_s >"$_sk"
            continue
        fi
        rm -f "$_sk"
        cp "$_f" "$_f.old" && : >"$_f" && log "capped $_f at $_sz bytes (previous copy in $_f.old)"
    done
}

# ── standby sentinel ────────────────────────────────────────────────────────
# While the screen is dark, one line per round into STANDBY_STAT:
#   <uptime> <cellular pkts/min> <tailscale-tunnel pkts/min> <wakeups/s for
#   each of STANDBY_PROGS, or - when that program restarted or is missing>
# The tunnel is counted separately rather than subtracted: traffic from
# other tailnet devices (a browser tab polling this device, say) is exactly
# the kind of waste the sentinel exists to show. Rounds far apart (the
# device slept) and lit-screen rounds are not recorded. Keeps 60 lines, in
# /tmp. doctor.sh does the judging.
find_pid() { # exact comm match; first hit
    for _d in "$PROC"/[0-9]*; do
        { read -r _c <"$_d/comm"; } 2>/dev/null || continue
        [ "$_c" = "$1" ] && { echo "${_d##*/}"; return; }
    done
}
standby_round() {
    _now=$(uptime_s)
    _w=0; _t=0
    while IFS=' :' read -r _if _rb _rp _re _rd _rf _rfr _rc _rm _tb _tp _rest; do
        case $_if in "$WAN_IF") _w=$((_rp + _tp)) ;; "$TS_IF") _t=$((_rp + _tp)) ;; esac
    done 2>/dev/null <"$NETDEV"
    _cur="$_now $_w $_t"
    for _n in $STANDBY_PROGS; do
        _p=$(find_pid "$_n")
        if [ -n "$_p" ]; then
            _s=$(awk '/^(non)?voluntary_ctxt_switches/ {x += $2} END {print x + 0}' "$PROC/$_p"/task/*/status 2>/dev/null)
            _cur="$_cur $_p:${_s:-0}"
        else
            _cur="$_cur -"
        fi
    done
    _prev=$(cat "$STATE/standby.prev" 2>/dev/null)
    mkdir -p "$STATE"; echo "$_cur" >"$STATE/standby.prev"
    _bl=0; { read -r _bl <"$BACKLIGHT"; } 2>/dev/null
    [ "${_bl:-0}" -gt 0 ] 2>/dev/null && return 0
    [ -n "$_prev" ] || return 0
    echo "$_prev $_cur" | awk -v gap="$WAKE_GAP" '{
        n = NF / 2                       # prev fields, then cur fields
        el = $(n + 1) - $1
        if (el <= 0 || el > gap) exit 1
        wan = ($(n + 2) - $2) * 60 / el; ts = ($(n + 3) - $3) * 60 / el
        if (wan < 0 || ts < 0) exit 1   # counters reset (interface came back)
        line = sprintf("%d %.0f %.0f", $(n + 1), wan, ts)
        for (i = 4; i <= n; i++) {
            split($i, a, ":"); split($(n + i), b, ":")
            if ($i == "-" || $(n + i) == "-" || a[1] != b[1] || b[2] < a[2]) line = line " -"
            else line = line sprintf(" %.1f", (b[2] - a[2]) / el)
        }
        print line
    }' >>"$STANDBY_STAT" || return 0
    tail -n 60 "$STANDBY_STAT" >"$STANDBY_STAT.tmp" && mv -f "$STANDBY_STAT.tmp" "$STANDBY_STAT"
}

# ── background jobs (docs/LEDGER.md §6) ─────────────────────────────────────
# Work that may be slow or trip over bad input runs in a background subshell
# that the main loop never waits for. While the previous job of the same name
# is still alive (same pid AND same start time, so a reused pid does not
# count) the round skips it. GUARD_LEDGER_FG=1 runs jobs in the foreground so
# tests can see their results at once.
proc_start() { # <pid> [<proc dir>]: its start time (stat field 22), empty if gone
    sed 's/^.*) //' "${2:-/proc}/$1/stat" 2>/dev/null | awk '{ print $20 }'
}
bg_job() { # bg_job <name> <function> — 0 started, 1 skipped (previous one still running)
    mkdir -p "$LTMP"
    if [ -n "$GUARD_LEDGER_FG" ]; then
        ("$2") </dev/null 2>>"$LOG"
        return 0
    fi
    _j=
    _js=
    _ju=
    { read -r _j _js _ju <"$LTMP/$1.pid"; } 2>/dev/null
    case "$_j" in '' | *[!0-9]*) _j= ;; esac
    if [ -n "$_j" ] && [ -n "$_js" ] && [ "$(proc_start "$_j")" = "$_js" ]; then
        # still running: log it once if it has been at it for 10 s or more
        if isint "$_ju" && [ $(($(uptime_s) - _ju)) -ge 10 ] && [ "$(cat "$LTMP/$1.slow" 2>/dev/null)" != "$_j" ]; then
            echo "$_j" >"$LTMP/$1.slow"
            log "background job $1 (pid $_j) still running after $(($(uptime_s) - _ju)) s; this round skips it"
        fi
        return 1
    fi
    ("$2") </dev/null 2>>"$LOG" &
    _j=$!
    echo "$_j $(proc_start "$_j") $(uptime_s)" >"$LTMP/$1.pid"
    return 0
}

# One line per round for the ledger job's hour line and coverage (docs/LEDGER.md
# §6, §8): "<uptime> started|skipped". The main loop is its only writer.
round_mark() { # <0 started | 1 skipped>
    now_up
    _rk=started
    [ "$1" = 1 ] && _rk=skipped
    mkdir -p "$LTMP" && echo "$_nu $_rk" >>"$LTMP/rounds"
    ROUNDS_N=$((${ROUNDS_N:-0} + 1))
    if [ $((ROUNDS_N % 60)) = 0 ]; then # never more than 180 lines: back to the last 120 every 60 rounds
        tail -n 120 "$LTMP/rounds" >"$LTMP/rounds.tmp" && mv -f "$LTMP/rounds.tmp" "$LTMP/rounds"
    fi
}

# Fingerprints of the programs the sentinel measures (docs/LEDGER.md §12):
# $LTMP/fp = "<program> <pid> <md5 prefix>" per line ("- -" when not running).
# The md5 of a 30 MB binary is only computed when its pid changes; when the
# binary itself changed, $LTMP/fp-changed gets the uptime, and doctor.sh then
# judges only standby records taken after it.
fp_round() {
    mkdir -p "$LTMP"
    for _n in $STANDBY_PROGS; do
        _p=$(find_pid "$_n")
        _old=$(awk -v n="$_n" '$1 == n { print $2, $3; exit }' "$LTMP/fp" 2>/dev/null)
        _op=${_old%% *}
        _om=${_old#* }
        if [ -z "$_p" ]; then
            echo "$_n - -"
        elif [ "$_p" = "$_op" ] && [ -n "$_om" ] && [ "$_om" != "-" ]; then
            echo "$_n $_p $_om"
        else
            _m=$(md5sum "$PROC/$_p/exe" 2>/dev/null | cut -c1-8)
            echo "$_n $_p ${_m:--}"
            if [ -n "$_m" ] && [ -n "$_om" ] && [ "$_om" != "-" ] && [ "$_m" != "$_om" ]; then
                uptime_s >"$LTMP/fp-changed"
            fi
        fi
    done >"$LTMP/fp.tmp" && mv -f "$LTMP/fp.tmp" "$LTMP/fp"
}

# ── the ledger (docs/LEDGER.md) ─────────────────────────────────────────────
# Everything in this section runs inside the background ledger job, which holds
# $LTMP/writer.lock: exactly one writer, never the main loop. A fatal shell
# error in here kills the job's subshell, nothing else.

ledger_clean() { # one line of printable ASCII without " or \, at most 160 chars
    printf '%s' "$*" | tr '\t\r\n' '   ' | tr -cd '\040-\176' | tr -d '"\\' | cut -c1-160
}
jstr() { printf '"%s"' "$(ledger_clean "$*")"; }
jnum() { # an integer or decimal as is, anything else null
    _jv=${1#-}
    case "$_jv" in
        '' | *[!0-9.]* | .* | *. | *.*.*) printf null ;;
        *) printf '%s' "$1" ;;
    esac
}
ledger_bootid() {
    _lb=$(cat "$BOOT_ID_FILE" 2>/dev/null)
    case "$_lb" in '' | *[!0-9a-f-]*) echo unknown ;; *) echo "$_lb" ;; esac
}
ledger_critical() { # written events of these kinds are fsynced at once
    case " boot boot_backfill ssr ssr_result ssr_dump oom svc_exit proc_restart uid datad_degraded calib reboot_request " in
        *" $1 "*) return 0 ;;
    esac
    return 1
}
fsync_file() {
    [ -z "$FSYNC_LOG" ] || echo "$1" >>"$FSYNC_LOG" # tests: which files were fsynced
    dd if=/dev/null of="$1" conv=notrunc,fsync 2>/dev/null
}

# The segment to append to; a new part once the current one is full.
ledger_seg() {
    _ls=$(cat "$LTMP/seg" 2>/dev/null)
    if [ -n "$_ls" ] && [ -f "$_ls" ] && [ "$(wc -c <"$_ls")" -lt "$LEDGER_SEG_MAX" ]; then
        echo "$_ls"
        return 0
    fi
    _lp=0
    if [ -n "$_ls" ]; then
        _lp=${_ls##*-}
        _lp=$(echo "${_lp%.jsonl}" | sed 's/^0*//')
        case "$_lp" in '' | *[!0-9]*) _lp=0 ;; esac
    fi
    _ls=$(printf '%s/boot-%06d-%s-%03d.jsonl' "$LEDGER_DIR" "$(num "$LTMP/seq")" "$(ledger_bootid | cut -c1-8)" $((_lp + 1)))
    : >>"$_ls" && echo "$_ls" >"$LTMP/seg" && echo "$_ls"
}

# ledger_append <k> <fragment> [<up>] [<id>] [<seq>]
# 0 = written (and fsynced when critical); 2 = dropped by policy (over the
# day's budget: gone); 1 = not written now (write error, or /data nearly full):
# keep whatever it came from and try again later.
ledger_append() {
    _ak=$1
    _af=$2
    _aup=${3:-$(cut -d' ' -f1 "$UPTIME_FILE" 2>/dev/null)}
    _aid=$4
    _aseq=${5:-$(num "$LTMP/seq")}
    _acrit=0
    ledger_critical "$_ak" && _acrit=1
    if [ -f "$LTMP/lowspace" ] && [ "$_ak" != boot ]; then
        return 1
    fi
    _aday=
    _abytes=0
    { read -r _aday _abytes <"$LTMP/budget"; } 2>/dev/null
    case "$_abytes" in '' | *[!0-9]*) _abytes=0 ;; esac
    _atoday=$(($(uptime_s) / 86400))
    [ "$_aday" = "$_atoday" ] || _abytes=0
    if [ "$_abytes" -ge "$LEDGER_DAY_MAX" ] && [ "$_acrit" = 0 ]; then
        case "$_ak" in hour | hour_power | hour_proc) ;; *) return 2 ;; esac
    fi
    _at=null
    _ack=
    _aoff=
    { read -r _ack _aoff _rest <"$STATE/clock-ok"; } 2>/dev/null
    case "$_aoff" in '' | *[!0-9]*) _ack=0 ;; esac
    _aupi=${_aup%.*}
    case "$_aupi" in '' | *[!0-9]*) _ack=0 ;; esac
    # another boot's event (drained spool, a recovered hour): this boot's clock says nothing about it
    [ "$_aseq" = "$(num "$LTMP/seq")" ] || _ack=0
    [ "$_ack" = 1 ] && _at=$((_aupi + _aoff))
    _an=$(($(num "$LTMP/n") + 1))
    _ahead="{\"v\":1,\"seq\":$(jnum "$_aseq"),\"n\":$_an,\"up\":$(jnum "$_aup"),\"t\":$_at,\"k\":\"$_ak\"${_aid:+,\"id\":\"$_aid\"}"
    _aline="$_ahead$_af}"
    [ "${#_aline}" -le 512 ] || _aline="$_ahead,\"trunc\":1}"
    _as=$(ledger_seg) || return 1
    # never glue a line onto a torn tail: that half line stays a bad line
    [ -n "$(tail -c 1 "$_as" 2>/dev/null)" ] && echo >>"$_as"
    printf '%s\n' "$_aline" >>"$_as" || return 1
    if [ "$_acrit" = 1 ]; then
        fsync_file "$_as" || return 1
    fi
    [ "$(tail -n 1 "$_as")" = "$_aline" ] || return 1
    echo "$_an" >"$LTMP/n"
    echo "$_atoday $((_abytes + ${#_aline} + 1))" >"$LTMP/budget"
    return 0
}

# This boot's sequence number: reused across guard restarts (tmpfs, bootmap),
# else the next one (docs/LEDGER.md §10).
ledger_seq_init() {
    _qc=$(cat "$LTMP/seq" 2>/dev/null)
    case "$_qc" in '' | *[!0-9]*) ;; *) return 0 ;; esac
    _qb=$(ledger_bootid)
    _ql=$(tail -n 1 "$LEDGER_DIR/bootmap" 2>/dev/null)
    _qls=${_ql%% *}
    _qlb=${_ql#* }
    _qlb=${_qlb%% *}
    case "$_qls" in '' | *[!0-9]*) ;; *)
        if [ "$_qlb" = "$_qb" ]; then
            echo "$_qls" >"$LTMP/seq"
            return 0
        fi
        ;;
    esac
    _qs=$(num "$LEDGER_DIR/seq")
    for _qx in $(ls "$LEDGER_DIR" 2>/dev/null | sed -n 's/^boot-0*\([0-9][0-9]*\)-.*/\1/p') \
        $(awk '{ print $1 }' "$LEDGER_DIR/bootmap" 2>/dev/null); do
        case "$_qx" in '' | *[!0-9]*) continue ;; esac
        [ "$_qx" -gt "$_qs" ] && _qs=$_qx
    done
    _qs=$((_qs + 1))
    echo "$_qs" >"$LEDGER_DIR/seq.tmp" && fsync_file "$LEDGER_DIR/seq.tmp" &&
        mv -f "$LEDGER_DIR/seq.tmp" "$LEDGER_DIR/seq" || return 1
    echo "$_qs $_qb $(uptime_s)" >>"$LEDGER_DIR/bootmap" && fsync_file "$LEDGER_DIR/bootmap" || return 1
    echo "$_qs" >"$LTMP/seq"
}

# Is this event already in the ledger? Found means found on flash too: the
# line may have been appended by a job killed before its fsync, and only the
# page cache has it. Every caller then drops its own copy (a spool file, a
# backfill step), so the segment is fsynced before saying yes.
ledger_has_id() { # <id> <from seq>
    for _hs in "$LEDGER_DIR"/boot-*.jsonl; do
        [ -f "$_hs" ] || continue
        _hq=${_hs##*/boot-}
        _hq=$(echo "${_hq%%-*}" | sed 's/^0*//')
        case "$_hq" in '' | *[!0-9]*) _hq=0 ;; esac
        [ "$_hq" -ge "$2" ] || continue
        if grep -q -F "\"id\":\"$1\"" "$_hs"; then
            fsync_file "$_hs" || return 1
            return 0
        fi
    done
    return 1
}

ledger_bad() { # set a malformed spool file aside (at most 20 kept)
    mkdir -p "$LEDGER_DIR/spool/bad"
    mv -f "$1" "$LEDGER_DIR/spool/bad/" 2>/dev/null || rm -f "$1"
    ls -t "$LEDGER_DIR/spool/bad" 2>/dev/null | tail -n +21 | while read -r _bo; do rm -f "$LEDGER_DIR/spool/bad/$_bo"; done
    log "ledger: malformed spool file ${1##*/} set aside"
}

# Take in the spool files (docs/LEDGER.md §5): written and fsynced first,
# deleted only after that; an id already in the ledger is not written twice.
ledger_drain() {
    _db=$(ledger_bootid)
    _dq=$(num "$LTMP/seq")
    for _df in "$LEDGER_DIR"/spool/*.ev "$SPOOL_TMP"/*.ev; do
        [ -f "$_df" ] || continue
        _dh=
        _dfr=
        { IFS= read -r _dh; IFS= read -r _dfr; } 2>/dev/null <"$_df"
        _did=${_dh%%"$TAB"*}
        _dr=${_dh#*"$TAB"}
        _deb=${_dr%%"$TAB"*}
        _dr=${_dr#*"$TAB"}
        _dup=${_dr%%"$TAB"*}
        _dk=${_dr#*"$TAB"}
        _dbase=${_df##*/}
        _dok=1
        case "$_did" in '' | *[!A-Za-z0-9._-]*) _dok=0 ;; esac
        [ "$_did" = "${_dbase%.ev}" ] || _dok=0
        case "$_deb" in '' | *[!0-9a-f-]*) _dok=0 ;; esac
        [ "$(jnum "$_dup")" = null ] && _dok=0
        case "$_dk" in '' | *[!a-z_]*) _dok=0 ;; esac
        case "$_dfr" in '' | ,*) ;; *) _dok=0 ;; esac
        [ "${#_dfr}" -le 400 ] || _dok=0
        [ -z "$(printf '%s' "$_dfr" | tr -d '\040-\176')" ] || _dok=0
        if [ "$_dok" = 0 ]; then
            ledger_bad "$_df"
            continue
        fi
        if [ "$_deb" = "$_db" ]; then
            _des=$_dq
        else
            _des=$(awk -v b="$_deb" '$2 == b { s = $1 } END { print s + 0 }' "$LEDGER_DIR/bootmap" 2>/dev/null)
        fi
        case "$_des" in '' | *[!0-9]*) _des=0 ;; esac
        if ledger_has_id "$_did" "$_des"; then
            rm -f "$_df"
            continue
        fi
        ledger_append "$_dk" "$_dfr" "$_dup" "$_did" "$_des"
        case $? in 0 | 2) rm -f "$_df" ;; esac
    done
}

ledger_ready() { # directories, and the low-space switch (docs/LEDGER.md §2)
    mkdir -p "$LEDGER_DIR/state" "$LEDGER_DIR/spool" "$LTMP" 2>/dev/null || return 1
    [ -w "$LEDGER_DIR" ] || return 1
    _rf=$($DF -k "$LEDGER_DIR" 2>/dev/null | tail -n 1 | awk '{ print $(NF - 2) }')
    case "$_rf" in '' | *[!0-9]*) _rf= ;; esac
    if [ -n "$_rf" ] && [ "$_rf" -lt "$LEDGER_FREE_MIN" ]; then
        touch "$LTMP/lowspace"
    else
        rm -f "$LTMP/lowspace"
    fi
}

jopt() { # a JSON string, or null when empty
    if [ -n "$1" ]; then jstr "$1"; else printf null; fi
}
ledger_this() { # this boot's segments, as a glob
    echo "$LEDGER_DIR/boot-$(printf '%06d' "$(num "$LTMP/seq")")-*.jsonl"
}

# The modem's network-mode preference, from uci zte_nwinfo (the package
# zte-agent reads too; zwrt_zte_nwinfo is another one and has no net_select).
net_select_now() {
    $UCI -q show zte_nwinfo 2>/dev/null | sed -n "s/^[^.]*\.[^.]*\.net_select='*\([^']*\)'*$/\1/p" | head -n 1
}
# uci show output -> "option=value option=value", at most 60 bytes: without
# the section's header line and the package.section. prefix, so the values
# themselves are what fits.
uci_brief() {
    sed -n "s/^[^.]*\.[^.]*\.\([^=]*\)='*\([^']*\)'*$/\1=\2/p" |
        awk '{ s = s sep $0; sep = " " } END { s = substr(s, 1, 60); sub(/ +$/, "", s); print s }'
}

# The reason-code line of the boot now running is the last one in key.log (the
# firmware writes it ~20 s after power-on, guard starts ~45 s).
ledger_boot_line() {
    _bcl=$(grep 'reboot_reason_code=[0-9]' "$KEYLOG" 2>/dev/null | tail -n 1)
    _bc=$(printf '%s' "$_bcl" | sed -n 's/.*reboot_reason_code=\([0-9][0-9]*\).*/\1/p')
    _bm=$(awk -F"'" '/option mode_main_state/ { print $2; exit }' "$MC_TMP" 2>/dev/null)
    # the build (…B27) is only in device_info; tr069's SoftwareVersion is a fixed "MU5250_SWV01"
    _bfw=$($UBUS -t 2 call zwrt_web device_info '{}' 2>/dev/null | sed -n 's/.*"wa_inner_version": *"\([^"]*\)".*/\1/p' | head -n 1)
    [ -n "$_bfw" ] || _bfw=$($UCI -q get zwrt_tr069.DeviceInfo.SoftwareVersion 2>/dev/null)
    _bns=$(net_select_now)
    _bw=$($UCI -q show zwrt_zte_mc.reboot_schedule 2>/dev/null | uci_brief)
    _bcut=$($UCI -q show zwrt_router.cutoff_protect 2>/dev/null | uci_brief)
    _bcf=$($UCI -q show zwrt_data_commit.wwaniface1 2>/dev/null | grep connect_fail_reboot | uci_brief)
    # the PMIC's power-on reason, raw (no table of its codes yet): read beside the reason code
    _bpo=$($UBUS -t 2 call zwrt_bsp.pm list '{}' 2>/dev/null | sed -n 's/.*"power_on_reason": *\(-\{0,1\}[0-9][0-9]*\).*/\1/p' | head -n 1)
    ledger_append boot ",\"boot\":$(jstr "$(ledger_bootid)"),\"code\":$(jnum "$_bc"),\"mode\":$(jopt "$_bm"),\"fw\":$(jopt "$_bfw"),\"net_select\":$(jopt "$_bns"),\"rb_weekly\":$(jopt "$_bw"),\"rb_cutoff\":$(jopt "$_bcut"),\"rb_connfail\":$(jopt "$_bcf"),\"pon\":$(jnum "$_bpo")"
}

# Which build is running: binaries by /proc/<pid>/exe, scripts by their own
# file (guard itself runs as /bin/sh, so its exe says nothing).
ledger_ver_line() { # <program> <pid, empty when not running> [<md5 prefix>]
    _vm=$3
    _vx=null
    if [ -n "$2" ]; then
        [ -n "$_vm" ] || _vm=$(md5sum "$PROC/$2/exe" 2>/dev/null | cut -c1-8)
        if [ "$1" = zwrt-datad ]; then
            _vu=$(tr '\0' '\n' 2>/dev/null <"$PROC/$2/environ" | sed -n 's/^ZWRT_DATAD_UBUS=//p' | head -n 1)
            _vx=$(jstr "ubus=${_vu:-cli}")
        fi
    fi
    ledger_append ver ",\"prog\":\"$1\",\"md5\":$(jopt "$_vm"),\"how\":\"exe\",\"pid\":$(jnum "$2"),\"extra\":$_vx"
}
ledger_ver_last() { # the md5 in this boot's last ver line for <program> (or null)
    grep -h "\"k\":\"ver\",\"prog\":\"$1\"" $(ledger_this) 2>/dev/null | tail -n 1 |
        sed -n 's/.*"md5":\([^,]*\),.*/\1/p' | tr -d '"'
}
ledger_ver() {
    _vprogs="zte-agent zwrt-datad u60pro-devui u60-uid tailscaled"
    for _vp in $_vprogs; do
        grep -q "\"k\":\"ver\",\"prog\":\"$_vp\"" $(ledger_this) 2>/dev/null && continue
        ledger_ver_line "$_vp" "$(find_pid "$_vp")" || return 1
    done
    for _vs in u60-guard.sh doctor.sh alert-lib.sh supervise.sh; do
        [ -f "$HERE/$_vs" ] || continue
        grep -q "\"k\":\"ver\",\"prog\":\"$_vs\"" $(ledger_this) 2>/dev/null && continue
        ledger_append ver ",\"prog\":\"$_vs\",\"md5\":$(jopt "$(md5sum "$HERE/$_vs" 2>/dev/null | cut -c1-8)"),\"how\":\"file\",\"pid\":null,\"extra\":null" || return 1
    done
}

# ── key.log backfill (docs/LEDGER.md §10) ──
# The anchor is the reason-code line last taken in: its file's inode, its line
# number, and the md5 of that line with the 20 before it. Line numbers, like
# byte offsets, stay put when key.log grows or is renamed to key.log.0.
ledger_anchor_md5() { # <file> <line>
    _am0=$(($2 > 20 ? $2 - 20 : 1))
    sed -n "${_am0},${2}p" "$1" 2>/dev/null | md5sum | cut -c1-32
}
ledger_inode() { ls -i "$1" 2>/dev/null | awk '{ print $1 }'; }
ledger_anchor_find() { # prints "<file> <line>" where the stored anchor is, or fails
    _ai=
    _an=
    _am=
    { read -r _ai _an _am <"$LEDGER_DIR/state/keylog.anchor"; } 2>/dev/null
    case "$_an" in '' | *[!0-9]*) return 1 ;; esac
    for _af in "$KEYLOG" "$KEYLOG.0"; do
        [ -f "$_af" ] || continue
        [ "$(ledger_inode "$_af")" = "$_ai" ] || continue
        [ "$(ledger_anchor_md5 "$_af" "$_an")" = "$_am" ] || continue
        echo "$_af $_an"
        return 0
    done
    return 1
}
ledger_backfill_file() { # <file> <after line> <before line, 0 = to the end> <gap 0|1>
    _bfi=$(ledger_inode "$1")
    grep -n 'reboot_reason_code=[0-9]' "$1" 2>/dev/null | while IFS=: read -r _bln _brest; do
        case "$_bln" in '' | *[!0-9]*) continue ;; esac
        [ "$_bln" -gt "$2" ] || continue
        [ "$3" = 0 ] || [ "$_bln" -lt "$3" ] || continue
        _bid="kl-$_bfi-$_bln"
        ledger_has_id "$_bid" "$(num "$LTMP/seq")" && continue
        _bcode=$(printf '%s' "$_brest" | sed -n 's/.*reboot_reason_code=\([0-9][0-9]*\).*/\1/p')
        ledger_append boot_backfill ",\"code\":$(jnum "$_bcode"),\"at\":$(jopt "$(printf '%s' "$_brest" | cut -c1-19)"),\"started\":null,\"gap\":$4" "" "$_bid" || exit 1
    done
}
ledger_backfill() {
    _kcur=$(grep -n 'reboot_reason_code=[0-9]' "$KEYLOG" 2>/dev/null | tail -n 1)
    _kcur=${_kcur%%:*}
    case "$_kcur" in '' | *[!0-9]*) return 0 ;; esac
    if [ -f "$LEDGER_DIR/state/init-done" ]; then
        if _kanc=$(ledger_anchor_find); then
            if [ "${_kanc% *}" = "$KEYLOG.0" ]; then
                ledger_backfill_file "$KEYLOG.0" "${_kanc#* }" 0 0 || return 1
                ledger_backfill_file "$KEYLOG" 0 "$_kcur" 0 || return 1
            else
                ledger_backfill_file "$KEYLOG" "${_kanc#* }" "$_kcur" 0 || return 1
            fi
        else
            ledger_backfill_file "$KEYLOG" 0 "$_kcur" 1 || return 1 # anchor lost: key.log only, marked
        fi
    fi
    # everything up to here is in (and fsynced): only now move the anchor
    printf '%s %s %s\n' "$(ledger_inode "$KEYLOG")" "$_kcur" "$(ledger_anchor_md5 "$KEYLOG" "$_kcur")" \
        >"$LEDGER_DIR/state/keylog.anchor.tmp" && fsync_file "$LEDGER_DIR/state/keylog.anchor.tmp" &&
        mv -f "$LEDGER_DIR/state/keylog.anchor.tmp" "$LEDGER_DIR/state/keylog.anchor"
}

# New crash files (docs/LEDGER.md §10): known ones are listed in state/
# crashlog.seen as "<program>/<file> <size> <md5>"; the first go-live only
# records what is there. Name + size is checked first, md5 only for new ones.
ledger_crashlog() { # <found: boot_init | round>
    _cls=$LEDGER_DIR/state/crashlog.seen
    : >"$LTMP/cl.new" # programs with a new crash file in this scan (proc_restart's crashlog field)
    _clfirst=0
    [ -f "$LEDGER_DIR/state/init-done" ] || _clfirst=1
    for _clf in "$CRASHLOG_DIR"/*/*.log; do
        [ -f "$_clf" ] || continue
        _clr=${_clf#"$CRASHLOG_DIR"/}
        _clsz=$(wc -c 2>/dev/null <"$_clf")
        grep -q -F "$_clr $_clsz " "$_cls" 2>/dev/null && continue
        _clm=$(md5sum "$_clf" 2>/dev/null | cut -c1-12)
        [ -n "$_clm" ] || continue
        if [ "$_clfirst" = 0 ] && ! ledger_has_id "cl-$_clm" 0; then
            _clst=$(sed -n 's/^status: *//p' "$_clf" 2>/dev/null | head -n 1)
            _clup=${_clf##*-up}
            _clup=${_clup%.log}
            case "$_clup" in '' | *[!0-9]*) _clup= ;; esac
            ledger_append svc_exit ",\"prog\":$(jstr "${_clr%%/*}"),\"file\":$(jstr "${_clf##*/}"),\"status\":$(jopt "$_clst"),\"found\":\"$1\",\"crash_up\":$(jnum "$_clup")" "" "cl-$_clm" || return 1
            echo "${_clr%%/*}" >>"$LTMP/cl.new"
        fi
        echo "$_clr $_clsz $_clm" >>"$_cls"
    done
    [ -f "$_cls" ] && fsync_file "$_cls"
    return 0
}

# One guard_start per line of state/started.log for this boot (written by
# guard itself before its first round, so it exists even when the ledger job
# never got to run).
ledger_starts() {
    _sb=$(ledger_bootid)
    _sb8=$(echo "$_sb" | cut -c1-8)
    grep "^$_sb " "$LEDGER_DIR/state/started.log" 2>/dev/null | while read -r _sx _sup _sreq _sby _swhy; do
        _sid="gs-$_sb8-$_sup"
        grep -q -x -F "$_sid" "$LTMP/gs.done" 2>/dev/null && continue
        if ! ledger_has_id "$_sid" "$(num "$LTMP/seq")"; then
            ledger_append guard_start ",\"requested\":$(jnum "$_sreq"),\"by\":$(jopt "$_sby"),\"why\":$(jopt "$_swhy")" "$_sup" "$_sid" || exit 1
        fi
        echo "$_sid" >>"$LTMP/gs.done"
    done
}

# recovery_seen: the first reading after each guard start, and any fall back
# to disabled after it was enabled.
ledger_recovery() {
    _rv=
    { read -r _rv <"$MSS_RECOVERY"; } 2>/dev/null
    [ -n "$_rv" ] || return 0
    _rl=$(cat "$LTMP/recovery.last" 2>/dev/null)
    if [ -z "$_rl" ] || { [ "$_rl" = enabled ] && [ "$_rv" = disabled ]; }; then
        _rby=0
        [ -f "$STATE/recovery-set" ] && _rby=1
        ledger_append recovery_seen ",\"value\":$(jstr "$_rv"),\"set_by_guard\":$_rby" || return 1
    fi
    echo "$_rv" >"$LTMP/recovery.last"
}

ledger_boot() { # once per boot; every step can be run again without doubling anything
    grep -q '"k":"boot"' $(ledger_this) 2>/dev/null || ledger_boot_line || return 1
    ledger_ver || return 1
    ledger_backfill || return 1
    ledger_crashlog boot_init || return 1
    [ -f "$LEDGER_DIR/state/init-done" ] || : >"$LEDGER_DIR/state/init-done"
    touch "$LTMP/boot.done"
}

# Guard came up in this boot: one line in state/started.log before the first
# round (docs/LEDGER.md §10). A stop-requested marker makes it a requested one.
ledger_started() {
    mkdir -p "$LEDGER_DIR/state" "$LTMP" || return 0
    _gr=0
    _gby=-
    _gwhy=-
    if [ -f "$STOP_REQ" ]; then
        _gr=1
        { read -r _gby _gwhy <"$STOP_REQ"; } 2>/dev/null
        rm -f "$STOP_REQ"
    fi
    _gby=$(ledger_clean "${_gby:--}" | tr ' ' '_')
    printf '%s %s %s %s %s\n' "$(ledger_bootid)" "$(uptime_s)" "$_gr" "${_gby:--}" "$(ledger_clean "${_gwhy:--}")" \
        >>"$LEDGER_DIR/state/started.log"
    if [ "$(wc -l <"$LEDGER_DIR/state/started.log")" -gt 50 ]; then
        tail -n 50 "$LEDGER_DIR/state/started.log" >"$LEDGER_DIR/state/started.log.tmp" &&
            mv -f "$LEDGER_DIR/state/started.log.tmp" "$LEDGER_DIR/state/started.log"
    fi
    fsync_file "$LEDGER_DIR/state/started.log"
    rm -f "$LTMP/recovery.last" # record the recovery state once more for this start
}

# ── what the ledger job samples each round (docs/LEDGER.md §4) ──

# A build that changed under a new pid: its ver line again. The sentinel's
# fingerprints ($LTMP/fp) already hold each new pid's md5, so nothing is
# hashed here; u60-uid, which the sentinel does not measure, is hashed when
# ledger_procs sees it restart.
ledger_ver_changed() {
    while read -r _fn _fp _fm; do
        [ -n "$_fm" ] && [ "$_fm" != - ] || continue
        grep -q "\"k\":\"ver\",\"prog\":\"$_fn\"" $(ledger_this) 2>/dev/null || continue # the first one is ledger_ver's
        [ "$(ledger_ver_last "$_fn")" = "$_fm" ] && continue
        ledger_ver_line "$_fn" "$_fp" "$_fm" || return 1
    done 2>/dev/null <"$LTMP/fp"
}

# 1 when <program>'s restart is a deployment's own: the transaction in
# $SHIP_TXN is of this boot, of the component that restarts it (u60-uid: touch
# or uid), and still running or ended at most 10 minutes ago. Else 0.
ship_restart() {
    [ -f "$SHIP_TXN" ] || { echo 0; return; }
    case "$1" in zwrt-datad) _srw=" datad " ;; zte-agent) _srw=" agent " ;; u60-uid) _srw=" touch uid " ;; *) echo 0; return ;; esac
    _srb=; _src=; _srp=; _srt=
    while IFS= read -r _sl; do
        case "$_sl" in
            boot_id=*) _srb=${_sl#boot_id=} ;;
            comp=*) _src=${_sl#comp=} ;;
            phase=*) _srp=${_sl#phase=} ;;
            t_phase=*) _srt=${_sl#t_phase=} ;;
        esac
    done <"$SHIP_TXN"
    case "$_srw" in *" $_src "*) ;; *) echo 0; return ;; esac
    [ -n "$_src" ] && [ "$_srb" = "$(ledger_bootid)" ] || { echo 0; return; }
    case "$_srp" in staged | trial | promote | check | manifest | rollback) echo 1; return ;; esac
    case "$_srt" in '' | *[!0-9]*) echo 0; return ;; esac
    _srn=$(uptime_s)
    [ "$_srn" -ge "$_srt" ] && [ $((_srn - _srt)) -le 600 ] && echo 1 || echo 0
}

# datad, the agent and u60-uid: a pid or start time other than the last one
# seen is a restart. A program that is gone keeps its record, so its restart
# counts when it comes back. crashlog=1 when this round's crashlog scan found
# a new file for it (a restart without one is S4's unexplained kind); ship=1
# when a deployment did it (not counted by S4).
ledger_procs() {
    for _pn in zwrt-datad zte-agent u60-uid; do
        _pp=$(find_pid "$_pn")
        [ -n "$_pp" ] || continue
        _ps=$(proc_start "$_pp" "$PROC")
        [ -n "$_ps" ] || continue
        _po=$(awk -v n="$_pn" '$1 == n { print $2, $3; exit }' "$LTMP/procs" 2>/dev/null)
        if [ -n "$_po" ] && [ "$_po" != "$_pp $_ps" ]; then
            _pid=pr-$(ledger_bootid | cut -c1-8)-$_pp-$_ps
            if ! ledger_has_id "$_pid" "$(num "$LTMP/seq")"; then
                _pc=0
                grep -q -x -F "$_pn" "$LTMP/cl.new" 2>/dev/null && _pc=1
                ledger_append proc_restart ",\"prog\":\"$_pn\",\"old\":$(jnum "${_po%% *}"),\"new\":$_pp,\"crashlog\":$_pc,\"ship\":$(ship_restart "$_pn")" "" "$_pid" || return 1
            fi
            if [ "$_pn" = u60-uid ]; then
                _pm=$(md5sum "$PROC/$_pp/exe" 2>/dev/null | cut -c1-8)
                if [ -n "$_pm" ] && [ "$(ledger_ver_last u60-uid)" != "$_pm" ]; then
                    ledger_ver_line u60-uid "$_pp" "$_pm" || return 1
                fi
            fi
        fi
        { grep -v "^$_pn " "$LTMP/procs" 2>/dev/null; echo "$_pn $_pp $_ps"; } >"$LTMP/procs.tmp" &&
            mv -f "$LTMP/procs.tmp" "$LTMP/procs"
    done
}

# u60-uid's log (/tmp, only appended, gone at reboot): give-ups and hand-backs
# to the vendor UI, plus the owner's requests so a reader can tell those
# apart. Its u60pro-devui exits are not repeated here: the crash files have them.
# Events are numbered by line, counted over the whole boot: $LTMP/uid.pos is
# "<lines read in the current file> <lines in the files before it>". Once read
# and over UID_LOG_CAP the file is renamed to .old (u60-uid opens it for each
# line, so its next line starts a new file) and the count goes on from there.
# A file that got shorter otherwise (the logcap backstop, or a rename this job
# did not finish) is handled the same way: what .old holds past the position
# is read first.
ledger_uid_lines() { # <file> <from line> <to line> <lines before the file>
    _ub=$(ledger_bootid | cut -c1-8)
    sed -n "$(($2 + 1)),${3}p" "$1" | {
        _ul=$(($4 + $2))
        while IFS= read -r _ux; do
            _ul=$((_ul + 1))
            case "${_ux#* }" in
                "giving up"*) _uw=gave_up ;;
                "starting the vendor UI"*) _uw=handback ;;
                "request: "* | "corner long-press:"*) _uw=other ;;
                *) continue ;;
            esac
            ledger_has_id "uid-$_ub-$_ul" "$(num "$LTMP/seq")" && continue
            ledger_append uid ",\"what\":\"$_uw\",\"detail\":$(jstr "$_ux")" "" "uid-$_ub-$_ul" || exit 1
        done
    }
}
ledger_uid_pos() { # <lines read> <lines before>
    echo "$1 $2" >"$LTMP/uid.pos.tmp" && mv -f "$LTMP/uid.pos.tmp" "$LTMP/uid.pos"
}
ledger_uid() {
    _ua=
    _ubase=
    { read -r _ua _ubase _rest <"$LTMP/uid.pos"; } 2>/dev/null
    isint "$_ua" || _ua=0
    isint "$_ubase" || _ubase=0
    if [ -f "$UID_LOG" ]; then
        _un=$(wc -l 2>/dev/null <"$UID_LOG")
        isint "$_un" || return 0
    elif [ "$_ua" -gt 0 ]; then
        _un=0
    else
        return 0
    fi
    if [ "$_un" -lt "$_ua" ]; then
        ledger_uid_old || return 1
    fi
    if [ "$_un" -gt "$_ua" ]; then
        ledger_uid_lines "$UID_LOG" "$_ua" "$_un" "$_ubase" || return 1
        _ua=$_un
        ledger_uid_pos "$_ua" "$_ubase"
    fi
    file_size "$UID_LOG"
    [ -n "$_fsz" ] && [ "$_fsz" -gt "$UID_LOG_CAP" ] || return 0
    # all of it is read: u60-uid starts a new file with its next line
    mv -f "$UID_LOG" "$UID_LOG.old" 2>/dev/null || return 0
    log "u60-uid log over $UID_LOG_CAP bytes: moved to $UID_LOG.old"
    ledger_uid_old
}
# The current file is now .old, read up to _ua of its lines: read the rest
# (written between the count and the rename), then count on past it from 0.
ledger_uid_old() {
    _uo=$(wc -l 2>/dev/null <"$UID_LOG.old")
    isint "$_uo" || _uo=0
    if [ "$_uo" -gt "$_ua" ]; then
        ledger_uid_lines "$UID_LOG.old" "$_ua" "$_uo" "$_ubase" || return 1
    else
        _uo=$_ua # .old is not that file (gone, or an older one): skip past what was read
    fi
    _ubase=$((_ubase + _uo))
    _ua=0
    ledger_uid_pos 0 "$_ubase"
}

# datad_degraded (docs/LEDGER.md §11). An episode is the marker's first line
# (since). It starts 5 min after boot when the marker is fresh and its since is
# new to this boot; it ends when the marker goes (gone), its since changes
# (new_episode; the new one starts at once) or it goes stale because the agent
# stopped refreshing it (stale, length unknown). One still open at a reboot is
# ended in the next boot (reboot). Lengths use this boot's uptime only.
ledger_datad_end() { # <how> <since> <episode boot8> <seconds or null>
    ledger_has_id "dde-$3-$2" 0 ||
        ledger_append datad_degraded ",\"state\":\"end\",\"since\":$2,\"reason\":null,\"dur_s\":$4,\"how\":\"$1\"" "" "dde-$3-$2" || return 1
    rm -f "$LTMP/datad.ep" "$LEDGER_DIR/state/datad.open"
}
ledger_datad() {
    _db8=$(ledger_bootid | cut -c1-8)
    _eo=
    _eb=
    { read -r _eo _eb <"$LEDGER_DIR/state/datad.open"; } 2>/dev/null
    isint "$_eo" || _eo=
    if [ -n "$_eo" ] && [ "$_eb" != "$_db8" ]; then
        ledger_datad_end reboot "$_eo" "$_eb" null || return 1
        _eo=
    fi
    _eu=
    { read -r _x _eu <"$LTMP/datad.ep"; } 2>/dev/null
    isint "$_eu" || _eu=
    _ms=
    _mr=
    _fresh=0
    if [ -f "$DATAD_MARKER" ]; then
        { read -r _ms; read -r _mr; } 2>/dev/null <"$DATAD_MARKER"
        isint "$_ms" || _ms=
        _mt=$(file_mtime "$DATAD_MARKER")
        isint "$_mt" && [ $(($(wall_s) - _mt)) -lt "$DATAD_FRESH" ] && _fresh=1
    fi
    _du=$(uptime_s)
    if [ -n "$_eo" ]; then
        _dh=
        if [ ! -f "$DATAD_MARKER" ]; then
            _dh=gone
        elif [ -n "$_ms" ] && [ "$_ms" != "$_eo" ]; then
            _dh=new_episode
        elif [ "$_fresh" = 0 ]; then
            _dh=stale
        fi
        if [ -n "$_dh" ]; then
            _dd=null
            [ "$_dh" != stale ] && [ -n "$_eu" ] && [ "$_du" -ge "$_eu" ] && _dd=$((_du - _eu))
            ledger_datad_end "$_dh" "$_eo" "$_db8" "$_dd" || return 1
            _eo=
        fi
    fi
    [ -z "$_eo" ] && [ -n "$_ms" ] && [ "$_fresh" = 1 ] && [ "$_du" -ge "$GRACE" ] || return 0
    grep -q -x -F "$_ms" "$LTMP/datad.seen" 2>/dev/null && return 0 # this episode was started (and ended) already
    if ! ledger_has_id "dds-$_db8-$_ms" "$(num "$LTMP/seq")"; then
        ledger_append datad_degraded ",\"state\":\"start\",\"since\":$_ms,\"reason\":$(jopt "$_mr"),\"dur_s\":null,\"how\":null" "" "dds-$_db8-$_ms" || return 1
    fi
    echo "$_ms" >>"$LTMP/datad.seen"
    echo "$_ms $_du" >"$LTMP/datad.ep"
    echo "$_ms $_db8" >"$LEDGER_DIR/state/datad.open.tmp" && fsync_file "$LEDGER_DIR/state/datad.open.tmp" &&
        mv -f "$LEDGER_DIR/state/datad.open.tmp" "$LEDGER_DIR/state/datad.open"
}

# clock (docs/LEDGER.md §7): the first trusted reading of this boot, then a
# revocation, trust again, or an offset that moved by more than a day.
ledger_clock() {
    _ck=
    _co=
    { read -r _ck _co _x _ch _rest <"$STATE/clock-ok"; } 2>/dev/null
    isint "$_co" || return 0
    _lk=
    _lo=
    { read -r _lk _lo <"$LTMP/clock.last"; } 2>/dev/null
    if [ "$_ck" = 1 ]; then
        if [ "$_lk" != 1 ] || ! isint "$_lo" || [ $((_co - _lo)) -gt 86400 ] || [ $((_lo - _co)) -gt 86400 ]; then
            ledger_append clock ",\"offset\":$_co,\"how\":$(jopt "$_ch")" || return 1
            echo "1 $_co" >"$LTMP/clock.last"
        fi
    elif [ "$_lk" = 1 ]; then
        ledger_append clock ",\"offset\":null,\"how\":\"revoked\"" || return 1
        echo "0 -" >"$LTMP/clock.last"
    fi
}

# The network as datad sees it, once a round: the net cache the crash watcher
# puts in each ssr event, and net_change when net_select or the serving
# country changed (docs/LEDGER.md §2, §4). MCC-MNC as text; the SIM's home
# network is the IMSI's prefix, worked out here and never stored whole.
mnc3() { # MCCs whose networks use 3-digit MNCs (same list as datad screen.rs, agent netinfo.rs)
    case "$1" in 302 | 31[0-6] | 334 | 338 | 342 | 344 | 346 | 348 | 354 | 356 | 358 | 360 | 365 | 376 | 405 | 708 | 722 | 732) return 0 ;; esac
    return 1
}
ledger_net() {
    _nf=$LTMP/state.json
    rm -f "$_nf"
    if [ "$(uptime_s)" -ge "$LEDGER_DEADLINE" ]; then
        log "ledger: no time left this round for datad /v2/state"
        return 0
    fi
    $WGET -q -T 2 -O "$_nf" "$DATAD_URL" 2>/dev/null && [ -s "$_nf" ] || { rm -f "$_nf"; return 0; }
    N_TYPE=
    N_BAND=
    N_NR=
    N_MCC=
    N_MNC=
    N_SST=
    N_IST=
    eval "$($JSONFILTER -e 'N_SST=@.blocks.signal.stale' -e 'N_TYPE=@.blocks.signal.data.type' \
        -e 'N_BAND=@.blocks.signal.data.band' -e 'N_NR=@.blocks.signal.data.nr_band' \
        -e 'N_MCC=@.blocks.signal.data.mcc' -e 'N_MNC=@.blocks.signal.data.mnc' \
        -e 'N_IST=@.blocks.sim.stale' 2>/dev/null <"$_nf")"
    _hi=$($JSONFILTER -e '@.blocks.sim.data.imsi' 2>/dev/null <"$_nf" | cut -c1-6)
    rm -f "$_nf"
    # /v2 keeps a stale block's last value; read it as a failed read, the way
    # the old /state did (STATE_V2.md V2-29). jsonfilter prints false as 0.
    case $N_SST in 0 | false) ;; *) N_TYPE= N_BAND= N_NR= N_MCC= N_MNC= ;; esac
    case $N_IST in 0 | false) ;; *) _hi= ;; esac
    uptime_s >"$LTMP/datad.read" # the datad source was readable this round (§8)
    _net=
    if isint "$N_MCC" && isint "$N_MNC" && [ "$N_MCC" -gt 0 ]; then
        if mnc3 "$N_MCC"; then _net=$(printf '%03d-%03d' "$N_MCC" "$N_MNC"); else _net=$(printf '%03d-%02d' "$N_MCC" "$N_MNC"); fi
    fi
    _home=
    case "$_hi" in
        [2-7][0-9][0-9][0-9][0-9][0-9])
            _hm=${_hi%???}
            _hn=${_hi#???}
            mnc3 "$_hm" || _hn=${_hn%?}
            _home=$_hm-$_hn
            ;;
    esac
    printf '%s\n%s\n%s\n' "$N_TYPE" "$N_BAND" "$N_NR" |
        awk -v up="$(uptime_s)" -v net="${_net:--}" -v home="${_home:--}" '
            { gsub(/[^!-~]/, "_"); gsub(/["\\]/, ""); v[NR] = (($0 == "") ? "-" : substr($0, 1, 24)) }
            END { print up, v[1], v[2], v[3], net, home }' >"$LTMP/net.tmp" && mv -f "$LTMP/net.tmp" "$LTMP/net"
    _ns=$(net_select_now)
    [ -n "$_ns" ] && [ -n "$_net" ] || return 0
    _nl=$(cat "$LEDGER_DIR/state/net.last" 2>/dev/null)
    [ "$_nl" = "$_ns ${_net%-*}" ] && return 0
    ledger_append net_change ",\"net_select\":$(jstr "$_ns"),\"net\":\"$_net\",\"home\":$(jopt "$_home")" || return 1
    echo "$_ns ${_net%-*}" >"$LEDGER_DIR/state/net.last.tmp" && fsync_file "$LEDGER_DIR/state/net.last.tmp" &&
        mv -f "$LEDGER_DIR/state/net.last.tmp" "$LEDGER_DIR/state/net.last"
}

# Is Tailscale healthy (S8)? Asked of tailscaled's LocalAPI once a round while
# it is meant on: the same GET the touch screen's card makes (path and Host).
# Healthy = the backend Running and the node online with the control plane,
# two of what apply.sh's relay mode waits for (its third, the subnet route
# still primary after a restart, compares two moments: not used here). See
# docs/LEDGER.md §6. Sets _tsok to
#   1  healthy;
#   0  not: the socket is gone, refused, no answer in time, a 5xx, or not
#      Running / not online;
#   "" not checked: no curl, one without unix sockets, a 4xx (our request, not
#      tailscaled), an answer without a BackendState, or no read time left.
# Seconds not checked count as uncovered, never as down.
ledger_ts() {
    _tsok=
    if [ ! -e "$TS_SOCK" ]; then
        _tsok=0
        rm -f "$LTMP/ts.note"
        return 0
    fi
    _tm=$((LEDGER_DEADLINE - $(uptime_s)))
    if [ "$_tm" -lt 1 ]; then
        ledger_ts_note "no read time left"
        return 0
    fi
    [ "$_tm" -gt 2 ] && _tm=2
    _tf=$LTMP/ts.json
    rm -f "$_tf"
    _tc=$($CURL -s -m "$_tm" --unix-socket "$TS_SOCK" -o "$_tf" -w '%{http_code}' \
        http://local-tailscaled.sock/localapi/v0/status 2>/dev/null)
    _trc=$?
    case "$_trc/$_tc" in
        0/200) ;;
        0/5[0-9][0-9] | 7/* | 28/* | 52/* | 55/* | 56/*) # refused, timed out, dropped, or tailscaled failing
            rm -f "$_tf" "$LTMP/ts.note"
            _tsok=0
            return 0
            ;;
        0/*)
            rm -f "$_tf"
            ledger_ts_note "HTTP $_tc"
            return 0
            ;;
        *)
            rm -f "$_tf"
            ledger_ts_note "curl exit $_trc"
            return 0
            ;;
    esac
    T_ST=
    T_ON=
    eval "$($JSONFILTER -e 'T_ST=@.BackendState' -e 'T_ON=@.Self.Online' 2>/dev/null <"$_tf")"
    rm -f "$_tf"
    if [ -z "$T_ST" ]; then
        ledger_ts_note "no BackendState in the answer"
        return 0
    fi
    rm -f "$LTMP/ts.note"
    _tsok=0
    # a boolean may come out of jsonfilter's export form as 1 or as true: take either
    [ "$T_ST" = Running ] && case "$T_ON" in 1 | true) _tsok=1 ;; esac
    return 0
}
ledger_ts_note() { # <why>: said in the log once, until the reason changes or a check works
    [ "$(cat "$LTMP/ts.note" 2>/dev/null)" = "$1" ] && return 0
    echo "$1" >"$LTMP/ts.note"
    log "ledger: Tailscale not checked ($1); those seconds count as uncovered"
}

# state/wall.last: the trusted wall clock, at most once an hour (docs/LEDGER.md §7).
ledger_wall() {
    clock_ok || return 0
    _wn=$(wall_s)
    _wl=$(num "$WALL_LAST")
    [ "$_wn" -ge $((_wl + 3600)) ] || return 0
    mkdir -p "${WALL_LAST%/*}" && echo "$_wn" >"$WALL_LAST.tmp" && fsync_file "$WALL_LAST.tmp" && mv -f "$WALL_LAST.tmp" "$WALL_LAST"
}

# ── the hourly summary (docs/LEDGER.md §4: hour, hour_power, hour_proc) ──
# Each ledger round adds what happened since the round before to $LTMP/acc
# ("key value" lines; this job is its only writer). When the hour turns (the
# wall clock's hour once the clock is trusted, before that each hour of
# uptime) the three hour lines go out and the sums start again. A value that
# could not be read stays null, never 0.
#
# Energy: the fuel gauge's charge counter (µAh, an integral: also right across
# a sleep, whose energy goes to the cell of the round that follows it) times
# the voltage; without a counter, V×I over the awake part of each interval
# only, and the hour is marked e_partial when it had any sleep. Charging time
# goes to chg_s, never into a cell.

rdv() { _rv=; { read -r _rv <"$1"; } 2>/dev/null; } # _rv = first line of <file>, empty if unreadable
# The application side's thermal zones, by type, once per boot. The modem-side
# ones (sdr*, mmw*, epm*, mdm*, mvmss*, …) are left alone: reading one is a
# QMI request to the modem, and the ledger only wants the device's own heat.
ledger_zones() {
    for _zd in "$THERMAL"/thermal_zone*; do
        rdv "$_zd/type"
        case "$_rv" in
            cpuss-* | aoss-* | sys-therm-* | xo-therm | pm*_tz | battery) echo "$_zd" ;;
        esac
    done
}
ledger_rss() { # <program>: its resident memory in kB, empty when not running
    _rp=$(find_pid "$1")
    [ -n "$_rp" ] && awk '/^VmRSS:/ { print $2; exit }' "$PROC/$_rp/status" 2>/dev/null
}
ledger_ticks() { # <pid>: its user + system CPU ticks (stat fields 14 and 15)
    sed 's/^.*) //' "$PROC/$1/stat" 2>/dev/null | awk '{ print $12 + $13 }'
}

ledger_hour_open() { # a new window from now
    _dp=$(find_pid zwrt-datad)
    _dt=
    _ds=
    if [ -n "$_dp" ]; then
        _dt=$(ledger_ticks "$_dp")
        _ds=$(proc_start "$_dp" "$PROC")
    fi
    {
        echo "key $_hk"
        echo "from $_hn"
        echo "ft ${_hft:--}"
        echo "last $_hn"
        echo "q $_bq" # the counter at the start: the first interval needs it
        echo "batt_from ${_bc:--}"
        echo "datad ${_dp:--} ${_ds:--} ${_dt:--}"
    } >"$LTMP/acc"
}

# The three hour lines for a window, ending now. Given a checkpoint file, the
# window is the previous boot's, recovered from /data (cut:1): it ends at that
# boot's last round, goes under its sequence, and what can only be read on the
# spot (main-loop rounds, memory, CPU, free space, the network) is null.
ledger_hour_close() { # [<checkpoint> <boot8> <seq>]
    _hacc=${1:-$LTMP/acc}
    _hcut=0
    [ -n "$1" ] && _hcut=1
    _hb8=${2:-$(ledger_bootid | cut -c1-8)}
    _hsq=${3:-$(num "$LTMP/seq")}
    _hf=$(awk '$1 == "from" { print $2; exit }' "$_hacc")
    isint "$_hf" || return 0
    _hto=$_hn
    _htx=${_htt:-null}
    _hrn=null _hrk=null _hrg=null _hfree=null _hdump=null _hbud=null _hnet= _hhome= _hcpu=null _hir=null _hiw=null
    if [ "$_hcut" = 1 ]; then
        _hto=$(awk '$1 == "last" { print $2; exit }' "$_hacc")
        isint "$_hto" || return 0
        _hft0=$(awk '$1 == "ft" { print $2; exit }' "$_hacc")
        _htx=null
        isint "$_hft0" && _htx=$((_hft0 + _hto - _hf))
    else
        # rounds of the main loop in the window, and the longest wait between two
        _hr=$(awk -v a="$_hf" -v b="$_hn" '$1 >= a && $1 < b {
                n++; if ($2 == "skipped") k++
                if (p != "" && $1 - p > g) g = $1 - p
                p = $1 }
            END { printf "%d %d %d\n", n, k, g }' "$LTMP/rounds" 2>/dev/null)
        set -- ${_hr:-0 0 0}
        _hrn=$1 _hrk=$2 _hrg=$3
        _hfree=$($DF -k "$LEDGER_DIR" 2>/dev/null | tail -n 1 | awk '{ print int($(NF - 2) / 1024) }')
        _hdump=$(du -sk "$DUMP_DIR" 2>/dev/null | awk '{ print int($1 / 1024) }')
        _hbud=0
        _hday=
        _hbytes=0
        { read -r _hday _hbytes <"$LTMP/budget"; } 2>/dev/null
        isint "$_hbytes" && [ "$_hday" = $((_hn / 86400)) ] && [ "$_hbytes" -ge "$LEDGER_DAY_MAX" ] && _hbud=1
        { read -r _x _x _x _x _hnet _hhome _rest <"$LTMP/net"; } 2>/dev/null
        # datad CPU over the window: only when the same process ran from start to end
        _hdp=$(find_pid zwrt-datad)
        set -- $(awk '$1 == "datad" { print $2, $3, $4; exit }' "$_hacc")
        if [ -n "$_hdp" ] && [ "$1" = "$_hdp" ] && [ "$2" = "$(proc_start "$_hdp" "$PROC")" ] && isint "$3" && [ "$_hn" -gt "$_hf" ]; then
            _hdt=$(ledger_ticks "$_hdp")
            isint "$_hdt" && [ "$_hdt" -ge "$3" ] && _hcpu=$(((_hdt - $3) * 10 / (_hn - _hf))) # ticks at 100 Hz -> per mille
        fi
        # the sentinel's dark, idle minutes in the window (doctor.sh's rule: cellular <= median + 3 MAD)
        _hidle=$(awk -v a="$_hf" -v b="$_hn" -v base="$STANDBY_BASE" '
            BEGIN { thr = -1
                while ((getline l < base) > 0) { split(l, f, " "); if (f[1] == "2" && f[2] != "-") thr = f[2] + 3 * f[3] } }
            thr >= 0 && $1 >= a && $1 < b && $2 <= thr { v[++n] = $2 }
            END {
                if (thr < 0) { print "null null"; exit }
                for (i = 2; i <= n; i++) { x = v[i]; j = i - 1; while (j > 0 && v[j] > x) { v[j + 1] = v[j]; j-- } v[j + 1] = x }
                if (n == 0) print "0 null"; else print n, v[int((n + 1) / 2)]
            }' "$STANDBY_STAT" 2>/dev/null)
        set -- ${_hidle:-null null}
        _hir=$1 _hiw=$2
    fi
    _hexit=$(awk '$1 ~ /^ts_/ { a[$1] = $2 } END { print 0, 0, a["ts_on_s"] + 0, a["ts_chk_s"] + 0, a["ts_ok_s"] + 0 }' "$_hacc")
    set -- $_hexit
    _hrss=",\"rss_agent\":null,\"rss_datad\":null,\"rss_devui\":null"
    [ "$_hcut" = 1 ] || _hrss=",\"rss_agent\":$(jnum "$(ledger_rss zte-agent)"),\"rss_datad\":$(jnum "$(ledger_rss zwrt-datad)"),\"rss_devui\":$(jnum "$(ledger_rss u60pro-devui)")"
    _htsr=null
    [ "$_hcut" = 1 ] || _htsr=$(jnum "$(ledger_rss tailscaled)")
    _hmix=
    _hpx=
    # Tailscale: meant on, of that checked (ledger_ts), of that healthy
    _hts=",\"ts_on_s\":$3,\"ts_chk_s\":$4,\"ts_ok_s\":$5"
    _hfrag=$(awk -v to="$_hto" -v tt="$_htx" -v rn="$_hrn" -v rk="$_hrk" -v rg="$_hrg" -v net="${_hnet:--}" \
        -v home="${_hhome:--}" -v free="$_hfree" -v dump="$_hdump" -v bud="$_hbud" -v cut="$_hcut" '
        function num(x) { return ((x ~ /^-?[0-9]+$/) ? x : "null") }
        function mwh(x) { return ((x == "") ? "null" : sprintf("%.1f", x / 1000)) } # stored as µWh
        function str(x) { return ((x == "" || x == "-") ? "null" : "\"" x "\"") }
        { a[$1] = $2 }
        END {
            printf ",\"from\":%s,\"to\":%s,\"ft\":%s,\"tt\":%s,\"awake\":%d,\"asleep\":%d,\"rounds\":%s,\"skipped\":%s,\"max_gap\":%s",
                num(a["from"]), to, num(a["ft"]), tt, a["awake"], a["asleep"], num(rn), num(rk), num(rg)
            printf ",\"net\":%s,\"home\":%s,\"abroad\":%d,\"free_mb\":%s,\"dump_mb\":%s,\"budget\":%s",
                str(net), str(home), a["abroad"], num(free), num(dump), num(bud)
            split("guard job watcher capture uidlog crashlog datad", g, " ")
            for (i = 1; i <= 7; i++) printf ",\"g_%s\":%d", g[i], a["g_" g[i]]
            if (cut == 1) printf ",\"cut\":1"
            printf "\n"
            e = ((a["n_e"] > 0) ? a["e"] : "")
            printf ",\"src\":%s,\"e\":%s", ((a["n_vi"] > 0) ? "\"vi\"" : ((a["n_e"] > 0) ? "\"counter\"" : "null")), mwh(e)
            split("on_home on_abroad off_home off_abroad", c, " ")
            for (i = 1; i <= 4; i++) printf ",\"%s_s\":%d,\"%s_e\":%s", c[i], a[c[i] "_s"], c[i], mwh(((a["n_e"] > 0) ? a[c[i] "_e"] + 0 : ""))
            printf ",\"chg_s\":%d,\"batt_from\":%s,\"batt_to\":%s,\"batt_tmax\":%s,\"zone_tmax\":%s,\"throttle_s\":%s,\"e_partial\":%d\n",
                a["chg_s"], num(a["batt_from"]), num(a["cap"]), num(a["batt_tmax"]), num(a["zone_tmax"]), ((a["n_thr"] > 0) ? a["throttle_s"] + 0 : "null"), a["e_partial"]
        }' "$_hacc")
    _hl1=$(printf '%s\n' "$_hfrag" | sed -n 1p)
    _hl2=$(printf '%s\n' "$_hfrag" | sed -n 2p)
    _hl3="$_hrss$_hmix,\"rss_ts\":$_htsr,\"cpu_datad\":$_hcpu,\"idle_rows\":$_hir,\"idle_wan\":$_hiw$_hpx$_hts"
    _hok=1
    ledger_has_id "h-$_hb8-$_hf" "$_hsq" || ledger_append hour "$_hl1" "$_hto" "h-$_hb8-$_hf" "$_hsq" || _hok=0
    ledger_has_id "hp-$_hb8-$_hf" "$_hsq" || ledger_append hour_power "$_hl2" "$_hto" "hp-$_hb8-$_hf" "$_hsq" || _hok=0
    ledger_has_id "hq-$_hb8-$_hf" "$_hsq" || ledger_append hour_proc "$_hl3" "$_hto" "hq-$_hb8-$_hf" "$_hsq" || _hok=0
    [ "$_hok" = 1 ] || log "ledger: the hour from uptime $_hf (boot $_hb8) could not be written in full"
    : >"$LTMP/summary.want" # the main loop has doctor.sh redo the summary lines
}

# The three summary lines doctor.sh's default output ends with, redone after
# each hour line (docs/LEDGER.md §2 summary). Its own background job: reading
# the whole ledger takes a while and must not hold the writer lock.
summary_job() {
    rm -f "$LTMP/summary.want"
    DOC_LEDGER_DIR=$LEDGER_DIR DOC_CRASHCAP_DIR=$CRASHCAP_DIR DOC_ALERTS=$ALERT_DIR DOC_STANDBY_STAT=$STANDBY_STAT \
        DOC_STANDBY_BASE=$STANDBY_BASE sh "$HERE/doctor.sh" --report 7d --summary >"$LEDGER_DIR/summary.tmp" 2>/dev/null &&
        mv -f "$LEDGER_DIR/summary.tmp" "$LEDGER_DIR/summary"
    rm -f "$LEDGER_DIR/summary.tmp"
}

# Coverage (docs/LEDGER.md §8): is each data source working this round?
# Sets _cbad to the sources that are not (for the hour line's g_* seconds),
# and writes a gap event when a source that was down works again.
ledger_cov() { # <awake seconds since the last round> <last round's uptime>
    _cbad=
    _cw=
    { read -r _cw <"$LTMP/w.up"; } 2>/dev/null
    _cwp=
    { read -r _cwp _rest <"$LTMP/watcher.pid"; } 2>/dev/null
    if ! isint "$_cw" || [ $((_hn - _cw)) -gt 10 ] || ! isint "$_cwp" || [ ! -d "/proc/$_cwp" ]; then _cbad="$_cbad watcher"; fi
    _ccp=$(num "$STATE/crashcap-pid")
    [ "$_ccp" -gt 0 ] && [ -d "/proc/$_ccp" ] || _cbad="$_cbad capture"
    _cul=$(tail -n 1 "$UID_LOG" 2>/dev/null)
    # just renamed or cut (ledger_uid, logcap): its last line is in .old
    [ -n "$_cul" ] || _cul=$(tail -n 1 "$UID_LOG.old" 2>/dev/null)
    case "$_cul" in [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T*) ;; *) _cbad="$_cbad uidlog" ;; esac
    [ -d "$CRASHLOG_DIR" ] && [ -r "$CRASHLOG_DIR" ] || _cbad="$_cbad crashlog"
    [ "$(cat "$LTMP/datad.read" 2>/dev/null)" = "$_hn" ] || _cbad="$_cbad datad"
    # gap events: open when a source first fails (from the last round it worked), written when it works again
    for _cs in watcher capture uidlog crashlog datad; do
        _cf=$(awk -v s="$_cs" '$1 == s { print $2; exit }' "$LTMP/cov" 2>/dev/null)
        case " $_cbad " in
            *" $_cs "*)
                [ -n "$_cf" ] || echo "$_cs $2" >>"$LTMP/cov" ;;
            *)
                [ -n "$_cf" ] || continue
                ledger_append gap ",\"src\":\"$_cs\",\"from\":$_cf,\"to\":$_hn" || return 1
                grep -v "^$_cs " "$LTMP/cov" >"$LTMP/cov.tmp"
                mv -f "$LTMP/cov.tmp" "$LTMP/cov" ;;
        esac
    done
}

# Retention (docs/LEDGER.md §2): whole boots go, oldest first, while the
# ledger is over LEDGER_TOTAL_MAX or a boot ended more than 30 days ago (by
# the wall clock of its last line, when that was trusted). Still over with
# every older boot gone (up for weeks): this boot's oldest segments go, never
# the one being written. Runs once an hour.
ledger_retain() {
    _rq=$(num "$LTMP/seq")
    _rw=$(wall_s)
    for _rs in $(ls "$LEDGER_DIR" 2>/dev/null | sed -n 's/^boot-0*\([0-9][0-9]*\)-.*\.jsonl$/\1/p' | sort -n -u); do
        [ "$_rs" -lt "$_rq" ] || break
        _rf=$(printf '%s/boot-%06d-' "$LEDGER_DIR" "$_rs")
        _rkb=$(du -sk "$LEDGER_DIR" 2>/dev/null | awk '{ print $1 }')
        _rt=$(tail -n 1 "$(ls "$_rf"*.jsonl | tail -n 1)" 2>/dev/null | sed -n 's/.*"t":\([0-9][0-9]*\),.*/\1/p')
        if isint "$_rkb" && [ $((_rkb * 1024)) -gt "$LEDGER_TOTAL_MAX" ]; then
            log "ledger: over $LEDGER_TOTAL_MAX bytes; boot $_rs removed"
        elif isint "$_rt" && [ $((_rw - _rt)) -gt 2592000 ]; then
            log "ledger: boot $_rs ended more than 30 days ago; removed"
        else
            break
        fi
        rm -f "$_rf"*.jsonl
    done
    ledger_trim_this "$_rq"
}

# ledger_trim_this <seq>: while over LEDGER_TOTAL_MAX, remove this boot's
# segments oldest part first. The newest part and the one in $LTMP/seg (the
# segment being appended to) always stay. Part numbers sort as numbers
# (%03d widens past 999).
ledger_trim_this() {
    _tc=$(cat "$LTMP/seg" 2>/dev/null)
    for _to in $(ls "$(printf '%s/boot-%06d-' "$LEDGER_DIR" "$1")"*.jsonl 2>/dev/null |
        awk -F- '{ p = $NF; sub(/[.]jsonl$/, "", p); print (p + 0), $0 }' | sort -n | sed '$d' | cut -d' ' -f2-); do
        _tkb=$(du -sk "$LEDGER_DIR" 2>/dev/null | awk '{ print $1 }')
        isint "$_tkb" && [ $((_tkb * 1024)) -gt "$LEDGER_TOTAL_MAX" ] || break
        [ "$_to" = "$_tc" ] && continue
        rm -f "$_to"
        log "ledger: over $LEDGER_TOTAL_MAX bytes with no older boot left; ${_to##*/} removed"
    done
}

ledger_hour() {
    now_up
    _hn=$_nu
    _hck=
    _hco=
    { read -r _hck _hco _rest <"$STATE/clock-ok"; } 2>/dev/null
    _hft=
    _htt=
    if [ "$_hck" = 1 ] && isint "$_hco"; then
        _hk=w$(((_hn + _hco) / 3600))
        _htt=$((_hn + _hco))
    else
        _hk=u$((_hn / 3600))
    fi
    _hft=$_htt
    # this round's readings
    rdv "$BAT/voltage_now"; _bv=$_rv
    rdv "$BAT/current_now"; _bi=$_rv
    rdv "$BAT/charge_counter"; _bq=$_rv
    rdv "$BAT/capacity"; _bc=$_rv
    isint "$_bc" || _bc=
    rdv "$BAT/temp"; _bt=$_rv
    rdv "$BAT/status"; _bs=$_rv
    rdv "$BACKLIGHT"; _bl=$_rv
    _zt=
    [ -f "$LTMP/zones" ] || ledger_zones >"$LTMP/zones"
    while read -r _z; do
        rdv "$_z/temp"
        isint "$_rv" || continue
        if [ -z "$_zt" ] || [ "$_rv" -gt "$_zt" ]; then _zt=$_rv; fi
    done <"$LTMP/zones"
    # throttled: a cpufreq policy capped below the highest cap it had this boot
    _thr=
    for _c in "$CPUFREQ"/policy*; do
        rdv "$_c/scaling_max_freq"
        isint "$_rv" || continue
        [ -n "$_thr" ] || _thr=0
        _pf=$LTMP/cpumax.${_c##*/}
        _pm=0
        { read -r _pm <"$_pf"; } 2>/dev/null
        isint "$_pm" || _pm=0
        if [ "$_rv" -gt "$_pm" ]; then echo "$_rv" >"$_pf"; elif [ "$_rv" -lt "$_pm" ]; then _thr=1; fi
    done
    _ab=0
    _hnet=
    _hhome=
    { read -r _x _x _x _x _hnet _hhome _rest <"$LTMP/net"; } 2>/dev/null
    case "$_hnet$_hhome" in *-*-*) [ "${_hnet%-*}" != "${_hhome%-*}" ] && _ab=1 ;; esac
    _chg=0
    case "$_bs" in Charging | Full) _chg=1 ;; esac
    case "$_bi" in -* | 0 | '') ;; *) isint "$_bi" && _chg=1 ;; esac # current_now > 0: charging
    # the exits (S8): meant to be on while the cellular link carries the default route, and healthy
    wan_route
    _tson=0
    [ "$_wrt" = 1 ] && grep -q -F "$TS_START" "$RC_LOCAL" 2>/dev/null && _tson=1
    _tsok=
    [ "$_tson" = 1 ] && ledger_ts
    _chon=0
    _chok=0
    _on=off
    [ "${_bl:-0}" -gt 0 ] 2>/dev/null && _on=on
    _cell=${_on}_home
    [ "$_ab" = 1 ] && _cell=${_on}_abroad
    if [ ! -s "$LTMP/acc" ]; then
        # the previous boot's last hour, from its copy in /data
        set -- $(awk '$1 == "boot" { print $2, $3; exit }' "$LEDGER_DIR/state/acc.last" 2>/dev/null)
        if [ -n "$1" ] && [ "$1" != "$(ledger_bootid | cut -c1-8)" ] && isint "$2"; then
            ledger_hour_close "$LEDGER_DIR/state/acc.last" "$1" "$2"
        fi
        rm -f "$LEDGER_DIR/state/acc.last"
        ledger_hour_open
        return 0
    fi
    _hp=$(awk '$1 == "last" { print $2; exit }' "$LTMP/acc")
    isint "$_hp" || _hp=$_hn
    _sl=0
    [ -f "$LTMP/w.slept" ] && [ "$_hn" -gt "$_hp" ] && _sl=$(awk -v a="$_hp" -v b="$_hn" '
        { f = (($1 > a) ? $1 : a); t = (($2 < b) ? $2 : b); if (t > f) s += t - f }
        END { print s + 0 }' "$LTMP/w.slept")
    ledger_cov "$_sl" "$_hp"
    # the main loop's own coverage, from its round marks (this round's is written
    # after this job): a wait between rounds of more than 180 s awake, and rounds
    # that skipped this job, since the last ledger round
    set -- $(awk -v a="$_hp" -v b="$_hn" -v sl="$_sl" -v iv="$INTERVAL" '
        $1 <= b {
            if ($1 > a) { if (p != "" && $1 - p - sl > 180) g += $1 - p - sl - iv; if ($2 == "skipped") k++ }
            p = $1 }
        END { if (p != "" && b - p - sl > 180) g += b - p - sl - iv; print g + 0, k * iv }' "$LTMP/rounds" 2>/dev/null)
    _cgg=${1:-0} _cgj=${2:-0}
    awk -v now="$_hn" -v sl="$_sl" -v cell="$_cell" -v chg="$_chg" -v ab="$_ab" -v v="$_bv" -v i="$_bi" -v q="$_bq" -v cap="$_bc" \
        -v bt="$_bt" -v zt="$_zt" -v thr="$_thr" -v bad="$_cbad" -v gg="$_cgg" -v gj="$_cgj" \
        -v chon="$_chon" -v chok="$_chok" -v tson="$_tson" -v tsok="$_tsok" -v out="$LTMP/acc.tmp" '
        function isnum(x) { return (x ~ /^-?[0-9]+$/) }
        { a[$1] = substr($0, index($0, " ") + 1) } # the whole value: the datad line has three
        END {
            dt = now - a["last"]
            if (dt < 0) dt = 0
            if (sl > dt) sl = dt
            awake = dt - sl
            a["awake"] += awake; a["asleep"] += sl
            if (ab == 1) a["abroad"] += awake
            e = ""; es = 0
            if (isnum(q) && isnum(a["q"]) && isnum(v)) { e = (a["q"] - q) * v / 1e6; es = dt }  # µAh x µV / 1e6 = µWh
            else if (isnum(v) && isnum(i) && i < 0) { e = v * (0 - i) * awake / 3.6e9; es = awake; a["n_vi"]++; if (sl > 0) a["e_partial"] = 1 }
            else if (dt > 0) a["e_partial"] = 1
            if (chg == 1) a["chg_s"] += dt
            else {
                a[cell "_s"] += ((e == "") ? awake : es)
                if (e != "" && e >= 0) { a[cell "_e"] += e; a["e"] += e; a["n_e"]++ }
            }
            if (isnum(bt) && (a["batt_tmax"] == "" || bt + 0 > a["batt_tmax"] + 0)) a["batt_tmax"] = bt
            if (isnum(zt) && (a["zone_tmax"] == "" || zt + 0 > a["zone_tmax"] + 0)) a["zone_tmax"] = zt
            if (thr != "") { a["n_thr"]++; if (thr == 1) a["throttle_s"] += awake }
            nb = split(bad, b, " ")
            for (j = 1; j <= nb; j++) a["g_" b[j]] += awake
            a["g_guard"] += gg; a["g_job"] += gj
            if (tson == 1) {
                a["ts_on_s"] += awake
                if (tsok != "") { a["ts_chk_s"] += awake; if (tsok == 1) a["ts_ok_s"] += awake }
            }
            a["last"] = now
            a["q"] = (isnum(q) ? q : "")
            if (isnum(cap)) a["cap"] = cap
            for (x in a) if (a[x] != "" || x == "q") print x, a[x] > out
        }' "$LTMP/acc" && mv -f "$LTMP/acc.tmp" "$LTMP/acc"
    # a copy in /data: after a reboot, the next boot writes this hour out (cut:1) instead of losing it
    { echo "boot $(ledger_bootid | cut -c1-8) $(num "$LTMP/seq")" && cat "$LTMP/acc"; } >"$LEDGER_DIR/state/acc.last.tmp" 2>/dev/null &&
        mv -f "$LEDGER_DIR/state/acc.last.tmp" "$LEDGER_DIR/state/acc.last"
    [ "$(awk '$1 == "key" { print $2; exit }' "$LTMP/acc")" = "$_hk" ] && return 0
    ledger_hour_close
    ledger_hour_open
    ledger_retain
}

# The background ledger job: one at a time (writer lock), never waited for.
ledger_job() {
    exec 8>>"$LTMP/writer.lock" || exit 0
    flock -n 8 || exit 0
    ledger_ready || exit 0
    ledger_seq_init || exit 0
    [ -f "$LTMP/boot.done" ] || ledger_boot
    ledger_starts
    ledger_recovery
    LEDGER_DEADLINE=$(($(uptime_s) + LEDGER_READ_BUDGET))
    if [ -f "$LTMP/boot.done" ]; then
        ledger_crashlog round
        ledger_procs
        ledger_ver_changed
        ledger_uid
        ledger_datad
        ledger_net
    fi
    ledger_clock
    ledger_wall
    ledger_drain
    [ -f "$LTMP/boot.done" ] && ledger_hour
}

# ── data service degraded (marker from zte-agent datad_feed.rs) ─────────────
#
# Alert once per degraded episode (an episode = one start time in the marker)
# when the marker is fresh and the fallback started more than DATAD_AFTER ago.
# A stale marker is left alone and not alerted: the agent refreshes it every
# 60 s, so staleness means the agent is gone and agent-silent covers it. We
# never delete the marker and never ask datad ourselves.
file_mtime() { # epoch mtime of <file>, empty if unknown
    date -r "$1" +%s 2>/dev/null || stat -c %Y "$1" 2>/dev/null
}

datad_round() {
    mkdir -p "$STATE"
    if [ ! -f "$DATAD_MARKER" ]; then
        rm -f "$STATE"/alerted-datad-*
        return 0
    fi
    # Just booted: the marker on /data may be the last boot's, and the agent
    # deletes it when it starts. Give it the same grace as the heartbeat.
    [ "$(uptime_s)" -lt "$GRACE" ] && return 0
    _now=$(wall_s)
    clock_ok || return 0
    _start=
    { read -r _start; read -r _reason; } 2>/dev/null <"$DATAD_MARKER"
    case "$_start" in '' | *[!0-9]*) return 0 ;; esac
    _mt=$(file_mtime "$DATAD_MARKER")
    case "$_mt" in '' | *[!0-9]*) return 0 ;; esac
    [ $((_now - _mt)) -lt "$DATAD_FRESH" ] || return 0
    [ $((_now - _start)) -gt "$DATAD_AFTER" ] || return 0
    alert_once "datad-$_start" datad-degraded "data service degraded for $((_now - _start))s: ${_reason:-unknown}"
}

lanv6_round() {
    [ -f "$LAN_V6_FLAG" ] || return 0
    _v=; { read -r _v <"$LAN_V6_SYSCTL"; } 2>/dev/null
    [ -z "$_v" ] || [ "$_v" = 1 ] && return 0
    echo 1 >"$LAN_V6_SYSCTL" 2>/dev/null && log "LAN IPv6 was on again: disabled it on br-lan"
}

mss_recovery_round() {
    _v=; { read -r _v <"$MSS_RECOVERY"; } 2>/dev/null
    [ "$_v" = disabled ] || return 0
    echo enabled >"$MSS_RECOVERY" 2>/dev/null && log "modem crash recovery was disabled: enabled it" &&
        { mkdir -p "$STATE" && uptime_s >"$STATE/recovery-set"; } 2>/dev/null
}

# ── kernel log to flash ─────────────────────────────────────────────────────

crashcap_file() {
    _b=$(cut -c1-8 "$BOOT_ID_FILE" 2>/dev/null)
    echo "$CRASHCAP_DIR/kmsg-${_b:-boot}.log"
}

# Which capture files stay: this boot's and the newest $CRASHCAP_KEEP-1 others
# in boot order ($CRASHCAP_DIR/.order, one line per boot, added when its guard
# starts). Not by mtime: a boot that ended before NTP leaves a 2025 mtime, and
# in a reboot loop those short boots are the ones that say what happened.
# Files that were never on the list (older than it) come last, newest first.
crashcap_order() { # <file>: this boot's file on the list, once
    _co=$CRASHCAP_DIR/.order
    grep -q -x -F "$1" "$_co" 2>/dev/null && return 0
    echo "$1" >>"$_co" && fsync_file "$_co"
    if [ "$(wc -l <"$_co")" -gt 50 ]; then
        tail -n 20 "$_co" >"$_co.tmp" && fsync_file "$_co.tmp" && mv -f "$_co.tmp" "$_co"
    fi
}
crashcap_prune() { # <this boot's file>: it counts as kept even before it exists
    ls -t "$CRASHCAP_DIR"/kmsg-*.log 2>/dev/null | awk -v f="$1" -v k="$CRASHCAP_KEEP" -v ord="$CRASHCAP_DIR/.order" '
        BEGIN { while ((getline l < ord) > 0) { no++; o[no] = l; on[l] = 1 } }
        { nt++; t[nt] = $0; ex[$0] = 1 }
        END {
            keep[f] = 1
            n = 1
            for (i = no; i > 0 && n < k; i--) if ((o[i] in ex) && !(o[i] in keep)) { keep[o[i]] = 1; n++ }
            for (i = 1; i <= nt && n < k; i++) if (!(t[i] in on) && !(t[i] in keep)) { keep[t[i]] = 1; n++ }
            for (i = 1; i <= nt; i++) if (!(t[i] in keep)) print t[i]
        }' | while read -r _old; do rm -f "$_old"; done
}

# Start the reader for this boot unless it is already running. The reader is
# a `sh -c` whose pid is what we remember: it outlives nothing, so when the
# pipeline ends the pid is gone and the fsync loop stops too.
# A reader started again in the same boot (crashcap_keep, or a guard restart)
# skips the records this boot's file already has: a new reader of /dev/kmsg
# starts at the oldest record the kernel still holds. A record's continuation
# lines (" KEY=value") go with it.
crashcap_start() {
    [ -e "$KMSG" ] || return 0
    mkdir -p "$CRASHCAP_DIR" "$STATE" 2>/dev/null || return 0
    _f=$(crashcap_file)
    crashcap_order "$_f"
    _p=$(num "$STATE/crashcap-pid")
    [ "$_p" -gt 0 ] && [ -d "$PROC/$_p" ] && return 0
    crashcap_prune "$_f"
    _cs=-1
    if [ -s "$_f" ]; then
        _cs=$(tail -n 200 "$_f" 2>/dev/null | awk -F, '/^[0-9]+,[0-9]+,/ { s = $2 } END { print s + 0 }')
        isint "$_cs" || _cs=-1
    fi
    sh -c 'cat "$1" | awk -v s="$4" "$3" >>"$2"' sh "$KMSG" "$_f" \
        'BEGIN { k = 1 } /^[0-9]+,[0-9]+,/ { split($0, f, ","); k = (f[2] + 0 > s) } k && !/ audit: | avc: / { print; fflush() }' "$_cs" &
    _p=$!
    echo "$_p" >"$STATE/crashcap-pid"
    echo "$(($(num "$STATE/crashcap-starts") + 1))" >"$STATE/crashcap-starts"
    uptime_s >"$STATE/crashcap-started"
    # fsync only this file (run.sh used to sync the whole filesystem every 0.3 s)
    sh -c 'while [ -d "$1/$2" ]; do dd if=/dev/null of="$3" conv=notrunc,fsync 2>/dev/null; sleep 2; done' \
        sh "$PROC" "$_p" "$_f" &
    log "kernel log capture to $_f (pid $_p)"
}

# The reader ends when cat does: EPIPE once the kernel has overwritten records
# it had not read yet, or anything else. Start it again from the main loop,
# at most CRASHCAP_RESTARTS times a boot and CRASHCAP_RESPAWN s after its
# last start, so a reader that cannot stay up does not spin.
crashcap_keep() {
    [ -e "$KMSG" ] && [ -f "$STATE/crashcap-pid" ] || return 0
    _p=$(num "$STATE/crashcap-pid")
    [ "$_p" -gt 0 ] && [ -d "$PROC/$_p" ] && return 0
    _n=$(num "$STATE/crashcap-starts")
    if [ "$_n" -gt "$CRASHCAP_RESTARTS" ]; then
        [ -f "$STATE/crashcap-gaveup" ] || { log "kernel log capture (pid $_p) ended: started again $CRASHCAP_RESTARTS times this boot already, not again"; : >"$STATE/crashcap-gaveup"; }
        return 0
    fi
    [ $(($(uptime_s) - $(num "$STATE/crashcap-started"))) -ge "$CRASHCAP_RESPAWN" ] || return 0
    log "kernel log capture (pid $_p) ended: starting it again"
    crashcap_start
}

crashcap_round() {
    _f=$(crashcap_file)
    file_size "$_f"
    [ -n "$_fsz" ] && [ "$_fsz" -gt "$CRASHCAP_MAX" ] || return 0
    tail -c $((CRASHCAP_MAX / 2)) "$_f" >"$_f.tmp" && cat "$_f.tmp" >"$_f" && rm -f "$_f.tmp" || return 0
    # the crash watcher's position in the file is gone: it reads it again (docs/LEDGER.md §9)
    mkdir -p "$STATE" && echo $(($(num "$STATE/crashcap-cuts") + 1)) >"$STATE/crashcap-cuts"
    log "cut $_f at $_fsz bytes"
}

# ── crash watcher (docs/LEDGER.md §9) ───────────────────────────────────────
# `u60-guard.sh watcher`, kept running at this script's version by
# watcher_round. Every WATCH_INTERVAL s it reads what the capture appended
# since the last loop, and
#   - opens a modem crash on its first kernel line, follows the cellular link
#     until it is back (or 10 min), and notes the dump files of the next 90 s;
#   - notes out-of-memory kills, the vendor's thermal throttling level, and
#     kernel suspend lines (the only evidence that a long pause was sleep, §8).
# It writes spool files only (§5); the ledger job takes them in. Its state is
# $LTMP/w.* (tmpfs), so a restarted watcher carries on where the last stopped.
# The watcher leaves the capture pipeline alone (crashcap_keep restarts it).

isint() { case "$1" in '' | *[!0-9]*) return 1 ;; esac; }
now_up() { # _nu = whole seconds of uptime, _nud = the same with one decimal (no fork)
    _nx=
    { read -r _nx _rest <"$UPTIME_FILE"; } 2>/dev/null
    _nu=${_nx%%.*}
    isint "$_nu" || _nu=0
    _nf=${_nx#*.}
    [ "$_nf" = "$_nx" ] && _nf=0
    _nf=${_nf%"${_nf#?}"}
    isint "$_nf" || _nf=0
    _nud=$_nu.$_nf
}
file_size() { # _fsz = size of <file> from its inode (busybox wc -c reads it all), empty if missing
    _fsz=
    set -- $(ls -ln "$1" 2>/dev/null)
    isint "$5" && _fsz=$5
}
jdash() { # a short JSON string (16 chars: the ssr fragment must stay under 400 bytes), or null for empty or -
    case "$1" in '' | -) printf null ;; *) jstr "$(printf '%s' "$1" | cut -c1-16)" ;; esac
}

# spool_put data|tmp <id> <kind> <uptime> <fragment> — 0 once the event is
# spooled (docs/LEDGER.md §5). "data" events must survive a reboot: temp name,
# fsync, rename, sync. When /data is nearly full, holds 200 events already, or
# will not take the file, they go to /tmp marked lowspace — still in the
# ledger this boot, rather than lost.
spool_put() {
    _pf=$5
    if [ "$1" = data ]; then
        _pd=$LEDGER_DIR/spool
        _pn=$(ls "$_pd" 2>/dev/null | wc -l)
        if [ ! -f "$LTMP/lowspace" ] && [ "${_pn:-0}" -le 200 ]; then
            [ -e "$_pd/$2.ev" ] && return 0
            if { mkdir -p "$_pd" &&
                printf '%s\t%s\t%s\t%s\n%s\n' "$2" "$W_BOOT" "$4" "$3" "$_pf" >"$_pd/$2.tmp" &&
                fsync_file "$_pd/$2.tmp" && mv -f "$_pd/$2.tmp" "$_pd/$2.ev" && sync; } 2>/dev/null; then
                return 0
            fi
            rm -f "$_pd/$2.tmp"
        fi
        _pf="$_pf,\"lowspace\":1"
    fi
    [ -e "$SPOOL_TMP/$2.ev" ] && return 0
    { mkdir -p "$SPOOL_TMP" &&
        printf '%s\t%s\t%s\t%s\n%s\n' "$2" "$W_BOOT" "$4" "$3" "$_pf" >"$SPOOL_TMP/$2.tmp" &&
        mv -f "$SPOOL_TMP/$2.tmp" "$SPOOL_TMP/$2.ev"; } 2>/dev/null
}

# What the capture holds from byte <from> on, one line per finding:
#   C <seq> <kernel s> <text>   modem crash      O <seq> 0 <text>  oom kill
#   H <seq> 0 <text>            thermal level    P <seq> 0 -       suspend
# then "E <bytes of complete lines> <last seq> -". Only records newer than
# <last seq> count, so a region read twice finds nothing twice; a line still
# missing its newline is left for the next loop. /dev/kmsg escapes every byte
# outside printable ASCII, so length() counts bytes.
watch_read() { # <file> <from byte> <last seq>
    { dd if="$1" iflag=skip_bytes skip="$2" bs=65536 2>/dev/null; printf '\n\001EOF\n'; } | awk -v last="$3" '
        function take(l,   f, s, m, lm) {
            b += length(l) + 1
            if (l !~ /^[0-9]+,[0-9]+,[0-9]+,[^;]*;/) return # continuation line, or the torn start of a cut file
            split(l, f, ",")
            s = f[2] + 0
            if (s <= last) return
            last = s
            m = substr(l, index(l, ";") + 1)
            lm = tolower(m)
            if (lm ~ /fatal error received|watchdog received|crash detected|rproc recovery|recovering .*remoteproc|subsystem restart|err_fatal/)
                print "C", s, int(f[3] / 1000000), m
            else if (m ~ /[Oo]ut of memory: Kill/) print "O", s, 0, m
            else if (m ~ /zte_thermal_update_throttling_level/) print "H", s, 0, m
            else if (m ~ /PM: (suspend|resume)/) print "P", s, 0, "-"
        }
        $0 == "\001EOF" { print "E", b + 0, last, "-"; exit }
        { if (have) take(held); held = $0; have = 1 }'
}

wan_route() { # _wrt = 1 when the main table's default route leaves by WAN_IF
    _wrt=0
    while read -r _ri _rd _rest; do
        if [ "$_ri" = "$WAN_IF" ] && [ "$_rd" = 00000000 ]; then
            _wrt=1
            break
        fi
    done 2>/dev/null <"$ROUTE"
}
wan_rx() { # _wrx = bytes received on WAN_IF, _wpk = packets both ways; empty when it is gone
    _wrx=
    _wpk=
    while IFS=' :' read -r _if _rb _rp _re _rd _rf _rfr _rc _rm _tb _tp _rest; do
        if [ "$_if" = "$WAN_IF" ]; then
            isint "$_rb" && _wrx=$_rb
            isint "$_rp" && isint "$_tp" && _wpk=$((_rp + _tp))
            break
        fi
    done 2>/dev/null <"$NETDEV"
}
wan_sample() { # _wup = 1 when the link is up: an IPv4 address, and the default route out of it
    wan_route
    _wad=0
    case "$($IP -4 -o addr show dev "$WAN_IF" 2>/dev/null)" in *"inet "*) _wad=1 ;; esac
    wan_rx
    _wup=0
    # a device that never had its default route in the main table: the address alone
    if [ "$_wad" = 1 ] && { [ "$_wrt" = 1 ] || [ "$W_RTSEEN" = 0 ]; }; then _wup=1; fi
}

# The open crash: S_ID S_T0 (uptime when found) S_KTS (kernel seconds of its
# first line) S_DOWN S_UP (uptime the link went down / came back, or -)
# S_RXL (last rx bytes) S_RXAT (first new traffic, or -) S_LAST (last sample)
# S_GAP (seconds without samples) S_RES S_SLEPT S_ND (no drop in 30 s).
watch_save_ssr() {
    echo "$S_ID $S_T0 $S_KTS $S_DOWN $S_UP $S_RXL $S_RXAT $S_LAST $S_GAP $S_RES $S_SLEPT $S_ND" >"$LTMP/w.ssr"
}
watch_ssr_valid() {
    case "$S_ID" in ssr-*) ;; *) return 1 ;; esac
    for _v in "$S_T0" "$S_KTS" "$S_LAST" "$S_GAP"; do isint "$_v" || return 1; done
    for _v in "$S_DOWN" "$S_UP" "$S_RXL" "$S_RXAT"; do [ "$_v" = - ] || isint "$_v" || return 1; done
    for _v in "$S_RES" "$S_SLEPT" "$S_ND"; do case "$_v" in 0 | 1) ;; *) return 1 ;; esac; done
}

watch_result() { # <ok|no_drop|merged|timeout>: how the open crash ended; closes it
    _rr=null
    _rx=null
    _rd=0
    [ "$S_DOWN" = - ] || _rd=1
    [ "$1" = ok ] && [ "$S_UP" != - ] && _rr=$((S_UP - S_T0))
    case $1 in ok | no_drop) [ "$S_RXAT" = - ] || _rx=$((S_RXAT - S_T0)) ;; esac
    spool_put data "res-$S_ID" ssr_result "$_nud" ",\"ssr\":\"$S_ID\",\"result\":\"$1\",\"recovered_s\":$_rr,\"rx_seen_s\":$_rx,\"down_seen\":$_rd,\"resumed\":$S_RES,\"gap_s\":$S_GAP,\"slept\":$S_SLEPT" || return 1
    S_ID=
    rm -f "$LTMP/w.ssr"
}

watch_open() { # <kmsg seq> <kernel s> <text>
    _oid=ssr-$W_B8-$1
    _nt=
    _nrat=
    _nb=
    _nnr=
    _nnet=
    _nhome=
    # the network as the ledger job last saw it, and how old that is
    { read -r _nt _nrat _nb _nnr _nnet _nhome _rest <"$LTMP/net"; } 2>/dev/null
    _nage=null
    isint "$_nt" && [ "$_nt" -le "$_nu" ] && _nage=$((_nu - _nt))
    _ol=${3#*fatal error received: } # the modem's own assertion, when it gives one
    spool_put data "$_oid" ssr "$_nud" ",\"kseq\":$1,\"line\":$(jstr "$_ol"),\"rat\":$(jdash "$_nrat"),\"band\":$(jdash "$_nb"),\"nrband\":$(jdash "$_nnr"),\"net\":$(jdash "$_nnet"),\"home\":$(jdash "$_nhome"),\"net_age\":$_nage,\"wan_ppm\":$(jnum "$W_PPM")" || return 1
    W_SSRLAST=$1
    W_SSRKTS=$2
    echo "$W_SSRLAST $W_SSRKTS" >"$LTMP/w.ssrlast"
    wan_sample
    S_ID=$_oid S_T0=$_nu S_KTS=$2 S_DOWN=- S_UP=- S_RXL=${_wrx:--} S_RXAT=- S_LAST=$_nu S_GAP=0 S_RES=0 S_SLEPT=0 S_ND=0
    if [ "$W_CATCHUP" = 1 ]; then # found in the first read after a start: we were not watching
        S_RES=1
        [ "$_nu" -ge "$W_PREV" ] && S_GAP=$((_nu - W_PREV))
    fi
    [ "$_wup" = 0 ] && S_DOWN=$_nu
    watch_save_ssr
    watch_dump_open
}

watch_crash() { # <kmsg seq> <kernel s> <text>: open, merge or ignore (docs/LEDGER.md §9)
    [ "$1" -le "$W_SSRLAST" ] && return 0 # opened already: a region read again after a restart
    if [ -n "$S_ID" ]; then
        if [ "$S_UP" != - ]; then
            watch_result ok || return 1 # back, still waiting for traffic: it ends here
        elif [ "$S_ND" = 1 ]; then
            watch_result no_drop || return 1
        elif [ $(($2 - S_KTS)) -le 60 ]; then
            return 0 # the same crash, still printing (kernel time: exact even in a catch-up read)
        else
            watch_result merged || return 1
        fi
    fi
    watch_open "$1" "$2" "$3"
}

watch_follow() { # one sample of the link while a crash is open
    [ -n "$S_ID" ] || return 0
    [ "$S_LAST" = "$_nu" ] && return 0 # opened in this loop: sampled already
    _fe=$((_nu - S_LAST))
    [ "$_fe" -gt "$WATCH_GAP" ] && S_GAP=$((S_GAP + _fe - WATCH_INTERVAL))
    S_LAST=$_nu
    wan_sample
    _fr=0
    isint "$_wrx" && isint "$S_RXL" && [ "$_wrx" -gt "$S_RXL" ] && _fr=1
    S_RXL=${_wrx:--}
    _fa=$((_nu - S_T0))
    if [ "$S_ND" = 1 ]; then # no drop in 30 s: waiting for traffic, up to 90 s
        [ "$_fr" = 1 ] && S_RXAT=$_nu
        if [ "$S_RXAT" != - ] || [ "$_fa" -ge 90 ]; then watch_result no_drop; fi
    elif [ "$S_DOWN" = - ]; then
        if [ "$_wup" = 0 ]; then
            S_DOWN=$_nu
        else
            [ "$_fr" = 1 ] && [ "$S_RXAT" = - ] && S_RXAT=$_nu
            if [ "$_fa" -ge 30 ]; then
                S_ND=1
                [ "$S_RXAT" = - ] || watch_result no_drop
            fi
        fi
    elif [ "$S_UP" = - ]; then
        if [ "$_wup" = 1 ]; then
            S_UP=$_nu
            S_RXAT=-
        elif [ "$_fa" -ge 600 ]; then
            watch_result timeout
        fi
    else
        [ "$_fr" = 1 ] && S_RXAT=$_nu
        if [ "$S_RXAT" != - ] || [ $((_nu - S_UP)) -ge 60 ]; then watch_result ok; fi
    fi
    [ -z "$S_ID" ] || watch_save_ssr
}

# Dump files: whatever appears in DUMP_DIR within 90 s of a crash goes with it
# (docs/LEDGER.md §9). A file is reported once its size held still for a
# loop, or when the window ends; names already there before are not news.
watch_dump_base() {
    ls -ln "$DUMP_DIR" 2>/dev/null | awk 'NF >= 9 && $1 ~ /^-/ { print $9 }' >"$LTMP/w.dbase"
    W_DBUP=$_nu
}
watch_dump_open() {
    [ -n "$W_DUMP" ] && watch_dump_step 1 # the previous crash's window ends here
    W_DUMP="$S_ID $((_nu + 90))"
    echo "$W_DUMP" >"$LTMP/w.dump"
    : >"$LTMP/w.dsz"
    : >"$LTMP/w.drep"
}
watch_dump_step() { # [1 = end the window now]
    [ -n "$W_DUMP" ] || return 0
    _dfin=${1:-0}
    [ "$_nu" -ge "${W_DUMP#* }" ] && _dfin=1
    ls -ln "$DUMP_DIR" 2>/dev/null | awk -v fin="$_dfin" -v bf="$LTMP/w.dbase" -v sf="$LTMP/w.dsz" -v rf="$LTMP/w.drep" '
        BEGIN {
            while ((getline l < bf) > 0) old[l] = 1
            while ((getline l < rf) > 0) old[l] = 1
            while ((getline l < sf) > 0) { split(l, a, " "); was[a[1]] = a[2] }
        }
        NF >= 9 && $1 ~ /^-/ && !($9 in old) {
            if (fin || ($9 in was && was[$9] == $5)) print "R", $9, $5
            else print "S", $9, $5
        }' >"$LTMP/w.dout"
    : >"$LTMP/w.dsz"
    _dok=1
    while read -r _dt _dn _ds; do
        if [ "$_dt" = R ] &&
            spool_put data "dump-$W_B8-$(printf '%s' "$_dn" | md5sum | cut -c1-8)" ssr_dump "$_nud" \
                ",\"ssr\":\"${W_DUMP%% *}\",\"file\":$(jstr "$_dn"),\"size\":$(jnum "$_ds")"; then
            echo "$_dn" >>"$LTMP/w.drep"
        else
            [ "$_dt" = R ] && _dok=0
            echo "$_dn $_ds" >>"$LTMP/w.dsz"
        fi
    done <"$LTMP/w.dout"
    if [ "$_dfin" = 1 ] && [ "$_dok" = 1 ]; then
        W_DUMP=
        rm -f "$LTMP/w.dump" "$LTMP/w.dsz" "$LTMP/w.drep"
        watch_dump_base
    fi
}

watch_sleep() { # <pm|stats|inferred>: the pending pause becomes a sleep event (docs/LEDGER.md §8)
    set -- "$1" $W_GAP
    W_NS=$((W_NS + 1))
    echo "$W_NL $W_NS" >"$LTMP/w.cnt"
    spool_put tmp "sleep-$W_B8-$W_NS" sleep "$3" ",\"from\":$2,\"to\":$3,\"ev\":\"$1\""
    if [ "$1" != inferred ]; then
        [ -n "$S_ID" ] && [ "$3" -gt "$S_T0" ] && S_SLEPT=1
        # the ledger job's hour line counts only this evidenced sleep as asleep (C14)
        echo "$2 $3 $1" >>"$LTMP/w.slept"
        if [ "$(wc -l <"$LTMP/w.slept")" -gt 200 ]; then
            tail -n 100 "$LTMP/w.slept" >"$LTMP/w.slept.tmp" && mv -f "$LTMP/w.slept.tmp" "$LTMP/w.slept"
        fi
    fi
    W_GAP=
}

watch_link() { # a link event whenever the default route comes or goes
    wan_route
    [ "$_wrt" = 1 ] && W_RTSEEN=1
    if [ -n "$W_LINK" ] && [ "$_wrt" != "$W_LINK" ]; then
        W_NL=$((W_NL + 1))
        echo "$W_NL $W_NS" >"$LTMP/w.cnt"
        _lc=other
        [ -n "$S_ID" ] && _lc=ssr
        _ls=down
        [ "$_wrt" = 1 ] && _ls=up
        spool_put tmp "link-$W_B8-$W_NL" link "$_nud" ",\"state\":\"$_ls\",\"cause\":\"$_lc\",\"res\":$((_nu - W_LINKUP))"
    fi
    [ "$_wrt" = "$W_LINK" ] || echo "$_wrt $W_RTSEEN" >"$LTMP/w.link"
    W_LINK=$_wrt
    W_LINKUP=$_nu
}

watch_ppm() { # packets a minute on WAN_IF over the last full minute (the ssr event's wan_ppm)
    [ $((_nu - W_PKUP)) -ge 60 ] || return 0
    wan_rx
    W_PPM=
    if isint "$_wpk" && isint "$W_PK" && [ "$_wpk" -ge "$W_PK" ] && [ "$W_PKUP" -gt 0 ]; then
        W_PPM=$(((_wpk - W_PK) * 60 / (_nu - W_PKUP)))
    fi
    W_PK=$_wpk
    W_PKUP=$_nu
    echo "$W_PKUP ${W_PK:--} ${W_PPM:--}" >"$LTMP/w.pk"
}

watch_load() { # the last watcher's state; anything damaged starts over
    W_POS=0 W_KSEQ=0 W_CUTS=0
    { read -r W_POS W_KSEQ W_CUTS <"$LTMP/w.pos"; } 2>/dev/null
    isint "$W_POS" && isint "$W_KSEQ" && isint "$W_CUTS" || { W_POS=0 W_KSEQ=0 W_CUTS=0; }
    W_PREV=0
    { read -r W_PREV <"$LTMP/w.up"; } 2>/dev/null
    isint "$W_PREV" || W_PREV=0
    W_SSRLAST=0 W_SSRKTS=0
    { read -r W_SSRLAST W_SSRKTS <"$LTMP/w.ssrlast"; } 2>/dev/null
    isint "$W_SSRLAST" && isint "$W_SSRKTS" || { W_SSRLAST=0 W_SSRKTS=0; }
    W_NL=0 W_NS=0
    { read -r W_NL W_NS <"$LTMP/w.cnt"; } 2>/dev/null
    isint "$W_NL" && isint "$W_NS" || { W_NL=0 W_NS=0; }
    W_LINK= W_RTSEEN=0
    { read -r W_LINK W_RTSEEN <"$LTMP/w.link"; } 2>/dev/null
    case "$W_LINK" in 0 | 1) ;; *) W_LINK= ;; esac
    [ "$W_RTSEEN" = 1 ] || W_RTSEEN=0
    W_LINKUP=$W_PREV
    W_PKUP=0 W_PK= W_PPM=
    { read -r W_PKUP W_PK W_PPM <"$LTMP/w.pk"; } 2>/dev/null
    isint "$W_PKUP" || W_PKUP=0
    isint "$W_PK" || W_PK=
    isint "$W_PPM" || W_PPM=
    W_DUMP=
    { read -r W_DUMP <"$LTMP/w.dump"; } 2>/dev/null
    case "$W_DUMP" in ssr-*" "*) isint "${W_DUMP#* }" || W_DUMP= ;; *) W_DUMP= ;; esac
    W_SUS=
    { read -r W_SUS <"$SUSPEND_STATS"; } 2>/dev/null
    isint "$W_SUS" || W_SUS=
    W_GAP= W_EVUP=0 W_EVH= W_READLOG=0 W_DBUP=0
    S_ID=
    { read -r S_ID S_T0 S_KTS S_DOWN S_UP S_RXL S_RXAT S_LAST S_GAP S_RES S_SLEPT S_ND <"$LTMP/w.ssr"; } 2>/dev/null
    if [ -n "$S_ID" ]; then
        if watch_ssr_valid; then
            S_RES=1 # a crash the last watcher was following: carry on (docs/LEDGER.md §9)
        else
            log "watcher: damaged w.ssr dropped"
            S_ID=
            rm -f "$LTMP/w.ssr"
        fi
    fi
}

watch_step() {
    now_up
    _el=0
    [ "$W_PREV" -gt 0 ] && [ "$_nu" -gt "$W_PREV" ] && _el=$((_nu - W_PREV))
    _read=1 # 1 = all the capture holds has been read and acted on: the heartbeat (§8)
    _pm=0
    _cuts=0
    { read -r _cuts <"$STATE/crashcap-cuts"; } 2>/dev/null
    isint "$_cuts" || _cuts=0
    file_size "$W_FILE"
    if [ -n "$_fsz" ]; then
        _pos=$W_POS
        # cut in place: read it all again; the seq check skips what was seen
        [ "$_cuts" != "$W_CUTS" ] || [ "$_fsz" -lt "$_pos" ] && _pos=0
        if [ "$_fsz" -gt "$_pos" ]; then
            _read=0
            _npos=
            _nkseq=
            _ok=1
            watch_read "$W_FILE" "$_pos" "$W_KSEQ" >"$LTMP/w.out"
            while read -r _t _s _k _m; do
                case $_t in
                    E) _npos=$_s _nkseq=$_k ;;
                    C) [ "$W_BASE" = 1 ] || watch_crash "$_s" "$_k" "$_m" || _ok=0 ;;
                    O) [ "$W_BASE" = 1 ] || spool_put data "oom-$W_B8-$_s" oom "$_nud" ",\"line\":$(jstr "$_m")" || _ok=0 ;;
                    H) [ "$W_BASE" = 1 ] || spool_put tmp "th-$W_B8-$_s" thermal "$_nud" \
                        ",\"level\":$(jopt "$(printf '%s' "$_m" | sed -n 's/.*throttling_level() to \(0x[0-9a-fA-F]*\).*/\1/p')"),\"line\":$(jstr "$_m")" || _ok=0 ;;
                    P) [ "$W_BASE" = 1 ] || _pm=1 ;;
                esac
            done <"$LTMP/w.out"
            if isint "$_npos" && isint "$_nkseq"; then
                if [ "$_ok" = 1 ]; then # everything found is spooled: only now move on (§1)
                    W_POS=$((_pos + _npos)) W_KSEQ=$_nkseq W_CUTS=$_cuts
                    echo "$W_POS $W_KSEQ $W_CUTS" >"$LTMP/w.pos"
                    _read=1
                fi
            elif [ "$W_READLOG" = 0 ]; then
                W_READLOG=1
                log "watcher: nothing read from $W_FILE at byte $_pos of $_fsz"
            fi
        elif [ "$_pos" != "$W_POS" ] || [ "$_cuts" != "$W_CUTS" ]; then
            W_POS=$_pos W_CUTS=$_cuts
            echo "$W_POS $W_KSEQ $W_CUTS" >"$LTMP/w.pos"
        fi
        if [ "$W_BASE" = 1 ] && [ "$_read" = 1 ]; then
            # first go-live: this boot so far happened before the watcher existed
            W_BASE=0
            { mkdir -p "$LEDGER_DIR/state" && : >"$LEDGER_DIR/state/watch-init"; } 2>/dev/null
            log "watcher: first go-live; the $W_POS bytes captured so far are taken as seen"
        fi
    fi
    # suspend evidence: a kernel suspend line read now, or the suspend counter went up
    _sus=
    { read -r _sus <"$SUSPEND_STATS"; } 2>/dev/null
    _evh=
    if isint "$_sus"; then
        isint "$W_SUS" && [ "$_sus" -gt "$W_SUS" ] && _evh=stats
        W_SUS=$_sus
    fi
    [ "$_pm" = 1 ] && _evh=pm
    if [ "$W_CATCHUP" != 1 ] && [ "$_el" -gt "$WATCH_GAP" ]; then
        [ -n "$W_GAP" ] && watch_sleep inferred
        W_GAP="$((W_PREV + WATCH_INTERVAL)) $_nu $((_nu + 3 * WATCH_INTERVAL))"
        # the suspend line may have been read in the loop just before the pause
        [ -z "$_evh" ] && [ "$W_EVUP" = "$W_PREV" ] && _evh=$W_EVH
    fi
    _evused=0
    if [ -n "$W_GAP" ]; then
        if [ -n "$_evh" ]; then
            watch_sleep "$_evh"
            _evused=1
        elif [ "$_nu" -ge "${W_GAP##* }" ]; then
            watch_sleep inferred
        fi
    fi
    if [ -n "$_evh" ] && [ "$_evused" = 0 ]; then W_EVUP=$_nu W_EVH=$_evh; else W_EVUP=0; fi
    watch_follow
    watch_dump_step
    [ -z "$W_DUMP" ] && [ $((_nu - W_DBUP)) -ge 60 ] && watch_dump_base
    watch_link
    watch_ppm
    W_PREV=$_nu
    [ "$_read" = 1 ] && echo "$_nu" >"$LTMP/w.up"
}

watcher() {
    [ "$WATCHER" = 1 ] || return 0
    mkdir -p "$LTMP" || return 0
    exec 7>>"$LTMP/watcher.lock" || return 0
    _wt=0
    until flock -n 7; do # the one being replaced may take a moment to go
        _wt=$((_wt + 1))
        [ "$_wt" -ge 5 ] && return 0
        $WATCH_SLEEP 1 7>&-
    done
    W_VER=$(md5sum "$HERE/u60-guard.sh" 2>/dev/null | cut -c1-8)
    echo "$$ $(proc_start $$) ${W_VER:--}" >"$LTMP/watcher.pid"
    W_BOOT=$(ledger_bootid)
    W_B8=$(echo "$W_BOOT" | cut -c1-8)
    W_FILE=$(crashcap_file)
    W_BASE=0
    [ -f "$LEDGER_DIR/state/watch-init" ] || W_BASE=1
    now_up
    watch_load
    [ -n "$W_DUMP" ] || watch_dump_base
    log "watcher started (pid $$, version ${W_VER:--})"
    W_CATCHUP=1
    _wl=0
    while :; do
        watch_step
        W_CATCHUP=0
        _wl=$((_wl + 1))
        [ -n "$WATCH_MAX" ] && [ "$_wl" -ge "$WATCH_MAX" ] && return 0
        if [ $((_wl % 30)) = 0 ]; then # a new u60-guard.sh on disk: make way for its watcher
            _wv=$(md5sum "$HERE/u60-guard.sh" 2>/dev/null | cut -c1-8)
            if [ -n "$_wv" ] && [ "$_wv" != "$W_VER" ]; then
                log "watcher: u60-guard.sh is now $_wv, not $W_VER: exiting"
                return 0
            fi
        fi
        $WATCH_SLEEP "$WATCH_INTERVAL" 7>&-
    done
}

# One crash watcher, at this script's version. It starts with every lock fd
# closed: a long-lived child holding fd 9 would hold the agent's Wi-Fi lock
# (RELIABILITY.md §1) until reboot.
watcher_round() {
    [ "$WATCHER" = 1 ] && [ -e "$KMSG" ] || return 0
    _wv=$(md5sum "$HERE/u60-guard.sh" 2>/dev/null | cut -c1-8)
    _wp=
    _ws=
    _wm=
    { read -r _wp _ws _wm <"$LTMP/watcher.pid"; } 2>/dev/null
    isint "$_wp" || _wp=
    if [ -n "$_wp" ] && [ -n "$_ws" ] && [ "$(proc_start "$_wp")" = "$_ws" ]; then
        [ "$_wm" = "${_wv:--}" ] && return 0
        log "watcher $_wp is version $_wm, not ${_wv:--}: replacing it"
        $KILL "$_wp" 2>/dev/null
    fi
    mkdir -p "$LTMP" || return 0
    sh "$HERE/u60-guard.sh" watcher </dev/null >/dev/null 2>>"$LOG" 7>&- 8>&- 9>&- &
}

# ship_round (in a subshell): an unfinished ship transaction with no live
# executor → start `u60-ship.sh recover-live` in the background (it checks
# again itself and refuses while the executor is alive; its own heartbeat
# keeps the next rounds from starting a second one). A transaction of the
# guard component is not ours to finish: its executor's timeouts and the
# boot-time u60-recover.sh cover it.
ship_round() {
    [ -f "$SHIP_TXN" ] || return 0
    _sph=
    _scomp=
    _stp=
    while IFS= read -r _sl; do
        case "$_sl" in
            phase=*) _sph=${_sl#phase=} ;;
            comp=*) _scomp=${_sl#comp=} ;;
            t_phase=*) _stp=${_sl#t_phase=} ;;
        esac
    done <"$SHIP_TXN"
    case "$_sph" in staged | trial | promote | check | manifest | rollback) ;; *) return 0 ;; esac
    [ "$_scomp" = guard ] && return 0
    [ -f "$SHIP" ] || return 0
    _now=$(uptime_s)
    _hu=
    _hp=
    { read -r _hu _hp _r <"$SHIP_HB"; } 2>/dev/null
    isint "$_hu" || _hu=
    isint "$_hp" || _hp=
    if [ -n "$_hu" ] && [ $((_now - _hu)) -le "$SHIP_STALE" ] && [ -n "$_hp" ] &&
        { tr '\0' ' ' <"$PROC/$_hp/cmdline"; } 2>/dev/null | grep -q 'u60-ship\.sh'; then
        return 0
    fi
    if [ "$_sph" = staged ] && [ -z "$_hu" ]; then
        # staged, executor not started yet: the Mac starts it seconds later
        isint "$_stp" && [ $((_now - _stp)) -le "$SHIP_STAGED" ] && return 0
    fi
    _sage=missing
    [ -n "$_hu" ] && _sage="$((_now - _hu))s old"
    log "u60-ship: transaction ($_scomp, $_sph) has no live executor (heartbeat $_sage); starting recover-live"
    sh "$SHIP" recover-live </dev/null >>"$LOG" 2>&1 7>&- 8>&- 9>&- &
}

round() {
    mss_recovery_round
    (ship_round) 2>>"$LOG"
    (clock_round) 2>>"$LOG"
    guard_round
    rtc_round
    datad_round
    sms_round
    logcap_round
    crashcap_round
    crashcap_keep
    watcher_round
    standby_round
    bg_job fp fp_round
    lanv6_round
    bg_job ledger ledger_job
    round_mark $?
    [ -f "$LTMP/summary.want" ] && bg_job summary summary_job
}

case "$1" in
    once)
        round
        ;;
    sms-text) # tests: the SMS text for <kind>, as sms_round would send it now
        sms_text "$2"
        ;;
    crashcap)
        crashcap_start
        ;;
    started)
        (ledger_started) 2>>"$LOG"
        ;;
    watcher)
        watcher
        ;;
    *)
        log "u60-guard starting (pid $$)"
        crashcap_start
        (ledger_started) 2>>"$LOG"
        while :; do
            round
            $SLEEP "$INTERVAL"
        done
        ;;
esac
