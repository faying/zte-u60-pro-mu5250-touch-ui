# u60pro-devui - cross-compile to a single static aarch64 binary.
#
# Build (in a POSIX shell: WSL, Git-Bash, or Linux) with an aarch64 musl
# toolchain on PATH, e.g. from https://musl.cc (aarch64-linux-musl-cross):
#
#   make CROSS_COMPILE=aarch64-linux-musl-
#
# The result is a self-contained `u60pro-devui` binary with no runtime deps.
#
# SPDX-License-Identifier: MIT

CROSS_COMPILE ?= aarch64-linux-musl-
CC := $(CROSS_COMPILE)gcc

TARGET   := u60pro-devui
ROOT     := .
LVGL_DIR := third_party/lvgl
LVGL_PATCH := patches/lvgl-v9.5.0-freetype-bold.patch
# git apply run from / (outside any repo, so a worktree's .git file or a
# half-mounted checkout cannot confuse it), re-rooted onto this directory.
LVGL_APPLY = cd / && git apply --directory=$(patsubst /%,%,$(CURDIR)) $(1) $(CURDIR)/$(LVGL_PATCH)

# 只编 LVGL 路径实际用到的文件（main.c/ui.c 及其依赖），排除旧 litehtml
# 渲染器专属的 htmlmain.c/devui_ext.c（含各自的 main() 或 litehtml 依赖），
# 以及独立工具 fbdump.c/fbserver.c/drm_test.c/touchsim.c（各带自己的 main()）。
# tailscale.c/esim.c 都已接入 LVGL 版，直接用各自的 getter，不走 HTML。
APP_SRCS  := src/main.c src/ui.c src/ui_logic.c src/ui_theme.c src/ui_kit.c src/ui_exec.c src/drm_disp.c src/touch_input.c src/backlight.c src/data.c src/key_input.c src/json.c src/tailscale.c src/esim.c src/speedtest.c src/scenario.c src/alerts.c src/netinfo.c src/estimate.c src/battery_est.c
LVGL_SRCS := $(shell find $(LVGL_DIR)/src -name '*.c' 2>/dev/null)
OBJS      := $(APP_SRCS:.c=.o) $(LVGL_SRCS:.c=.o)

FT_DIR ?= /opt/freetype-musl

CFLAGS := -std=c11 -Os -ffunction-sections -fdata-sections \
          -Wall -Wextra -Wno-unused-parameter \
          -D_GNU_SOURCE -DLV_CONF_INCLUDE_SIMPLE \
          -I$(ROOT) -Iinclude -I$(LVGL_DIR) -Ithird_party/stb -I$(FT_DIR)/include

LDFLAGS := -static -Wl,--gc-sections -pthread -lm -L$(FT_DIR)/lib -lfreetype

.PHONY: all clean check-lvgl render-test render-golden

all: check-lvgl $(TARGET)

check-lvgl:
	@test -f $(LVGL_DIR)/lvgl.h || { \
	  echo "ERROR: LVGL not found at $(LVGL_DIR)."; \
	  echo "Run: git clone --depth 1 --branch v9.5.0 https://github.com/lvgl/lvgl.git $(LVGL_DIR)"; \
	  echo "  (or clone it there manually)"; exit 1; }
	@# Synthetic-bold CJK needs a small LVGL patch (git apply: the build image has no patch(1)). The source repo carries it
	@# already committed (reverse dry-run succeeds → nothing to do); a fresh
	@# clone gets it applied here. A patch that neither reverses nor applies
	@# means LVGL changed underneath us: stop instead of building without bold.
	@if ($(call LVGL_APPLY,-R --check)) >/dev/null 2>&1; then :; \
	elif ($(call LVGL_APPLY,--check)) >/dev/null 2>&1; then \
	  ($(call LVGL_APPLY,)) && echo "applied $(LVGL_PATCH)"; \
	else echo "ERROR: $(LVGL_PATCH) does not apply to $(LVGL_DIR) (expected LVGL v9.5.0)."; exit 1; fi
	@if [ -f $(FT_DIR)/lib/libfreetype.a ] && ! $(CROSS_COMPILE)nm $(FT_DIR)/lib/libfreetype.a 2>/dev/null | grep -q " T FT_GlyphSlot_Embolden"; then \
	  echo "ERROR: $(FT_DIR)/lib/libfreetype.a has no FT_GlyphSlot_Embolden; rebuild it with scripts/_build_freetype.sh (needs base/ftsynth)."; exit 1; fi

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo "built $(TARGET):"
	@$(CROSS_COMPILE)size $(TARGET) 2>/dev/null || true

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Standalone helper: corner long-press listener that hands the screen back to
# the DevUI while the vendor UI is on screen. Links only touch_input.c + libc,
# so it builds without LVGL or FreeType.
CORNER_TARGET := corner-wake
CORNER_SRCS   := src/corner_wake.c src/touch_input.c
CORNER_CFLAGS := -std=c11 -Os -ffunction-sections -fdata-sections \
                 -Wall -Wextra -Wno-unused-parameter \
                 -D_GNU_SOURCE -I$(ROOT) -Iinclude

