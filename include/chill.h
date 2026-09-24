/*
 * chill.h - "CHILL" 面板（ShellCrash / mihomo, clash API）status + control.
 *
 * Reads the local clash-compatible API (default 127.0.0.1:9999) directly rather
 * than going through zwrt-datad: the actions below have to live in this binary
 * anyway (act: handlers are compiled in), so keeping the data here too avoids a
 * second fork to maintain.
 *
 * Site settings come from /data/plugins/u60pro-devui/chill.conf:
 *     port=9999
 *     secret=
 *     group=🚀 节点选择
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60_CHILL_H
#define U60_CHILL_H

/*
 * Poll the API if `active` and the cache has expired. Pass 0 when this screen
 * doesn't need CHILL data (mirrors tailscale_poll()/esim_poll()) — the main
 * loop is the only caller, gated on path_is_signal_home()/path_is_chill().
 * Returns 1 if a fetch actually ran (caller should consider a re-render).
 */
int chill_poll(int active);

/* Home-screen / lock-screen summary card. Pure formatter (no I/O), same
 * pattern as tailscale_card_html(). locked=1 hides mode/node/speed. */
const char *chill_card_html(int locked);

/* Display values. All return a stable pointer valid until the next refresh. */
const char *chill_core(void);      /* 运行中 / 已停止 */
const char *chill_mode(void);      /* 出口：代理 / 直连·AI 不动 / 全部直连 / 全局 / - */
const char *chill_exit_raw(void);  /* proxy / direct_keep_ai / direct_all / global / "" */
/* 切出口（走 zte-agent PUT /api/services/chill/exit，带登录）。1 = 成功。会阻塞到 agent 回应。 */
int         chill_set_exit(const char *state);
const char *chill_mode_raw(void);  /* rule / global / direct / "" */
const char *chill_group(void);     /* 主选择器组名 */
const char *chill_node(void);      /* 该组当前选中的节点 */
const char *chill_traffic(void);   /* 累计 "↓1.2G ↑45M" */
const char *chill_speed(void);     /* 实时 "↓1.2M/s ↑45K/s" */
const char *chill_chain(void);     /* 真实出口链路 "SNTP → TW01" */
int         chill_restart_armed(void); /* 1 = 已亮起，再按一次才执行 */
const char *chill_conns(void);     /* 活跃连接数 */
int         chill_online(void);    /* 1 = API 可达 */

const char *chill_conn_split(void);      /* "代理 12 · 直连 8" */
const char *chill_grouplist_html(void);  /* 组切换按钮 */
int         chill_select_group(int index);
int         chill_group_selectable(void);/* URLTest/Fallback 组不可手选 */

/* Same group list, as data, for the LVGL path — mirrors chill_node_info_t.
 * auto_pick: 1 = URLTest/Fallback（内核自动挑，不可手选节点，仍可切换查看）。 */
typedef struct { char name[64]; int selected; int auto_pick; } chill_group_info_t;
int  chill_group_count(void);
void chill_get_group(int i, chill_group_info_t *out);

/* Generated <a href="act:scnode:N"> list for the node picker page. */
const char *chill_nodelist_html(void);

/* Same node list, as data, for renderers that bind widgets instead of parsing
 * markup (the LVGL path) — mirrors tailscale_get_status()'s pattern.
 * delay: >0 = milliseconds, 0 = timed out, -1 = not measured yet. */
typedef struct { char name[64]; int delay; int selected; } chill_node_info_t;
int  chill_node_count(void);
void chill_get_node(int i, chill_node_info_t *out);

/*
 * 流量按"命中的分流组 -> 实际出口节点"这一对关系汇总，取当前 top 6——跟
 * chill_group()/chill_node()/chill_chain() 不是一回事：那三个只描述"节点
 * 选择"这一个配置好的组，规则模式下大部分流量根本不走它。这里的数据来自
 * /connections 里每条连接自带的 chains 数组。
 *
 * 分组和节点曾经拆成两张独立 top N 表，各自按总字节数排序——界面上摆在
 * 一起容易被看成"第 i 条分组对应第 i 条节点"，实际是两个互不相干的排名。
 * 现在按 (分流组, 节点) 二元组做 key，name 直接是格式化好的
 * "组名 -> 节点名"，才真的回答得了"这条规则流量去哪了"（2026-09-22 反馈）。
 *
 * 持久累计统计，不是"当前还活着的连接"快照——按连接 id 跟踪每轮轮询的
 * 字节增量再累加，连接关闭不会丢数据，只在 devui 进程重启时清零（跟
 * mihomo 自己的 downloadTotal/uploadTotal 一个道理）。
 * traffic 已经格式化成 "↓1.2M ↑45K" 这种字符串；bytes 是上下行之和，
 * 按它降序排列，画比例条之类的用得上。
 */
typedef struct { char name[64]; char traffic[40]; long bytes; } chill_traffic_item_t;
int  chill_top_pair_count(void);
void chill_get_top_pair(int i, chill_traffic_item_t *out);

/* Controls. Each returns 1 on success and forces the next refresh. */
int chill_set_mode(const char *mode);   /* rule|global|direct */
int chill_select_node(int index);       /* index into the cached node list */
int chill_restart_core(void);
/* 档位：eco / standard / perf（chill.sh profile）。raw 是用户选的，effective 是实际在跑的
 * （温度降档时是 eco）。set 经 agent 的 PUT /api/services/chill/profile，进出 eco 会重启核心。 */
const char *chill_profile_raw(void);
const char *chill_profile_effective_raw(void);
int chill_thermal_eco(void);
int chill_set_profile(const char *profile);
int chill_test_delay(void);             /* 组测延迟（异步）：1 = 已发起，2 = 上一轮还在跑，0 = 发不出去 */
int chill_delay_pending(void);          /* 1 = 测延迟还在进行，结果到了会自动填进节点列表 */

/*
 * 一键退回原厂 UI。命令来自配置项 restore_cmd（默认拉起原厂 init）。
 * 返回 1 = 接管方已确认存活，此时调用方才可以退出进程；0 = 没起来，别退。
 */
int devui_restore_stock(void);

/* 面板方向：1 = 转 180°（默认，U60 倒装），0 = 正装。来自配置项 rotate=180|0 */
int devui_rotate180(void);

#endif /* U60_CHILL_H */
