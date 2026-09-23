/*
 * esim.h - eSIM 切换页：列出可插拔 eUICC 卡上的 profile，点两次切换。
 *
 * 不自己跑 lpac，而是调本机 zte-agent（u60p 仓库，127.0.0.1:9090）的
 * /api/esim/ 系列接口：切换之后还要让 ZTE 协议栈重新读卡（UIM 重上电 + 重启
 * zte_topsw_mdm，不收敛就重启整机），这套逻辑 agent 里已经做好并实测过，
 * 这里只做界面。只有切换，不做下载/删除/改名。
 *
 * agent 的登录密码默认从 /data/zte-agent.env（没有则 /data/local/tmp/start_zte_agent.sh）里的
 * ZTE_AGENT_PASSWORD 读；可选配置 /data/plugins/u60pro-devui/esim.conf：
 *     port=9090
 *     password=
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60_ESIM_H
#define U60_ESIM_H

/*
 * 主循环每轮调用。active = eSIM 页正显示在亮着的屏幕上。
 * 页面没开、也没有本机发起的切换在跑时直接返回，不碰 agent；
 * 刚打开页面时立刻读一次列表，之后自己节流。返回 1 = 显示内容有变化，需要重绘。
 */
int esim_poll(int active);

/* 显示值，只读缓存，不发请求。 */
const char *esim_current(void);     /* 当前启用的 profile */
const char *esim_state(void);       /* 状态行：就绪 / 切换中 · 已 N 秒 / 失败原因… */
const char *esim_list_html(void);   /* 生成的 profile 列表（act:esim:N） */

#define ESIM_SEL_FAIL    -1
#define ESIM_SEL_ARMED    0         /* 第一次点：亮起，4 秒内再点才切换 */
#define ESIM_SEL_STARTED  1
#define ESIM_SEL_CURRENT  2         /* 点的就是正在用的 */
#define ESIM_SEL_BUSY     3         /* agent 上已有操作在跑 */
#define ESIM_SEL_COOLDOWN 4         /* 卡片冷却中（agent 429），esim_state() 有具体等待时间 */

/* 点了第 index 个 profile。两段式确认，返回 ESIM_SEL_*。 */
int esim_select(int index);

/* 带登录的 POST（无正文）到本机 zte-agent，给别的页面用。
 * 返回 HTTP 状态码，0 = 连不上。会阻塞，最多几秒（agent 卡住时）。 */
int agent_post(const char *path);
/* 同上，可带方法和 JSON 正文（json 可为 NULL）。 */
int agent_request(const char *method, const char *path, const char *json);

/* Raw list access for renderers that build native widgets instead of parsing
 * esim_list_html()'s markup (the LVGL path). */
int esim_profile_count(void);

typedef struct {
    char name[96];
    char sub[192];
    int  enabled;   /* this is the currently-active profile */
    int  armed;     /* first tap landed on this one; a second tap within the
                      * confirm window switches to it */
    int  going;     /* a switch to this profile is in flight */
} esim_profile_t;

void esim_get_profile(int index, esim_profile_t *out);

/* True while the list is display-only (a switch is running on this device or
 * another client, or the agent isn't responding) — mirrors esim_list_html()'s
 * "ro" vs clickable distinction. */
int esim_locked(void);

#endif /* U60_ESIM_H */
