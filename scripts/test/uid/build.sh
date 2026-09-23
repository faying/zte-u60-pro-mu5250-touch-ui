#!/bin/sh
# Cross-build tests/uid_core_test in the u60-devui-build image.
# SPDX-License-Identifier: MIT
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
exec docker run --rm --platform linux/amd64 -v "$ROOT":/src -w /src u60-devui-build:latest bash -c \
    'export PATH=/opt/aarch64--musl--stable-2025.08-1/bin:$PATH; make -s uid-test CROSS_COMPILE=aarch64-linux-'
