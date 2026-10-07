/*
 * data.c tests: noticing that datad went away after having answered.
 * A forked fake datad answers /v2/state and opens /v2/events, then goes silent
 * while keeping the stream open (half-dead TCP), then a fresh one comes back.
 * Also: the /v2 blocks put back together read exactly like the old /state.
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

static const char *SNAP = "{\"epoch\":\"e1\",\"seq\":0,\"blocks\":{\"signal\":{\"revision\":1,\"observed_at\":1,"
                          "\"stale\":false,\"data\":{\"type\":\"NR5G_SA\",\"operator\":\"X\"}}}}";

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
    if (!strncmp(rq, "GET /v2/state", 13)) {
        snprintf(out, sizeof out, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s", strlen(SNAP), SNAP);
        (void)!write(c, out, strlen(out));
    } else if (!strncmp(rq, "GET /v2/events", 14)) {
        snprintf(out, sizeof out, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\nevent: snapshot\ndata: %s\n\n", SNAP);
        (void)!write(c, out, strlen(out));
        if (keep_open_silent) sleep(30);      /* stream stays open, says nothing */
    }
}

/* mode 0: answer everything normally (stream closes after the first event is
 * read by the client and the handler returns — reconnects keep succeeding).
 * mode 1: answer the first /v2/state and /v2/events, then hold the stream open and
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

/* One SSE event through the stream parser, as datad frames it. */
static int feed(const char *name, const char *json)
{
    static char ev[65536];
    int n = snprintf(ev, sizeof ev, "event: %s\ndata: %s\n\n", name, json);
    return process_sse_event(ev, (size_t)n);
}

/* What the old /state said, block by block (the /v2 block data is the same
 * object, STATE_V2.md §4; live = {system, runtime, traffic}). */
#define L_NET  "{\"type\":\"NR5G_SA\",\"operator\":\"CMCC\",\"band\":\"3\",\"nr_band\":\"78\",\"bars\":4," \
               "\"nr_rsrp\":-91,\"lte_rsrp\":-99,\"mcc\":460,\"mnc\":0,\"nrca\":\"78,1,627264,100\",\"HSR\":false}"
#define L_BAT  "{\"percent\":77,\"temp\":31,\"charging\":1,\"charger_connect\":1,\"bat_uv\":4012000}"
#define L_PWR  "{\"direct_supply\":{\"mode\":\"disable\"}}"
#define L_SYS  "{\"uptime\":1234,\"cpu_temp\":45,\"cpu_usage\":7,\"model\":\"MU5250\",\"imei\":\"86\"}"
#define L_TRF  "{\"rx_speed\":1000,\"tx_speed\":20,\"day_rx_bytes\":5000}"
#define L_LIST "[{\"id\":9,\"num\":\"10086\",\"date\":\"08-27 04:00\",\"unread\":1,\"text\":\"测试 \\\"q\\\"\"}," \
               "{\"id\":8,\"num\":\"10010\",\"date\":\"08-26 01:00\",\"unread\":0,\"text\":\"{x}\"}]"
#define L_SIM  "{\"state\":\"ready\",\"iccid\":\"8986\",\"imsi\":\"46000\",\"spn\":\"\"}"
#define L_QOS  "{\"qci\":9,\"ambr_dl\":\"1000.5\",\"ambr_ul\":\"200\",\"usb_mode\":\"rndis\"}"
#define L_CLI  "{\"total\":2,\"wifi\":1,\"lan\":1,\"list\":[{\"name\":\"mac\",\"ip\":\"192.168.0.2\",\"mac\":\"aa\"}]}"
#define L_WLAN "{\"ssid\":\"U60\",\"key\":\"k\",\"enc\":\"psk2\",\"enabled\":1}"
#define L_DHCP "{\"ip\":\"192.168.0.1\",\"start\":\"100\",\"limit\":\"50\",\"leasetime\":\"12h\"}"
#define L_IF   "{\"cellular\":{\"enable\":1,\"roam_enable\":0,\"connect_status\":\"ipv4_connected\"}}"
#define L_TYPEC "{\"power_role\":\"source\",\"data_role\":\"host\",\"cc_attch_state\":1}"
#define BLK(n, d) "\"" n "\":{\"revision\":1,\"observed_at\":1,\"stale\":false,\"data\":" d "}"

