/*
 * screen_feed.h - zwrt-datad's GET /v2/screen: the home signal card's and the
 * status bar's conclusions, computed by datad from the same snapshot the
 * screen shows (net_view.h). Read once per new snapshot.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60_SCREEN_FEED_H
#define U60_SCREEN_FEED_H

#include "net_view.h"

enum { SF_NONE = 0, SF_OK, SF_OLD_DATAD, SF_FAILING };

/* Call each refresh with data_backend_version(): fetches when the snapshot
 * changed (at most every 300 ms; after a failure every 2 s). Returns 1 when
 * the view changed. */
int screen_feed_poll(unsigned long long snapshot_version);

/* The latest view; NULL until the first good reply, while datad answers
 * 404 (too old for /v2/screen), or when the snapshot moved on and fetching
 * its view has failed for 30 s. */
const net_view_t *screen_feed_net(void);

/* SF_* — why screen_feed_net() is NULL. */
int screen_feed_status(void);

/* E4 (datad STATE_V2.md V2-40): how long datad's executor has made no
 * progress, from the last good reply; -1 when not known (older datad, no
 * reply yet). Fetched at least every 5 s while polled, since a stuck
 * executor never moves the snapshot. Stuck = SF_STUCK_MS or more: the
 * screen says 数据服务没响应 and greys its write controls. */
#define SF_STUCK_MS 20000
long screen_feed_exec_age(void);
int  screen_feed_stuck(void);

/* E4 T13 (V2-34): the reply's "op" object as JSON text — {rollback_enabled,
 * active, last} — or "" when datad sent none. */
const char *screen_feed_op(void);

#endif
