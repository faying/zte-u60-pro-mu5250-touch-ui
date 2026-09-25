# eSIM 切换页

> 后端对接（`src/esim.c` 调 zte-agent `/api/esim/*`）两个版本共用；下文的 `{{ES_*}}` 令牌、`act:esim:*` 动作和 `ui/functions/esim.html` 只属于 litehtml 版。

在 DevUI「更多功能」里加一个 eSIM 页：列出 SIM 槽里那张可插拔 eUICC 卡（eSTK.me / 5ber 这类）
上的 profile，点两次切换。**只做切换**，下载、删除、改名都留给网页后台。

## 后端：调 zte-agent，不自己跑 lpac

读卡和切换都走本机 zte-agent（u60p 仓库，`127.0.0.1:9090`）的 `/api/esim/*`：

```
lpac(qmi_qrtr) ◀── zte-agent esim.rs ──HTTP(127.0.0.1:9090)──▶ src/esim.c ──▶ {{ES_*}} 令牌
                                                                     ▲
                                                     act:esim:N ─────┘
```

切换本身只是一条 `lpac profile enable`，难的是之后让 ZTE 协议栈认到新卡：它只在守护进程
启动时读一次 SIM，所以 agent 要做 UIM 重上电 → 重启 `zte_topsw_mdm` → 等 IMSI 变化，
30 秒内不收敛就重启整机。这套在 agent 里已经做好并实测过（一般 10 秒内完成），
DevUI 再写一遍只会分叉，所以这里只做界面。

前提：设备上装了 u60p 的 eSIM 组件（`/data/esim` + 带 `/api/esim/*` 的 zte-agent），
没装时页面显示「未安装 eSIM 组件」。

## 配置

agent 要登录。密码默认从它的启动脚本 `/data/local/tmp/start_zte_agent.sh` 里的
`ZTE_AGENT_PASSWORD` 读，一般不用配。要覆盖就写 `/data/plugins/u60pro-devui/esim.conf`：

```
port=9090
password=
```

token 有效期 1 小时，过期（或 agent 重启过）收到 401 会自动重登一次。

## 模板令牌与动作

| 令牌 | 内容 |
| --- | --- |
| `{{ES_CUR}}` | 当前启用的 profile（有备注名用备注名） |
| `{{ES_STATE}}` | 就绪 / 切换中 · 已 N 秒 / 已切换到 X（N 秒）/ 切换失败：原因 / 网页端正在切换… |
| `{{ES_LIST}}` | profile 列表（生成的 HTML）：名字、运营商 · 原名 · ICCID 尾号，使用中标绿 |

| 动作 | 行为 |
| --- | --- |
| `act:esim:<序号>` | **两段式确认**：第一次点亮起「再点一次」，4 秒内再点才 `POST /api/esim/switch` |

## 刷新策略

令牌只读缓存，**所有请求都在主循环的 `esim_poll()` 里发**，而且只在 eSIM 页亮着显示时才发
（实测离开页面后 10 秒内对 agent 零请求）：

- 刚打开页面：立刻查一次 job、读一次列表（lpac `profile list`，实测约 65ms）。
- 页面开着：每 2 秒查一次 `/api/esim/job`（1ms 级，不碰卡）。列表不定时重读，
  只在**任何 job 结束**时重读——网页端做的下载/删除/切换也都是 job，这样也能跟上。
- 读卡失败（插的是普通 SIM）**不自动重试**，免得页面开着就每 2 秒敲一次卡；重新进页面再读。
- 本机发起的切换进行中：每秒查一次 job；离开页面或熄屏也继续查，直到出结果。
- agent 没响应：退到每 5 秒一次，免得每轮都卡 1.5 秒的读超时；恢复后自动重读列表。

`esim_poll()` 返回的是「显示内容指纹变没变」，没变不重绘。

## 实现上的几个点

**为什么同步请求就够。** CHILL 的组测延迟得做成异步，是因为要等最慢节点 3 秒；
这里 `/api/esim/job` 是读内存、`profiles` 约 65ms、`switch` 立刻返回 job id（活在 agent 的后台线程里干），
agent 又是多线程的，同步请求不会让界面卡住。

