# 测速

[English](SPEEDTEST.md) · **中文**

触屏的测速页用的是 zte-agent 自带的测速引擎（manager 仓库 `zte-agent/src/speedtest.rs`，管理网页也用它）；屏幕这边是 `src/speedtest.c` 和 `src/ui_parts/speedtest.c`。设备上不用另装东西。

- 开始 / 停止：`POST /api/speedtest/start`、`POST /api/speedtest/stop`（zte-agent 在后台线程里跑）。
- 进度：`GET /api/speedtest/progress`，页面开着时大约每秒读一次。
- 服务器：`GET /api/speedtest/servers`（zte-agent 缓存 5 分钟）；第一行是「自动」（zte-agent 挑最好的）。
- 每个请求都带 zte-agent 的 Bearer token（`src/agent_client.c`）。连不上 zte-agent 时页面直接说明，不发起测速。

旧的可选插件 `better-speedtest`（litehtml 时代）从没在真机上装过，已不再支持；见 tag `legacy-litehtml`。
