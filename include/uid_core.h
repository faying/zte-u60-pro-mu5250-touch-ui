/*
 * uid_core.h - the decisions of u60-uid, the screen-owner daemon, with no I/O.
 *
 * u60-uid is the only thing that starts or stops u60pro-devui (our UI) and the
 * only thing that hands the panel to the vendor UI. Everything here is a pure
 * function of state the caller gathered, so it can be unit-tested off-device
 * (tests/uid_core_test.c). src/uid.c does the I/O.
 *
 * Why the counting is shaped the way it is: on this firmware a UI that dies
 * again and again gets escalated into a whole-device reboot (2026-09-16), and
 * that reboot can come before we get to record "it crashed". So the count is
 * of *launch attempts*, written to /data and synced before each launch, and
 * cleared only after the UI has run steadily for UID_STABLE_MS. A crash that
 * takes the device down with it therefore still counts, across boots. When a
 * launch would be attempt number UID_MAX_ATTEMPTS + 1, we stop and give the
 * screen to the vendor UI instead; only a corner long-press or SSH resets it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60_UID_CORE_H
#define U60_UID_CORE_H

#define UID_MAX_ATTEMPTS 2           /* unsteady launches before giving up */
#define UID_STABLE_MS    (10L * 60 * 1000)

/* Persisted in /data/u60-uid/ (one small file each). */
typedef struct {
    int want_devui;   /* 1 = our UI should own the screen; 0 = the owner chose the vendor UI */
    int gave_up;      /* stopped relaunching after repeated failures */
    int attempts;     /* launches not yet confirmed steady */
} uid_state_t;

/* What the caller observed this round. */
typedef struct {
    int  devui_alive;   /* a u60pro-devui* process exists (ours or adopted) */
    long alive_ms;      /* how long it has been up, as far as we know */
    int  vendor_alive;  /* zte_topsw_devui is running */
} uid_obs_t;

typedef enum {
    UID_NOTHING = 0,
    UID_LAUNCH,          /* bump attempts (and sync), stop vendor, start devui */
    UID_MARK_STEADY,     /* devui has been up UID_STABLE_MS: attempts = 0 */
    UID_GIVE_UP,         /* set gave_up, alert, then hand the screen to the vendor UI */
    UID_START_VENDOR,    /* devui is not wanted/allowed and the vendor UI is not up */
    UID_STOP_DEVUI,      /* the owner chose the vendor UI but devui is still up */
} uid_action_t;

/* The one decision per round. */
uid_action_t uid_decide(const uid_state_t *s, const uid_obs_t *o);

/* How a devui exit counts.
 *   requested: we asked it to stop (switch to vendor, service stop)
 *   status:    waitpid status, or -1 if unknown (an adopted process: not our
 *              child, so there is no status to read)
 * Returns 1 if it is a failure worth an alert. Attempts are NOT changed here:
 * the launch already counted, and an unknown exit must not count twice. */
int uid_exit_is_failure(int requested, int status);

/* Parse a persisted attempts file. Missing file → 0. Anything unreadable or
 * non-numeric → UID_MAX_ATTEMPTS: when in doubt, do not relaunch. */
int uid_parse_attempts(const char *text, int file_exists);

/* Corner long-press recogniser, fed one touch sample at a time. */
typedef struct {
    int  fresh;        /* a real release has been seen since listening began */
    int  armed;
    long press_start;
} uid_corner_t;

/* Returns 1 exactly once when the press inside the box has been held for
 * hold_ms. A press already down when listening starts never counts (the
 * device reports one on open with no finger on the glass). */
int uid_corner_feed(uid_corner_t *c, int in_box, int pressed, long now_ms, long hold_ms);

#endif /* U60_UID_CORE_H */
