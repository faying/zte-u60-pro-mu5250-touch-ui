/*
 * uid_core.c - u60-uid decisions, no I/O. See include/uid_core.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "uid_core.h"

#include <stdlib.h>
#include <sys/wait.h>

uid_action_t uid_decide(const uid_state_t *s, const uid_obs_t *o)
{
    if (!s->want_devui) {
        /* The owner picked the vendor UI (the button, or SSH). */
        if (o->devui_alive) return UID_STOP_DEVUI;
        return o->vendor_alive ? UID_NOTHING : UID_START_VENDOR;
    }
    if (s->gave_up) {
        /* A devui that is up anyway was started by hand: leave it be. Only
         * the corner gesture or SSH clears gave_up and resumes launching. */
        if (o->devui_alive) return UID_NOTHING;
        return o->vendor_alive ? UID_NOTHING : UID_START_VENDOR;
    }
    if (o->devui_alive) {
        return (s->attempts > 0 && o->alive_ms >= UID_STABLE_MS) ? UID_MARK_STEADY : UID_NOTHING;
    }
    /* Not running and wanted. Launching would be attempt attempts+1. */
    if (s->attempts >= UID_MAX_ATTEMPTS) return UID_GIVE_UP;
    return UID_LAUNCH;
}

int uid_exit_is_failure(int requested, int status)
{
    if (requested) return 0;
    if (status < 0) return 0;                 /* adopted: nothing to judge by */
    if (WIFEXITED(status)) return WEXITSTATUS(status) != 0;
    return 1;                                  /* killed by a signal */
}

int uid_parse_attempts(const char *text, int file_exists)
{
    char *end;
    long v;

    if (!file_exists) return 0;
    if (!text) return UID_MAX_ATTEMPTS;
    while (*text == ' ' || *text == '\t') text++;
    v = strtol(text, &end, 10);
    if (end == text) return UID_MAX_ATTEMPTS;
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;
    if (*end || v < 0) return UID_MAX_ATTEMPTS;
    return v > 1000 ? 1000 : (int)v;
}

int uid_corner_feed(uid_corner_t *c, int in_box, int pressed, long now_ms, long hold_ms)
{
    if (!pressed) {
        c->fresh = 1;
        c->armed = 0;
        return 0;
    }
    if (!c->fresh || !in_box) {
        c->armed = 0;
        return 0;
    }
    if (!c->armed) {
        c->armed = 1;
        c->press_start = now_ms;
        return 0;
    }
    if (now_ms - c->press_start >= hold_ms) {
        /* Fire once; a new release is needed before the next one. */
        c->armed = 0;
        c->fresh = 0;
        return 1;
    }
    return 0;
}
