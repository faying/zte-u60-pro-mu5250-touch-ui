/*
 * http.h - the one small HTTP/1.x client every touch-UI module shares.
 *
 * Before this, each feature module (alerts, eSIM, speed test, network info,
 * scenario, Tailscale, …) carried a copy of the same connect + write +
 * read-until-close + parse code, and the copies drifted (one missed chunked
 * bodies, c3fddb1). Everything
 * here is synchronous with a per-step timeout, uses caller-owned buffers and
 * never allocates: the UI is single-threaded, so a slow peer costs at most
 * the timeout per step, exactly as before.
 *
 * data.c (datad /v2/state, /v2/events, /control) keeps its own code on purpose: its
 * SSE stream and "park /control until the answer is readable" behaviour are a
 * frozen contract (see data.c).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

typedef struct {
    int    status;      /* HTTP status code, 0 = no usable response */
    char  *body;        /* points into the caller's buffer, NUL-terminated */
    size_t body_len;    /* after de-chunking */
    int    truncated;   /* 1 = the buffer filled up before the peer closed */
} http_resp_t;

/* select() on one fd. 1 ready, 0 timeout, -1 error. */
int http_wait(int fd, int want_write, int ms);

/* Connect to an IPv4 address (dotted quad). The connect itself is non-blocking
 * with `ms` as its limit; the returned fd is back in blocking mode. -1 on error. */
int http_connect_tcp(const char *ipv4, int port, int ms);

/* Connect to a unix socket. Fails at once (no waiting) if the peer's backlog
 * is full. -1 on error. */
int http_connect_unix(const char *path);

/* Write the whole request. 0 ok, -1 error. */
int http_send(int fd, const char *req, size_t len);

/* Read until the peer closes, a read waits longer than io_ms, or buf is full
 * (then *truncated = 1). Always NUL-terminates. Returns bytes read. */
size_t http_read_all(int fd, char *buf, size_t cap, int io_ms, int *truncated);

/* Parse a complete response held in buf[0..n) (NUL-terminated). Decodes a
 * chunked body in place. Returns 1 and fills *r when the status line and
 * header end are there, else 0 with r->status = 0. */
int http_parse(char *buf, size_t n, http_resp_t *r);

/* Decode an HTTP/1.1 chunked body in place; returns the decoded length. Stops
 * at the last chunk or at the first malformed / short chunk. */
size_t http_dechunk(char *body, size_t len);

/* Send req on fd, read, parse, close fd (always). Returns r->status. */
int http_exchange(int fd, const char *req, char *buf, size_t cap, int io_ms, http_resp_t *r);

/* Build an HTTP/1.0 request to 127.0.0.1:port. json NULL = no body.
 * extra_headers is inserted verbatim ("" or lines ending in \r\n).
 * Returns the length, or 0 if it didn't fit. */
size_t http_build(char *req, size_t cap, const char *method, const char *path, const char *host,
                  const char *extra_headers, const char *json);

#endif
