#!/bin/sh
# estimate.c tests (tests/estimate_test.c): host build with ASan/UBSan in a
# throwaway container. Also checks that tests/fixtures/battery-estimate.json
# is the copy recorded in battery-estimate.sha256 (= manager
# docs/battery-estimate.md). No device needed.
# SPDX-License-Identifier: MIT
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
exec docker run --rm -v "$ROOT":/src:ro -w /src rust:1-slim sh -c '
want=$(cat tests/fixtures/battery-estimate.sha256)
got=$(sha256sum tests/fixtures/battery-estimate.json | cut -d" " -f1)
if [ "$want" != "$got" ]; then echo "FAIL fixtures sha256 $got, want $want"; exit 1; fi
echo "  ok   fixtures sha256"
cc -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-function \
   -Iinclude tests/estimate_test.c src/json.c -lm -o /tmp/estimate_test && /tmp/estimate_test'
