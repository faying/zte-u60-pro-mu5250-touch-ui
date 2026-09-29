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

#endif
