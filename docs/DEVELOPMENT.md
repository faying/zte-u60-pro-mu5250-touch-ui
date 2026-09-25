# 开发说明

当前的触屏界面是 **LVGL 版**（`make` 编出来的那个）。仓库里还留着旧的 litehtml 版（`src/htmlmain.c`、`ui/*.html`、`scripts/build.sh`），
它不再维护，装机包也不收，下面不再讲它。

## 代码结构

| 文件 | 作用 |
|---|---|
| `src/main.c` | 入口：初始化 DRM、触摸、按键、背光，主循环 |
| `src/ui.c`、`src/ui_kit.c`、`src/ui_theme.c` | 5 个标签和各子页面的布局、通用控件、浅色/深色主题和字体加载 |
| `src/ui_logic.c`、`src/ui_exec.c` | 不依赖 LVGL 的判断逻辑和「点了以后做什么」，可单独测试 |
| `src/data.c`、`src/json.c` | 从 `zwrt-datad`（`127.0.0.1:9460` 的 `/state` + `/events`）读数据 |
| `src/esim.c`、`src/tailscale.c`、`src/speedtest.c`、`src/netinfo.c`、`src/scenario.c`、`src/alerts.c` | 各功能的后端：eSIM / APN 等写操作走 zte-agent（`127.0.0.1:9090`） |
| `src/drm_disp.c`、`src/touch_input.c`、`src/key_input.c`、`src/backlight.c` | 硬件接口，见 [HARDWARE.md](HARDWARE.md) |
| `src/uid.c`、`src/uid_core.c` | 屏幕守护进程 `u60-uid`（判断逻辑在 `uid_core.c`，可单独测试） |
| `include/devui_config.h` | 端口、路径等编译期常量 |
| `patches/` | 给 LVGL v9.5.0 的小补丁（FreeType 位图模式的合成粗体），`make` 时自动打 |
| `scripts/` | 设备端脚本：`supervise.sh` 和 `*.init`（procd 监督）、`u60-guard.sh`（Wi-Fi 兜底 + 告警短信）、`alert-lib.sh`、`doctor.sh`、`config-backup.sh`、`agent-auth.sh`、`chaos.sh` 等 |

界面的设计规则（层级、颜色、点击反馈、确认方式）在 manager 仓库 `docs/DESIGN.md` §4。

## 构建