static void check_v2_matches_legacy(void)
{
    static const char legacy[] = "{\"net\":" L_NET ",\"battery\":" L_BAT ",\"power\":" L_PWR
        ",\"system\":" L_SYS ",\"traffic\":" L_TRF ",\"sms\":{\"unread\":1,\"list\":" L_LIST "}"
        ",\"sim\":" L_SIM ",\"qos\":" L_QOS ",\"clients\":" L_CLI ",\"wlan\":" L_WLAN
        ",\"dhcp\":" L_DHCP ",\"interfaces\":" L_IF ",\"typec\":" L_TYPEC ",\"powerbank\":{\"state\":0},\"ts\":1}";
    static const char snap[] = "{\"epoch\":\"e7\",\"seq\":41,\"blocks\":{"
        BLK("signal", L_NET) "," BLK("battery", L_BAT) "," BLK("charger", L_PWR) ","
        BLK("live", "{\"system\":" L_SYS ",\"runtime\":{\"x\":1},\"traffic\":" L_TRF "}") ","
        BLK("sms", "{\"unread\":1,\"max_id\":9,\"count\":2}") "," BLK("sms_list", "{\"list\":" L_LIST "}") ","
        BLK("sim", L_SIM) "," BLK("qos", L_QOS) "," BLK("clients", L_CLI) "," BLK("wlan", L_WLAN) ","
        BLK("dhcp", L_DHCP) "," BLK("interfaces", L_IF) "," BLK("op", "{\"active\":null}") ","
        BLK("typec", L_TYPEC) "," BLK("powerbank", "{\"state\":0}") ","
        "\"nfc\":{\"revision\":0,\"observed_at\":0,\"stale\":true,\"data\":null}}}";
    static devui_data_t want, got;
    char ev[512];

    memset(&want, 0, sizeof want);
    memset(&got, 0, sizeof got);
    CHECK(parse_snapshot(&want, legacy) == 1);
    CHECK(want.sms_n == 2 && want.bat_percent == 77 && want.clients_total == 2);
    CHECK(want.usb_cc == 1 && want.powerbank == 0 && !strcmp(want.usb_power_role, "source") &&
          !strcmp(want.usb_data_role, "host"));

    /* the snapshot event: every field the screen reads, byte for byte */
    CHECK(feed("snapshot", snap) == 1);
    CHECK(g_backend.have_seq && g_backend.seq == 41 && !strcmp(g_backend.epoch, "e7"));
    CHECK(parse_snapshot(&got, g_backend.live_json) == 1);
    CHECK(memcmp(&want, &got, sizeof want) == 0);
    /* GET /v2/state carries the same: same result, stream position untouched */
    CHECK(v2_apply_snapshot(snap, 0) == 0);
    CHECK(g_backend.seq == 41);

    /* a block event moves one block; a heartbeat moves only seq */
    CHECK(feed("block", "{\"epoch\":\"e7\",\"seq\":42,\"name\":\"battery\",\"revision\":2,"
                        "\"observed_at\":2,\"stale\":false,\"data\":{\"percent\":76}}") == 1);
    CHECK(g_backend.live_data.bat_percent == 76 && g_backend.live_data.sms_n == 2);
    CHECK(feed("heartbeat", "{\"epoch\":\"e7\",\"seq\":43,\"blocks\":{},\"exec_age_ms\":0}") == 0);
    CHECK(feed("block", "{\"epoch\":\"e7\",\"seq\":44,\"name\":\"op\",\"revision\":2,"
                        "\"observed_at\":2,\"stale\":false,\"data\":{}}") == 0);   /* not read here */

    /* stale: left out like a failed read in the old /state, not the kept value */
    CHECK(feed("block", "{\"epoch\":\"e7\",\"seq\":45,\"name\":\"clients\",\"revision\":2,"
                        "\"observed_at\":1,\"stale\":true,\"data\":" L_CLI "}") == 1);
    CHECK(g_backend.live_data.clients_total == 0 && g_backend.live_data.client_n == 0);
    CHECK(feed("block", "{\"epoch\":\"e7\",\"seq\":46,\"name\":\"sms_list\",\"revision\":2,"
                        "\"observed_at\":1,\"stale\":true,\"data\":{\"list\":" L_LIST "}}") == 1);
    CHECK(g_backend.live_data.sms_n == 0 && g_backend.live_data.sms_unread == 1);
    /* typec stale: unknown (-1), not "nothing plugged in" */
    CHECK(feed("block", "{\"epoch\":\"e7\",\"seq\":47,\"name\":\"typec\",\"revision\":2,"
                        "\"observed_at\":1,\"stale\":true,\"data\":" L_TYPEC "}") == 1);
    CHECK(g_backend.live_data.usb_cc == -1 && g_backend.live_data.powerbank == 0);
    CHECK(!g_backend.resync);

    /* a gap (V2-3), another epoch, or a block before any snapshot: resync, nothing applied */
    snprintf(ev, sizeof ev, "{\"epoch\":\"e7\",\"seq\":49,\"name\":\"battery\",\"revision\":3,"
                            "\"observed_at\":3,\"stale\":false,\"data\":{\"percent\":10}}");
    CHECK(feed("block", ev) == 0);
    CHECK(g_backend.resync && g_backend.live_data.bat_percent == 76);
    g_backend.resync = 0;
    CHECK(feed("heartbeat", "{\"epoch\":\"e8\",\"seq\":48,\"blocks\":{}}") == 0);
    CHECK(g_backend.resync);
    g_backend.resync = 0;
    g_backend.have_seq = 0;
    CHECK(feed("block", "{\"epoch\":\"e7\",\"seq\":47,\"name\":\"battery\",\"stale\":false,"
                        "\"data\":{\"percent\":10}}") == 0);
    CHECK(g_backend.resync && g_backend.live_data.bat_percent == 76);

    /* reset for the socket tests below */
    memset(&g_backend, 0, sizeof g_backend);
    g_backend.sse_fd = -1;
    g_backend.inited = 1;
    for (int i = 0; i < B_COUNT; i++) v2_blk[i].fresh = 0;
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
    backend_init_once();
    check_v2_matches_legacy();

    /* never answered: not valid, no alive time */
    CHECK(data_refresh(&d) == 0);
    CHECK(data_backend_alive_wall() == 0);
    CHECK(!data_backend_silent());           /* never answered is not "silent" */

    /* never answered, datad hangs up or holds silent: retries follow
     * DEVUI_BACKEND_RETRY_MS (100 ms here), not every pass of a 1 ms loop.
     * 500 ms ≈ 6 tries × 2 connects (/v2/state, /v2/events); per pass would be ~1000. */
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
