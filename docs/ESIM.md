# eSIM 切换页

触屏「蜂窝 → SIM 与 eSIM」列出 SIM 槽里那张可插拔 eUICC 卡（5ber、eSTK.me 这类）上的 profile，点两次切换。
**只做切换**；下载、删除、改名在管理网页里做。

## 数据流：调 zte-agent，不自己跑 lpac

```
lpac（qmi_qrtr）◀── zte-agent（esim.rs）──HTTP 127.0.0.1:9090──▶ src/esim.c ──▶ 界面
```

用到的接口：`GET /api/esim/profiles`、`POST /api/esim/switch`、`GET /api/esim/job`（查切换进度）。

切换本身只是一条 `lpac profile enable`，难的是让中兴的协议栈认到新卡：它只在守护进程启动时读一次 SIM，
所以 agent 会给 UIM 重新上电、重启 `zte_topsw_mdm`、等 IMSI 变化，一般 10 秒内完成。这些都在 agent 里做，界面只显示进度和结果。

前提：设备上装了 eSIM 组件（`/data/esim` 的 lpac + 带 `/api/esim/*` 的 zte-agent，装机包 `./install.sh esim`），没装时页面会提示。

## 登录

界面要用后台密码登录 agent。密码从 `/data/zte-agent.env` 的 `ZTE_AGENT_PASSWORD` 读（装机包写的），一般不用配。
要覆盖就写 `/data/plugins/u60pro-devui/esim.conf`。

## 注意

- 有些卡（如 eSTK.me）短时间内连续操作会返回 `catBusy`：agent 有 5 分钟冷却保护，界面会显示原因，别反复点。
- 如果你是通过这台 U60 自己的网络远程操作，别切到没有流量的 profile，切过去就连不回来了，只能到设备跟前在屏幕上切回。