$(CORNER_TARGET): $(CORNER_SRCS)
	$(CC) $(CORNER_CFLAGS) $(CORNER_SRCS) -o $@ -static -Wl,--gc-sections
	@echo "built $(CORNER_TARGET):"
	@$(CROSS_COMPILE)size $(CORNER_TARGET) 2>/dev/null || true

# Standalone helper: one-shot query of which process holds the DRM device
# (default /dev/dri/card0). Pure libc, no LVGL/FreeType/touch_input deps.
# Feasibility of the underlying /proc/<pid>/fd scan confirmed 2026-09-17 via
# a manual SSH probe on the device (root can read other processes' fd tables
# on this kernel); this target is the standalone tool built from that.
DRMOWNER_TARGET := drm-owner
DRMOWNER_SRCS   := src/drm_owner.c
DRMOWNER_CFLAGS := -std=c11 -Os -ffunction-sections -fdata-sections \
                   -Wall -Wextra -Wno-unused-parameter \
                   -D_GNU_SOURCE -I$(ROOT) -Iinclude

$(DRMOWNER_TARGET): $(DRMOWNER_SRCS)
	$(CC) $(DRMOWNER_CFLAGS) $(DRMOWNER_SRCS) -o $@ -static -Wl,--gc-sections
	@echo "built $(DRMOWNER_TARGET):"
	@$(CROSS_COMPILE)size $(DRMOWNER_TARGET) 2>/dev/null || true

# u60-uid: the screen-owner daemon (starts/stops u60pro-devui, hands the panel
# to the vendor UI, corner long-press back). Replaces corner-wake. libc +
# touch_input.c only, like corner-wake. Decisions live in uid_core.c so they
# can be unit-tested: `make uid-test` → tests/uid_core_test (static, runs in an
# arm64 busybox container via scripts/test/docker.sh).
UID_TARGET := u60-uid
UID_SRCS   := src/uid.c src/uid_core.c src/touch_input.c
UID_CFLAGS := -std=c11 -Os -ffunction-sections -fdata-sections \
              -Wall -Wextra -Wno-unused-parameter \
              -D_GNU_SOURCE -I$(ROOT) -Iinclude

$(UID_TARGET): $(UID_SRCS) include/uid_core.h
	$(CC) $(UID_CFLAGS) $(UID_SRCS) -o $@ -static -Wl,--gc-sections
	@echo "built $(UID_TARGET):"
	@$(CROSS_COMPILE)size $(UID_TARGET) 2>/dev/null || true

uid-test: tests/uid_core_test.c src/uid_core.c include/uid_core.h
	$(CC) $(UID_CFLAGS) tests/uid_core_test.c src/uid_core.c -o scripts/test/uid/uid_core_test -static

# Pure UI decisions (appearance, exec guard, status-bar/battery/signal state)
# live in ui_logic.c so they can be unit-tested without LVGL:
# `make ui-logic-test` → scripts/test/ui_logic/ui_logic_test (static, arm64 container).
ui-logic-test: tests/ui_logic_test.c src/ui_logic.c include/ui_logic.h
	$(CC) $(UID_CFLAGS) tests/ui_logic_test.c src/ui_logic.c -o scripts/test/ui_logic/ui_logic_test -static

# Theme switch = exec self: same pid, same comm, no fd leaks (arm64 container).
ui-exec-test: tests/ui_exec_test.c src/ui_exec.c src/ui_logic.c include/ui_exec.h include/ui_logic.h
	$(CC) $(UID_CFLAGS) tests/ui_exec_test.c src/ui_exec.c src/ui_logic.c -o scripts/test/ui_exec/ui_exec_test -static

# Offscreen render test (tests/render): ui.c + fixtures + LVGL, static arm64.
# Run it with scripts/test/render/render.sh (docker; needs the device fonts).
RENDER_BIN  := scripts/test/render/render_test
RENDER_SRCS := tests/render/render_test.c tests/render/fixtures.c src/ui_logic.c src/ui_theme.c src/ui_kit.c src/estimate.c
render-test-bin: check-lvgl $(LVGL_SRCS:.c=.o) $(RENDER_SRCS) src/ui.c tests/render/fixtures.h
	$(CC) $(CFLAGS) -Itests/render -Wno-unused-function $(RENDER_SRCS) $(LVGL_SRCS:.c=.o) -o $(RENDER_BIN) $(LDFLAGS)

render-test:
	sh scripts/test/render/render.sh
render-golden:
	sh scripts/test/render/render.sh --write-golden

clean:
	rm -f $(OBJS) $(TARGET) $(CORNER_TARGET) $(DRMOWNER_TARGET) $(UID_TARGET) scripts/test/uid/uid_core_test scripts/test/ui_logic/ui_logic_test scripts/test/ui_exec/ui_exec_test $(RENDER_BIN)
