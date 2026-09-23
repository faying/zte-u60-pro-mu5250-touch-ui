/*
 * scenario.h - 首页情景卡片：读 zte-agent 免登录的 /api/public/status 里的 scenario 段。
 *
 * 情景引擎（在家 / 外出 / 国外）跑在 zte-agent 里，这里只读不写：卡片告诉你
 * 设备现在认为自己在哪、Wi-Fi 是不是它关的、有没有被手动固定。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60_SCENARIO_H
#define U60_SCENARIO_H

/*
 * 主循环每轮调用。active = 首页正显示在亮着的屏幕上。
 * 不 active 时直接返回；刚切到首页时立刻读一次，之后自己节流。
 * 返回 1 = 显示内容有变化，需要重绘。
 */
int scenario_poll(int active);

typedef struct {
    int  available;      /* agent 有回应且引擎已配置 —— 否则卡片不出现 */
    int  enabled;        /* 引擎开着 */
    char name[48];       /* 当前情景名（在家 / 外出 / 国外），刚开机还没判定时为空 */
    int  wifi_off;       /* 当前情景把 AP 关了 */
    char pin[48];        /* 被手动固定到的情景 id，"" = 自动 */
    long last_switch;    /* 墙钟秒，0 = 从未切换 */
} scenario_status_t;

void scenario_get_status(scenario_status_t *out);

#endif /* U60_SCENARIO_H */
