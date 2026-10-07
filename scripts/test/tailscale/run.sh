#!/bin/sh
# tailscale.c parser tests (tests/tailscale_test.c): host build with
# ASan/UBSan in a throwaway container. No device, no tailscaled.
# SPDX-License-Identifier: MIT
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
exec docker run --rm -v "$ROOT":/src:ro -w /src rust:1.99.0-slim@sha256:24e632c09342c20abf8312cf4f61430a911c01ed3a5e4c02b87292b1c39c5273 sh -c '
cc -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-function \
   -Iinclude tests/tailscale_test.c src/json.c src/http.c src/agent_client.c -o /tmp/tailscale_test && /tmp/tailscale_test'
