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

# 只编 LVGL 路径实际用到的文件（main.c/ui.c 及其依赖），排除旧 litehtml
# 渲染器专属的 htmlmain.c/devui_ext.c（含各自的 main() 或 litehtml 依赖），
# 以及独立工具 fbdump.c/fbserver.c/drm_test.c/touchsim.c（各带自己的 main()）。
# chill.c/tailscale.c/esim.c 都已接入 LVGL 版（2026-09-21 起 CHILL 有首页卡片
# 和二级页，直接用 chill.c 的 getter，不走 HTML）。
APP_SRCS  := src/main.c src/ui.c src/drm_disp.c src/touch_input.c \
             src/backlight.c src/data.c src/key_input.c src/json.c \
             src/tailscale.c src/esim.c src/chill.c src/speedtest.c src/scenario.c src/alerts.c
LVGL_SRCS := $(shell find $(LVGL_DIR)/src -name '*.c' 2>/dev/null)
OBJS      := $(APP_SRCS:.c=.o) $(LVGL_SRCS:.c=.o)

FT_DIR ?= /opt/freetype-musl

CFLAGS := -std=c11 -Os -ffunction-sections -fdata-sections \
          -Wall -Wextra -Wno-unused-parameter \
          -D_GNU_SOURCE -DLV_CONF_INCLUDE_SIMPLE \
          -I$(ROOT) -Iinclude -I$(LVGL_DIR) -Ithird_party/stb -I$(FT_DIR)/include

LDFLAGS := -static -Wl,--gc-sections -pthread -lm -L$(FT_DIR)/lib -lfreetype

.PHONY: all clean check-lvgl

all: check-lvgl $(TARGET)

check-lvgl:
	@test -f $(LVGL_DIR)/lvgl.h || { \
	  echo "ERROR: LVGL not found at $(LVGL_DIR)."; \
	  echo "Run: git clone --depth 1 --branch v9.5.0 https://github.com/lvgl/lvgl.git $(LVGL_DIR)"; \
	  echo "  (or clone it there manually)"; exit 1; }

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

clean:
	rm -f $(OBJS) $(TARGET) $(CORNER_TARGET) $(DRMOWNER_TARGET) $(UID_TARGET) scripts/test/uid/uid_core_test
