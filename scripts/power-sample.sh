#!/bin/sh
# ─────────────────────────────────────────────────────────────────────────────
# power-sample.sh — where does the battery go, and is the modem ever allowed
# to rest?
#
#   sh /data/u60-guard/power-sample.sh [minutes]              one window, default 10
#   sh /data/u60-guard/power-sample.sh window <label> <min>    one labelled window
#   sh /data/u60-guard/power-sample.sh aa <n> <min>            n back-to-back windows
#                                                              (A/A: measures the noise)
#
# Run it detached and with NO SSH session open while it measures:
#   nohup sh /data/u60-guard/power-sample.sh aa 3 20 >/dev/null 2>&1 &
# SSH to this device goes over Tailscale, and Tailscale goes over the cellular
# link, so an open session is itself cellular traffic and Tailscale work.
#
# Per window it reports:
#   - energy: the fuel gauge's charge counter (µAh) × mean voltage, which is
#     an integral and does not alias against the modem's 5-6 s bursts the way
#     sampling current_now would. V×I every 5 s is kept as a cross-check;
#   - the cellular link: seconds with any packet, the share of time spent in
#     quiet gaps of 10 s or more, the median gap, packets per minute. Counted
#     from /proc/net/dev every second (one file read, no process started);
#   - CPU idle: the share of CPU time spent in states deeper than WFI;
#   - wakeups per program (voluntary + involuntary context switches summed
#     over ALL threads; /proc/<pid>/status alone counts the main thread only)
#     and CPU per program incl. children, both from window start/end snapshots;
#   - confounders: signal, band, network type, scenario, Tailscale peers,
#     temperature, charge; and why a window is invalid (charger seen, screen
#     lit, scenario or band changed mid-window);
#   - the sampler's own cost, so it can be shown to be negligible.
# Read-only. Reports go to /data/power/, one TSV line per window to
# /data/power/summary.tsv.
# SPDX-License-Identifier: MIT
# ─────────────────────────────────────────────────────────────────────────────

BAT=${PS_BAT:-/sys/class/power_supply/battery}
BACKLIGHT=${PS_BACKLIGHT:-/sys/class/leds/led:lcd/brightness}
OUT_DIR=${PS_OUT:-/data/power}
NETDEV=${PS_NETDEV:-/proc/net/dev}
WAN=${PS_WAN:-rmnet_data0}
TSIF=${PS_TSIF:-tailscale0}
CPUDIR=${PS_CPUDIR:-/sys/devices/system/cpu}
DATAD=${PS_DATAD:-http://127.0.0.1:9460/state}
AGENT=${PS_AGENT:-http://127.0.0.1:9090/api/public/status}
TS_CLI=${PS_TS_CLI:-/data/tailscale/tailscale}
CURL=${PS_CURL:-/usr/bin/curl}
STEP=5                  # battery / cpuidle every STEP seconds
PROGS="tailscaled mihomo u60pro-devui zwrt-datad zte-agent"

# ── pure helpers (also exercised by scripts/test/power-sample) ──────────────

# stdin: one packet count per second → "busy_seconds total quiet10_share_pct median_quiet_gap_s"
wan_stats() {
    awk '
        { n++; if ($1 > 0) { busy++; if (g > 0) gaps[++k] = g; g = 0 } else g++ }
        END {
            if (g > 0) gaps[++k] = g
            q = 0; for (i = 1; i <= k; i++) if (gaps[i] >= 10) q += gaps[i]
            # median length of the quiet gaps (runs of packet-free seconds); 0 when there are none
            m = 0; c = 0
            for (i = 1; i <= k; i++) all[++c] = gaps[i]
            for (i = 2; i <= c; i++) { v = all[i]; j = i - 1; while (j > 0 && all[j] > v) { all[j + 1] = all[j]; j-- } all[j + 1] = v }
            # busybox awk parses "m = (c % 2) ? a : b" as m = c % 2; parenthesise the whole ?:
            if (c > 0) m = ((c % 2) ? all[(c + 1) / 2] : (all[c / 2] + all[c / 2 + 1]) / 2)
            printf "%d %d %.1f %g\n", busy, n, (n > 0 ? q * 100 / n : 0), m
        }'
}

# $1 start µAh, $2 end µAh, $3 mean µV, $4 seconds → average mW drawn (positive = discharge)
energy_mw() {
    awk -v a="$1" -v b="$2" -v v="$3" -v s="$4" 'BEGIN {
        if (s <= 0 || a == "" || b == "") { print "?"; exit }
        printf "%.0f\n", (a - b) * v / 1e9 / (s / 3600)
    }'
}


# ── device reads (no process started for the per-second path) ───────────────

# Sets NW / NT to the wan and tailscale packet counts (rx + tx), from one read
# of NETDEV. Sets variables rather than echoing: in busybox ash, $(...) forks
# a subshell even for a function, and this runs every second.
net_read() {
    NW=0; NT=0
    while IFS=' :' read -r ifc rb rp re rdr rf rfr rc rm tb tp rest; do
        case $ifc in
            "$WAN") NW=$((rp + tp)) ;;
            "$TSIF") NT=$((rp + tp)) ;;
        esac
    done < "$NETDEV"
}
net_pkts() { net_read; echo "$NW $NT"; }

