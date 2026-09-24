#!/bin/sh
# Offscreen render test: every scene x light/dark, in an arm64 container.
#
#   scripts/test/render/build.sh
#   U60_DEVICE_FONTS=<dir with ZTEZhengYuan.ttf, Roboto.ttf> \
#   U60_NUNITO_DIR=<dir with Nunito-600/700/800.ttf> \
#   scripts/test/render/render.sh [--write-golden] [--png DIR] [--dump] [--cjk-fallback] [scene…]
#
# --cjk-fallback: no device CJK font, only the bundled subset
# ($U60_NUNITO_DIR/u60-cjk-fallback.ttf, scripts/fonts/build-cjk-fallback.sh).
# Pixel goldens are for the device font, so they are not compared; the text
# and missing-glyph checks still run on every page.
#
# The device fonts are not in the repo (ZTEZhengYuan is proprietary): copy
# them from /usr/ui/fonts on a U60 Pro. Golden hashes depend on the exact
# font files. --write-golden records the current pixels as correct (after a
# deliberate visual change); without it, any pixel change fails.
# SPDX-License-Identifier: MIT
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
BIN=scripts/test/render/render_test
mode=--golden
png=
dump=
scenes=
while [ $# -gt 0 ]; do
    case $1 in
        --write-golden) mode=--write-golden ;;
        --png) png=$2; shift ;;
        --dump) dump=--dump ;;
        --cjk-fallback) mode=none ;;
        *) scenes="$scenes $1" ;;
    esac
    shift
done
[ -x "$ROOT/$BIN" ] || { echo "  FAIL $BIN not built (scripts/test/render/build.sh)"; exit 1; }
for f in ZTEZhengYuan.ttf Roboto.ttf; do
    [ -f "${U60_DEVICE_FONTS:-/nonexistent}/$f" ] || { echo "  FAIL set U60_DEVICE_FONTS to a dir with $f (from /usr/ui/fonts on the device)"; exit 1; }
done
[ -f "${U60_NUNITO_DIR:-/nonexistent}/Nunito-800.ttf" ] || { echo "  FAIL set U60_NUNITO_DIR to a dir with Nunito-600/700/800.ttf"; exit 1; }
cjk=/fonts/dev/ZTEZhengYuan.ttf
if [ "$mode" = none ]; then
    [ -f "$U60_NUNITO_DIR/u60-cjk-fallback.ttf" ] || { echo "  FAIL no u60-cjk-fallback.ttf in U60_NUNITO_DIR (scripts/fonts/build-cjk-fallback.sh)"; exit 1; }
    cjk=/nonexistent/ZTEZhengYuan.ttf
fi
[ -n "$scenes" ] || scenes="good weak nosignal datad-down loading nosim abroad lowbat full-charging long-names empty"
pngmount=
[ -n "$png" ] && { mkdir -p "$png"; pngmount="-v $(cd "$png" && pwd):/png"; }

exec docker run --rm --platform linux/arm64 -v "$ROOT":/src:rw $pngmount \
    -v "$U60_DEVICE_FONTS":/fonts/dev:ro -v "$U60_NUNITO_DIR":/fonts/nunito:ro \
    -e U60_DEVUI_CJK_FONT=$cjk -e U60_DEVUI_ROBOTO=/fonts/dev/Roboto.ttf \
    -e U60_DEVUI_FONT_DIR=/fonts/nunito -w /src busybox:latest sh -c '
rc=0; mode=$1; png=$2; dump=$3; shift 3
for s in "$@"; do for t in light dark; do
    g=tests/render/golden/$s-$t.txt
    p=; [ -n "$png" ] && p=--png=/png
    gs=$mode=$g; [ "$mode" = none ] && gs=
    timeout 120 ./'"$BIN"' --scene=$s --theme=$t $gs --expect=tests/render/expect.txt $p $dump 2>/tmp/err.$s.$t
    r=$?
    if [ "$mode" = none ] && ! grep -q "fonts cjk=bundled" /tmp/err.$s.$t; then rc=1; echo "  FAIL [$s/$t] bundled CJK font not used: $(grep "fonts cjk" /tmp/err.$s.$t)"; fi
    [ $r -eq 0 ] || { rc=1; [ $r -ge 124 ] && echo "  FAIL [$s/$t] crashed or hung (exit $r)"; tail -5 /tmp/err.$s.$t; }
done; done
# right after a theme switch (--tab=3): a real tap on the other appearance must switch
for t in light dark; do
    timeout 60 ./'"$BIN"' --scene=good --theme=$t --launched-tab=3 2>/dev/null | grep -q "exec calls 1" \
        || { rc=1; echo "  FAIL [good/$t] tap after a --tab=3 start did not switch"; }
done
exit $rc' sh "$mode" "$png" "$dump" $scenes
