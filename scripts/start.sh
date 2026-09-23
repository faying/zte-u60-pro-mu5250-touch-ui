#!/bin/sh
# Boot launcher for the U60Pro screen UI. Installed at
# /data/plugins/u60pro-devui/start.sh
# and invoked either by the procd init service or by the legacy rc.local hook.
# In procd mode it execs the foreground UI process. In legacy mode it keeps the
# old nohup-based launch path for manual installs and older plugins.
#
# SPDX-License-Identifier: MIT
DEVUI_DIR=/data/plugins/u60pro-devui
DATAD_DIR=/data/plugins/zwrt-datad
UI_DIR=$DEVUI_DIR/ui
LEGACY_UI_DIR=/data/ui
DEVUI_BIN=$DEVUI_DIR/u60pro-devui
CORNER_BIN=$DEVUI_DIR/corner-wake
DATAD_BIN=$DATAD_DIR/zwrt-datad
DATAD_LAN_PORT=${DATAD_LAN_PORT:-9461}
DATAD_LAN_BIND=${DATAD_LAN_BIND:-0.0.0.0}
DATAD_TOKEN_FILE=${DATAD_TOKEN_FILE:-$DATAD_DIR/auth.token}
DATAD_MODEM_REMOTE_STREAM=${DATAD_MODEM_REMOTE_STREAM:-1}
DATAD_MODEM_REMOTE_STALE_SEC=${DATAD_MODEM_REMOTE_STALE_SEC:-6}
MODE="${1:-legacy}"

mkdir -p "$DEVUI_DIR" "$DATAD_DIR" "$UI_DIR"

count_ui_pages() {
    find "$1" -maxdepth 1 -type f -name '*.html' 2>/dev/null | wc -l | tr -d ' '
}

migrate_legacy_ui() {
    [ -d "$LEGACY_UI_DIR" ] || return 0
    [ -f "$LEGACY_UI_DIR/.lockpin" ] && [ ! -f "$UI_DIR/.lockpin" ] \
        && cp -af "$LEGACY_UI_DIR/.lockpin" "$UI_DIR/.lockpin" 2>/dev/null
    old_count=$(count_ui_pages "$LEGACY_UI_DIR")
    new_count=$(count_ui_pages "$UI_DIR")
    if [ "$new_count" -le 0 ] && [ "$old_count" -gt 0 ]; then
        cp -af "$LEGACY_UI_DIR"/. "$UI_DIR"/ 2>/dev/null || true
    fi
}

read_mode_main_state() {
    awk -F"'" '/option mode_main_state/ { print $2; exit }' /etc/config/zwrt_zte_mc_tmp 2>/dev/null
}

read_reboot_reason_code() {
    awk -F"'" '/option reboot_reason_code/ { print $2; exit }' /etc/config/zwrt_zte_mc_tmp 2>/dev/null
}

boot_trace() {
    LOG="$DEVUI_DIR/boot-trace.log"
    {
        echo "=== $(date '+%Y-%m-%d %H:%M:%S') ==="
        echo "mode_main_state=$mode_main_state"
        echo "reboot_reason_code=$reboot_reason_code"
        echo "bootmode=$BOOTMODE"
        echo -n "cmdline="
        cat /proc/cmdline 2>/dev/null
        for f in \
            /sys/class/power_supply/usb/online \
            /sys/class/power_supply/usb/voltage_now \
            /sys/class/power_supply/battery/status \
            /sys/class/power_supply/battery/capacity \
            /sys/class/power_supply/charger_zte/present_mbb \
            /sys/class/power_supply/charger_zte/status_mbb \
            /sys/class/power_supply/type-c_zte/present_mbb \
            /sys/class/power_supply/type-c_zte/real_type_mbb \
            /sys/class/power_supply/statistics_zte/batt_status \
            /sys/class/power_supply/statistics_zte/batt_online \
            /sys/class/power_supply/battery_zte/status_mbb \
            /sys/class/power_supply/battery_zte/online_mbb
        do
            [ -e "$f" ] && echo "$f=$(cat "$f" 2>/dev/null)"
        done
        for e in /dev/input/event*; do
            [ -e "$e" ] || continue
            echo "$e=$(cat /sys/class/input/${e##*/}/device/name 2>/dev/null)"
        done
        echo
    } >> "$LOG"
    tail -n 160 "$LOG" > "$LOG.tmp" 2>/dev/null && mv "$LOG.tmp" "$LOG"
}

stop_vendor_ui() {
    had_vendor=0
    pidof zte_topsw_devui >/dev/null 2>&1 && had_vendor=1
    /etc/init.d/zte_topsw_devui stop 2>/dev/null
    killall -9 zte_topsw_devui 2>/dev/null
    [ "$had_vendor" -eq 1 ] && sleep 1
}

# corner-wake is the ONLY way back from the vendor UI once act:switchvendor
# hands the screen over (the vendor UI has no button that knows we exist).
# It self-gates on devui_running() — safe to leave running all the time,
# under either UI — so it belongs here next to the UI launch, not behind a
# vendor-only conditional. This was never actually wired into any boot path
# (not rc.local, not here) until 2026-09-22: it only ever ran when someone
# happened to start it by hand, so a device restart — or just that manual
# process dying — silently took away the switch-back gesture with no error
# anywhere, discovered only when a user got stuck in the vendor UI.
start_corner_wake() {
    [ -x "$CORNER_BIN" ] || return 0
    pidof corner-wake >/dev/null 2>&1 && return 0
    nohup "$CORNER_BIN" >/tmp/corner-wake.log 2>&1 </dev/null &
}

