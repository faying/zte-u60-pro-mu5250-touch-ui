/*
 * ui_exec_test - does ui_exec_self() keep what u60-uid relies on?
 *   same pid, comm = basename of the real binary (not "exe"), fds above stderr
 *   closed in the new image, launch args delivered, and a binary replaced on
 *   disk is refused. Run it under its real name and under a side name
 *   (u60pro-devui.test): scripts/test/ui_exec/run.sh
 *
 * SPDX-License-Identifier: MIT
 */
#include "ui_exec.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int pass, fail;
#define CHECK(desc, cond) do { if (cond) { pass++; printf("  ok   %s\n", desc); } \
                               else { fail++; printf("  FAIL %s  [%s]\n", desc, #cond); } } while (0)

static void read_comm(char *out, size_t n)
{
    FILE *f = fopen("/proc/self/comm", "r");
    out[0] = 0;
    if (f) { if (fgets(out, (int)n, f)) out[strcspn(out, "\n")] = 0; fclose(f); }
}

int main(int argc, char **argv)
{
    ui_launch_t L;
    ui_launch_parse(argc, argv, &L);

    if (argc > 1 && !strcmp(argv[1], "--delete-self")) {
        char p[512];
        ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
        if (n > 0) { p[n] = 0; unlink(p); }
        ui_launch_t l = { 1, 0, 0, -1, -1, { 0, 0, 0 } };
        CHECK("replaced binary: exec refused, returns -1", ui_exec_self(argv[0], &l) == -1);
        printf("passed %d, failed %d\n", pass, fail);
        return fail ? 1 : 0;
    }

    if (L.tab < 0) {   /* first image */
        char comm[64], pid[32];
        read_comm(comm, sizeof comm);
        snprintf(pid, sizeof pid, "%d", (int)getpid());
        setenv("T_PID", pid, 1);
        setenv("T_COMM", comm, 1);
        int fds[2];
        if (pipe(fds) == 0) {
            char s[16];
            snprintf(s, sizeof s, "%d", fds[1]);
            setenv("T_FD", s, 1);
        }
        int dn = open("/dev/null", O_RDONLY);
        (void)dn;
        ui_launch_t l = { 2, 340, 1, 30000, 200, { 100, 100, 1 } };
        ui_exec_self(argv[0], &l);
        printf("  FAIL exec returned\npassed 0, failed 1\n");
        return 1;
    }

    char comm[64];
    read_comm(comm, sizeof comm);
    printf("  comm before=%s after=%s\n", getenv("T_COMM"), comm);
    CHECK("same pid", getenv("T_PID") && atoi(getenv("T_PID")) == (int)getpid());
    CHECK("comm unchanged (not \"exe\")", getenv("T_COMM") && !strcmp(getenv("T_COMM"), comm));
    CHECK("comm starts with u60-uid's prefix when run as u60pro-devui*",
          strncmp(getenv("T_COMM") ? getenv("T_COMM") : "", "u60pro-devui", 12) != 0 ||
          strncmp(comm, "u60pro-devui", 12) == 0);
    int leaked = 0;
    for (int fd = 3; fd < 64; fd++) if (fcntl(fd, F_GETFD) >= 0) leaked++;
    CHECK("no fd above stderr survived", leaked == 0);
    CHECK("launch args delivered", L.tab == 2 && L.scroll_y == 340 && L.screen_off && L.autooff_ms == 30000 &&
          L.bright == 200 && L.hist.count == 1 && L.hist.first_s == 100);
    CHECK("boot clock ticks", ui_boot_s() > 0);
    printf("passed %d, failed %d\n", pass, fail);
    return fail ? 1 : 0;
}
