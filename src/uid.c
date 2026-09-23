/*
 * uid.c - u60-uid, the screen-owner daemon. Runs under procd (u60-uid.init).
 *
 * The panel is bare DRM with no compositor: exactly one of our UI
 * (u60pro-devui) and the vendor UI (zte_topsw_devui) can own it. Before this,
 * three things started our UI (rc.local → start.sh, corner-wake, the vendor
 * switch button's own shell trick) and nothing restarted it when it died.
 * u60-uid is now its only owner:
 *
 *   - launches it (or adopts one already running — never starts a second),
 *   - counts launches that did not stay up and, after UID_MAX_ATTEMPTS, stops
 *     and gives the screen to the vendor UI rather than risk the firmware's
 *     crash → whole-device-reboot escalation (uid_core.h has the reasoning),
 *   - takes "vendor" / "devui" requests on a FIFO (the UI's switch button; SSH),
 *   - while the vendor UI is on screen, watches for the corner long-press that
 *     brings ours back (this replaces corner-wake), which also clears a give-up.
 *
 * Decisions are in uid_core.c (unit-tested); this file is the I/O around them.
 *
 * Reset by hand over SSH:  echo devui > /tmp/u60-uid.ctl
 * Pick the vendor UI:      echo vendor > /tmp/u60-uid.ctl
 * (only while u60-uid runs — it removes the FIFO when it stops).
 *
 * Never leave the panel without a UI: on 2026-09-23 the device rebooted itself
 * (reboot_reason_code 1185) a few minutes after our UI was stopped with
 * nothing started in its place. Every path here that stops one UI starts the
 * other in the same step.
 *
 * Two rules inherited from corner-wake: never EVIOCGRAB the touch device (the
 * vendor UI must keep getting every event), and match our UI by comm PREFIX
 * (side-by-side test builds run as u60pro-devui.<something>).
 *
 * SPDX-License-Identifier: MIT
 */
#include "uid_core.h"
#include "touch_input.h"
#include "devui_config.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEVUI_DIR    "/data/plugins/u60pro-devui"
#define DEVUI_BIN    DEVUI_DIR "/u60pro-devui"
#define DEVUI_LOG    "/tmp/u60pro-devui.log"
#define START_SH     DEVUI_DIR "/start.sh"
#define STATE_DIR    "/data/u60-uid"
#define F_ATTEMPTS   STATE_DIR "/attempts"
#define F_GAVE_UP    STATE_DIR "/gave-up"
/* On tmpfs on purpose: a reboot always comes back to our UI, as before u60-uid.
 * Only the give-up (and the attempt count) must survive a reboot. */
#define F_WANT       "/tmp/u60-uid.want"        /* "vendor" = owner picked the vendor UI */
#define CTL_FIFO     "/tmp/u60-uid.ctl"
#define UID_LOG      "/tmp/u60-uid.log"
#define ALERT_LIB    "/data/u60-guard/alert-lib.sh"
#define CRASH_DIR    "/data/crashlog/u60pro-devui"
#define DRM_DEV      "/dev/dri/card0"
#define VENDOR_INIT  "/etc/init.d/zte_topsw_devui"

#define TICK_MS            1000
#define CORNER_BOX         64
#define CORNER_HOLD_MS     3000
#define LAUNCH_GAP_MS      3000     /* never two launches closer than this */
#define VENDOR_RETRY_MS    30000    /* don't hammer the vendor init script */
#define STOP_GRACE_MS      5000
#define DRM_WAIT_MS        5000

/* ── small helpers ─────────────────────────────────────────────────────── */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void logf_(const char *fmt, ...)
{
    char msg[512], stamp[32];
    time_t t = time(NULL);
    struct tm tm;
    va_list ap;
    FILE *f;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    /* Device local time: the clock is local wall time under TZ=UTC. */
    localtime_r(&t, &tm);
    strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S", &tm);
    fprintf(stderr, "u60-uid: %s\n", msg);
    f = fopen(UID_LOG, "a");
    if (f) {
        fprintf(f, "%s %s\n", stamp, msg);
        fclose(f);
    }
}

