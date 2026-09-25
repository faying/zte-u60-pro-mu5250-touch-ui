/*
 * estimate.h - 电池预估时间。规则见 manager docs/battery-estimate.md，
 * 和管理网页 web/src/lib/batteryEstimate.ts 跑同一份
 * tests/fixtures/battery-estimate.json。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60_ESTIMATE_H
#define U60_ESTIMATE_H

#include <stddef.h>

#define EST_WINDOW_S     180
#define EST_MIN_UA       50000L
#define EST_MAX_SAMPLES  64
#define EST_NONE         (-1LL)   /* 节点缺失 */

typedef struct {
    double t;      /* 秒，单调时钟 */
    long   ua;     /* 电池电流 µA，正 = 充电 */
    int    online; /* 充电器插着 */
} est_sample_t;

typedef struct {
    const est_sample_t *s;
    int n;
    int soc;
    long long full_uah;     /* EST_NONE = 缺 */
    long long counter_uah;  /* EST_NONE = 缺 */
    int target_pct;
    int paused_at_limit;
} est_input_t;

enum { EST_UNKNOWN = 0, EST_CHARGING, EST_DISCHARGING, EST_REACHED, EST_PAUSED };

typedef struct {
    int  kind;
    long minutes;  /* 只有 EST_CHARGING / EST_DISCHARGING 有效 */
} est_t;

/* 时间加权平均电流；没有采样返回 0 且 *ok = 0 */
double est_average(const est_sample_t *s, int n, int *ok);
est_t  est_compute(const est_input_t *in);
/* 首页那一句，UTF-8，不用 %f */
void   est_text(est_t e, int target_pct, char *out, size_t cap);

/* 加一条采样，丢掉窗口外的；返回新条数 */
int est_push(est_sample_t *buf, int n, est_sample_t s);

/* 从 <root>/battery 读 charge_full / charge_counter（µAh），缺的给 EST_NONE */
void est_read_capacity(const char *root, long long *full_uah, long long *counter_uah);

/* 解析 zte-agent GET /api/device/charge-control 的 data 对象，
 * 得到目标电量和「到上限暂停」（限制开 + 已停充 + 非手动 + charger_connect）。 */
void est_parse_charge_control(const char *data, int charger_connect, int *target_pct, int *paused);

#endif
