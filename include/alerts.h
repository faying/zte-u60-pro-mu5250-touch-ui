/*
 * alerts.h - 告警页的数据源：zte-agent 的 /api/alerts（要登录，和 eSIM 页一样
 * 用 agent 的密码换 token）。事件是 supervise.sh / u60-guard / u60-uid 写的，
 * 契约见 manager 仓库 docs/RELIABILITY.md。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60_ALERTS_H
#define U60_ALERTS_H

#define ALERTS_MAX 12

typedef struct {
    long seq;
    long time;          /* 设备时钟秒（当地时间标成 UTC），0 = 当时时钟未校准 */
    long uptime;        /* 开机后秒数，time 为 0 时用它 */
    char label[96];     /* 按类别写好的中文一句话 */
    char text[128];     /* 事件原文（英文技术说明），小字显示 */
    int  unread;
} alert_item_t;

/* 主循环每轮调用。active = 告警页正显示在亮着的屏幕上：刚打开时立即读，之后 10 秒一次。
 * 返回 1 = 内容变了，需要重绘。 */
int alerts_poll(int active);

int  alerts_count(void);
int  alerts_unread(void);
void alerts_get(int index, alert_item_t *out);
const char *alerts_error(void);   /* "" = 正常 */

/* 体检（/api/health，doctor.sh 的结果）里不正常的项：系统页「健康」写「N 项注意」
 * 的就是这些，和告警的已读/未读是两回事，所以同一页里分开列（2026-09-25）。 */
#define HEALTH_MAX 8
typedef struct {
    int  bad;           /* 1 = 异常，0 = 注意 */
    char id[24];
    char label[48];
    char detail[320];
} health_item_t;

int  health_count(void);          /* 不正常的项数；-1 = 还没读到，-2 = 读过但没读到 */
int  health_checked(void);        /* 正常的项数（读到了才有意义） */
void health_get(int index, health_item_t *out);

/* 全部标为已读（发给 agent，随后立即重读）。 */
void alerts_mark_all_read(void);

#endif /* U60_ALERTS_H */