static int file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static int read_small(const char *p, char *buf, size_t n)
{
    FILE *f = fopen(p, "r");
    size_t got;

    if (!f) return -1;
    got = fread(buf, 1, n - 1, f);
    buf[got] = 0;
    fclose(f);
    return 0;
}

/* Write whole + fsync + rename: the attempts count must survive a crash that
 * takes the device down right after we launch. */
static void write_synced(const char *p, const char *content)
{
    char tmp[128];
    int fd;

    mkdir(STATE_DIR, 0700);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        logf_("cannot write %s: %s", p, strerror(errno));
        return;
    }
    if (write(fd, content, strlen(content)) < 0) logf_("write %s: %s", p, strerror(errno));
    fsync(fd);
    close(fd);
    rename(tmp, p);
    fd = open(STATE_DIR, O_RDONLY);
    if (fd >= 0) { fsync(fd); close(fd); }
}

static void load_state(uid_state_t *s)
{
    char buf[64];
    int have = read_small(F_ATTEMPTS, buf, sizeof buf) == 0;

    s->attempts = uid_parse_attempts(have ? buf : NULL, have);
    s->gave_up = file_exists(F_GAVE_UP);
    s->want_devui = !(read_small(F_WANT, buf, sizeof buf) == 0 && strncmp(buf, "vendor", 6) == 0);
}

static void set_attempts(int n)
{
    char b[16];
    snprintf(b, sizeof b, "%d\n", n);
    write_synced(F_ATTEMPTS, b);
}

/* Run a shell snippet with arguments, without a shell parsing them. */
static void run_sh(const char *script, const char *a1, const char *a2)
{
    pid_t p = fork();
    if (p == 0) {
        execl("/bin/sh", "sh", "-c", script, "sh", a1 ? a1 : "", a2 ? a2 : "", (char *)NULL);
        _exit(127);
    }
    if (p > 0) waitpid(p, NULL, 0);
}

static void alert(const char *kind, const char *text)
{
    if (!file_exists(ALERT_LIB)) return;
    run_sh(". " ALERT_LIB " && alert_add \"$1\" \"$2\"", kind, text);
}

static void record_crash(const char *why)
{
    run_sh("d=" CRASH_DIR "; mkdir -p \"$d\" || exit 0; "
           "f=$d/$(date +%Y%m%d-%H%M%S)-up$(cut -d. -f1 /proc/uptime).log; "
           "{ echo 'program: u60pro-devui'; echo \"status:  $1\"; "
           "  echo \"time:    $(date '+%Y-%m-%d %H:%M:%S') device local\"; "
           "  echo '--- last 200 lines of " DEVUI_LOG " ---'; tail -n 200 " DEVUI_LOG "; } >\"$f\" 2>/dev/null; "
           "ls -t \"$d\"/*.log 2>/dev/null | tail -n +6 | while read -r o; do rm -f \"$o\"; done",
           why, NULL);
}

/* ── processes ─────────────────────────────────────────────────────────── */

/* Is `pid` alive and still a process whose comm starts with `prefix`? (A bare
 * kill(pid, 0) cannot tell our process from a new one that reused the pid.) */
static int pid_is(pid_t pid, const char *prefix)
{
    char path[64], comm[64];
    FILE *f;
    int ok = 0;

    snprintf(path, sizeof path, "/proc/%d/comm", (int)pid);
    f = fopen(path, "r");
    if (!f) return 0;
    if (fgets(comm, sizeof comm, f)) ok = strncmp(comm, prefix, strlen(prefix)) == 0;
    fclose(f);
    return ok;
}

/* First pid whose comm starts with `prefix`, or 0. */
static pid_t find_comm(const char *prefix)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    pid_t found = 0;
    size_t n = strlen(prefix);

    if (!d) return 0;
    while (!found && (e = readdir(d)) != NULL) {
        char path[288], comm[64];
        FILE *f;
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        if (atoi(e->d_name) == getpid()) continue;
        snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
        f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof comm, f) && strncmp(comm, prefix, n) == 0) found = (pid_t)atoi(e->d_name);
        fclose(f);
    }
    closedir(d);
    return found;
}