start_datad_legacy() {
    [ "$BOOTMODE" = normal ] || return 0
    [ -x "$DATAD_BIN" ] || return 0
    pidof zwrt-datad >/dev/null 2>&1 && return 0
    killall -9 u60-datad 2>/dev/null
    # zwrt-datad only opens the LAN listener (and keeps loopback auth-free) when
    # an explicit --lan-bind is given; --lan-port alone does neither, which left
    # DevUI's unauthenticated loopback polling getting 401 on every /state.
    # It also refuses to start without a non-empty token file, so seed one.
    if [ ! -s "$DATAD_TOKEN_FILE" ]; then
        mkdir -p "$(dirname "$DATAD_TOKEN_FILE")" || return 1
        token_tmp=$(mktemp "$DATAD_TOKEN_FILE.tmp.XXXXXX") || return 1
        chmod 600 "$token_tmp" || {
            rm -f "$token_tmp"
            return 1
        }
        (umask 077
         head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$token_tmp"
        )
        if [ ! -s "$token_tmp" ]; then
            rm -f "$token_tmp"
            return 1
        fi
        mv -f "$token_tmp" "$DATAD_TOKEN_FILE" || {
            rm -f "$token_tmp"
            return 1
        }
    fi
    # The token protects the LAN listener; keep new and existing credentials
    # private even when provisioning used a loose umask.
    chmod 600 "$DATAD_TOKEN_FILE" || return 1
    # ZWRT_DATAD_OTA_DISABLE_AUTO: belt and braces. Upstream's Rust datad
    # fetched and INSTALLED updates on its own from a netdisk share and GitHub;
    # our fork (faying/zte-u60-pro-mu5250-data-service, 107b2f6) has no
    # built-in servers and defaults to off, so the device depends on nothing
    # outside itself. Updates go through the side-by-side procedure like every
    # other binary. /data/zwrt-datad/ota.json says enabled=false as well. The
    # C datad ignores all of this.
    nohup env DATAD_MODEM_REMOTE_STREAM="$DATAD_MODEM_REMOTE_STREAM" \
        DATAD_MODEM_REMOTE_STALE_SEC="$DATAD_MODEM_REMOTE_STALE_SEC" \
        ZWRT_DATAD_OTA_DISABLE_AUTO=1 \
        "$DATAD_BIN" -i 1000 \
        --lan-bind "$DATAD_LAN_BIND" --lan-port "$DATAD_LAN_PORT" \
        --auth-token-file "$DATAD_TOKEN_FILE" >/tmp/zwrt-datad.log 2>&1 </dev/null &
    sleep 1
}

# Power-off charging boots do not expose silent_boot.mode=nonsilent. We still
# start DevUI there now, but only in its own full-screen charging mode.
BOOTMODE=charge
mode_main_state="$(read_mode_main_state)"
reboot_reason_code="$(read_reboot_reason_code)"
case "$mode_main_state" in
    mode_power_off_*) BOOTMODE=charge ;;
    mode_power_on|mode_power_on_charger) BOOTMODE=normal ;;
    *)
        if grep -q 'silent_boot.mode=nonsilent' /proc/cmdline 2>/dev/null; then
            BOOTMODE=normal
        fi
        ;;
esac
boot_trace
migrate_legacy_ui

case "$MODE" in
    procd)
        stop_vendor_ui
        start_corner_wake
        [ -x "$DEVUI_BIN" ] || exit 1
        exec "$DEVUI_BIN"
        ;;
    legacy)
        stop_vendor_ui
        # Data aggregator first on normal boots (the UI reads its snapshot).
        # For charge-only boots the full-screen charging UI can fall back to
        # sysfs, so there is no need to wake extra polling daemons.
        start_datad_legacy
        start_corner_wake
        if [ -x "$DEVUI_BIN" ]; then
            nohup "$DEVUI_BIN" >/tmp/u60pro-devui.log 2>&1 </dev/null &
            # Startup guard, added 2026-09-21 while the LVGL UI is on trial.
            # If the UI is gone 15s after boot, put the fallback binary back
            # and start that instead: a UI that dies on every boot otherwise
            # gets escalated by the vendor watchdog into a whole-device reboot
            # loop, which is very hard to recover from without a console.
            # Deleting u60pro-devui.fallback disables this entirely.
            if [ -x "$DEVUI_DIR/u60pro-devui.fallback" ]; then
                ( sleep 15
                  pidof u60pro-devui >/dev/null 2>&1 && exit 0
                  cp -f "$DEVUI_DIR/u60pro-devui.fallback" "$DEVUI_BIN.tmp" \
                      && mv -f "$DEVUI_BIN.tmp" "$DEVUI_BIN"
                  nohup "$DEVUI_BIN" >/tmp/u60pro-devui.log 2>&1 </dev/null &
                  echo "$(date '+%Y-%m-%d %H:%M:%S') startup-guard: UI died, restored fallback" \
                      >> "$DEVUI_DIR/boot-trace.log"
                ) >/dev/null 2>&1 &
            fi
        fi
        ;;
    *)
        echo "usage: $0 [procd|legacy]" >&2
        exit 2
        ;;
esac
