#!/bin/sh
# Cross-build the offscreen render test (static aarch64) in the u60-devui-build
# image. FreeType with base/ftsynth is built once and cached in the docker
# volume u60-ftcache.
# SPDX-License-Identifier: MIT
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
exec docker run --rm --platform linux/amd64 -v "$ROOT":/src -v u60-ftcache:/cache -w /src u60-devui-build:latest bash -c '
set -e; export HOME=/root PATH=/opt/aarch64--musl--stable-2025.08-1/bin:$PATH
ln -sf /opt/aarch64--musl--stable-2025.08-1 /root/
if ! aarch64-linux-nm /cache/ft/lib/libfreetype.a 2>/dev/null | grep -q " T FT_GlyphSlot_Embolden"; then
  bash scripts/_build_freetype.sh >/tmp/ft.log 2>&1 || { tail -20 /tmp/ft.log; exit 1; }
  rm -rf /cache/ft; cp -a /root/freetype-musl /cache/ft
fi
make -s -j8 render-test-bin CROSS_COMPILE=aarch64-linux- FT_DIR=/cache/ft'