见 [README](../README.md#构建)。要点：

- 用 `Dockerfile.build` 的镜像；`HOME=/opt bash scripts/_build_freetype.sh` 在镜像里把 FreeType 编到 `/opt/freetype-musl`（Makefile 的默认 `FT_DIR`）。
- `make CROSS_COMPILE=aarch64-linux-` 编界面，`make CROSS_COMPILE=aarch64-linux- u60-uid` 编守护进程。
- LVGL 不在仓库里，`make` 找不到时会告诉你 clone 哪个版本。

## 字体

加载顺序在 `src/ui_theme.c` 的 `ui_fonts_load()`：

| 用途 | 首选 | 找不到时 |
|---|---|---|
| 中文和正文 | 设备自带 `/usr/ui/fonts/ZTEZhengYuan.ttf`（运行时加载，仓库不带） | `/data/plugins/u60pro-devui/fonts/u60-cjk-fallback.ttf` → LVGL 自带 Montserrat（没有中文） |
| 数字 | `fonts/Nunito-600/700/800.ttf`（OFL） | 设备自带 Roboto → 中文字体 |

粗体都是合成的。启动日志里 `ui: fonts cjk=… numerals=…` 说明实际用了哪套。
中文兜底字体由 `scripts/fonts/build-cjk-fallback.sh <输出目录>` 从 Resource Han Rounded（OFL）裁出来。
字体文件都不进仓库，由装机包（`DEVUI_FONTS_DIR`）带到设备。

## 测试

```sh
scripts/test/docker.sh                 # 设备端 shell 脚本，busybox 容器里跑，命令全部打桩
make CROSS_COMPILE=aarch64-linux- uid-test ui-logic-test ui-exec-test   # 纯逻辑单元测试（静态程序，产物在 scripts/test/*/，由 docker.sh 运行）
scripts/test/render/build.sh           # 离屏渲染测试：每个场景 × 浅色/深色
U60_DEVICE_FONTS=<有 ZTEZhengYuan.ttf、Roboto.ttf 的目录> U60_NUNITO_DIR=<Nunito 目录> \
  scripts/test/render/render.sh [--png 输出目录] [场景…]
```

渲染测试会检查每个字符有没有字形、每一页的像素是否和 `tests/render/golden/` 一致；有意改界面后用 `--write-golden` 更新。
设备字体要从设备 `/usr/ui/fonts/` 自己拷，不进仓库。

## 设备上的布局

| 路径 | 内容 |
|---|---|
| `/data/plugins/u60pro-devui/u60pro-devui` | 界面程序（正式位置） |
| `/data/plugins/u60pro-devui/u60-uid`、`/etc/init.d/u60-uid` | 屏幕守护进程 |
| `/data/plugins/u60pro-devui/fonts/` | Nunito 和中文兜底字体 |
| `/data/plugins/zwrt-datad/` | 数据服务 |
| `/data/u60-guard/`、`/etc/init.d/{zte-agent,zwrt-datad,u60-guard}` | 监督和看门狗脚本 |

开机自启只走 `/etc/rc.local` 里的 `/etc/init.d/<名字> start`，不用 `enable`。

`u60-uid` 的控制：`echo vendor > /tmp/u60-uid.ctl` 把屏幕交给原厂界面，`echo devui > /tmp/u60-uid.ctl` 换回来。
它连续两次拉不起界面会放弃并交还原厂界面，计数在 `/data/u60-uid/attempts`、`/data/u60-uid/gave-up`。

## 在真机上试新版本

两条硬约束：**一启动就崩的界面会被固件升级成整机重启循环**；**屏幕上几分钟没有任何界面，固件也会整机重启**。
所以新版本先用别的文件名试跑，确认稳定后再换正式位置；两个界面也不能同时抢 `/dev/dri/card0`。

普通用户直接用装机包 `./install.sh devui`，它会做这些检查。手动试跑的流程（全程在一次 SSH 会话里连续做完）：

```sh
cd /data/plugins/u60pro-devui
cat > u60pro-devui.test && chmod 755 u60pro-devui.test     # 从电脑用 ssh 管道传进来
cp -p u60pro-devui u60pro-devui.known-good                 # 备份现在的正式版本
/etc/init.d/u60-uid stop                                   # 停守护进程，界面本身还在
kill $(pidof u60pro-devui)                                 # 停正式界面……
nohup ./u60pro-devui.test > /tmp/devui-test.log 2>&1 &     # ……紧接着起测试版本
sleep 20; pidof u60pro-devui.test && tail -n 20 /tmp/devui-test.log   # 20 秒后还活着才算过
```

- **通过**：`cp u60pro-devui.test u60pro-devui.tmp && mv u60pro-devui.tmp u60pro-devui`（原子替换），
  然后立刻 `kill $(pidof u60pro-devui.test)`、`rm -f /data/u60-uid/attempts /data/u60-uid/gave-up`、`/etc/init.d/u60-uid start`。
- **没通过**：正式位置还是旧版本，直接 `/etc/init.d/u60-uid start` 让它拉起旧版本。

注意：

- 用 `pidof`（按进程名）找测试进程，别用 `pgrep -f`：后者会匹配到这条 SSH 命令自己的 shell，把它杀掉，后面的步骤就不跑了，屏幕会一直空着。
- `u60-uid` 不在跑的时候别往 `/tmp/u60-uid.ctl` 写东西，会卡住。
- 10 分钟内 `u60-uid start` 三次，它会以为是崩溃循环而放弃；所以上面要先清掉计数。
- 设备上没有 scp/sftp，传文件用 `ssh … 'cat > 路径' < 本地文件`。
