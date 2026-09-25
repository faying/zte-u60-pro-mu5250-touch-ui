#!/bin/sh
# 生成首页结论块右上角的运营商 logo（透明底 PNG）：
#
#   scripts/logos/build-operator-logos.sh <SVG 目录> <输出目录>
#
# SVG 目录一般是 manager 的 web/public/operator-logos（网页首页用的同一批）。
# 每个 <slug>.svg 产出两张，装到 /data/plugins/u60pro-devui/operator-logos/：
#   <slug>.png     原色，浅色主题用
#   <slug>-w.png   单色白（界面里再压到约 85% 不透明），深色主题用：深蓝、深红的 logo 在深绿底上看不见
# 高 14 像素，宽按比例、最多 64（界面里就是这个大小，不缩放）。
#
# logo 是商标：PNG 和 SVG 都不进开源仓库（tools/sync-public.sh 删），PNG 是二进制，不进任何 git，
# 只随装机包走。全在 Docker（alpine + rsvg-convert + ImageMagick）里做，本机不用装东西。
# SPDX-License-Identifier: MIT
set -eu

SRC=${1:?用法: build-operator-logos.sh <SVG 目录> <输出目录>}
OUT=${2:?用法: build-operator-logos.sh <SVG 目录> <输出目录>}
mkdir -p "$OUT"
SRC=$(cd "$SRC" && pwd); OUT=$(cd "$OUT" && pwd)

docker run --rm -v "$SRC:/src:ro" -v "$OUT:/out" alpine:3.20 sh -euc '
apk add -q --no-cache rsvg-convert imagemagick >/dev/null
H=14 MAXW=64
for f in /src/*.svg; do
    s=$(basename "$f" .svg)
    # 按高度 14 渲染，太宽的再按宽度 64 重渲，保证不超框也不糊
    rsvg-convert -h $H "$f" -o /tmp/a.png
    w=$(magick identify -format %w /tmp/a.png)
    [ "$w" -gt $MAXW ] && rsvg-convert -w $MAXW "$f" -o /tmp/a.png
    magick /tmp/a.png -trim +repage -strip "PNG32:/out/$s.png"
    # 原图里本来是白的部分（LG 的脸、M1 的字）先抠掉，否则整块刷白就成了实心圆
    magick "/out/$s.png" -fuzz 12% -transparent white -fill white -colorize 100 -strip "PNG32:/out/$s-w.png"
    echo "$s $(magick identify -format %wx%h "/out/$s.png")"
done
'
