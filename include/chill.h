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
const char *chill_mode(void);      /* 规则 / 全局 / 直连 / - */
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

/* Generated <a href="act:scnode:N"> list for the node picker page. */
const char *chill_nodelist_html(void);

/* Controls. Each returns 1 on success and forces the next refresh. */
int chill_set_mode(const char *mode);   /* rule|global|direct */
int chill_select_node(int index);       /* index into the cached node list */
int chill_restart_core(void);
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
