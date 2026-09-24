# CHILL 屏幕控制页（ShellCrash / mihomo）

在 DevUI 里内建一个 ShellCrash / mihomo 控制页：查看内核状态与实时速率、切换代理组、
挑选节点、测延迟、重启内核。功能覆盖 mihomo manager 的常用项，并额外提供节点选择。

## 命名：屏幕上叫 CHILL

**设备上显示的名字统一用 CHILL**：功能页入口标题、
页面文件 `ui/functions/chill.html`、配置文件 `chill.conf`、`style.css` 里的分节注释，
源码模块也随之叫 `src/chill.c` / `include/chill.h`（符号前缀 `chill_`）。
后端仍是 ShellCrash/mihomo，所以二进制里保留 `/etc/init.d/shellcrash restart`
这一条真实服务命令——ShellCrash 自身装在 `/etc/ShellCrash`。
令牌前缀 `SC_` 与动作前缀 `act:sc*` 是缩写，保持不变，
这样只换二进制不换页面时的令牌兼容性也不受影响。

## LVGL 版的 CHILL 页

上面这些令牌和动作是 litehtml 版的。LVGL 版（默认构建）是原生页面，`src/ui.c` 的 `build_sub_chill()`：

- **状态**：当前节点、延迟、组，右上角总开关；连接数（代理 / 直连）、累计流量。
- **出口**：代理 / 全局 / 直连·AI 不动 / 全部直连，经 zte-agent 的 `PUT /api/services/chill/exit`。
- **档位**：省电 / 标准 / 性能，经 `PUT /api/services/chill/profile`（agent 调 `chill.sh profile`）。
  当前值读本机 `/tmp/chill.state` 的 `profile`、`profile_effective`、`thermal_eco`；进出「省电」要重启核心，
  连接会断大约 10 秒，页面右上角写明；设备过热、`chill.sh` 暂时按省电跑时显示「太热 · 暂按省电」。
  三档具体改什么见 manager 仓库 `scripts/chill/chill.sh` 的「档位」一节。
- **节点**、**规则 → 节点**：二级页，直接读 mihomo 的 `/group`、`/proxies`、`/connections`。

## 为什么直接读 clash API，而不是走 zwrt-datad

上游的设计原则是「UI 只读 zwrt-datad 的本机接口」。这里做了一处偏离，理由是：

- `act:` 动作是**编译进二进制**的，要做「切模式 / 选节点 / 重启内核」，无论如何都得改 DevUI；
- 数据若再放进 zwrt-datad，就要同时维护两个 fork，每次跟上游都要 rebase 两次。

既然动作那一半躲不掉，把数据也放在同一个代码库里，只需维护一个 fork。
读的仍然是**本机** HTTP 接口（`127.0.0.1:9999`），与「不直接调 ubus」的初衷一致。

```
ShellCrash/mihomo ──clash API(127.0.0.1:9999)──▶ src/chill.c ──▶ {{SC_*}} 令牌
                                                        ▲
                                        act:sc* 动作 ────┘
```

## 配置

`/data/plugins/u60pro-devui/chill.conf`，**改完不用重编**，重启 DevUI 即生效：

```
port=9999
secret=
group=🚀 节点选择
```

`group` 只是**默认**选中的组；页面上可以随时切换，切换后以页面选择为准。
若配置里的组名不存在，自动退回第一个组。

## 模板令牌

| 令牌 | 内容 |
| --- | --- |
| `{{SC_CORE}}` | 内核状态：运行中 / 已停止 |
| `{{SC_MODE}}` | 规则 / 全局 / 直连 |
| `{{SC_CHAIN}}` | **真实出口链路**，如 `OOCN → JP2-VLESS-Reality` |
| `{{SC_NODE}}` | 当前组选中项（可能是另一个组） |
| `{{SC_SPEED}}` | 实时速率 `↓1.2M/s ↑45K/s` |
| `{{SC_TRAFFIC}}` | 累计流量 `↓517M ↑8M` |
| `{{SC_CONNS}}` | 活跃连接数 |
| `{{SC_CONNSPLIT}}` | `代理 12 · 直连 8` |
| `{{SC_GROUP}}` | 当前组名（已去 emoji） |
| `{{SC_GROUPLIST}}` | 代理组切换按钮（生成的 HTML） |
| `{{SC_NODELIST}}` | 节点列表（生成的 HTML，含延迟与高亮） |
| `{{SC_GRPNOTE}}` | 自动组的只读提示 |
| `{{SC_CLS_RULE/GLOBAL/DIRECT}}` | 模式按钮高亮用的 class |
| `{{SC_RESTART_LBL/CLS}}` | 重启按钮的两段式确认文案与样式 |
| `{{SC_DELAY_LBL/CLS}}` | 测延迟按钮：进行中显示「测试中…」并变灰（`sc-busy`） |

