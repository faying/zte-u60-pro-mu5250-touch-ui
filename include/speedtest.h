/*
 * speedtest.h - network speed test, backed by zte-agent's own Ookla-compatible
 * engine (zte-agent/src/speedtest.rs, already used by the admin web page and
 * mobile apps) instead of the old optional `better-speedtest` third-party
 * plugin (see docs/SPEEDTEST.md — that plugin was never installed on real
 * devices, so this page was a permanent "not installed" placeholder).
 *
 * zte-agent requires a Bearer token for every /api/* route except login —
 * same requirement esim.c already meets to reach its own zte-agent
 * endpoints, so this reuses that exact login/retry pattern rather than
 * inventing a second one.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60_SPEEDTEST_H
#define U60_SPEEDTEST_H

/* Poll /api/speedtest/progress if `active` and the cache has expired —
 * mirrors esim_poll(). Returns 1 if a fetch actually ran. */
int speedtest_poll(int active);

typedef enum {
    ST_IDLE, ST_LATENCY, ST_DOWNLOAD, ST_UPLOAD,
    ST_COMPLETE, ST_CANCELLED, ST_ERROR
} speedtest_phase_t;

speedtest_phase_t speedtest_phase(void);
const char *speedtest_phase_label(void);   /* 中文短语，可直接显示 */
int    speedtest_progress_pct(void);       /* 0-100 */
double speedtest_live_mbps(void);          /* 下载/上传阶段的实时速率 */
double speedtest_ping_ms(void);            /* < 0 = 还没测出 */
double speedtest_jitter_ms(void);          /* < 0 = 还没测出 */
double speedtest_download_mbps(void);      /* < 0 = 还没出结果 */
double speedtest_upload_mbps(void);        /* < 0 = 还没出结果 */
const char *speedtest_server(void);        /* "赞助商 (服务器名)"，agent 挑的 */
const char *speedtest_error(void);         /* "" = 无错误 */
int    speedtest_running(void);

/* 0 = 连不上 zte-agent 或密码不对——页面据此显示"测速服务不可用"而不是
 * 空白/一直转圈。跟 esim.c 的 s_offline 同一个用途。 */
int    speedtest_agent_reachable(void);

int speedtest_start(void);   /* 1 = 已发起（agent 立刻在后台线程跑，不等结果） */
int speedtest_stop(void);    /* 1 = 已发起停止 */

/*
 * 服务器列表——管理网页那边有下拉选服务器，触屏这版一开始图省事漏了，
 * 一律用 agent 自动挑的第一个。补上：agent 的 /api/speedtest/servers
 * 本来就有，start() 也本来就接受可选的 server_id。
 */
#define ST_MAX_SERVERS 24
typedef struct { long id; char name[48]; char sponsor[64]; char country[32]; } speedtest_server_t;

/* 拉一次服务器列表（有缓存，agent 侧 5 分钟 TTL）。跟 speedtest_poll()
 * 一样，页面打开时调用，列表非空后不用每轮都拉。 */
int  speedtest_servers_poll(int active);
int  speedtest_servers_count(void);
void speedtest_get_server(int i, speedtest_server_t *out);

/* -1 = 自动（agent 挑最佳），否则是 speedtest_get_server() 里的下标。 */
int  speedtest_selected_index(void);
void speedtest_select_server(int index);

#endif /* U60_SPEEDTEST_H */
