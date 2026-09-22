# 信号卡片与锁屏预览

收口 `u60pro-devui` 第一页信号卡片的当前行为。后端抓取、modem 解码、回放和 raw log 归
`zwrt-datad` 维护；这里只记录 DevUI 如何消费已经归一化的值并渲染到屏幕。

## 页面入口

- `ui/01-signal.html` 用 `{{SIGNALCARDS}}` 作为默认第一页的完整信号区。
- `{{NEIGHBORCARDS}}` 紧随其后，作为可展开的邻小区列表入口。
- `{{TAILSCALECARD}}` 放在邻小区后面，是 Tailscale 状态卡片（见下文）；设备上没有
  tailscaled 时为空串。
- `{{CARRIERS}}` 保留给旧自定义模板，只展开载波列表。
- 测速入口已迁移到"更多功能"二级页；首页不再直接放 `{{STHOMEBTN}}` 或 `{{STHOMEINLINE}}`。

## 载波数据归属

- PCell / SCC 身份仍以 ZTE/datad 归一化状态为准，例如 `net.nr_band`、`net.nr_bw`、
  `net.nrca`、`net.lteca`、`net.ltecasig`。
- 信令解析出来的 modem 指标只作为附加行追加到既有载波卡片下面，不能替换原本的 SCC
  数据来源。
- NR 附加指标可显示 MIMO、layers、DL/UL grants、DL/UL RB、BLER、SSB、ML1
  RSRP/RSRQ/SINR。LTE 附加指标可显示 MCS/modulation、layers、grants、RB、BLER、
  ML1 RSRP/RSRQ/SINR。
- 没有新鲜可信值时显示 `-`；不要在不同载波之间编造或串用旧值。

## 邻小区

- 邻小区来自 `zwrt-datad` 的 `/modem/signal-metrics` 中 `lte.neighbors` 和
  `nr.neighbors` 数组。
- 默认首页只显示一张"邻小区"入口卡，点击后展开列表；再次点击"收起"折叠。
- 每个 LTE/NR 邻区一行，从左到右固定显示 `PCI / 频段 / RSRP / RSRQ`。
- 信令解析开关关闭时，`{{NEIGHBORCARDS}}` 返回空串，入口和展开卡片都隐藏。
- 邻小区仅作为附加观测，不替换 PCell/SCC 的原始数据来源。

## 未激活载波

- RSRP 小于等于 floor sentinel `-140` 的载波视为"已配置但未激活"。
- 未激活卡片仍保留正常的频段、频宽、EARFCN/ARFCN、PCI 展示；只有信号值走 `.q-off`，
  并增加"未激活"标签——这是状态提示，不代表"信号极差"。

## 高铁专网标签

- `net.HSR=true` 是 datad 根据信令确认的高铁模式，会让第一页主信号卡片进入紫色高铁模式。
- 单载波上的"高铁专网"标签只是白名单提示：仅当 `mcc == 460` 且 LTE/NR 的
  EARFCN/ARFCN 命中维护的白名单时显示。它不是 `net.HSR` 的真值来源，两者不要混用。
- 高铁载波卡片应保留紫色边框和左侧强调线。圆角露出方形背景时，应修渲染器的
  `border-radius` 裁切或卡片 overflow，不要删掉紫色边框来规避。

## 标签布局

- `.coff` 渲染"未激活"，`.chsr` 渲染"高铁专网"。两个标签都是紧凑的 `inline-block`，
  真机字体下文字需要视觉上移约 1px 才更接近上下居中。
- 如果以后继续调标签，优先真机截图验证；桌面浏览器的字体盒模型不能代表 litehtml +
  设备 CJK 字体的最终效果。

## Tailscale 卡片

- 数据来自 tailscaled 的 LocalAPI：unix socket 上 `GET /localapi/v0/status`（HTTP/1.0，
  `Host: local-tailscaled.sock`）。不 exec `tailscale` CLI——拉起一次 Go 程序比请求本身
  贵几个数量级；LocalAPI 实测 2ms、9 台设备约 14KB。实现在 `src/tailscale.c`。
- socket 依次探测 `/tmp/tailscaled.sock`（u60p `tailscale-start.sh` 用的）和官方默认
  `/var/run/tailscale/tailscaled.sock`；每次读都重新探测，因为开机时 DevUI 往往比
  tailscaled 先起来。都不存在就不出卡片；socket 在但没响应显示"未运行"，退到 15 秒一次。
- 只在首页亮着显示时读（含锁屏预览），5 秒一次；刚切到首页立刻读。请求在主循环的
  `tailscale_poll()` 里发，令牌只读缓存。
- 字段口径：
  - 状态：`BackendState`；`Running` 但 `Self.Online=false`（控制服务器认为离线）显示"离线"。
    颜色复用 `.q-good/.q-mid/.q-bad/.q-off`。
  - 标题副标题是 `Self.DNSName` 第一段（控制台里的机器名），不是 `HostName`。
  - 子网路由用 `Self.PrimaryRoutes`：本机**实际在转发**的（已批准），不是申请的。
  - 活跃连接：`Peer[*].Active=true` 的设备数；其中 `CurAddr` 非空算直连，空的算走 DERP 中继。
  - 出口节点只在有 `Peer[*].ExitNode=true` 时出现；健康警告取 `Health` 第一条。
- `Peer` 是对象的对象，`json_get` 只认第一层键且会一直往后扫，所以逐台设备临时截断再取字段。

## 锁屏预览

- 锁屏预览态（`g_lock_state == 1`）复用第一页 `g_pages[0]`，并在屏幕下方画原生锁图标。
- 预览态是只读概览：滑动进入 PIN 键盘，普通页面动作不应该暴露为可用控件。
- 预览态隐藏可选测速 UI；测速二级页入口和 native gauge/chart 都不暴露。
- 预览态的 Tailscale 卡片只露状态和在线设备数，机器名、地址、子网、活跃连接、健康警告都藏起来。
- `speedtest_poll()` 保持运行，这样解锁后仍能立即显示测速后端是否存在以及正在测速/已完成的状态。

## 真机预览标签

做视觉 QA 时，可以临时把 `01-signal.html` 换成只包含 `.card`、`.ccd`、`.coff`、`.chsr`
的最小预览页。检查完必须恢复正式模板。

```sh
cp /data/plugins/u60pro-devui/ui/01-signal.html \
   /data/plugins/u60pro-devui/ui/01-signal.html.bak-label-preview
# 在这里推送临时预览版 01-signal.html
touch /tmp/u60-dumpfb
killall -9 u60pro-devui
nohup sh /data/plugins/u60pro-devui/start.sh legacy >/tmp/u60pro-boot.log 2>&1 &

# 检查 framebuffer 后恢复
mv /data/plugins/u60pro-devui/ui/01-signal.html.bak-label-preview \
   /data/plugins/u60pro-devui/ui/01-signal.html
rm -f /tmp/u60-dumpfb /tmp/u60-force-hsr
killall -9 u60pro-devui
nohup sh /data/plugins/u60pro-devui/start.sh legacy >/tmp/u60pro-boot.log 2>&1 &
```

Framebuffer dump、截图和临时二进制都是本地 QA 产物，不能提交。
