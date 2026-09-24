/*
 * ui_exec.h - replace this process with a fresh copy of itself (theme switch).
 *
 * Same pid, so u60-uid (which watches the pid and checks /proc/<pid>/comm
 * starts with "u60pro-devui") sees no restart. The binary is exec'd by its
 * real path, not /proc/self/exe: the kernel names the new image after the
 * basename of the path given to execve, and "exe" would fail u60-uid's check
 * and get a second UI started next to this one (two UIs on /dev/dri/card0
 * reboot the device, 2026-09-17).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60PRO_UI_EXEC_H
#define U60PRO_UI_EXEC_H

#include "ui_logic.h"

/* Seconds on CLOCK_BOOTTIME (see ui_launch_t). */
long ui_boot_s(void);

/* Exec the running binary with argv[0] kept and `l` appended. Every fd above
 * stderr is marked close-on-exec first, so the new process opens DRM, touch,
 * key and sockets itself. Returns only on failure (-1, reason logged, this
 * process unchanged and still usable): binary replaced on disk since start
 * ("(deleted)"), readlink or execv error. */
int ui_exec_self(const char *argv0, const ui_launch_t *l);

#endif /* U60PRO_UI_EXEC_H */