## 动作

| 动作 | 行为 |
| --- | --- |
| `act:scmode:rule\|global\|direct` | `PATCH /configs` 切换模式 |
| `act:scgrp:<序号>` | 切换当前操作的代理组，清空节点与延迟缓存 |
| `act:scnode:<序号>` | `PUT /proxies/<组>` 选择节点 |
| `act:scdelay` | `GET /group/<组>/delay` 组测延迟，**异步**：发出请求就返回，结果到了自动填进列表；进行中再点只提示「进行中」 |
| `act:screstart` | 重启内核，**两段式确认**：首次仅亮起，4 秒内再点才执行 |

## 实现上的几个坑

**组测延迟必须异步。** 内核要等最慢的节点（`timeout=3000` ⇒ 最长 3 秒才回包），而
`sc_http()` 的读超时是 1.5 秒：同步等会在结果回来之前放弃，界面报「测试失败」——
测试设备上一半节点慢时必现（实测一轮 3.2 秒）。又不能让 UI 线程干等 3 秒，所以连上、
发完请求后把 socket 留着（非阻塞），每次 `chill_refresh()` 顺手收一点，对端关闭
（`Connection: close`）就解析；8 秒没收完放弃。换组时丢弃进行中的请求（结果属于旧组）。
日志里有 `chill: delay test started/done in N ms` 可查。

**滚动列表时误选节点。** 三个来源，都在 `htmlmain.c` 主循环里堵上了：
① 手指按下去是为了停住惯性滚动——这一整次按压不产生点击（`swallow_tap`，每次释放清零，
别残留到下一次按压）；② 惯性滚动中一次很快的点按整个落在两次轮询之间、事后从队列回放——
若回放时正在惯性滚动，只当作「停下来」；③ 释放时手指位移超过 14px 的一律不当点击
（原来的 `release_was_tap` 算了但没用上：横向拖、中途丢帧都会变成释放点上的点击）。

**`sc_http()` 返回静态缓冲区。** 下一次调用会覆盖上一次的返回内容。要用同一份响应里的
多个字段，必须在发下一个请求之前全部取完。追出口链路时会嵌套调用，最初写成「边解析边
发请求」，结果节点列表读不到、界面空白——症状出现在**别的功能**上，很难联想。

**延迟结果要跨刷新保留。** 刷新每 2 秒一次，而一轮组测延迟要好几秒。若刷新时无条件清零，
测完的结果会立刻被抹掉，界面上永远看不到。现在按原名把已有结果继承过来。

**emoji 必须净化。** 设备字体没有 emoji 字形，节点名里的 🇯🇵/🚀 会渲染成豆腐块。
国旗由两个「区域指示符」组成，正好对应 A–Z，还原成国家码字母（🇯🇵 → JP）既能显示又不丢
信息；其余 emoji 丢弃。**显示名和 API 用名分开存**——调接口必须用原名，否则服务端认不出。
国家码若与名字重复（`🇯🇵 JP2-…`）会去掉前缀，320px 的列表宽度很紧张。

**URLTest / Fallback 组不可手选。** 这类组由内核自动挑选，`PUT` 会被拒绝。页面上标灰、
节点列表转为只读，并在标题注明，避免用户白点。

**磁贴的副标题来自页面 `<head>` 里的 `<meta name="description" content="...">`**（标题来自
`<title>`），没写就显示「自定义页面」。CHILL 页写的是 `CHILL Your Network`。

**自定义页面只保留 `<body>` 内容。** 宿主会截取 `<body>` 内层，套进自己的壳
（含 `<head>`、状态栏、返回栏）。所以页面里**不要**写 `<style>`（会被丢弃，样式全失效）、
不要写 `{{STATUSBAR}}`、也不要自己加返回栏（会出现两个）。样式一律进 `ui/style.css`。

## 构建

上游未钉 litehtml 版本，而 master 已把 `on_lbutton_down` 的第 5 个参数从
`position::vector&` 改成 `std::function` 回调，直接用 master 会编译失败。
**用 `v0.10`**（撰写时最新的兼容 tag）。

```sh
# 工具链和依赖默认装在 $HOME 下（路径见 scripts/build.sh）
bash scripts/_setup_toolchain.sh   # Bootlin aarch64 musl（需 x86_64 Linux 宿主）
git -C $HOME/litehtml checkout v0.10
bash scripts/_build_freetype.sh
bash scripts/_build_litehtml.sh
bash scripts/build.sh              # -> u60pro-devui.stripped
```

## 部署

