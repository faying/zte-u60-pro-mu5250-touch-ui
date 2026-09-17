/*
 * corner_wake.c - hand the screen back to the DevUI with a corner long-press.
 *
 * The panel is a bare DRM framebuffer with no compositor: exactly one process
 * owns /dev/dri/card0 at a time, so the vendor UI cannot host a "switch back"
 * button and knows nothing about us. Touch input, however, *is* readable by
 * several processes at once (measured on this device: mtdev2tuio and
 * u60pro-devui both hold /dev/input/event3 open), so a small resident listener
 * can watch for a gesture while the vendor UI is on screen and hand the screen
 * over by running the same start.sh that rc.local uses at boot.
 *
 * Two rules this file must never break:
 *   - never EVIOCGRAB the input device: the vendor UI has to keep receiving
 *     every event, we are only an extra reader;
 *   - only listen while the DevUI is NOT running, otherwise the same gesture
 *     would fire under our own UI and the wakeups would fight the idle-power
 *     goal.
 *
 * SPDX-License-Identifier: MIT
 */
#include "touch_input.h"
#include "devui_config.h"

#include <dirent.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CORNER_BR 0
#define CORNER_BL 1
#define CORNER_TR 2
#define CORNER_TL 3

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* True while our own UI owns the screen. /proc/<pid>/comm is enough here: we
 * only need "is it up", not who holds DRM. */
static int devui_running(void)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    int found = 0;

    if (!d) return 0;
    while (!found && (e = readdir(d)) != NULL) {
        char path[288], comm[64];   /* d_name can be long; sized to silence -Wformat-truncation */
        FILE *f;

        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
        f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof comm, f)) {
            comm[strcspn(comm, "\n")] = '\0';
            /* Prefix match, not exact: /proc/<pid>/comm truncates to 15 chars,
             * and a side-by-side test build (e.g. "u60pro-devui.vswitch") shows
             * up as "u60pro-devui.v" — an exact match on "u60pro-devui" missed
             * that and kept listening while a differently-named build was
             * already on screen, firing the gesture again and racing a second
             * copy of the vendor slot's binary onto the display (found the
             * hard way on 2026-09-17: two UI processes fighting for DRM). */
            if (strncmp(comm, "u60pro-devui", 12) == 0) found = 1;
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

/* Coordinates arrive already scaled and rotated by touch_input.c, so this box
 * is in the same space the user sees on the panel. */
static int in_corner(int x, int y, int w, int h, int corner, int box)
{
    switch (corner) {
    case CORNER_BL: return x < box        && y >= h - box;
    case CORNER_TR: return x >= w - box   && y < box;
    case CORNER_TL: return x < box        && y < box;
    default:        return x >= w - box   && y >= h - box;
    }
}

static int parse_corner(const char *s)
{
    if (!s) return CORNER_BR;
    if (!strcmp(s, "bl")) return CORNER_BL;
    if (!strcmp(s, "tr")) return CORNER_TR;
    if (!strcmp(s, "tl")) return CORNER_TL;
    return CORNER_BR;
}

static const char *corner_name(int c)
{
    switch (c) {
    case CORNER_BL: return "bottom-left";
    case CORNER_TR: return "top-right";
    case CORNER_TL: return "top-left";
    default:        return "bottom-right";
    }
}

static int env_int(const char *name, int fallback)
{
    const char *v = getenv(name);
    return v && *v ? atoi(v) : fallback;
}

int main(int argc, char **argv)
{
    const int W = DEVUI_FALLBACK_WIDTH;
    const int H = DEVUI_FALLBACK_HEIGHT;
    /* --dump prints every touch sample and never switches anything. Use it to
     * confirm empirically which physical corner maps to which coordinates
     * before trusting the rotation convention. */
    const int dump = (argc > 1 && strcmp(argv[1], "--dump") == 0);
    const int corner = parse_corner(getenv("CW_CORNER"));
    const int box = env_int("CW_BOX", 64);
    const long hold_ms = env_int("CW_HOLD_MS", 3000);
    const char *script = getenv("CW_SCRIPT");

    touch_input_t t;
    int have_fd = 0, armed = 0, last_dump = -1;
    /* Opening the device can inherit a touch that is already down (observed on
     * this hardware: a fresh listener immediately reported pressed=1 with no
     * finger on the glass). Require one real release first, otherwise a stale
     * press sitting inside the corner would fire the switch instantly. */
    int fresh = 0;
    long press_start = 0;

    if (!script || !*script) script = "/data/plugins/u60pro-devui/start.sh";
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("corner-wake: corner=%s box=%d hold=%ldms script=%s%s\n",
           corner_name(corner), box, hold_ms, script, dump ? " [DUMP]" : "");

    for (;;) {
        if (!dump && devui_running()) {
            if (have_fd) {
                touch_input_close(&t);
                have_fd = 0;
                armed = 0;
                printf("devui is up: listener idle\n");
            }
            sleep(3);
            continue;
        }

        if (!have_fd) {
            if (touch_input_init(&t, W, H) != 0) {
                sleep(3);
                continue;
            }
            have_fd = 1;
            armed = 0;
            fresh = 0;
            printf("vendor mode: listening\n");
        }

        /* Poll fast only while a candidate press is being timed; otherwise wake
         * a few times a minute just to notice the DevUI coming back. A held
         * finger may emit no further events, so the timeout path must also be
         * able to complete the long press. */
        struct pollfd p = { .fd = t.fd, .events = POLLIN, .revents = 0 };
        poll(&p, 1, armed ? 250 : 3000);

        int x = 0, y = 0, pressed = 0;
        touch_input_read(&t, &x, &y, &pressed);

        if (dump) {
            int key = (pressed << 20) ^ (x << 10) ^ y;
            if (key != last_dump) {
                printf("touch x=%3d y=%3d pressed=%d  %s\n", x, y, pressed,
                       in_corner(x, y, W, H, corner, box) ? "IN-CORNER" : "");
                last_dump = key;
            }
            continue;
        }

        if (!pressed) {
            fresh = 1;              /* a real release: arming is allowed from here on */
            armed = 0;
        } else if (fresh && in_corner(x, y, W, H, corner, box)) {
            if (!armed) {
                armed = 1;
                press_start = now_ms();
            } else if (now_ms() - press_start >= hold_ms) {
                char cmd[320];
                printf("corner long-press held %ldms: handing screen to DevUI\n",
                       now_ms() - press_start);
                /* Release the input fd before the switch so the incoming DevUI
                 * starts from a clean state. */
                touch_input_close(&t);
                have_fd = 0;
                armed = 0;
                snprintf(cmd, sizeof cmd, "sh %s", script);
                if (system(cmd) < 0) printf("corner-wake: %s failed\n", script);
                sleep(5);
            }
        } else {
            armed = 0;
        }
    }
    return 0;
}
