#!/bin/sh
# op_view.c tests (tests/op_view_test.c): parsing datad's /v2/screen "op"
# (E4 write transactions). Host build with ASan/UBSan.
# SPDX-License-Identifier: MIT
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
exec docker run --rm -v "$ROOT":/src:ro -w /src rust:1-slim sh -c '
cc -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-function \
   -I. -Iinclude -Itests tests/op_view_test.c src/json.c src/lang.c -o /tmp/op_view_test && /tmp/op_view_test'
