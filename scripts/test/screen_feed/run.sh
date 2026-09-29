#!/bin/sh
# screen_feed.c tests (tests/screen_feed_test.c): reading datad's /v2/screen
# from a fake datad on loopback. Host build with ASan/UBSan.
# SPDX-License-Identifier: MIT
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
exec docker run --rm -v "$ROOT":/src:ro -w /src rust:1-slim sh -c '
cc -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-function \
   -Iinclude -Itests tests/screen_feed_test.c src/json.c -o /tmp/screen_feed_test && /tmp/screen_feed_test'
