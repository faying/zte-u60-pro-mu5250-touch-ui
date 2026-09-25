#!/bin/sh
# tailscale.c parser tests (tests/tailscale_test.c): host build with
# ASan/UBSan in a throwaway container. No device, no tailscaled.
# SPDX-License-Identifier: MIT
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
exec docker run --rm -v "$ROOT":/src:ro -w /src rust:1-slim sh -c '
cc -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-function \
   -Iinclude tests/tailscale_test.c src/json.c -o /tmp/tailscale_test && /tmp/tailscale_test'
