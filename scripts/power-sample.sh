#!/bin/sh
# ─────────────────────────────────────────────────────────────────────────────
# power-sample.sh — where does the battery go? (plan E5, baseline measurement)
#
#   sh /data/u60-guard/power-sample.sh [minutes]      default 10
#
# Samples every 5 s for the given time, then reports
#   - the battery: status, average current and power, capacity change. Only
#     meaningful when the device is NOT on a charger (the report says so);
#   - CPU time per program over the window, including what its short-lived
#     children used (cutime/cstime: the agent's uci/ubus calls land there),
#     as a share of all busy CPU. CPU is the part of the draw software can
#     change; radios, screen and modem are not in it.
# Read-only. The report is printed and also kept in /data/power/.
# SPDX-License-Identifier: MIT
# ─────────────────────────────────────────────────────────────────────────────

MIN=${1:-10}
STEP=5
BAT=${PS_BAT:-/sys/class/power_supply/battery}
OUT_DIR=${PS_OUT:-/data/power}
W=$(mktemp -d) || exit 1
trap 'rm -rf "$W"' EXIT

# pid comm jiffies(utime+stime+cutime+cstime), one line per process
snap() {
    for d in /proc/[0-9]*; do
        [ -r "$d/stat" ] || continue
        # comm is in parentheses and may contain spaces: cut after the last ')'
        s=$(cat "$d/stat" 2>/dev/null) || continue
        comm=${s#*(}; comm=${comm%)*}
        rest=${s##*) }
        set -- $rest
        # after ')': state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime cutime cstime
        echo "${d#/proc/} $(echo "$comm" | tr ' ' '_') $((${12} + ${13} + ${14} + ${15}))"
    done
}
busy() { awk '/^cpu / {print $2 + $3 + $4 + $7 + $8 + $9; exit}' /proc/stat; }
total() { awk '/^cpu / {s = 0; for (i = 2; i <= NF; i++) s += $i; print s; exit}' /proc/stat; }

snap > "$W/a"
b0=$(busy); t0=$(total)
cap0=$(cat "$BAT/capacity" 2>/dev/null)
status0=$(cat "$BAT/status" 2>/dev/null)
n=0; cur_sum=0; pow_sum=0; charging_seen=0
end=$(( $(cut -d. -f1 /proc/uptime) + MIN * 60 ))
while [ "$(cut -d. -f1 /proc/uptime)" -lt "$end" ]; do
    c=$(cat "$BAT/current_now" 2>/dev/null); v=$(cat "$BAT/voltage_now" 2>/dev/null)
    st=$(cat "$BAT/status" 2>/dev/null)
    case "$st" in Charging | Full) charging_seen=1 ;; esac
    case "$c$v" in *[!0-9-]* | '') ;; *)
        cur_sum=$((cur_sum + c)); pow_sum=$((pow_sum + c / 1000 * (v / 1000) / 1000)); n=$((n + 1)) ;;
    esac
    sleep "$STEP"
done
snap > "$W/b"
b1=$(busy); t1=$(total)
cap1=$(cat "$BAT/capacity" 2>/dev/null)
cores=$(grep -c '^processor' /proc/cpuinfo)
hz=100

mkdir -p "$OUT_DIR"
report="$OUT_DIR/$(date +%Y%m%d-%H%M%S).txt"
{
    echo "U60 power sample — $(date '+%Y-%m-%d %H:%M') device local, $MIN min, every ${STEP}s"
    echo
    echo "Battery"
    echo "  status: $status0 → $(cat "$BAT/status" 2>/dev/null); capacity: ${cap0:-?}% → ${cap1:-?}%"
    if [ "$n" -gt 0 ]; then
        echo "  average current: $((cur_sum / n / 1000)) mA   average power: $((pow_sum / n)) mW   ($n samples)"
    fi
    if [ "$charging_seen" = 1 ]; then
        echo "  ON A CHARGER during the window: the current is the charger's balance,"
        echo "  not what the device uses. Unplug and run again for the battery baseline."
    else
        echo "  (current_now is as the firmware reports it; which sign means discharge is not"
        echo "   verified yet — the capacity change per hour above is the number to trust)"
    fi
    echo
    busy_d=$((b1 - b0)); total_d=$((t1 - t0))
    echo "CPU over the window: $((busy_d * 100 / (total_d > 0 ? total_d : 1)))% of $cores cores busy"
    echo
    echo "Per program (CPU incl. its children; share of all busy CPU; % of one core)"
    # join the two snapshots on pid, sum by comm; processes that started during
    # the window count from zero, ones that ended are missed (their parent's
    # cutime/cstime still catches most of them)
    awk -v busy="$busy_d" -v secs="$((MIN * 60))" -v hz="$hz" '
        NR == FNR { a[$1] = $3; next }
        { d = $3 - (($1 in a) ? a[$1] : 0); if (d > 0) by[$2] += d }
        END {
            for (c in by) printf "%d %s\n", by[c], c
        }' "$W/a" "$W/b" | sort -rn | head -15 | awk -v busy="$busy_d" -v secs="$((MIN * 60))" -v hz="$hz" '
        { share = 0; if (busy > 0) share = $1 * 100 / busy   # busybox awk misparses an unparenthesised ?:
          core = $1 * 100 / (secs * hz)
          printf "  %-22s %5.1f%%   %5.1f%% of a core\n", $2, share, core }'
} > "$report"
cat "$report"
echo
echo "saved: $report"
