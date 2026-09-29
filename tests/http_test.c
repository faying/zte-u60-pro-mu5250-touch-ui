/*
 * http.c + agent_client.c tests: response parsing (Content-Length, chunked,
 * malformed input) and real round trips against forked local servers
 * (timeout, refused, truncation, unix socket, 401 → log in again).
 * Host build with ASan/UBSan: scripts/test/http/run.sh
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/http.c"
#include "../src/agent_client.c"

#include <signal.h>
#include <sys/wait.h>
#include <time.h>

static int s_fail, s_pass_n;

#define CHECK(c) do { if (c) s_pass_n++; else { s_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static int parse_str(const char *in, char *buf, size_t cap, http_resp_t *r)
{
    size_t n = strlen(in);
    if (n + 1 > cap) return -1;
    memcpy(buf, in, n + 1);
    return http_parse(buf, n, r);
}

static void test_parse(void)
{
    char b[512];
    http_resp_t r;

    CHECK(parse_str("HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\n{}", b, sizeof b, &r) == 1);
    CHECK(r.status == 200 && !strcmp(r.body, "{}") && r.body_len == 2);

    CHECK(parse_str("HTTP/1.1 401 Unauthorized\r\n\r\n", b, sizeof b, &r) == 1);
    CHECK(r.status == 401 && r.body_len == 0 && r.body[0] == 0);

    /* chunked, header name in odd case, two chunks + extension + trailer */
    CHECK(parse_str("HTTP/1.1 200 OK\r\nTransfer-ENCODING: chunked\r\n\r\n"
                    "4\r\n{\"a\"\r\n3;x=y\r\n:1}\r\n0\r\nX-T: 1\r\n\r\n", b, sizeof b, &r) == 1);
    CHECK(r.status == 200 && !strcmp(r.body, "{\"a\":1}") && r.body_len == 7);

    /* uppercase hex size */
    CHECK(parse_str("HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\nA\r\n0123456789\r\n0\r\n\r\n", b, sizeof b, &r) == 1);
    CHECK(!strcmp(r.body, "0123456789"));

    /* short last chunk: keep what arrived */
    CHECK(parse_str("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10\r\nabc", b, sizeof b, &r) == 1);
    CHECK(!strcmp(r.body, "abc"));

    /* malformed size: stop, no crash */
    CHECK(parse_str("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nabc", b, sizeof b, &r) == 1);
    CHECK(r.body_len == 0 && r.body[0] == 0);

    /* absurd size larger than the buffer */
    CHECK(parse_str("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nffffffffffffffffff\r\nabc", b, sizeof b, &r) == 1);
    CHECK(r.body_len == 0);

    /* "chunked" in another header does not trigger de-chunking */
    CHECK(parse_str("HTTP/1.1 200 OK\r\nTransfer-Encoding: identity\r\nX-Note: chunked\r\n\r\n4\r\nab", b, sizeof b, &r) == 1);
    CHECK(!strcmp(r.body, "4\r\nab"));

    /* not HTTP / no header end / garbage status */
    CHECK(parse_str("SSH-2.0-dropbear\r\n\r\n", b, sizeof b, &r) == 0 && r.status == 0);
    CHECK(parse_str("HTTP/1.0 200 OK\r\nContent-Length: 2\r\n", b, sizeof b, &r) == 0 && r.status == 0);
    CHECK(parse_str("HTTP/1.0 abc\r\n\r\n", b, sizeof b, &r) == 0);
    CHECK(parse_str("", b, sizeof b, &r) == 0);
    CHECK(http_parse(NULL, 0, &r) == 0);
}

static void test_dechunk_direct(void)
{
    char b[64];
    strcpy(b, "0\r\n\r\n");
    CHECK(http_dechunk(b, strlen(b)) == 0 && b[0] == 0);
    strcpy(b, "3\r\nabc");                /* no CRLF after data */
    CHECK(http_dechunk(b, strlen(b)) == 3 && !strcmp(b, "abc"));
    strcpy(b, "3");                       /* size only */
    CHECK(http_dechunk(b, strlen(b)) == 0);
}

