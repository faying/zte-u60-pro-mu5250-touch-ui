#!/bin/sh
# diag_view.c tests (tests/diag_view_test.c): parsing zte-agent's
# /api/diagnose run. Host build with ASan/UBSan in a throwaway container.
# SPDX-License-Identifier: MIT
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
exec docker run --rm -v "$ROOT":/src:ro -w /src rust:1-slim sh -c '
cc -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-function \
   -I. -Iinclude -Itests tests/diag_view_test.c src/json.c src/lang.c -o /tmp/diag_view_test && /tmp/diag_view_test'
