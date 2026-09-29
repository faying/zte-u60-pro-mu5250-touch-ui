/*
 * screen_feed.c tests against a forked fake datad: 404 (too old), a good
 * reply, one fetch per snapshot, failures, and the 30 s (here 300 ms) limit
 * on showing a view that belongs to an older snapshot.
 * Host build with ASan/UBSan: scripts/test/screen_feed/run.sh
 *
 * SPDX-License-Identifier: MIT
 */
#define SF_PORT      19461
#define SF_GAP_MS    0
#define SF_RETRY_MS  0
#define SF_STALE_MS  300
#include "../src/screen_feed.c"
#include "../src/net_view.c"
#include "../src/http.c"
#include "render/views.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/mman.h>

static int s_fail, s_pass_n;
#define CHECK(c) do { if (c) s_pass_n++; else { s_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static int *s_hits;    /* shared with the child: requests served */

static pid_t fake(int status)
{
    struct sockaddr_in sa;
    int lfd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(SF_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) || listen(lfd, 8)) { perror("bind"); exit(2); }
    pid_t pid = fork();
    if (pid == 0) {
        static char body[16384], out[20000], rq[1024];
        snprintf(body, sizeof body, "{\"v\":1,\"ts\":1,\"net\":%s}", k_views[0].json);
        for (;;) {
            int c = accept(lfd, NULL, NULL);
            if (c < 0) _exit(1);
            (void)!read(c, rq, sizeof rq);
            __atomic_add_fetch(s_hits, 1, __ATOMIC_SEQ_CST);
            if (status == 200)
                snprintf(out, sizeof out, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
            else
                snprintf(out, sizeof out, "HTTP/1.1 %d X\r\nContent-Length: 0\r\n\r\n", status);
            (void)!write(c, out, strlen(out));
            close(c);
        }
    }
    close(lfd);
    return pid;
}

static void stop(pid_t pid) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); usleep(20000); }

int main(void)
{
    pid_t pid;
    signal(SIGPIPE, SIG_IGN);
    s_hits = mmap(NULL, sizeof *s_hits, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    CHECK(screen_feed_net() == NULL && screen_feed_status() == SF_NONE);

    /* an old datad without /v2/screen */
    pid = fake(404);
    screen_feed_poll(1);
    CHECK(screen_feed_net() == NULL && screen_feed_status() == SF_OLD_DATAD);
    stop(pid);

    /* a good reply */
    pid = fake(200);
    *s_hits = 0;
    CHECK(screen_feed_poll(1) == 1);
    const net_view_t *v = screen_feed_net();
    CHECK(v && !strcmp(v->story.headline, "顺畅") && screen_feed_status() == SF_OK);
    /* same snapshot: no more requests */
    screen_feed_poll(1); screen_feed_poll(1);
    CHECK(*s_hits == 1);
    /* a new snapshot: one more */
    screen_feed_poll(2);
    CHECK(*s_hits == 2 && screen_feed_net() != NULL);
    stop(pid);

    /* datad gone, snapshot moved on: the old view stays for a while, then not */
    screen_feed_poll(3);
    CHECK(screen_feed_net() != NULL && screen_feed_status() == SF_FAILING);
    usleep(400000);
    screen_feed_poll(3);
    CHECK(screen_feed_net() == NULL);

    /* back: live again */
    pid = fake(200);
    CHECK(screen_feed_poll(3) == 1 && screen_feed_net() != NULL && screen_feed_status() == SF_OK);
    stop(pid);

    /* the snapshot did not move: an unchanged view is still current, however old */
    usleep(400000);
    screen_feed_poll(3);
    CHECK(screen_feed_net() != NULL);

    printf("passed %d, failed %d\n", s_pass_n, s_fail);
    return s_fail != 0;
}
