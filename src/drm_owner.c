/*
 * drm_owner.c - report which process currently holds the DRM device.
 *
 * The panel is a bare DRM framebuffer with no compositor: exactly one
 * process can hold /dev/dri/card0 open at a time (see corner_wake.c for the
 * same fact applied to touch input, which *is* shareable). Dual-mode
 * switching needs to know, at any moment, which process that is before
 * deciding whether it is safe to hand the screen to the other UI.
 *
 * This is a one-shot query tool, not a resident daemon: scans /proc/<pid>/fd
 * for every process, resolves each fd symlink, and reports the pid + comm of
 * whoever holds a link to the target device (default /dev/dri/card0).
 *
 * Deliberately does NOT hardcode an expected process name (e.g.
 * "u60pro-devui") and does not judge whether the result is "correct" -
 * T20 was caused by a different tool (corner-wake) making that judgment
 * with an exact-match on a process name, which broke the moment a
 * side-by-side test binary used a different name. This tool only reports
 * facts; the caller (dual-mode logic, still to be written) decides what to
 * do with them.
 *
 * Exit codes: 0 = holder found (printed as "<pid> <comm>" to stdout),
 * 1 = no holder found, 2 = usage/scan error.
 *
 * SPDX-License-Identifier: MIT
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_DEVICE "/dev/dri/card0"
#define MAX_HITS       8

typedef struct {
    char pid[16];
    char comm[64];
} hit_t;

static int read_comm(const char *pid, char *out, size_t out_sz)
{
    char path[64];
    FILE *f;

    snprintf(path, sizeof path, "/proc/%.32s/comm", pid);
    f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(out, (int)out_sz, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    out[strcspn(out, "\n")] = '\0';
    return 0;
}

/* Scans one process's fd table for a link to `device`. Returns 1 if found. */
static int pid_holds_device(const char *pid, const char *device)
{
    char fd_dir[64];
    DIR *d;
    struct dirent *e;
    int found = 0;

    snprintf(fd_dir, sizeof fd_dir, "/proc/%.32s/fd", pid);
    d = opendir(fd_dir);
    if (!d) return 0; /* permission denied or process gone: not a match */

    while (!found && (e = readdir(d)) != NULL) {
        char link_path[96], target[256];
        ssize_t n;

        if (e->d_name[0] == '.') continue;
        snprintf(link_path, sizeof link_path, "%.64s/%.16s", fd_dir, e->d_name);
        n = readlink(link_path, target, sizeof target - 1);
        if (n < 0) continue;
        target[n] = '\0';
        if (strcmp(target, device) == 0) found = 1;
    }
    closedir(d);
    return found;
}

int main(int argc, char **argv)
{
    const char *device = argc > 1 ? argv[1] : DEFAULT_DEVICE;
    const char *self_pid_str;
    char self_pid[16];
    DIR *proc;
    struct dirent *e;
    hit_t hits[MAX_HITS];
    int nhits = 0;

    snprintf(self_pid, sizeof self_pid, "%d", getpid());
    self_pid_str = self_pid;

    proc = opendir("/proc");
    if (!proc) {
        fprintf(stderr, "drm_owner: cannot open /proc: %s\n", strerror(errno));
        return 2;
    }

    while (nhits < MAX_HITS && (e = readdir(proc)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        if (strcmp(e->d_name, self_pid_str) == 0) continue; /* we hold no fd to it ourselves, but skip anyway for clarity */

        if (pid_holds_device(e->d_name, device)) {
            hit_t *h = &hits[nhits];
            snprintf(h->pid, sizeof h->pid, "%.*s", (int)sizeof h->pid - 1, e->d_name);
            if (read_comm(e->d_name, h->comm, sizeof h->comm) != 0) {
                snprintf(h->comm, sizeof h->comm, "?");
            }
            nhits++;
        }
    }
    closedir(proc);

    if (nhits == 0) {
        fprintf(stderr, "drm_owner: no process holds %s\n", device);
        return 1;
    }

    for (int i = 0; i < nhits; i++) {
        printf("%s %s\n", hits[i].pid, hits[i].comm);
    }
    fflush(stdout);
    /* More than one holder is itself a fact worth surfacing: on a
     * single-owner DRM device this should never happen, and if it does
     * (e.g. mid-handover race) the caller needs to see all of them, not
     * just the first, to decide whether a conflict is in progress. */
    return 0;
}
