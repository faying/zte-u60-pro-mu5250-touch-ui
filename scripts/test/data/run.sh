#!/bin/sh
# data.c tests (tests/data_test.c): datad going silent after having answered.
# Host build with ASan/UBSan in a throwaway container, fake datad on loopback.
# SPDX-License-Identifier: MIT
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
exec docker run --rm -v "$ROOT":/src:ro -w /src rust:1.99.0-slim@sha256:24e632c09342c20abf8312cf4f61430a911c01ed3a5e4c02b87292b1c39c5273 sh -c '
cc -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-function \
   -I. -Iinclude tests/data_test.c src/json.c src/ui_logic.c src/key_input.c src/lang.c -o /tmp/data_test 2>&1 | grep -v "^$" | grep -E "error|warning" ; /tmp/data_test'
