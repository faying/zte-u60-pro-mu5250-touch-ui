/*
 * http.c - shared HTTP/1.x client. See http.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int http_wait(int fd, int want_write, int ms)
{
    fd_set s;
    struct timeval tv;

    if (fd < 0 || fd >= FD_SETSIZE) return -1;
    FD_ZERO(&s);
    FD_SET(fd, &s);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return select(fd + 1, want_write ? NULL : &s, want_write ? &s : NULL, NULL, &tv);
}

int http_connect_tcp(const char *ipv4, int port, int ms)
{
    struct sockaddr_in sa;
    int fd, f, rc;

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ipv4, &sa.sin_addr) != 1) return -1;
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) return -1;
    f = fcntl(fd, F_GETFL, 0);
    if (f < 0 || fcntl(fd, F_SETFL, f | O_NONBLOCK) < 0) { close(fd); return -1; }
    rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    if (rc < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    if (rc < 0) {
        int err = 0;
        socklen_t el = sizeof err;
        if (http_wait(fd, 1, ms) <= 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) {
            close(fd);
            return -1;
        }
    }
    if (fcntl(fd, F_SETFL, f) < 0) { close(fd); return -1; }
    return fd;
}

int http_connect_unix(const char *path)
{
    struct sockaddr_un sa;
    int fd, f;

    if (!path || strlen(path) >= sizeof sa.sun_path) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, path, strlen(path) + 1);
    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) return -1;
    /* non-blocking connect: a full backlog fails at once instead of hanging */
    f = fcntl(fd, F_GETFL, 0);
    if (f < 0 || fcntl(fd, F_SETFL, f | O_NONBLOCK) < 0 ||
        connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0 ||
        fcntl(fd, F_SETFL, f) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int http_send(int fd, const char *req, size_t len)
{
    size_t off = 0;

    /* send(MSG_NOSIGNAL), not write(): the process doesn't ignore SIGPIPE, and a
     * peer that hangs up first (tailscaled, a restarting agent) would otherwise
     * kill the screen */
    while (off < len) {
        ssize_t w = send(fd, req + off, len - off, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

size_t http_read_all(int fd, char *buf, size_t cap, int io_ms, int *truncated)
{
    size_t n = 0;

    if (truncated) *truncated = 0;
    if (!buf || cap == 0) return 0;
    for (;;) {
        ssize_t rd;
        if (n + 1 >= cap) { if (truncated) *truncated = 1; break; }
        if (http_wait(fd, 0, io_ms) <= 0) break;
        rd = read(fd, buf + n, cap - 1 - n);
        if (rd < 0 && errno == EINTR) continue;
        if (rd <= 0) break;
        n += (size_t)rd;
    }
    buf[n] = 0;
    return n;
}

/* Case-insensitive search inside [hay, end): header names aren't case-normalized
 * by every server (Go's net/http isn't). */
static const char *ci_find(const char *hay, const char *end, const char *needle)
{
    size_t nl = strlen(needle);

    for (const char *h = hay; h + nl <= end; h++) {
        size_t i = 0;
        for (; i < nl; i++) {
            char a = h[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
            if (a != b) break;
        }
        if (i == nl) return h;
    }
    return NULL;
}

/*
 * Write position never runs ahead of read position, so decoding in place is
 * safe. Malformed or short input stops decoding where it is: a truncated list
 * beats a crash or an endless loop.
 */
size_t http_dechunk(char *body, size_t len)
{
    size_t r = 0, w = 0;

    for (;;) {
        size_t sz = 0;
        int digits = 0;

        while (r < len) {
            char c = body[r];
            int d = (c >= '0' && c <= '9') ? c - '0' :
                    (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                    (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0) break;
            if (sz > (SIZE_MAX >> 4)) { digits = 0; break; }  /* would overflow: malformed */
            sz = sz * 16 + (size_t)d;
            digits++;
            r++;
        }
        if (!digits) break;
        while (r < len && body[r] != '\r') r++;              /* skip ;chunk-ext */
        if (r + 1 >= len || body[r + 1] != '\n') break;
        r += 2;
        if (sz == 0) break;                                  /* last chunk */
        if (sz > len - r) sz = len - r;                      /* short read: keep what arrived */
        memmove(body + w, body + r, sz);
        w += sz;
        r += sz;
        if (r + 1 < len && body[r] == '\r' && body[r + 1] == '\n') r += 2;
    }
    body[w] = 0;
    return w;
}

int http_parse(char *buf, size_t n, http_resp_t *r)
{
    char *hend;

    memset(r, 0, sizeof *r);
    if (!buf || n < 12 || strncmp(buf, "HTTP/1.", 7) || buf[8] != ' ') return 0;
    if (!(hend = strstr(buf, "\r\n\r\n"))) return 0;
    r->status = atoi(buf + 9);
    if (r->status < 100 || r->status > 999) { r->status = 0; return 0; }
    r->body = hend + 4;
    r->body_len = n - (size_t)(r->body - buf);
    {
        const char *te = ci_find(buf, hend, "\r\ntransfer-encoding:");
        const char *eol = te ? strstr(te + 2, "\r\n") : NULL;
        if (te && ci_find(te, eol ? eol : hend, "chunked"))
            r->body_len = http_dechunk(r->body, r->body_len);
    }
    return 1;
}

int http_exchange(int fd, const char *req, char *buf, size_t cap, int io_ms, http_resp_t *r)
{
    size_t n;
    int trunc = 0;

    memset(r, 0, sizeof *r);
    if (fd < 0) return 0;
    if (http_send(fd, req, strlen(req)) < 0) { close(fd); return 0; }
    n = http_read_all(fd, buf, cap, io_ms, &trunc);
    close(fd);
    http_parse(buf, n, r);
    r->truncated = trunc;
    return r->status;
}

size_t http_build(char *req, size_t cap, const char *method, const char *path, const char *host,
                  const char *extra_headers, const char *json)
{
    int n;

    if (json)
        n = snprintf(req, cap,
                     "%s %s HTTP/1.0\r\nHost: %s\r\n%s"
                     "Content-Type: application/json\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n\r\n%s",
                     method, path, host, extra_headers ? extra_headers : "", strlen(json), json);
    else
        n = snprintf(req, cap,
                     "%s %s HTTP/1.0\r\nHost: %s\r\n%s"
                     "Connection: close\r\n\r\n",
                     method, path, host, extra_headers ? extra_headers : "");
    return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}
