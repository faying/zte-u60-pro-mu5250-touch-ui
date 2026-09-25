/*
 * tailscale.h - 首页 Tailscale 状态卡片。
 *
 * 直接走 tailscaled 的 LocalAPI（unix socket 上的 GET /localapi/v0/status），
 * 不拉起 tailscale CLI：一次 2ms 级、十几 KB。socket 按常见路径探测，
 * 设备上没有 tailscaled 时卡片整个不出现。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60_TAILSCALE_H
#define U60_TAILSCALE_H

/*
 * 主循环每轮调用。active = 首页正显示在亮着的屏幕上（含锁屏预览）。
 * 不 active 时直接返回；刚切到首页时立刻读一次，之后自己节流。
 * 返回 1 = 显示内容有变化，需要重绘。
 */
int tailscale_poll(int active);

/* 整张卡片的 HTML；没装 tailscaled 时返回 ""。locked = 锁屏预览，只露状态和在线数。 */
const char *tailscale_card_html(int locked);

/* Raw fields behind the HTML card, for renderers that bind widgets directly
 * instead of parsing markup (the LVGL path). Mirrors tailscale_poll()'s
 * internal state 1:1 — call after tailscale_poll() to read the latest data. */
typedef struct {
    int  available;      /* tailscaled socket found on this device */
    int  ok;              /* last poll got a response */
    char state[24];       /* BackendState: Running/Starting/NeedsLogin/... */
    int  self_online;
    char name[64];
    char ip[48];
    char relay[32];
    char routes[160];
    char exit_node[64];
    int  exit_online;
    char health[160];
    int  peers, peers_online, active, direct;
    /* 详情页（2026-09-25）：本机和 tailnet 的更多信息 */
    char ip6[48];
    char os[16];
    char version[24];     /* "1.102.4"（去掉 -t… 构建后缀） */
    char tailnet[64];     /* CurrentTailnet.Name */
    char key_expiry[16];  /* "2027-03-01"；"" = 不过期 / 不知道 */
} tailscale_status_t;

void tailscale_get_status(tailscale_status_t *out);

/* Per-peer detail for the Tailscale subpage. The counters above are derived
 * from the same walk over the LocalAPI "Peer" map; this keeps the first
 * TS_PEER_MAX entries so a renderer can list them instead of just counting.
 * direct: Active with a CurAddr (hole-punched) — otherwise it is relayed. */
#define TS_PEER_MAX 12
typedef struct {
    char name[40];
    char ip[24];
    int  online, active, direct, exit_node;
    char os[16];          /* linux / macOS / iOS / windows / android … */
    char cur_addr[48];    /* 直连时对端的公网地址:端口 */
    char relay[16];       /* 对端的首选 DERP */
    long long rx, tx;     /* 本机从它收到 / 发给它的字节（tailscaled 启动以来） */
    long hs_ago;          /* 距上次握手的秒数，-1 = 从没握手 */
    long seen_ago;        /* 离线的：最后在线多久了，-1 = 不知道 */
} tailscale_peer_t;
int  tailscale_peer_count(void);
void tailscale_get_peer(int i, tailscale_peer_t *out);

#endif /* U60_TAILSCALE_H */
