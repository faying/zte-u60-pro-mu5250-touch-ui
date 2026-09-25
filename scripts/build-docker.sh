#!/bin/sh
# 在 Docker 里编触屏界面（LVGL 版）和屏幕守护进程 u60-uid，产物放 out/：
#
#   scripts/build-docker.sh [输出目录]     # 默认 ./out
#
#   out/u60pro-devui-lvgl.stripped   触屏界面（静态 aarch64，已 strip）
#   out/u60-uid                      屏幕守护进程（静态 aarch64，已 strip）
#
# 工具链（Bootlin aarch64 musl）是 x86_64 Linux 程序，镜像按 linux/amd64 建；
# Apple Silicon 的 Docker Desktop 会用模拟层跑，慢一些（第一次十几分钟）但能用。
# 源码树只读挂进容器，拷一份再编：不会在仓库里留 .o、不改 third_party/lvgl、
# 不覆盖仓库根目录已有的 u60pro-devui / u60-uid。
# LVGL 固定 v9.5.0（本仓库 patches/ 里的补丁针对这个版本）。
# SPDX-License-Identifier: MIT
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/out}
IMAGE=${DEVUI_BUILD_IMAGE:-u60-devui-build}
LVGL_REF=v9.5.0
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

docker build --platform linux/amd64 -t "$IMAGE" -f "$ROOT/Dockerfile.build" "$ROOT"
docker run --rm --platform linux/amd64 -v "$ROOT":/src:ro -v "$OUT":/out "$IMAGE" bash -c "
set -e
git config --global safe.directory '*'
mkdir -p /b && cd /src
tar --exclude='./out' --exclude='*.o' --exclude='./third_party/lvgl' --exclude='./.git' -cf - . | tar -C /b -xf -
cd /b
if [ -f /src/third_party/lvgl/lvgl.h ] && git -C /src/third_party/lvgl describe --tags 2>/dev/null | grep -qx $LVGL_REF; then
  git clone -q /src/third_party/lvgl third_party/lvgl 2>/dev/null || cp -a /src/third_party/lvgl third_party/lvgl
  git -C third_party/lvgl checkout -q -- . 2>/dev/null || true
else
  git -c advice.detachedHead=false clone -q --depth 1 --branch $LVGL_REF https://github.com/lvgl/lvgl.git third_party/lvgl
fi
make -j\$(nproc) CROSS_COMPILE=aarch64-linux- >/tmp/make.log 2>&1 || { tail -30 /tmp/make.log; exit 1; }
make u60-uid CROSS_COMPILE=aarch64-linux- >>/tmp/make.log 2>&1 || { tail -30 /tmp/make.log; exit 1; }
aarch64-linux-strip -o /out/u60pro-devui-lvgl.stripped u60pro-devui
aarch64-linux-strip -o /out/u60-uid u60-uid
grep -q 'pages+chrome built' /out/u60pro-devui-lvgl.stripped
ls -l /out
"