**请求用 HTTP/1.0。** 客户端不解分块编码。tiny_http 本来就回 `Content-Length`，但换个服务端
（比如测试时试过的设备自带 uhttpd）就会分块，1.0 从协议上避免。

**profile 列表要逐个对象截断再取字段。** `json_get` 只认第一层的键、而且会一直往后扫，
直接在数组上取 `iccid` 会取到下一个对象里去。所以按括号配对找到每个 `{...}`，
临时在结尾写个 `\0` 再取字段。profile 可能带 base64 图标，响应缓冲给到 64KB。

**亮起和点到正在用的那张都不弹提示框。** 提示框（`.toast`，`top: 210px`）正好盖在列表第 2、3 行上，
点击命中的是提示框而不是下面那一行——最初亮起时弹「再点一次确认切换」，结果确认的第二下
被它吃掉，永远切不过去。现在亮起只靠行本身变红 +「再点一次」，并顺手清掉残留的提示框。
**同样的问题 CHILL 也有**（「已切换节点」的提示框盖住节点列表，2.5 秒内点不了别的节点）。

**亮起记的是 ICCID，不是序号。** 两次点击之间列表可能被重读过（有 job 结束），
按序号记会点到别的卡上。

**列表什么时候只读。** agent 上有 job 在跑（不管是谁发起的）、或 agent 连不上时，列表整体变成
`<div>`，不给点；agent 那边对并发切换也会回 409。

**结果要留住，但别过时。** 离开页面期间出的结果，回来还要看得到，超过 1 分钟的才在进页面时清掉；
别人（网页端）的操作结束时，本机上一次的结果立刻清掉，免得「切换失败」挂在一张已经换好的卡上。

**切换中途 agent 重启。** 旧 token 失效（401 → 自动重登），job id 从 0 重新数，
和本机记的对不上 ⇒ 提示「切换结果未知，请看当前配置」并重读列表。

## 不碰真卡的验证

真切换会断网，对只能经设备自己蜂窝出口连过去的机器尤其危险：切到没流量的 profile
就再也连不上了。所以流程验证用假 agent `scripts/esim-mock-agent.lua`（lua + nixio，设备自带）：

```sh
# 设备上
lua /tmp/esim-mock-agent.lua 9091 mocktoken &          # 密码固定 mock
printf "port=9091\npassword=mock\n" > /data/plugins/u60pro-devui/esim.conf
# 重启 DevUI，进 eSIM 页。按 CHILL.md 的 touchsim / fb.dump 方法操作和抓屏
```

它会：切换跑 6 秒出结果；切往第 4 张模拟失败；换个 token 参数重启 = 模拟 agent 重启；
直接 `curl -H "Authorization: Bearer mocktoken" -d '{"iccid":"…"}' 127.0.0.1:9091/api/esim/switch`
= 模拟网页端在操作。测完**删掉 `esim.conf`**、停掉假 agent、重启 DevUI，页面才会回到真 agent。

在测试设备上照这个走过：亮起 → 确认 → 切换中计秒 → 完成、失败、网页端并发、agent 中途
重启、agent 挂掉与恢复、浅色主题；用真卡在触屏上从 CTM 切到 CMLINK：5 秒完成、没重启，
IMSI 跟着变、数据重新拨上；远程经 tailnet 断了约 1 分钟（公网出口换了）后自己连回来——
所以远程切完别急着判定失败。

## 部署

```sh
bash scripts/build.sh    # 在 x86_64 Linux 构建机上
D=/data/plugins/u60pro-devui
# 推 u60pro-devui.stripped、ui/functions/esim.html、ui/style.css，重启方式同 CHILL.md
```

前提是设备上有 u60p 的 eSIM 组件；没装也不影响别的页面，eSIM 页显示「未安装 eSIM 组件」。

**别用 `start-stop-daemon -S -b -x /bin/sh -- start.sh` 拉起 DevUI**：设备上总有别的 sh 进程，
它会报 `/bin/sh is already running` 直接不启动，前面刚 kill 掉的 DevUI 就没了。
要么加 `-m -p <pidfile>`，要么照 CHILL.md 用 nohup。