/* Does any process hold the DRM device open? (drm_owner.c's scan.) */
static int drm_held(void)
{
    DIR *proc = opendir("/proc");
    struct dirent *e;
    int held = 0;

    if (!proc) return 0;
    while (!held && (e = readdir(proc)) != NULL) {
        char fd_dir[64];
        DIR *fds;
        struct dirent *f;
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        snprintf(fd_dir, sizeof fd_dir, "/proc/%.32s/fd", e->d_name);
        fds = opendir(fd_dir);
        if (!fds) continue;
        while (!held && (f = readdir(fds)) != NULL) {
            char link[96], target[64];
            ssize_t n;
            if (f->d_name[0] == '.') continue;
            snprintf(link, sizeof link, "%.64s/%.16s", fd_dir, f->d_name);
            n = readlink(link, target, sizeof target - 1);
            if (n > 0) {
                target[n] = 0;
                held = strcmp(target, DRM_DEV) == 0;
            }
        }
        closedir(fds);
    }
    closedir(proc);
    return held;
}

static void wait_drm_free(void)
{
    long t0 = now_ms();
    while (drm_held() && now_ms() - t0 < DRM_WAIT_MS) usleep(200 * 1000);
    if (drm_held()) logf_("%s still held after %d ms; going ahead", DRM_DEV, DRM_WAIT_MS);
}

static void stop_vendor(void)
{
    if (!find_comm("zte_topsw_devui")) return;
    run_sh(VENDOR_INIT " stop >/dev/null 2>&1; killall -9 zte_topsw_devui 2>/dev/null", NULL, NULL);
    usleep(500 * 1000);
}

static void start_vendor(void)
{
    wait_drm_free();
    logf_("starting the vendor UI");
    run_sh(VENDOR_INIT " start >/dev/null 2>&1", NULL, NULL);
}

/* ── the daemon ────────────────────────────────────────────────────────── */

static pid_t s_child;          /* our own child (status readable) */
static pid_t s_adopted;        /* found running, not our child (status unknown) */
static long  s_since;          /* when the current devui was launched/adopted */
static int   s_requested;      /* we asked the current devui to stop */
static volatile sig_atomic_t s_term;

static void on_term(int sig) { (void)sig; s_term = 1; }

static pid_t current_devui(void) { return s_child ? s_child : s_adopted; }

