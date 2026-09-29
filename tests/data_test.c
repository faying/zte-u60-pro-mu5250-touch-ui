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
    devui_data_t d;
    pid_t pid;

    signal(SIGPIPE, SIG_IGN);

    /* never answered: not valid, no alive time */
    CHECK(data_refresh(&d) == 0);
    CHECK(data_backend_alive_wall() == 0);
    CHECK(!data_backend_silent());           /* never answered is not "silent" */

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

    printf("passed %d, failed %d\n", s_pass_n, s_fail);
    return s_fail != 0;
}
