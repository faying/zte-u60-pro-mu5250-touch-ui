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
 * 不 active 时也照样按节流读（30 秒一次，本机请求）：顶栏的「后台失联」
 * 提示任何页面都要能看到。刚切到首页时立刻读一次。
 * 返回 1 = 显示内容有变化，需要重绘。
 */
int scenario_poll(int active);

/*
 * zte-agent 是否失联、有几条未读告警（/api/public/status 的 alerts.unread）。
 * lost_secs > 0 = 已经这么多秒读不到 public status，且超过 2 分钟；0 = 正常。
 * 刚启动还没读成功过的也从启动时刻算起。
 */
typedef struct {
    long lost_secs;
    int  unread;
    int  checked;     /* the device check (doctor.sh, via the agent) has run */
    int  bad, warn;   /* its counts; details are on the admin web's health page */
} agent_health_t;

void agent_health(agent_health_t *out);

typedef struct {
    int  available;      /* agent 有回应且引擎已配置 —— 否则卡片不出现 */
    int  enabled;        /* 引擎开着 */
    char name[48];       /* 当前情景名（在家 / 外出 / 国外），刚开机还没判定时为空 */
    int  wifi_off;       /* 当前情景把 AP 关了 */
    char pin[48];        /* 被手动固定到的情景 id，"" = 自动 */
    long last_switch;    /* 墙钟秒，0 = 从未切换 */
    int  abroad;         /* 当前情景是「国外」（引擎开着时才算） */
    int  chill_on;       /* CHILL 总开关：1 开 / 0 关 / -1 还不知道 */
    int  chill_back;     /* 在国外关掉的 CHILL，回国会被自动打开 */
    int  auto_direct;    /* 国外情景会自动把「🚀 节点选择」切直连（AI 不动） */
} scenario_status_t;

void scenario_get_status(scenario_status_t *out);

/*
 * 开 / 关 CHILL 总开关：走 zte-agent（带登录，和 eSIM 页同一套），agent 在
 * 国外关掉时会记下「回国自动打开」。返回 1 = agent 接受了。本地状态先按
 * 请求改掉（下一轮 scenario_poll 返回 1 重绘），几秒后再读一次真实状态。
 */
int scenario_chill_set(int on);

#endif /* U60_SCENARIO_H */
