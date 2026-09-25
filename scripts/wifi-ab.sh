#!/bin/sh
# ─────────────────────────────────────────────────────────────────────────────
# wifi-ab.sh — A/B the Wi-Fi transmitter settings against battery drain.
#
#   sh /data/u60-guard/wifi-ab.sh [minutes per window]      (default 10)
#
# Runs power-sample.sh windows in this order, each after the setting has
# been applied and has settled:
#   base  → 5g80 → tx50 → 2g20 → only5g → all → base
#     5g80    5 GHz channel width EHT160 → EHT80
#     tx50    transmit power 100% → 50% on both radios
#     2g20    2.4 GHz channel width EHT40 → EHT20
#     only5g  2.4 GHz AP off (the 5 GHz AP should stay — but on B27 it does
#             not: both APs go down, see the check below)
#     all     5g80 + tx50 + 2g20 together
# The starting values are saved first and put back on exit, Ctrl-C or kill
# (anything but kill -9); if the script dies anyway, the saved values are in
# /data/power/wifi-ab.orig as uci set lines.
# Every change is made under the agent's Wi-Fi lock (/tmp/u60-wifi.lock, see
# zte-agent wifi_radio.rs) and applied with zwrt_wlan reload, like the agent.
# Each switch drops Wi-Fi clients for about 10 s.
#
# Do not poll the device while this runs: an ssh session from outside comes
# in over Tailscale and the cellular link, which is what is being measured.
# Start it, wait (MIN × 7 + ~10) minutes, then read /data/power/wifi-ab.log.
# SPDX-License-Identifier: MIT
# ─────────────────────────────────────────────────────────────────────────────

MIN=${1:-10}
SAMPLER=${WIFI_AB_SAMPLER:-/data/u60-guard/power-sample.sh}
LOCK=/tmp/u60-wifi.lock
ORIG=/data/power/wifi-ab.orig
LOG=/data/power/wifi-ab.log
KEYS="wireless.wifi0.htmode wireless.wifi1.htmode wireless.wifi0.txpowerpercent wireless.wifi1.txpowerpercent wireless.main_2g.disabled"

mkdir -p /data/power
log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >>"$LOG"; echo "$*"; }

# the hostapd processes themselves (column 5), not their ujail wrappers (busybox ps w: PID USER COMMAND)
hostapds() { ps w | awk '$3 == "/usr/sbin/hostapd" {n++} END {print n+0}'; }

# busybox flock has no -w: poll -n with a deadline (the agent holds it ≤ 120 s)
take_lock() {
    exec 9>>"$LOCK"
    _t=0
    while ! flock -n 9; do
        _t=$((_t + 1)); [ "$_t" -gt 150 ] && { log "Wi-Fi lock busy for 150 s; giving up"; return 1; }
        sleep 1
    done
    echo $$ >"$LOCK"
}
drop_lock() { flock -u 9; exec 9>&-; }

# the scenario engine owns main_2g/main_5g.disabled (home turns both APs off):
# if it switches mid-run, stop, and leave its AP settings alone on restore
scenario() { sed -n 's/.*"current": *"\([^"]*\)".*/\1/p' /data/scenario/state.json 2>/dev/null; }
SCEN0=$(scenario)

# $1 2g htmode, $2 5g htmode, $3 txpower %, $4 main_2g.disabled
apply() {
    if [ "$(scenario)" != "$SCEN0" ]; then
        log "scenario changed $SCEN0 -> $(scenario): stopping"; exit 1
    fi
    take_lock || return 1
    uci set wireless.wifi0.htmode="$1"
    uci set wireless.wifi1.htmode="$2"
    uci set wireless.wifi0.txpowerpercent="$3"
    uci set wireless.wifi1.txpowerpercent="$3"
    uci set wireless.main_2g.disabled="$4"
    uci commit wireless
    ubus -t 60 call zwrt_wlan reload >/dev/null 2>&1
    _want=2; [ "$4" = 1 ] && _want=1
    _t=0
    while [ "$(hostapds)" -ne "$_want" ] && [ "$_t" -lt 60 ]; do sleep 2; _t=$((_t + 2)); done
    drop_lock
    sleep 30   # let the radios settle before measuring
    log "applied 2g=$1 5g=$2 tx=$3% main_2g.disabled=$4: $(hostapds) hostapd; $(iw dev 2>/dev/null | awk '/Interface/ {i=$2} /channel/ {printf "%s %s %s%s ", i, $2, $6, $7} /txpower/ {printf "%s dBm; ", $2}')"
}

restore() {
    [ -f "$ORIG" ] || return
    log "restoring the starting values"
    take_lock
    _s=$(scenario)
    while read -r line; do
        case "$line" in wireless.main_2g.disabled=*)
            [ "$_s" != "$SCEN0" ] && { log "scenario is now $_s: leaving main_2g.disabled to it"; continue; } ;;
        esac
        uci set "$line"
    done <"$ORIG"
    uci commit wireless
    ubus -t 60 call zwrt_wlan reload >/dev/null 2>&1
    _t=0
    while [ "$(hostapds)" -lt 1 ] && [ "$(uci -q get wireless.main_2g.disabled)" != 1 ] && [ "$_t" -lt 60 ]; do sleep 2; _t=$((_t + 2)); done
    drop_lock
    log "restored: $(hostapds) hostapd"
    rm -f "$ORIG"
}

if [ -f "$ORIG" ]; then
    echo "$ORIG exists: an earlier run did not restore. Restore it first (uci set each line, commit, zwrt_wlan reload)."
    exit 1
fi
: >"$ORIG.tmp"
for k in $KEYS; do echo "$k=$(uci -q get "$k")" >>"$ORIG.tmp"; done
mv "$ORIG.tmp" "$ORIG"
H2=$(uci -q get wireless.wifi0.htmode); H5=$(uci -q get wireless.wifi1.htmode)
TP=$(uci -q get wireless.wifi0.txpowerpercent); M2=$(uci -q get wireless.main_2g.disabled)
M2=${M2:-0}
H2N=$(echo "$H2" | sed 's/40$/20/'); H5N=$(echo "$H5" | sed 's/160$/80/')
trap 'restore; exit' INT TERM
trap restore EXIT
log "=== start ($MIN min windows): 2g=$H2 5g=$H5 tx=$TP% main_2g.disabled=$M2"

# WIFI_AB_STEPS picks the steps, e.g. "base 5g80 tx50 all base2" when a
# phone is on the 5 GHz AP (only5g would cut it off: both APs go down)
STEPS=${WIFI_AB_STEPS:-base 5g80 tx50 2g20 only5g all base2}
want() { case " $STEPS " in *" $1 "*) return 0 ;; esac; return 1; }

win() { sh "$SAMPLER" window "wifi-$1" "$MIN" | grep -E 'INVALID|average|busy:' | sed "s/^/  [$1] /" | tee -a "$LOG"; }

want base && apply "$H2"  "$H5"  "$TP" "$M2" && win base
want 5g80 && apply "$H2"  "$H5N" "$TP" "$M2" && win 5g80
want tx50 && apply "$H2"  "$H5"  50    "$M2" && win tx50
want 2g20 && apply "$H2N" "$H5"  "$TP" "$M2" && win 2g20
want only5g && apply "$H2"  "$H5"  "$TP" 1     && { [ "$(hostapds)" -gt 0 ] && win only5g || log "only5g: no AP came up (both went down); window skipped"; }
want all && apply "$H2N" "$H5N" 50    "$M2" && win all   # 2.4 GHz stays on: turning it off takes 5 GHz down too
want base2 && apply "$H2"  "$H5"  "$TP" "$M2" && win base2
log "=== done"
