#!/bin/sh
# alerts.c parser tests (tests/alerts_test.c): host build with ASan/UBSan in
# a throwaway container. No device needed.
# SPDX-License-Identifier: MIT
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
exec docker run --rm -v "$ROOT":/src:ro -w /src rust:1-slim sh -c '
cc -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-function \
   -Iinclude tests/alerts_test.c src/json.c -o /tmp/alerts_test && /tmp/alerts_test'
