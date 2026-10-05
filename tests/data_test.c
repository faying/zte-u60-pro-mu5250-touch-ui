/*
 * data.c tests: noticing that datad went away after having answered.
 * A forked fake datad answers /state and opens /events, then goes silent while
 * keeping the stream open (half-dead TCP), then a fresh one comes back.
 * Host build with ASan/UBSan: scripts/test/data/run.sh
 *
 * SPDX-License-Identifier: MIT
 */
#define DEVUI_BACKEND_PORT      19460
#define DEVUI_BACKEND_SILENT_MS 400
#define DEVUI_BACKEND_RETRY_MS  100
#define DEVUI_BACKEND_SILENT_RETRY_MS 150
#include "../src/data.c"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>

static int s_fail, s_pass_n;
#define CHECK(c) do { if (c) s_pass_n++; else { s_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static const char *SNAP = "{\"net\":{\"type\":\"NR5G_SA\",\"operator\":\"X\"},\"ts\":1}";

static int listen_port(void)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(DEVUI_BACKEND_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) || listen(fd, 16)) { perror("bind"); exit(2); }
    return fd;
}

static void answer(int c, int keep_open_silent)
{
    char rq[1024], out[512];
    ssize_t n = read(c, rq, sizeof rq - 1);
    if (n <= 0) return;
    rq[n] = 0;
    if (!strncmp(rq, "GET /state", 10)) {
        snprintf(out, sizeof out, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s", strlen(SNAP), SNAP);
        (void)!write(c, out, strlen(out));
    } else if (!strncmp(rq, "GET /events", 11)) {
        snprintf(out, sizeof out, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\nevent: state\ndata: %s\n\n", SNAP);
        (void)!write(c, out, strlen(out));
        if (keep_open_silent) sleep(30);      /* stream stays open, says nothing */
    }
}

/* mode 0: answer everything normally (stream closes after the first event is
 * read by the client and the handler returns — reconnects keep succeeding).
 * mode 1: answer the first /state and /events, then hold the stream open and
 * never accept again. */
static pid_t fake_datad(int mode)
{
    int lfd = listen_port();
    pid_t pid = fork();
    if (pid == 0) {
        for (int i = 0;; i++) {
            int c = accept(lfd, NULL, NULL);
            if (c < 0) _exit(1);
            answer(c, mode == 1 && i == 1);
            if (mode == 0) { usleep(20000); close(c); }
        }
    }
    close(lfd);
    return pid;
}

/* Accept and count without a word (datad up but not answering yet): every
 * accept writes one byte to `tally` for the parent to count. hold=0 hangs up
 * at once; hold=1 keeps the connection open and silent (each try then costs
 * the client its I/O timeouts). */
static pid_t closing_datad(int tally, int hold)
{
    int lfd = listen_port();
    pid_t pid = fork();
    if (pid == 0) {
        for (;;) {
            int c = accept(lfd, NULL, NULL);
            if (c < 0) _exit(1);
            (void)!write(tally, "x", 1);
            if (!hold) close(c);
        }
    }
    close(lfd);
    return pid;
}

static void spin(int ms)
{
    uint32_t end = mono_ms() + (uint32_t)ms;
    while ((int32_t)(end - mono_ms()) > 0) {
        if (data_backend_poll(mono_ms())) (void)data_backend_commit_latest();
        usleep(20000);
    }
}

int main(void)
{
    {   /* 移动数据：enable 0 但数据连着（开机后默认）算开着；0 且断着才是关 */
        struct { const char *cell; int want; } c[] = {
            { "{\"enable\":0,\"connect_status\":\"ipv4_ipv6_connected\"}", 1 },
            { "{\"enable\":0,\"connect_status\":\"connecting\"}", 1 },
            { "{\"enable\":0,\"connect_status\":\"disconnected\"}", 0 },
            { "{\"enable\":0,\"connect_status\":\"\"}", -1 },
            { "{\"enable\":0}", -1 },
            { "{\"enable\":1,\"connect_status\":\"disconnected\"}", 1 },
        };
        for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
            char buf[256];
            devui_data_t dd;
            memset(&dd, 0, sizeof dd);
            snprintf(buf, sizeof buf, "{\"interfaces\":{\"cellular\":%s},\"ts\":1}", c[i].cell);
            parse_snapshot(&dd, buf);
            CHECK(dd.cell_data == c[i].want);
        }
    }

    devui_data_t d;
    pid_t pid;

    signal(SIGPIPE, SIG_IGN);

    /* never answered: not valid, no alive time */
    CHECK(data_refresh(&d) == 0);
    CHECK(data_backend_alive_wall() == 0);
    CHECK(!data_backend_silent());           /* never answered is not "silent" */

    /* never answered, datad hangs up or holds silent: retries follow
     * DEVUI_BACKEND_RETRY_MS (100 ms here), not every pass of a 1 ms loop.
     * 500 ms ≈ 6 tries × 2 connects (/state, /events); per pass would be ~1000. */
    for (int hold = 0; hold <= 1; hold++) {
        int tally[2];
        char buf[4096];
        ssize_t n, got = 0;
        uint32_t end;
        CHECK(pipe(tally) == 0);
        pid = closing_datad(tally[1], hold);
        usleep(50000);
        end = mono_ms() + 500;
        while ((int32_t)(end - mono_ms()) > 0) {
            (void)data_backend_poll(mono_ms());
            usleep(1000);
        }
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(tally[1]);
        while ((n = read(tally[0], buf, sizeof buf)) > 0) got += n;
        close(tally[0]);
        printf("  connects in 500 ms with datad %s: %zd\n", hold ? "holding silent" : "hanging up", got);
        CHECK(got >= 1 && got <= 20);
        CHECK(data_refresh(&d) == 0);
    }

    pid = fake_datad(1);
    usleep(50000);
    spin(150);
    CHECK(data_refresh(&d) == 1 && !strcmp(d.net_type, "NR5G_SA"));
    long alive = data_backend_alive_wall();
    CHECK(alive > 0);

    CHECK(!data_backend_silent());

    /* silent past DEVUI_BACKEND_SILENT_MS with the stream still open: reported
     * silent; the last snapshot is still handed out (the screen dims it) */
    spin(700);
    CHECK(data_backend_silent());
    CHECK(data_refresh(&d) == 1 && !strcmp(d.net_type, "NR5G_SA"));
    CHECK(data_backend_alive_wall() == alive);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);

    /* a healthy datad again: back to live */
    pid = fake_datad(0);
    usleep(50000);
    spin(400);
    CHECK(data_refresh(&d) == 1);
    CHECK(!data_backend_silent());
    CHECK(data_backend_alive_wall() >= alive);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);

    /* reconnect is due across the 32-bit ms wrap (49.7 days of uptime): a
     * deadline set just before the wrap, polled just after it, must retry;
     * so must 0 ("due") once now_ms is past 2^31. One poll each, counted. */
    {
        struct { uint32_t due, now; int want; } w[] = {
            { 0xFFFFFF00u, 0x00000010u, 1 },   /* deadline before the wrap, now after */
            { 0u,          0x80000010u, 1 },   /* 0 = due, now in the upper half */
            { 0x00000100u, 0x00000010u, 0 },   /* not yet due */
            { 0x00000100u, 0xFFFFFFF0u, 0 },   /* not yet due, deadline after the wrap */
        };
        for (size_t i = 0; i < sizeof w / sizeof w[0]; i++) {
            int tally[2];
            char buf[64];
            ssize_t n, got = 0;
            CHECK(pipe(tally) == 0);
            pid = closing_datad(tally[1], 0);
            usleep(50000);
            close_sse_stream();
            g_backend.next_retry_ms = w[i].due;
            (void)data_backend_poll(w[i].now);
            usleep(50000);
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            close(tally[1]);
            while ((n = read(tally[0], buf, sizeof buf)) > 0) got += n;
            close(tally[0]);
            printf("  wrap case %zu: due %08x now %08x -> %zd connects\n", i, w[i].due, w[i].now, got);
            CHECK(w[i].want ? got >= 1 : got == 0);
        }
    }

    printf("passed %d, failed %d\n", s_pass_n, s_fail);
    return s_fail != 0;
}
