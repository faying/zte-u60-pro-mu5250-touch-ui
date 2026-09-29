/*
 * screen_feed.c - see screen_feed.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "screen_feed.h"
#include "http.h"
#include "json.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef SF_PORT
#define SF_PORT      9460
#endif
#define SF_IO_MS     300        /* same budget as the /state fetch in data.c */
#ifndef SF_GAP_MS
#define SF_GAP_MS    300
#endif
#ifndef SF_RETRY_MS
#define SF_RETRY_MS  5000       /* a fetch blocks the UI for up to 2 × SF_IO_MS: not often */
#endif
#ifndef SF_STALE_MS
#define SF_STALE_MS  30000
#endif

static net_view_t s_view;
static int  s_have, s_status;
static long long s_last_try, s_behind_since;   /* behind = a newer snapshot than s_view */
static unsigned long long s_version, s_view_version;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int fetch(void)
{
    static char resp[16384], net[8192];
    char req[160];
    http_resp_t r;
    net_view_t v;
    int fd;

    if (!http_build(req, sizeof req, "GET", "/v2/screen", "127.0.0.1", "", NULL)) return 0;
    if ((fd = http_connect_tcp("127.0.0.1", SF_PORT, SF_IO_MS)) < 0) { s_status = SF_FAILING; return 0; }
    http_exchange(fd, req, resp, sizeof resp, SF_IO_MS, &r);
    if (r.status == 404) { s_status = SF_OLD_DATAD; s_have = 0; return 1; }
    if (r.status != 200 || !r.body || r.truncated ||
        !json_get(r.body, "net", net, sizeof net) || !net_view_parse(net, &v)) {
        s_status = SF_FAILING;
        return 0;
    }
    int changed = !s_have || memcmp(&v, &s_view, sizeof v) != 0;
    s_view = v;
    s_have = 1;
    s_status = SF_OK;
    return changed;
}

int screen_feed_poll(unsigned long long version)
{
    long long t = now_ms();
    int changed = 0, was = screen_feed_net() != NULL;

    s_version = version;
    if (s_view_version != s_version || !s_have) {
        long long gap = s_status == SF_OK || s_status == SF_NONE ? SF_GAP_MS : SF_RETRY_MS;
        if (!s_behind_since) s_behind_since = t;
        if (!s_last_try || t - s_last_try >= gap) {
            s_last_try = t;
            changed = fetch();
            if (s_status == SF_OK) { s_view_version = s_version; s_behind_since = 0; }
        }
    }
    return changed || was != (screen_feed_net() != NULL);
}

/* The view belongs to an older snapshot while a fetch keeps failing: keep
 * showing it for a while (a hiccup), then stop claiming it is current.
 * datad answers with its newest snapshot, which can be one sample newer than
 * the one the screen committed; the card takes all its figures from the view,
 * so it stays self-consistent, and the next commit fetches again. */
const net_view_t *screen_feed_net(void)
{
    if (!s_have) return NULL;
    if (s_view_version != s_version && s_behind_since && now_ms() - s_behind_since > SF_STALE_MS) return NULL;
    return &s_view;
}

int screen_feed_status(void) { return s_status; }
