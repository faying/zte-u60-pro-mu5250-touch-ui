/*
 * battery_est.h - 首页电池那一句预估的数据：datad 推送的电流 + 自己低频读的
 * 满充容量（sysfs，10 分钟一次）+ 插电时问 zte-agent 的充电上限（30 秒一次）。
 * 公式在 estimate.c。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60_BATTERY_EST_H
#define U60_BATTERY_EST_H

#include "data.h"

/* 每次 datad 数据更新后调用；5 秒最多采一条。返回 1 = 那一句变了。 */
int battery_est_feed(const devui_data_t *d);
/* 当前那一句（"—" = 还算不出来） */
const char *battery_est_text(void);
/* 离屏渲染测试用：直接指定那一句，之后 feed 不再改它；NULL 恢复正常 */
void battery_est_override(const char *text);

#endif
