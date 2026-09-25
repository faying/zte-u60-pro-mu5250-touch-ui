/*
 * Unit tests for tailscale.c's LocalAPI /status parser (the fields the
 * Tailscale page lists). No tailscaled needed:
 *   scripts/test/tailscale/run.sh
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/tailscale.c"

static int pass, fail;

#define CHECK(desc, cond)                                              \
    do {                                                               \
        if (cond) { pass++; printf("  ok   %s\n", desc); }             \
        else      { fail++; printf("  FAIL %s  [%s]\n", desc, #cond); } \
    } while (0)

static char body[16384];

static void iso(char *out, size_t n, long ago)
{
    time_t t = time(NULL) - ago;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%dT%H:%M:%S.123456789Z", &tm);
}

int main(void)
{
    char hs[40], seen[40];
    tailscale_status_t st;
    tailscale_peer_t pe;

    iso(hs, sizeof hs, 42);
    iso(seen, sizeof seen, 3 * 3600);
    snprintf(body, sizeof body,
        "{\"Version\":\"1.102.4-t0123abcd-g4567\",\"BackendState\":\"Running\",\"Health\":[],"
        "\"CurrentTailnet\":{\"Name\":\"me@example.com\",\"MagicDNSSuffix\":\"tail0.ts.net\"},"
        "\"Self\":{\"DNSName\":\"u60-pro.tail0.ts.net.\",\"HostName\":\"u60\",\"OS\":\"linux\","
        "\"TailscaleIPs\":[\"100.64.0.7\",\"fd7a:115c:a1e0::7\"],\"Relay\":\"hkg\",\"Online\":true,"
        "\"PrimaryRoutes\":[\"192.168.0.0/24\"],\"KeyExpiry\":\"2027-03-01T08:00:00Z\"},"
        "\"Peer\":{"
        "\"nodekey:a\":{\"DNSName\":\"macbook-pro.tail0.ts.net.\",\"OS\":\"macOS\",\"TailscaleIPs\":[\"100.64.0.2\"],"
        "\"Relay\":\"tok\",\"CurAddr\":\"203.0.113.5:41641\",\"RxBytes\":1234567890,\"TxBytes\":5555,"
        "\"Online\":true,\"Active\":true,\"ExitNode\":false,\"LastHandshake\":\"%s\",\"LastSeen\":\"0001-01-01T00:00:00Z\"},"
        "\"nodekey:b\":{\"DNSName\":\"iphone.tail0.ts.net.\",\"OS\":\"iOS\",\"TailscaleIPs\":[\"100.64.0.3\"],"
        "\"Relay\":\"hkg\",\"CurAddr\":\"\",\"RxBytes\":0,\"TxBytes\":0,"
        "\"Online\":false,\"Active\":false,\"LastHandshake\":\"0001-01-01T00:00:00Z\",\"LastSeen\":\"%s\"}}}",
        hs, seen);
    parse_status(body);
    tailscale_get_status(&st);
    CHECK("self name, ip", !strcmp(st.name, "u60-pro") && !strcmp(st.ip, "100.64.0.7"));
    CHECK("self ipv6", !strcmp(st.ip6, "fd7a:115c:a1e0::7"));
    CHECK("version without build suffix", !strcmp(st.version, "1.102.4"));
    CHECK("tailnet name", !strcmp(st.tailnet, "me@example.com"));
    CHECK("key expiry date", !strcmp(st.key_expiry, "2027-03-01"));
    CHECK("self os", !strcmp(st.os, "linux"));
    CHECK("two peers", tailscale_peer_count() == 2 && st.peers == 2 && st.direct == 1);
    tailscale_get_peer(0, &pe);
    CHECK("peer os, endpoint, relay", !strcmp(pe.os, "macOS") && !strcmp(pe.cur_addr, "203.0.113.5:41641") && !strcmp(pe.relay, "tok"));
    CHECK("peer bytes (over 1 GB)", pe.rx == 1234567890LL && pe.tx == 5555);
    CHECK("handshake about 42 s ago", pe.hs_ago >= 41 && pe.hs_ago <= 45);
    CHECK("zero LastSeen is unknown", pe.seen_ago == -1);
    tailscale_get_peer(1, &pe);
    CHECK("never handshaked: -1", pe.hs_ago == -1);
    CHECK("offline peer last seen about 3 h ago", pe.seen_ago >= 3 * 3600 - 2 && pe.seen_ago <= 3 * 3600 + 5);

    parse_status("{\"BackendState\":\"NeedsLogin\",\"Self\":{\"KeyExpiry\":\"0001-01-01T00:00:00Z\"},\"Peer\":null}");
    tailscale_get_status(&st);
    CHECK("no expiry, no version, no peers", !st.key_expiry[0] && !st.version[0] && tailscale_peer_count() == 0);

    printf("\npassed %d, failed %d\n", pass, fail);
    return fail != 0;
}
