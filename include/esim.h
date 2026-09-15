/*
 * esim.h - eSIM 切换页：列出可插拔 eUICC 卡上的 profile，点两次切换。
 *
 * 不自己跑 lpac，而是调本机 zte-agent（u60p 仓库，127.0.0.1:9090）的
 * /api/esim/ 系列接口：切换之后还要让 ZTE 协议栈重新读卡（UIM 重上电 + 重启
 * zte_topsw_mdm，不收敛就重启整机），这套逻辑 agent 里已经做好并实测过，
 * 这里只做界面。只有切换，不做下载/删除/改名。
 *
 * agent 的登录密码默认从 /data/local/tmp/start_zte_agent.sh 里的
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

/* 点了第 index 个 profile。两段式确认，返回 ESIM_SEL_*。 */
int esim_select(int index);

#endif /* U60_ESIM_H */
