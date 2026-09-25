#!/bin/sh
# ─────────────────────────────────────────────────────────────────────────────
# ddr-ab.sh — A/B the Qualcomm DDR/L3 frequency governors' sampling interval
# against battery drain.
#
#   sh /data/u60-guard/ddr-ab.sh [minutes per window] [slow ms]   (10, 20)
#
# Windows, in order:  orig → slow → orig2
#   orig   the vendor values (bwmon-ddr sample_ms 4, memlat sample_ms 8);
#          also the away measurement of whatever Tailscale build is running
#   slow   both sample_ms raised to [slow ms]: the kernel workers that poll
#          the bandwidth monitors (~8800 wakeups/s together) run less often,
#          but DDR may also stay at a high step longer after a burst
#   orig2  vendor values again, to see drift between the first and last window
# These are sysfs values: a reboot puts the vendor ones back. The starting
# values are also put back on exit, Ctrl-C or kill (anything but kill -9).
#
# Do not poll the device while this runs (ssh from outside comes in over
# Tailscale and the cellular link). Start it, wait (MIN × 3 + ~5) minutes,
# then read /data/power/ddr-ab.log.
# SPDX-License-Identifier: MIT
# ─────────────────────────────────────────────────────────────────────────────

MIN=${1:-10}
SLOW=${2:-20}
SAMPLER=${DDR_AB_SAMPLER:-/data/u60-guard/power-sample.sh}
LOG=/data/power/ddr-ab.log
BW=/sys/devices/system/cpu/bus_dcvs/DDR/19091000.qcom,bwmon-ddr/sample_ms
ML=/sys/devices/system/cpu/bus_dcvs/memlat_settings/sample_ms

mkdir -p /data/power
log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >>"$LOG"; echo "$*"; }

for f in "$BW" "$ML"; do
    [ -w "$f" ] || { log "not writable: $f"; exit 1; }
done
BW0=$(cat "$BW"); ML0=$(cat "$ML")

# kernel-worker wakeups/s over 10 s (memlat_wq + bwmon_wq threads)
kwork() {
    _s() { for t in /proc/[0-9]*/task/*; do
        case "$(cat "$t/comm" 2>/dev/null)" in kworker*) ;; *) continue ;; esac
        awk '/^voluntary_ctxt/ {v=$2} /^nonvoluntary_ctxt/ {n=$2} END {print v + n}' "$t/status" 2>/dev/null
    done | awk '{s += $1} END {print s + 0}'; }
    _a=$(_s); sleep 10; _b=$(_s)
    echo $(( (_b - _a) / 10 ))
}

set_ms() {
    echo "$1" >"$BW"; echo "$2" >"$ML"
    sleep 20
    log "set bwmon sample_ms=$(cat "$BW") memlat sample_ms=$(cat "$ML"); kworker wakeups ≈ $(kwork)/s"
}

restore() {
    echo "$BW0" >"$BW"; echo "$ML0" >"$ML"
    log "restored bwmon sample_ms=$(cat "$BW") memlat sample_ms=$(cat "$ML")"
}
trap 'restore; exit' INT TERM
trap restore EXIT

log "=== start ($MIN min windows, slow=$SLOW ms): bwmon=$BW0 memlat=$ML0 tailscaled=$(readlink /proc/$(pidof tailscaled)/exe 2>/dev/null)"

win() { sh "$SAMPLER" window "ddr-$1" "$MIN" | grep -E 'INVALID|average|busy:|tailscaled  ' | sed "s/^/  [$1] /" | tee -a "$LOG"; }

set_ms "$BW0" "$ML0" && win orig
set_ms "$SLOW" "$SLOW" && win slow
set_ms "$BW0" "$ML0" && win orig2
log "=== done"