rd() { _v=; read -r _v < "$1" 2>/dev/null; echo "$_v"; }

# total µs in idle states deeper than state0 (WFI), and in all states, summed over CPUs
idle_us() {
    _deep=0; _all=0
    for c in "$CPUDIR"/cpu[0-9]*; do
        for st in "$c"/cpuidle/state*; do
            [ -r "$st/time" ] || continue
            read -r _t < "$st/time"
            _all=$((_all + _t))
            case $st in */state0) ;; *) _deep=$((_deep + _t)) ;; esac
        done
    done
    echo "$_deep $_all"
}

# "name<TAB>wakeups" for PROGS, summing every thread; "name<TAB>-" when not running
wakeups() {
    for n in $PROGS; do   # n, p, s are clobbered: callers must not rely on them
        p=$(pidof "$n" 2>/dev/null | cut -d' ' -f1)
        if [ -z "$p" ]; then printf '%s\t-\t-\n' "$n"; continue; fi
        s=$(awk '/^(non)?voluntary_ctxt_switches/ {x += $2} END {print x + 0}' /proc/"$p"/task/*/status 2>/dev/null)
        printf '%s\t%s\t%s\n' "$n" "$p" "$s"
    done
}

# pid comm jiffies(utime+stime+cutime+cstime), one line per process
snap() {
    for d in /proc/[0-9]*; do
        [ -r "$d/stat" ] || continue
        s=; read -r s < "$d/stat" 2>/dev/null; [ -n "$s" ] || continue   # builtin read: no process per pid
        comm=${s#*(}; comm=${comm%)*}; comm=${comm// /_}
        rest=${s##*) }
        set -- $rest
        echo "${d#/proc/} $comm $((${12} + ${13} + ${14} + ${15}))"
    done
}
busy() { awk '/^cpu / {print $2 + $3 + $4 + $7 + $8 + $9; exit}' /proc/stat; }
total() { awk '/^cpu / {s = 0; for (i = 2; i <= NF; i++) s += $i; print s; exit}' /proc/stat; }
forks() { awk '/^processes/ {print $2}' /proc/stat; }
self_jiffies() { read -r s < /proc/$$/stat; set -- ${s##*) }; echo $((${12} + ${13} + ${14} + ${15})); }

# "rsrp band rat" from the data service; "? ? ?" when it does not answer
radio() {
    j=$($CURL -s -m 3 "$DATAD" 2>/dev/null)
    r=$(echo "$j" | sed -n 's/.*"nr_rsrp":\(-\{0,1\}[0-9]*\).*/\1/p' | head -1)
    b=$(echo "$j" | sed -n 's/.*"net":{[^}]*"band":"\([^"]*\)".*/\1/p' | head -1)
    t=$(echo "$j" | sed -n 's/.*"radio_network_type":"\([^"]*\)".*/\1/p' | head -1)
    echo "${r:-?} ${b:-?} ${t:-?}"
}
scenario() {
    $CURL -s -m 3 "$AGENT" 2>/dev/null | sed -n 's/.*"scenario":{[^}]*"current":"\([^"]*\)".*/\1/p' | head -1
}
ts_peers() {
    [ -x "$TS_CLI" ] || { echo "?"; return; }
    "$TS_CLI" --socket=/tmp/tailscaled.sock status 2>/dev/null | awk 'NR > 1 && !/offline/ && NF > 2 {n++} END {print n + 0}'
}

# ── one window ───────────────────────────────────────────────────────────────
# $1 label, $2 minutes. Writes the report and appends to summary.tsv.
run_window() {
    label=$1; min=$2
    W=$(mktemp -d) || exit 1
    FIFO=$W/tick; mkfifo "$FIFO"; exec 3<>"$FIFO"

    ncpu=$(grep -c '^processor' /proc/cpuinfo)
    radio0=$(radio); scen0=$(scenario); peers0=$(ts_peers)
    wakeups > "$W/wk0"; snap > "$W/a"
    b0=$(busy); t0=$(total); f0=$(forks); self0=$(self_jiffies)
    set -- $(idle_us); deep0=$1; idle0=$2
    q0=$(rd "$BAT/charge_counter"); cap0=$(rd "$BAT/capacity"); temp0=$(rd "$BAT/temp")
    net_read; wp=$NW; tp=$NT

    : > "$W/wan"; : > "$W/net"
    vsum=0; vn=0; pw_sum=0; charging=0; lit=0
    secs=$((min * 60)); i=0
    while [ $i -lt $secs ]; do
        read -t 1 _ <&3            # a one-second wait without starting a process
        i=$((i + 1))
        net_read
        dw=$((NW - wp)); dt=$((NT - tp)); wp=$NW; tp=$NT
        echo "$dw" >> "$W/wan"
        echo "$dw $dt" >> "$W/net"
        if [ $((i % STEP)) -eq 0 ]; then
            # plain reads into variables: no subshell every 5 s
            v=; c=; st=; bl=
            read -r v < "$BAT/voltage_now" 2>/dev/null; read -r c < "$BAT/current_now" 2>/dev/null
            read -r st < "$BAT/status" 2>/dev/null; read -r bl < "$BACKLIGHT" 2>/dev/null
            case "$st" in Charging | Full) charging=1 ;; esac
            [ "${bl:-0}" -gt 0 ] 2>/dev/null && lit=$((lit + STEP))
            case "$v$c" in *[!0-9-]* | '') ;; *)
                vsum=$((vsum + v)); vn=$((vn + 1)); pw_sum=$((pw_sum + c / 1000 * (v / 1000) / 1000)) ;;
            esac
        fi
    done
    exec 3>&-

    q1=$(rd "$BAT/charge_counter"); cap1=$(rd "$BAT/capacity"); temp1=$(rd "$BAT/temp")
    set -- $(idle_us); deep1=$1; idle1=$2
    b1=$(busy); t1=$(total); f1=$(forks)
    snap > "$W/b"; wakeups > "$W/wk1"
    radio1=$(radio); scen1=$(scenario); peers1=$(ts_peers)
    self1=$(self_jiffies)

    vavg=0; [ "$vn" -gt 0 ] && vavg=$((vsum / vn))
    mw_q=$(energy_mw "$q0" "$q1" "$vavg" "$secs")
    mw_vi="?"; [ "$vn" -gt 0 ] && mw_vi=$((-pw_sum / vn))   # current_now is negative while discharging
    set -- $(wan_stats < "$W/wan"); wbusy=$1; wn=$2; q10=$3; mgap=$4
    ppm=$(awk -v s="$secs" '{t += $1} END {printf "%.1f", (s > 0 ? t * 60 / s : 0)}' "$W/wan")
    # WAN minus Tailscale, clamped at 0 per second: an approximation (DERP runs over TCP)
    ppm_nots=$(awk -v s="$secs" '{d = $1 - $2; if (d < 0) d = 0; t += d} END {printf "%.1f", (s > 0 ? t * 60 / s : 0)}' "$W/net")
    deep_pct=$(awk -v a="$((deep1 - deep0))" -v b="$ncpu" -v s="$secs" 'BEGIN {printf "%.1f", (s > 0 ? a / (b * s * 1e6) * 100 : 0)}')
    busy_d=$((b1 - b0)); total_d=$((t1 - t0))
    self_pct=$(awk -v j="$(( self1 - self0 ))" -v s="$secs" 'BEGIN {printf "%.2f", j / s}')   # jiffies/s at 100 Hz = % of a core

    invalid=""
    [ "$charging" = 1 ] && invalid="${invalid}charger,"
    [ "$lit" -gt 0 ] && invalid="${invalid}screen-on:${lit}s,"
    [ "$scen0" != "$scen1" ] && invalid="${invalid}scenario:$scen0>$scen1,"
    set -- $radio0; band0=$2; rat0=$3; rsrp0=$1
    set -- $radio1; band1=$2; rat1=$3; rsrp1=$1
    [ "$band0" != "$band1" ] && invalid="${invalid}band:$band0>$band1,"
    [ "$rat0" != "$rat1" ] && invalid="${invalid}rat:$rat0>$rat1,"
    invalid=${invalid%,}

    mkdir -p "$OUT_DIR"
    ls -t "$OUT_DIR"/*.txt 2>/dev/null | tail -n +60 | while read -r old; do rm -f "$old"; done
    stamp=$(date +%Y%m%d-%H%M%S)
    report="$OUT_DIR/$stamp-$label.txt"
    {
        echo "U60 power sample \"$label\" — $(date '+%Y-%m-%d %H:%M') device local, $min min"
        [ -n "$invalid" ] && echo "!! INVALID WINDOW: $invalid — rerun it"
        echo
        echo "Energy"
        echo "  charge counter: ${q0:-?} → ${q1:-?} µAh, mean ${vavg} µV → average ${mw_q} mW   (primary)"
        echo "  V×I every ${STEP}s: ${mw_vi} mW ($vn samples; cross-check)"
        echo "  capacity ${cap0:-?}% → ${cap1:-?}%, battery temp ${temp0:-?} → ${temp1:-?} (0.1 °C)"
        echo
        echo "Cellular link ($WAN)"
        echo "  seconds with packets: $wbusy / $wn"
        echo "  time in quiet gaps ≥10 s: ${q10}%   median gap: ${mgap} s"
        echo "  packets/min: $ppm   (minus $TSIF, approx.: $ppm_nots)"
        echo
        echo "CPU"
        echo "  busy: $((busy_d * 100 / (total_d > 0 ? total_d : 1)))% of $ncpu cores   deep idle (below WFI): ${deep_pct}%"
        echo "  new processes: $(( (f1 - f0) / (secs > 0 ? secs : 1) ))/s"
        echo
        echo "Wakeups per second (all threads)"
        awk -F'\t' -v s="$secs" 'NR == FNR {p[$1] = $2; w[$1] = $3; next}
            { if ($3 == "-" || w[$1] == "-") printf "  %-14s not running\n", $1
              else if ($2 != p[$1]) printf "  %-14s restarted during the window\n", $1
              else printf "  %-14s %8.1f\n", $1, ($3 - w[$1]) / s }' "$W/wk0" "$W/wk1"
        echo
        echo "Per program (CPU incl. children; share of busy CPU; % of one core)"
        awk 'NR == FNR { a[$1] = $3; next }
             { d = $3 - (($1 in a) ? a[$1] : 0); if (d > 0) by[$2] += d }
             END { for (c in by) printf "%d %s\n", by[c], c }' "$W/a" "$W/b" | sort -rn | head -12 |
            awk -v busy="$busy_d" -v secs="$secs" '
            { share = 0; if (busy > 0) share = $1 * 100 / busy   # busybox awk misparses an unparenthesised ?:
              printf "  %-22s %5.1f%%   %5.1f%% of a core\n", $2, share, $1 * 100 / (secs * 100) }'
        echo
        echo "Conditions"
        echo "  signal: nr_rsrp $rsrp0 → $rsrp1 dBm, band $band0 → $band1, $rat0 → $rat1"
        echo "  scenario: ${scen0:-?} → ${scen1:-?}   Tailscale peers online: $peers0 → $peers1"
        echo "  sampler itself: ${self_pct}% of a core (incl. its own curl/awk at the window edges)"
    } > "$report"

    tsv=$OUT_DIR/summary.tsv
    [ -f "$tsv" ] || printf 'stamp\tlabel\tmin\tinvalid\tmw_charge\tmw_vi\twan_busy_s\tquiet10_pct\tmedian_gap_s\tppm\tppm_no_ts\tdeep_idle_pct\tbusy_pct\ttailscaled_wps\tmihomo_wps\tdevui_wps\tdatad_wps\tagent_wps\tnr_rsrp\tband\trat\tscenario\tpeers\tsampler_pct\n' > "$tsv"
    wps=$(awk -F'\t' -v s="$secs" 'NR == FNR {p[$1] = $2; w[$1] = $3; next}
        { if ($3 == "-" || w[$1] == "-" || $2 != p[$1]) printf "-\t"; else printf "%.1f\t", ($3 - w[$1]) / s }' "$W/wk0" "$W/wk1")
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$stamp" "$label" "$min" "${invalid:-ok}" "$mw_q" "$mw_vi" "$wbusy" "$q10" "$mgap" "$ppm" "$ppm_nots" \
        "$deep_pct" "$((busy_d * 100 / (total_d > 0 ? total_d : 1)))" "$wps" "$rsrp1" "$band1" "$rat1" \
        "${scen1:-?}" "$peers1" "$self_pct" >> "$tsv"

    cat "$report"; echo; echo "saved: $report"
    rm -rf "$W"
}

case "$1" in
    --wan-stats) wan_stats; exit ;;                       # test hooks
    --energy-mw) energy_mw "$2" "$3" "$4" "$5"; exit ;;
    --net-pkts) net_pkts; exit ;;
    --idle-us) idle_us; exit ;;
    window) run_window "${2:-window}" "${3:-10}" ;;
    aa)
        # distinct names: busybox ash has no function-local variables by default,
        # and wakeups() loops with n (a window count named n was clobbered with
        # "zte-agent" after the first window — seen 2026-09-23)
        AA_COUNT=${2:-3}; AA_MIN=${3:-20}; AA_I=1
        while [ "$AA_I" -le "$AA_COUNT" ]; do run_window "aa-$AA_I" "$AA_MIN"; AA_I=$((AA_I + 1)); done ;;
    ''|[0-9]*) run_window sample "${1:-10}" ;;
    *) sed -n '5,10p' "$0"; exit 2 ;;
esac
