/*
 * Unit tests for uid_core.c (u60-uid's decisions). No device needed:
 *   scripts/test/uid/run.sh   (cross-builds, runs in an arm64 busybox container)
 *
 * SPDX-License-Identifier: MIT
 */
#include "uid_core.h"

#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>

static int pass, fail;

#define CHECK(desc, cond)                                              \
    do {                                                               \
        if (cond) { pass++; printf("  ok   %s\n", desc); }             \
        else      { fail++; printf("  FAIL %s  [%s]\n", desc, #cond); } \
    } while (0)

static uid_action_t decide(int want, int gave_up, int attempts, int alive, long alive_ms, int vendor)
{
    uid_state_t s = { want, gave_up, attempts };
    uid_obs_t o = { alive, alive_ms, vendor };
    return uid_decide(&s, &o);
}

static int w_exit(int code) { return (code & 0xff) << 8; }  /* like waitpid for exit(code) */
static int w_sig(int sig) { return sig & 0x7f; }            /* like waitpid for a signal death */

int main(void)
{
    puts("launching and giving up");
    CHECK("wanted, not running, no attempts yet: launch", decide(1, 0, 0, 0, 0, 1) == UID_LAUNCH);
    CHECK("one unsteady attempt so far: launch again", decide(1, 0, 1, 0, 0, 1) == UID_LAUNCH);
    CHECK("two unsteady attempts: give up (never a third)", decide(1, 0, 2, 0, 0, 1) == UID_GIVE_UP);
    CHECK("more than the limit (e.g. corrupt file → max): give up", decide(1, 0, 7, 0, 0, 0) == UID_GIVE_UP);

    puts("steady running");
    CHECK("running under 10 min: nothing yet", decide(1, 0, 1, 1, UID_STABLE_MS - 1, 0) == UID_NOTHING);
    CHECK("running 10 min with attempts pending: mark steady", decide(1, 0, 1, 1, UID_STABLE_MS, 0) == UID_MARK_STEADY);
    CHECK("already steady (attempts 0): nothing to write", decide(1, 0, 0, 1, UID_STABLE_MS * 3, 0) == UID_NOTHING);

    puts("after giving up");
    CHECK("gave up, vendor down: start vendor", decide(1, 1, 2, 0, 0, 0) == UID_START_VENDOR);
    CHECK("gave up, vendor up: leave it", decide(1, 1, 2, 0, 0, 1) == UID_NOTHING);
    CHECK("gave up but someone started devui by hand: leave it", decide(1, 1, 2, 1, 5000, 0) == UID_NOTHING);

    puts("owner chose the vendor UI");
    CHECK("devui still up: stop it", decide(0, 0, 0, 1, 99999, 0) == UID_STOP_DEVUI);
    CHECK("devui gone, vendor down: start vendor", decide(0, 0, 0, 0, 0, 0) == UID_START_VENDOR);
    CHECK("devui gone, vendor up: nothing", decide(0, 0, 0, 0, 0, 1) == UID_NOTHING);
    CHECK("vendor chosen never relaunches devui, whatever the count", decide(0, 0, 0, 0, 0, 1) != UID_LAUNCH);

    puts("how exits count");
    CHECK("requested stop is not a failure even if killed", !uid_exit_is_failure(1, w_sig(SIGKILL)));
    CHECK("adopted process (unknown status) is not a failure", !uid_exit_is_failure(0, -1));
    CHECK("clean exit 0 is not a failure", !uid_exit_is_failure(0, w_exit(0)));
    CHECK("exit 1 is a failure", uid_exit_is_failure(0, w_exit(1)));
    CHECK("SIGSEGV is a failure", uid_exit_is_failure(0, w_sig(SIGSEGV)));
    CHECK("unrequested SIGTERM is a failure", uid_exit_is_failure(0, w_sig(SIGTERM)));

    puts("attempts file");
    CHECK("missing: 0", uid_parse_attempts(NULL, 0) == 0);
    CHECK("\"1\\n\": 1", uid_parse_attempts("1\n", 1) == 1);
    CHECK("empty file: treat as max (stop)", uid_parse_attempts("", 1) == UID_MAX_ATTEMPTS);
    CHECK("garbage: max", uid_parse_attempts("x1", 1) == UID_MAX_ATTEMPTS);
    CHECK("trailing junk: max", uid_parse_attempts("1x", 1) == UID_MAX_ATTEMPTS);
    CHECK("negative: max", uid_parse_attempts("-3", 1) == UID_MAX_ATTEMPTS);

    puts("corner long-press");
    {
        uid_corner_t c = { 0 };
        CHECK("press already down when listening starts: ignored",
              !uid_corner_feed(&c, 1, 1, 0, 3000) && !uid_corner_feed(&c, 1, 1, 5000, 3000));
        CHECK("release arms it", !uid_corner_feed(&c, 1, 0, 5100, 3000));
        CHECK("press in the box starts timing", !uid_corner_feed(&c, 1, 1, 6000, 3000));
        CHECK("not yet at 2.9 s", !uid_corner_feed(&c, 1, 1, 8900, 3000));
        CHECK("fires at 3 s", uid_corner_feed(&c, 1, 1, 9000, 3000));
        CHECK("does not fire again while still held", !uid_corner_feed(&c, 1, 1, 15000, 3000));
    }
    {
        uid_corner_t c = { 1, 0, 0 };
        uid_corner_feed(&c, 1, 1, 0, 3000);
        uid_corner_feed(&c, 0, 1, 1500, 3000);   /* slid out of the box */
        CHECK("sliding out of the box resets the timer", !uid_corner_feed(&c, 1, 1, 3200, 3000));
        CHECK("…and times from re-entry", uid_corner_feed(&c, 1, 1, 6200, 3000));
    }

    printf("\npassed %d, failed %d\n", pass, fail);
    return fail ? 1 : 0;
}
