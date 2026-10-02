/*
 * diagnose.h - the 网络诊断 page's requests to zte-agent (deep_diag.rs):
 * start a run, read it, add the capped speed test, say right / wrong.
 * Logged in like esim.c (agent_client.c): the user never sees a password.
 * The run itself is parsed by diag_view.c.
 *
 * Every request is synchronous (≤ AGENT_IO_MS per step): the page paints
 * "正在检查…" first, then calls.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60_DIAGNOSE_H
#define U60_DIAGNOSE_H

#include "diag_view.h"

/* Read GET /api/diagnose when `active` (the page is up on a lit screen) and
 * it is time: every second while a run or its speed test is going, every
 * 5 s otherwise, at once after diagnose_kick(). Returns 1 when what the page
 * shows changed. */
int diagnose_poll(int active);
void diagnose_kick(void);

/* The last run read. A failed read keeps the run that was there (its rows
 * stay on screen); state DG_NONE before anything arrived. */
const diag_run_t *diagnose_run(void);

/* The agent has no run to show (none yet, or the last one finished over
 * 10 minutes ago: diagnose_run() still holds it, for the screen to keep). */
int diagnose_idle(void);

/* The agent did not answer (or the login failed) on the last request:
 * 0 = fine, 1 = no reply, 2 = login refused. */
int diagnose_agent_err(void);

/* POST /api/diagnose: start a run, or join the one going. 1 = accepted
 * (the run in the reply is taken at once). */
int diagnose_start(void);

/* POST /api/diagnose/speed for the current run. 1 = accepted; 0 = refused,
 * with the agent's words (error / error_en) in diagnose_error(). */
int diagnose_speed(void);

/* POST /api/diagnose/feedback. 1 = recorded (or recorded before). */
int diagnose_feedback(int right);

/* Why the last start / speed request was refused, "" = it was not. */
const char *diagnose_error(void);

#endif
