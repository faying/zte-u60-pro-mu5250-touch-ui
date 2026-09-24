/*
 * ui_exec.c - exec a fresh copy of this binary; see ui_exec.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "ui_exec.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

long ui_boot_s(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0) clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

static void cloexec_all(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d) {
        for (int fd = 3; fd < 1024; fd++) {
            int fl = fcntl(fd, F_GETFD);
            if (fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
        }
        return;
    }
    int self = dirfd(d);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int fd = atoi(e->d_name);
        if (fd <= 2 || fd == self) continue;
        int fl = fcntl(fd, F_GETFD);
        if (fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
    }
    closedir(d);
}

int ui_exec_self(const char *argv0, const ui_launch_t *l)
{
    char path[512];
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) {
        fprintf(stderr, "ui: exec skipped: readlink /proc/self/exe: %s\n", strerror(errno));
        return -1;
    }
    path[n] = '\0';
    static const char deleted[] = " (deleted)";
    size_t dl = sizeof deleted - 1;
    if ((size_t)n >= dl && strcmp(path + n - dl, deleted) == 0) {
        /* The file was replaced while we ran. Whatever sits at that path now
         * has not been side-by-side tested; don't be the one to start it. */
        fprintf(stderr, "ui: exec skipped: running binary was replaced on disk (%s)\n", path);
        return -1;
    }

    char store[UI_LAUNCH_MAXARG][UI_LAUNCH_ARGLEN];
    char *av[UI_LAUNCH_MAXARG + 2];
    av[0] = (char *)(argv0 && *argv0 ? argv0 : path);
    int na = ui_launch_argv(l, store, av + 1);
    av[na + 1] = NULL;

    fprintf(stderr, "ui: exec %s", path);
    for (int i = 1; i <= na; i++) fprintf(stderr, " %s", av[i]);
    fputc('\n', stderr);
    fflush(stderr);
    fflush(stdout);

    cloexec_all();
    execv(path, av);
    fprintf(stderr, "ui: exec failed: %s\n", strerror(errno));
    return -1;
}