static void test_build(void)
{
    char req[256];
    size_t n = http_build(req, sizeof req, "POST", "/x", "127.0.0.1:9090", "A: b\r\n", "{}");
    CHECK(n > 0 && strstr(req, "POST /x HTTP/1.0\r\nHost: 127.0.0.1:9090\r\nA: b\r\n"));
    CHECK(strstr(req, "Content-Length: 2\r\n") && strstr(req, "\r\n\r\n{}"));
    n = http_build(req, sizeof req, "GET", "/y", "h", "", NULL);
    CHECK(n > 0 && !strstr(req, "Content-Length") && strstr(req, "Connection: close\r\n\r\n"));
    CHECK(http_build(req, 20, "GET", "/a-very-long-path-that-does-not-fit", "h", "", NULL) == 0);
}

/* ---- tiny forked servers ---- */

typedef void (*handler_t)(int conn, int idx);

static int listen_tcp(int *port)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) || listen(fd, 8)) { perror("bind"); exit(2); }
    getsockname(fd, (struct sockaddr *)&sa, &sl);
    *port = ntohs(sa.sin_port);
    return fd;
}

/* read one request (headers + Content-Length body) into rq */
static void read_req(int c, char *rq, size_t cap)
{
    size_t n = 0;
    char *h;
    while (n + 1 < cap) {
        ssize_t r = read(c, rq + n, cap - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
        rq[n] = 0;
        if ((h = strstr(rq, "\r\n\r\n")) != NULL) {
            const char *cl = strstr(rq, "Content-Length: ");
            size_t want = cl ? (size_t)atoi(cl + 16) : 0;
            if (n >= (size_t)(h + 4 - rq) + want) break;
        }
    }
    rq[n] = 0;
}

static pid_t serve(int lfd, int conns, handler_t h)
{
    pid_t pid = fork();
    if (pid == 0) {
        for (int i = 0; i < conns; i++) {
            int c = accept(lfd, NULL, NULL);
            if (c < 0) _exit(1);
            h(c, i);
            close(c);
        }
        _exit(0);
    }
    close(lfd);
    return pid;
}

static void reap(pid_t pid) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); }

static void h_ok(int c, int i)
{
    char rq[2048];
    (void)i;
    read_req(c, rq, sizeof rq);
    const char *resp = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    write(c, resp, strlen(resp));
    usleep(20000);                                         /* chunk arrives in a later read */
    write(c, "5\r\nhello\r\n0\r\n\r\n", 15);
}

static void h_silent(int c, int i)
{
    char rq[2048];
    (void)i;
    read_req(c, rq, sizeof rq);
    sleep(2);                                              /* never answers within io_ms */
}

static void h_big(int c, int i)
{
    char rq[2048], blk[1024];
    (void)i;
    read_req(c, rq, sizeof rq);
    write(c, "HTTP/1.0 200 OK\r\n\r\n", 19);
    memset(blk, 'x', sizeof blk);
    for (int k = 0; k < 8; k++) if (write(c, blk, sizeof blk) < 0) break;
}


/* fake zte-agent: 0 login → token t1; 1 request → 401; 2 login → t2; 3 request → 200 */
static void h_agent(int c, int i)
{
    char rq[1024];
    const char *r;
    read_req(c, rq, sizeof rq);
    if (i == 0 || i == 2) {
        r = i == 0 ? "HTTP/1.0 200 OK\r\n\r\n{\"ok\":true,\"data\":{\"token\":\"t1\"}}"
                   : "HTTP/1.0 200 OK\r\n\r\n{\"ok\":true,\"data\":{\"token\":\"t2\"}}";
    } else if (i == 1) {
        r = strstr(rq, "Bearer t1") ? "HTTP/1.0 401 Unauthorized\r\n\r\n{}" : "HTTP/1.0 500 X\r\n\r\n";
    } else {
        r = strstr(rq, "Bearer t2") && strstr(rq, "\r\n\r\n{\"x\":1}") ? "HTTP/1.0 200 OK\r\n\r\n{\"done\":1}"
                                                                      : "HTTP/1.0 500 X\r\n\r\n";
    }
    write(c, r, strlen(r));
}