```sh
adb push u60pro-devui.stripped /data/plugins/u60pro-devui/u60pro-devui
adb push ui/functions/chill.html /data/plugins/u60pro-devui/ui/functions/
adb push ui/style.css /data/plugins/u60pro-devui/ui/
adb shell 'killall -9 u60pro-devui; nohup /data/plugins/u60pro-devui/u60pro-devui >/tmp/u60pro-devui.log 2>&1 &'
```

页面文件每次渲染都会重新读取，改 HTML/CSS **不必重启进程**；改二进制才需要。

## 用远程构建机时

交叉工具链只支持 x86_64 Linux 宿主。在另一台 Linux 机器上编译时：

**方向是单向的：只能仓库 → 构建机，永远不从构建机往回拿代码。**
构建机上的工作目录视为随时可删除重建，不在那里编辑源码。这个不对称有理由：

```
仓库 → 构建机   只丢构建产物（可重新生成），无损
构建机 → 仓库   可能丢掉仓库里的提交，且没有记录
```

曾经因为没守住这条，同一份代码在「本地仓库 / 构建机 / 三台设备」四处各不相同，
出现过**代码里修好了、设备上跑的却是修复前的产物**——表现成「改了没生效」，
很容易误判成修复本身有问题，然后越改越乱。

### 两个坑

* `scripts/build.sh` 里工具链路径写死成 `$HOME/aarch64--musl--stable-2025.08-1`，
  工具链装在别的目录时要把 `HOME` 设成那个目录，否则会报 `toolchain missing`。
* 怀疑「设备上跑的不是最新代码」时的判据：先比 md5，再看源码与产物的时间戳，
  最后**重新编一次看能不能复现出同样的产物**——能复现就说明构建侧没问题，
  问题在部署。

## 远程验证（不用凑到屏幕前）

* **模拟触摸**：`src/touchsim.c` 往 evdev 节点写事件，用工具链单独编：
  `aarch64-linux-gcc -static -Os -o touchsim src/touchsim.c`，推到 `/tmp/touchsim`，
  `/tmp/touchsim auto tap X Y` / `auto swipe X0 Y0 X1 Y1 MS`。**坐标是面板原始坐标**：
  U60 倒装 180°，截图里看到的 `(x,y)` 要换成 `(319-x, 479-y)` 再送进去。
* **抓屏**：`touch /tmp/u60-dumpfb` 后每次 `render()` 会把整屏 RGB565 写到 `/tmp/fb.dump`
  （已转正，不用再旋转）。滚动走的是位块传送不经过 `render()`，要等滚动停下重绘后才更新。
  **验完删掉 `/tmp/u60-dumpfb`**，否则每次重绘都写 300KB。
* **先看背光**：`/sys/class/leds/led:lcd/brightness` 为 0 就是熄屏，**熄屏时所有触摸被丢弃、
  只有电源键能唤醒**，模拟触摸看起来像「没反应」。自动熄屏由 `devui.conf` 的 `autooff`
  决定（有的设备是 30 秒），一串操作要在一次 ssh 会话里做完。用 shell 注入电源键短按：

  ```sh
  # struct input_event（aarch64 共 24 字节）：16 字节时间戳 + type(2) + code(2) + value(4)
  # EV_KEY=1, KEY_POWER=116(\164)；按下 → SYN → 150ms → 抬起 → SYN
  { printf '\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\001\0\164\0\001\0\0\0'
    printf '\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0'; } > /dev/input/event0
  sleep 0.15
  { printf '\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\001\0\164\0\0\0\0\0'
    printf '\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0'; } > /dev/input/event0
  ```
  短按是「切换」，先确认背光为 0 再注入，否则会把亮着的屏幕关掉。
* 验证节点选择有没有被误触：对照 clash API
  `curl 127.0.0.1:9999/proxies/<urlencode 组名>` 里的 `now`，改了再 `PUT {"name":...}` 恢复。

## 部署到设备

```sh
D=/data/plugins/u60pro-devui
scp u60pro-devui.stripped root@<设备>:$D/u60pro-devui.new     # dropbear 在 2222
ssh -p 2222 root@<设备> "
  md5sum $D/u60pro-devui.new                 # 与本地核对后再就位
  mv $D/u60pro-devui.new $D/u60pro-devui
  chmod 755 $D/u60pro-devui
  kill \$(pgrep u60pro-devui | head -1); sleep 3
  nohup sh $D/start.sh >/tmp/u60pro-restart.log 2>&1 &
"
```

换二进制而不换 `ui/` 时，先验 token 兼容：

```sh
grep -oE '"SC_[A-Z_]+"' src/htmlmain.c | tr -d '"' | sort -u          # 代码产出的
grep -oE '\{\{SC_[A-Z_]+\}\}' ui/functions/chill.html | tr -d '{}' | sort -u
```

设备 html 用到的必须是代码产出的子集；反过来代码多产出几个无害。
