#!/bin/sh
# power-sample.sh helpers against fixtures (busybox container, no device).
# SPDX-License-Identifier: MIT
SCRIPTS=${SCRIPTS:-/scripts}
PS="sh $SCRIPTS/power-sample.sh"
PASS=0; FAIL=0
ok() { PASS=$((PASS + 1)); echo "  ok   $1"; }
bad() { FAIL=$((FAIL + 1)); echo "  FAIL $1"; }
check() { _d=$1; shift; if eval "$@"; then ok "$_d"; else bad "$_d  [$*]"; fi; }
T=$(mktemp -d)

# cellular link: 3 busy seconds, then a 12 s gap, then 1 busy, then a 4 s gap
printf '5\n3\n7\n0\n0\n0\n0\n0\n0\n0\n0\n0\n0\n0\n0\n9\n0\n0\n0\n0\n' > "$T/wan"
out=$($PS --wan-stats < "$T/wan")
check "busy seconds and total" '[ "$(echo $out | cut -d" " -f1-2)" = "4 20" ]'
check "12 s of 20 in gaps >= 10 s" '[ "$(echo $out | cut -d" " -f3)" = "60.0" ]'
check "median quiet gap of (12, 4) = 8" '[ "$(echo $out | cut -d" " -f4)" = "8" ]'
out=$(printf '1\n1\n1\n' | $PS --wan-stats)
check "never quiet: 0% and median 0" '[ "$out" = "3 3 0.0 0" ]'
out=$(printf '0\n0\n' | $PS --wan-stats)
check "all quiet, short: no gap >= 10 s" '[ "$out" = "0 2 0.0 2" ]'

# energy: 190 mAh (190000 µAh) at 4.2 V over 1 h = 798 mW
check "charge counter energy" '[ "$($PS --energy-mw 9190000 9000000 4200000 3600)" = "798" ]'
check "half the charge in half the time = same power" '[ "$($PS --energy-mw 9095000 9000000 4200000 1800)" = "798" ]'
check "unknown counter → ?" '[ "$($PS --energy-mw "" 9000000 4200000 3600)" = "?" ]'

# /proc/net/dev parsing, including a counter glued to the colon
cat > "$T/netdev" <<X
Inter-|   Receive                                                |  Transmit
 face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed
    lo: 100 2 0 0 0 0 0 0 100 2 0 0 0 0 0 0
rmnet_data0:12345678 1000 0 0 0 0 0 0 555 400 0 0 0 0 0 0
tailscale0: 999 30 0 0 0 0 0 0 888 20 0 0 0 0 0 0
X
check "wan and tailscale packets (rx+tx)" '[ "$(PS_NETDEV=$T/netdev $PS --net-pkts)" = "1400 50" ]'
check "missing interfaces read as 0" '[ "$(PS_NETDEV=$T/netdev PS_WAN=wwan9 PS_TSIF=none $PS --net-pkts)" = "0 0" ]'

# cpuidle: deep = everything but state0, summed over CPUs
for c in 0 1; do for s in 0 1 2; do mkdir -p "$T/cpu/cpu$c/cpuidle/state$s"; done
    echo 100 > "$T/cpu/cpu$c/cpuidle/state0/time"; echo 20 > "$T/cpu/cpu$c/cpuidle/state1/time"; echo 5 > "$T/cpu/cpu$c/cpuidle/state2/time"; done
check "idle µs: deep 50 of 250" '[ "$(PS_CPUDIR=$T/cpu $PS --idle-us)" = "50 250" ]'

# several windows in a row (aa): all of them must run. Stubs keep it offline.
mkdir -p "$T/bat" "$T/out"
echo 9000000 > "$T/bat/charge_counter"; echo 80 > "$T/bat/capacity"; echo 300 > "$T/bat/temp"
echo 4200000 > "$T/bat/voltage_now"; echo -300000 > "$T/bat/current_now"; echo Discharging > "$T/bat/status"
echo 0 > "$T/bl"; printf '#!/bin/sh\nexit 1\n' > "$T/nocurl"; chmod +x "$T/nocurl"
PS_BAT=$T/bat PS_OUT=$T/out PS_NETDEV=$T/netdev PS_CPUDIR=$T/cpu PS_BACKLIGHT=$T/bl PS_CURL=$T/nocurl \
    PS_TS_CLI=/nonexistent $PS aa 3 0 > "$T/aa.log" 2>&1
check "aa 3 windows: three summary rows" '[ "$(grep -c "aa-[123]" "$T/out/summary.tsv")" = 3 ]'
check "aa: no shell errors" '! grep -q "out of range\|bad number\|not found" "$T/aa.log"'

rm -rf "$T"
echo "power-sample: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
