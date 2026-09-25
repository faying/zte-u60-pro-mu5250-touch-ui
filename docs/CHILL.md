# CHILL 页（mihomo 控制）

CHILL 是设备上的透明代理（原生 mihomo，TUN 模式），由 manager 仓库的 `scripts/chill/` 安装和监督（`./install.sh chill`）。
触屏上它在「出口」标签里（`src/ui.c` 的 `build_sub_chill()`，数据模块 `src/chill.c`）。

## 页面上有什么

- **状态**：当前节点、延迟、代理组，总开关；连接数（代理 / 直连）、累计流量。
- **出口**：代理 / 全局 / 直连·AI 不动 / 全部直连，经 zte-agent 的 `PUT /api/services/chill/exit`。
- **档位**：省电 / 标准 / 性能，经 `PUT /api/services/chill/profile`（agent 调 `chill.sh profile`）。
  当前值读 `/tmp/chill.state`；进出「省电」要重启核心，连接会断约 10 秒，页面上写明。设备过热暂按省电跑时显示「太热 · 暂按省电」。
- **节点**、**规则 → 节点**：二级页，直接读 mihomo 的 `/group`、`/proxies`、`/connections`。

## 为什么直接读 mihomo 接口

切模式、选节点这些动作本来就要写在界面程序里；数据如果再放进 zwrt-datad，就得同时维护两个 fork。
所以 `src/chill.c` 直接读**本机**的 mihomo 控制接口（`127.0.0.1:9999`），不经过网络。

## 配置

不需要配置文件，默认值：`external-controller` 为 `127.0.0.1:9999`、secret 为空、默认代理组「🚀 节点选择」。
要改就写 `/data/plugins/u60pro-devui/chill.conf`（重启界面生效）：

```
port=9999
secret=
group=🚀 节点选择
```

`group` 只是默认选中的组，页面上可以切换；配置里的组不存在时退回第一个组。

## 实现要点

- **组测延迟是异步的**：mihomo 要等最慢的节点（最长约 3 秒），界面不能干等。发出请求后留着 socket，每次刷新收一点，8 秒收不完放弃；换组时丢掉进行中的请求。
- **延迟结果跨刷新保留**：刷新每 2 秒一次，一轮测速要好几秒，按节点原名继承已有结果。
- **emoji 要处理**：国旗还原成国家码字母（🇯🇵 → JP），其他 emoji 去掉。显示名和调接口用的原名分开存。
- **URLTest / Fallback 组不能手选**：由内核自动选，`PUT` 会被拒绝，页面上标灰、只读。
- **`sc_http()` 返回静态缓冲区**：下一次调用会覆盖，同一份响应里的字段要在发下一个请求前取完。

## 命名

屏幕、代码、配置里一律叫 CHILL（`chill.c`、`chill_` 前缀）。令牌前缀 `SC_`、动作前缀 `act:sc*` 是缩写，保持不变。
