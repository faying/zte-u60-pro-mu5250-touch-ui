#!/bin/sh
# Device-side installer: restores the known-good boot path: vendor
# zte_topsw_devui stays enabled for early panel/touch bring-up, and rc.local
# later runs /data/plugins/u60pro-devui/start.sh to hand over to our DevUI.
#
# Expects the binaries + start.sh already copied into the plugin dirs.
#   adb push u60pro-devui scripts/start.sh /data/plugins/u60pro-devui/
#   adb push zwrt-datad /data/plugins/zwrt-datad/
#   adb push scripts/install-autostart.sh /tmp/ && adb shell sh /tmp/install-autostart.sh
#
# SPDX-License-Identifier: MIT
DEVUI_DIR=/data/plugins/u60pro-devui
DATAD_DIR=/data/plugins/zwrt-datad
UI_DIR=$DEVUI_DIR/ui
LEGACY_DIR=/data/u60pro
LEGACY_UI_DIR=/data/ui
RC=${AUTOSTART_RC:-/etc/rc.local}
HOOK="[ -x $DEVUI_DIR/start.sh ] && sh $DEVUI_DIR/start.sh >/tmp/u60pro-boot.log 2>&1 & # u60pro_devui"

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

# rc.local: drop the old start.sh hooks and put ours before the first exit 0,
# in one candidate written next to rc.local (/tmp is RAM: a mv from there is a
# copy, not a rename) with rc.local's mode (cp -p). The candidate must pass
# sh -n; then sync, mv over rc.local, sync: a power cut leaves the old file or
# the new one, never half of one (half = nothing starts at boot). Unchanged:
# nothing written. Changed: the file as it was is kept as $RC.pre-autostart.
# Fails (1) with rc.local untouched when it is missing or anything goes wrong.
update_rc_hook() {
    [ -f "$RC" ] || return 1
    _rt="$RC.autostart.tmp"
    rm -f "$_rt"
    if ! { cp -p "$RC" "$_rt" &&
        grep -v -e "$DEVUI_DIR/start.sh" -e "$LEGACY_DIR/start.sh" -e "u60pro_devui" "$RC" |
        awk -v hook="$HOOK" '/^exit 0/ && !d { print hook; d = 1 } { print }' >"$_rt"; }; then
        rm -f "$_rt"
        echo "rc.local: could not write $_rt; left as it was" >&2
        return 1
    fi
    if cmp -s "$_rt" "$RC"; then
        rm -f "$_rt"
        return 0
    fi
    if ! sh -n "$_rt"; then
        rm -f "$_rt"
        echo "rc.local: the new version fails sh -n; left as it was" >&2
        return 1
    fi
    if ! { cp -p "$RC" "$RC.pre-autostart" && sync && mv -f "$_rt" "$RC" && sync; }; then
        rm -f "$_rt"
        echo "rc.local: could not replace it; left as it was" >&2
        return 1
    fi
}

mkdir -p "$DEVUI_DIR" "$DATAD_DIR" "$UI_DIR"
[ -f "$LEGACY_DIR/devui.conf" ] && [ ! -f "$DEVUI_DIR/devui.conf" ] && cp -f "$LEGACY_DIR/devui.conf" "$DEVUI_DIR/devui.conf"
migrate_legacy_ui
chmod 755 "$DEVUI_DIR/start.sh" "$DEVUI_DIR/u60pro-devui" "$DATAD_DIR/zwrt-datad" 2>/dev/null
rm -f "$DATAD_DIR/u60-datad" "$DEVUI_DIR"/*.new "$DATAD_DIR"/*.new \
      "$DEVUI_DIR/ui.tar.gz" "$DEVUI_DIR/u60pro_ui.tar.gz" "$DEVUI_DIR/boot-trace.log.tmp" 2>/dev/null
rm -rf "$DEVUI_DIR/ui_extract" 2>/dev/null

update_rc_hook

/etc/init.d/u60pro-devui disable 2>/dev/null
# /etc/init.d/zwrt-datad may now be the procd service from the install kit
# (zwrt-datad.init: it runs datad through supervise.sh). That one is not a
# leftover — leave it alone, or this script stops the data service it relies on.
datad_is_kit_service() { grep -q supervise.sh /etc/init.d/zwrt-datad 2>/dev/null; }
datad_is_kit_service || /etc/init.d/zwrt-datad disable 2>/dev/null
/etc/init.d/u60-datad disable 2>/dev/null
rm -f /etc/rc.d/S*u60pro-devui /etc/rc.d/K*u60pro-devui \
      $(datad_is_kit_service || echo /etc/rc.d/S*zwrt-datad /etc/rc.d/K*zwrt-datad) \
      /etc/rc.d/S*u60-datad /etc/rc.d/K*u60-datad

/etc/init.d/u60pro-devui stop 2>/dev/null
datad_is_kit_service || /etc/init.d/zwrt-datad stop 2>/dev/null
/etc/init.d/u60-datad stop 2>/dev/null

killall -9 u60pro-devui 2>/dev/null
datad_is_kit_service || killall -9 zwrt-datad 2>/dev/null
killall -9 u60-datad 2>/dev/null

rm -f "$LEGACY_DIR/u60pro-devui" "$LEGACY_DIR/u60-datad" "$LEGACY_DIR/zwrt-datad" \
      "$LEGACY_DIR/start.sh" "$LEGACY_DIR/u60pro_ui.tar.gz" "$LEGACY_DIR/ui.tar.gz" \
      "$LEGACY_DIR/boot-trace.log" "$LEGACY_DIR/boot-trace.log.tmp" "$LEGACY_DIR"/*.new 2>/dev/null
rm -rf "$LEGACY_DIR/ui_extract" 2>/dev/null
rm -rf "$LEGACY_DIR" "$LEGACY_UI_DIR"

/etc/init.d/zte_topsw_devui enable 2>/dev/null
/etc/init.d/zte_topsw_devui start 2>/dev/null

sh "$DEVUI_DIR/start.sh" >/tmp/u60pro-boot.log 2>&1 &

echo "--- service status ---"
if [ -x /etc/init.d/zte_topsw_devui ]; then
    /etc/init.d/zte_topsw_devui enabled >/dev/null 2>&1
echo "zte_topsw_devui_enabled=$?"
fi
echo "u60pro_devui_enabled=legacy_rc_local"
ps | grep -E 'zte_topsw_devui|u60pro-devui|zwrt-datad|u60-datad' | grep -v grep || true

echo "--- rc.local tail ---"
grep -n "u60pro\|plugins\|exit 0" "$RC" 2>/dev/null || true