static void launch(uid_state_t *s)
{
    int fd;
    pid_t p;
    struct stat st;

    /* Count first, synced: if this launch takes the device down, it still counts. */
    s->attempts++;
    set_attempts(s->attempts);
    stop_vendor();
    wait_drm_free();

    if (stat(DEVUI_LOG, &st) == 0 && st.st_size > 1024 * 1024) truncate(DEVUI_LOG, 0);
    p = fork();
    if (p == 0) {
        /* Nothing of ours leaks into the UI: the FIFO ends, the touch fd. */
        for (int i = 3; i < 256; i++) close(i);
        setsid();   /* its own group: a stop of u60-uid must not take it down */
        fd = open(DEVUI_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        fd = open("/dev/null", O_RDONLY);
        if (fd >= 0) { dup2(fd, 0); close(fd); }
        if (chdir(DEVUI_DIR) != 0) { /* not fatal */ }
        execl(DEVUI_BIN, DEVUI_BIN, (char *)NULL);
        _exit(127);
    }
    if (p < 0) {
        logf_("fork failed: %s", strerror(errno));
        return;
    }
    s_child = p;
    s_adopted = 0;
    s_since = now_ms();
    s_requested = 0;
    logf_("launched u60pro-devui pid %d (attempt %d of %d before giving up)", (int)p, s->attempts, UID_MAX_ATTEMPTS);
}

/* The single place a child's end is handled. */
static void child_ended(int status)
{
    char why[64];

    if (WIFEXITED(status)) snprintf(why, sizeof why, "exit %d", WEXITSTATUS(status));
    else snprintf(why, sizeof why, "killed by signal %d", WTERMSIG(status));
    logf_("u60pro-devui pid %d ended: %s%s", (int)s_child, why, s_requested ? " (requested)" : "");
    if (uid_exit_is_failure(s_requested, status)) {
        char text[96];
        record_crash(why);
        snprintf(text, sizeof text, "u60pro-devui %s", why);
        alert("devui-crash", text);
    }
    s_child = 0;
    s_requested = 0;
}

/* Non-blocking check of our child. Returns 1 once it is gone. */
static int poll_child(int block)
{
    int status;
    pid_t r;

    if (!s_child) return 1;
    r = waitpid(s_child, &status, block ? 0 : WNOHANG);
    if (r == s_child) {
        child_ended(status);
        return 1;
    }
    if (r < 0 && errno == ECHILD) {
        logf_("lost track of u60pro-devui pid %d (already reaped?)", (int)s_child);
        s_child = 0;
        s_requested = 0;
        return 1;
    }
    return 0;
}

static int adopted_gone(void)
{
    if (!s_adopted) return 1;
    if (pid_is(s_adopted, "u60pro-devui")) return 0;
    logf_("adopted u60pro-devui pid %d is gone (exit status unknown, not counted)", (int)s_adopted);
    s_adopted = 0;
    s_requested = 0;
    return 1;
}

static void stop_devui(void)
{
    pid_t p = current_devui();
    long t0 = now_ms();

    if (!p) return;
    s_requested = 1;
    logf_("stopping u60pro-devui pid %d (requested)", (int)p);
    kill(p, SIGTERM);
    for (;;) {
        if (s_child ? poll_child(0) : adopted_gone()) return;
        if (now_ms() - t0 >= STOP_GRACE_MS) break;
        usleep(100 * 1000);
    }
    kill(p, SIGKILL);
    if (s_child) {
        poll_child(1);
    } else {
        usleep(300 * 1000);
        adopted_gone();
    }
}

/* Notice the end of the current devui (ours or adopted). */
static void check_devui_exit(void)
{
    if (s_child) poll_child(0);
    else if (s_adopted) adopted_gone();
}

static int ctl_open(void)
{
    int fd;

    unlink(CTL_FIFO);
    if (mkfifo(CTL_FIFO, 0600) != 0) {
        logf_("mkfifo %s: %s", CTL_FIFO, strerror(errno));
        return -1;
    }
    fd = open(CTL_FIFO, O_RDONLY | O_NONBLOCK);
    /* Hold a writer ourselves so the FIFO never reports EOF between writers. */
    if (fd >= 0) open(CTL_FIFO, O_WRONLY | O_NONBLOCK);
    return fd;
}

static void ctl_handle(int fd, uid_state_t *s)
{
    char buf[128];
    ssize_t n = read(fd, buf, sizeof buf - 1);

    if (n <= 0) return;
    buf[n] = 0;
    if (strstr(buf, "vendor")) {
        logf_("request: vendor UI");
        write_synced(F_WANT, "vendor\n");
        s->want_devui = 0;
    } else if (strstr(buf, "devui")) {
        logf_("request: our UI (clears give-up and the attempt count)");
        unlink(F_WANT);
        unlink(F_GAVE_UP);
        set_attempts(0);
        s->want_devui = 1;
        s->gave_up = 0;
        s->attempts = 0;
    }
}

int main(void)
{
    uid_state_t s;
    touch_input_t t;
    uid_corner_t corner = { 0 };
    int touch_open = 0, ctl;
    long last_launch = -LAUNCH_GAP_MS, last_vendor = -VENDOR_RETRY_MS;

    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGPIPE, SIG_IGN);
    mkdir(STATE_DIR, 0700);
    logf_("starting (pid %d)", (int)getpid());

    /* corner-wake is retired: it would start our UI behind our back. */
    run_sh("killall corner-wake 2>/dev/null", NULL, NULL);
    /* One-time boot chores that used to live in start.sh's launch path. */
    if (file_exists(START_SH)) run_sh("sh " START_SH " prep >/dev/null 2>&1", NULL, NULL);

    ctl = ctl_open();
    s_adopted = find_comm("u60pro-devui");
    if (s_adopted) {
        s_since = now_ms();   /* real start time unknown: be conservative */
        logf_("adopted running u60pro-devui pid %d", (int)s_adopted);
    }

    while (!s_term) {
        uid_obs_t o;
        uid_action_t a;
        pid_t cur;
        struct pollfd pf[2];
        int npf = 0, listen;

        check_devui_exit();
        load_state(&s);
        if (!current_devui()) {
            s_adopted = find_comm("u60pro-devui");   /* started by hand? */
            if (s_adopted) {
                s_since = now_ms();
                logf_("adopted u60pro-devui pid %d", (int)s_adopted);
            }
        }
        cur = current_devui();
        o.devui_alive = cur != 0;
        o.alive_ms = cur ? now_ms() - s_since : 0;
        /* The vendor UI only matters when ours is not up or not wanted; skip
         * the /proc scan otherwise (this runs every second, forever). */
        o.vendor_alive = (!cur || !s.want_devui || s.gave_up) ? find_comm("zte_topsw_devui") != 0 : 0;

        a = uid_decide(&s, &o);
        switch (a) {
        case UID_LAUNCH:
            if (now_ms() - last_launch >= LAUNCH_GAP_MS) {
                last_launch = now_ms();
                launch(&s);
            }
            break;
        case UID_MARK_STEADY:
            logf_("u60pro-devui steady for %ld min: attempt count cleared", UID_STABLE_MS / 60000);
            set_attempts(0);
            break;
        case UID_GIVE_UP: {
            char text[96];
            snprintf(text, sizeof text, "%d launches did not stay up; vendor UI on screen", s.attempts);
            logf_("giving up: %s. Corner long-press or 'echo devui > %s' to retry", text, CTL_FIFO);
            write_synced(F_GAVE_UP, "1\n");
            alert("devui-gave-up", text);
            last_vendor = now_ms();
            start_vendor();
            break;
        }
        case UID_START_VENDOR:
            if (now_ms() - last_vendor >= VENDOR_RETRY_MS) {
                last_vendor = now_ms();
                start_vendor();
            }
            break;
        case UID_STOP_DEVUI:
            stop_devui();
            last_vendor = now_ms();
            start_vendor();
            break;
        case UID_NOTHING:
            break;
        }

        /* Listen for the corner only while our UI is not on screen. */
        listen = !current_devui() && (!s.want_devui || s.gave_up);
        if (listen && !touch_open) {
            if (touch_input_init(&t, DEVUI_FALLBACK_WIDTH, DEVUI_FALLBACK_HEIGHT) == 0) {
                touch_open = 1;
                memset(&corner, 0, sizeof corner);
                logf_("vendor UI on screen: corner long-press listener on");
            }
        } else if (!listen && touch_open) {
            touch_input_close(&t);
            touch_open = 0;
        }

        if (ctl >= 0) { pf[npf].fd = ctl; pf[npf].events = POLLIN; pf[npf].revents = 0; npf++; }
        if (touch_open) { pf[npf].fd = t.fd; pf[npf].events = POLLIN; pf[npf].revents = 0; npf++; }
        /* A held finger emits nothing, so time out quickly while one is down. */
        poll(pf, npf, corner.armed ? 250 : TICK_MS);

        if (ctl >= 0 && (pf[0].revents & POLLIN)) ctl_handle(ctl, &s);
        if (touch_open) {
            int x = 0, y = 0, pressed = 0;
            touch_input_read(&t, &x, &y, &pressed);
            int in_box = x >= DEVUI_FALLBACK_WIDTH - CORNER_BOX && y >= DEVUI_FALLBACK_HEIGHT - CORNER_BOX;
            if (uid_corner_feed(&corner, in_box, pressed, now_ms(), CORNER_HOLD_MS)) {
                logf_("corner long-press: back to our UI (give-up and attempts cleared)");
                touch_input_close(&t);
                touch_open = 0;
                unlink(F_WANT);
                unlink(F_GAVE_UP);
                set_attempts(0);
            }
        }
    }

    /* Stopping the daemon leaves the UI on screen; the next u60-uid adopts it.
     * Remove the FIFO: with no reader left, a shell `echo … > ctl` would block
     * forever (it did, 2026-09-23 — and the deploy script stuck behind it left
     * the panel with no UI until the firmware rebooted the device). */
    unlink(CTL_FIFO);
    logf_("stopping (u60pro-devui left running)");
    return 0;
}
