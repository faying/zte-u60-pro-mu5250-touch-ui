/*
 * battery_est.h - 系统页电池那一句预估。zte-agent 算（battery_eta.rs，自己每 5 秒
 * 采样），这里只在系统页亮着时每 10 秒取一次 GET /api/screen，写成文字。
 * 取不到超过 30 秒就显示「—」，不留旧数当新数。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60_BATTERY_EST_H
#define U60_BATTERY_EST_H

/* 每轮主循环调用；active = 显示这一句的页面亮着。返回 1 = 那一句变了。 */
int battery_est_poll(int active);
/* 当前那一句（"—" = agent 还没给、读不到或过期） */
const char *battery_est_text(void);
/* 离屏渲染测试用：直接指定那一句，之后 poll 不再改它；NULL 恢复正常 */
void battery_est_override(const char *text);

#endif