static void test_roundtrip(void)
{
    char buf[4096], req[256];
    http_resp_t r;
    int port, fd;
    pid_t pid;

    /* chunked answer split over two reads */
    pid = serve(listen_tcp(&port), 1, h_ok);
    fd = http_connect_tcp("127.0.0.1", port, 500);
    CHECK(fd >= 0);
    http_build(req, sizeof req, "GET", "/", "x", "", NULL);
    CHECK(http_exchange(fd, req, buf, sizeof buf, 500, &r) == 200 && !strcmp(r.body, "hello") && !r.truncated);
    reap(pid);

    /* peer never answers: bounded by io_ms, status 0 */
    pid = serve(listen_tcp(&port), 1, h_silent);
    fd = http_connect_tcp("127.0.0.1", port, 500);
    {
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        CHECK(http_exchange(fd, req, buf, sizeof buf, 200, &r) == 0);
        clock_gettime(CLOCK_MONOTONIC, &b);
        long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
        CHECK(ms >= 150 && ms < 1000);
    }
    reap(pid);

    /* response larger than the buffer: truncated flag, still parsed */
    pid = serve(listen_tcp(&port), 1, h_big);
    fd = http_connect_tcp("127.0.0.1", port, 500);
    CHECK(http_exchange(fd, req, buf, 2048, 500, &r) == 200 && r.truncated == 1 && strlen(buf) == 2047);
    reap(pid);

    /* refused: nothing listening on the port we just freed */
    {
        int lfd = listen_tcp(&port);
        close(lfd);
        CHECK(http_connect_tcp("127.0.0.1", port, 300) < 0);
    }
    CHECK(http_connect_tcp("not-an-ip", 80, 100) < 0);
    CHECK(http_exchange(-1, req, buf, sizeof buf, 100, &r) == 0);

    /* unix socket */
    {
        struct sockaddr_un sa;
        const char *path = "/tmp/http_test.sock";
        int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
        unlink(path);
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        strcpy(sa.sun_path, path);
        bind(lfd, (struct sockaddr *)&sa, sizeof sa);
        listen(lfd, 4);
        pid = serve(lfd, 1, h_ok);
        fd = http_connect_unix(path);
        CHECK(fd >= 0 && http_exchange(fd, req, buf, sizeof buf, 500, &r) == 200 && !strcmp(r.body, "hello"));
        reap(pid);
        unlink(path);
        CHECK(http_connect_unix(path) < 0);
        CHECK(http_connect_unix(NULL) < 0);
    }

    /* agent: expired token → one fresh login → request repeated once with body */
    pid = serve(listen_tcp(&port), 4, h_agent);
    agent_client_config(port, "p\"w");
    {
        char *b;
        CHECK(agent_api("POST", "/api/x", "{\"x\":1}", buf, sizeof buf, &b) == 200 && b && !strcmp(b, "{\"done\":1}"));
        CHECK(!strcmp(s_token, "t2"));
    }
    reap(pid);

    /* agent unreachable: 0 and NULL body */
    {
        int lfd = listen_tcp(&port);
        char *b = buf;
        close(lfd);
        agent_client_config(port, NULL);
        CHECK(agent_api("GET", "/api/y", NULL, buf, sizeof buf, &b) == 0 && b == NULL);
    }
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    test_parse();
    test_dechunk_direct();
    test_build();
    test_roundtrip();
    printf("passed %d, failed %d\n", s_pass_n, s_fail);
    return s_fail != 0;
}
