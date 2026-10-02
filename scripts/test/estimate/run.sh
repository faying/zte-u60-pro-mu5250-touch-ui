#!/bin/sh
# estimate.c tests (tests/estimate_test.c): the estimate's wording and parsing
# zte-agent's report. Host build with ASan/UBSan in a throwaway container.
# The estimate itself is computed and tested in zte-agent (battery_eta.rs).
# SPDX-License-Identifier: MIT
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
exec docker run --rm -v "$ROOT":/src:ro -w /src rust:1-slim sh -c '
cc -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-function \
   -I. -Iinclude tests/estimate_test.c src/json.c src/lang.c -o /tmp/estimate_test && /tmp/estimate_test'
