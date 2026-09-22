# ZTE U60 Pro (MU5250) Touch UI

> **社区项目，与中兴（ZTE）无关联。** 基于 [33333s/u60pro-devui](https://github.com/33333s/u60pro-devui)（MIT），
> 加上 Wei REN 后续的 CHILL 面板、eSIM 切换页、Tailscale 卡片和 LVGL 重写。本仓库从清理后的快照开始，
> 不含原始提交历史；来源和删减内容见 [NOTICE](NOTICE)。管理网页与设备端 API 在配套仓库
> [zte-u60-pro-mu5250-manager](https://github.com/faying/zte-u60-pro-mu5250-manager)。

ZTE U60 Pro（MU5250）及同系 SDX 5G MiFi 设备前面板屏幕 UI 的 clean-room 开源替代实现，跑在标准
Linux 的 **DRM/KMS** + **evdev** 之上，编译成单个静态 aarch64 二进制，独立于原厂 UI 运行。

```text
后端 zwrt-datad ──▶ HTTP /state + SSE /events (127.0.0.1:9460) ──▶ u60pro-devui ──▶ 屏幕（DRM/KMS）
```

## 两套渲染器

仓库里现在共存两套实现，`src/` 下同一批数据/业务模块（`chill.c`/`esim.c`/`tailscale.c`/`speedtest.c`
等）分别接到两个前端：

| | litehtml（原版） | LVGL（重写，默认构建） |
|---|---|---|
| 入口 | `src/htmlmain.c` + `src/devui_ext.c` | `src/main.c` + `src/ui.c` |
| 界面来源 | `/data/plugins/u60pro-devui/ui/*.html`，**不用重编译，改 HTML 即生效** | 原生 C 布局，改界面需要重新编译 |
| 排版 | [litehtml](https://github.com/litehtml/litehtml)（BSD）+ FreeType | [LVGL](https://github.com/lvgl/lvgl) v9.5（MIT）+ FreeType |
| 构建 | `bash scripts/build.sh`（本机 Bootlin 工具链，见下） | `make`（默认目标，Docker 见下） |

**如果你只想改界面文字/样式**，用 litehtml 版：跳到 [自定义界面](#自定义界面litehtml-版) 一节，改
HTML 推到设备上就生效，不用编译。**如果你想改交互逻辑、加原生动画或对接新数据**，两边都要碰代码；
LVGL 版是当前主线开发方向，细节和踩坑记录在 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)。

## 构建

### LVGL 版（默认，推荐用 Docker）

```sh
docker build --platform linux/amd64 -t u60-devui-build -f Dockerfile.build .
docker run --rm --platform linux/amd64 -v "$(pwd)":/src -w /src u60-devui-build \
  bash -c 'git clone --depth 1 --branch v9.5.0 https://github.com/lvgl/lvgl.git third_party/lvgl; make'
```

产物：`./u60pro-devui`（静态 aarch64 ELF）。本机没有交叉工具链也能跑，Dockerfile 里已经固化了
Bootlin 工具链和 FreeType 版本。

### litehtml 版（本机工具链）

```sh
bash scripts/_setup_toolchain.sh   # 一次性：Bootlin aarch64 musl 工具链
bash scripts/_build_freetype.sh    # 一次性：静态 FreeType
bash scripts/_build_litehtml.sh    # 一次性：静态 litehtml
bash scripts/build.sh              # -> ./u60pro-devui(.stripped)
```

需要 POSIX shell（WSL / Linux / Git-Bash），不需要 root。也可以到
[Releases](https://github.com/33333s/u60pro-devui/releases) 下载上游编译好的二进制。

## 在设备上运行

```sh
adb shell 'mkdir -p /data/plugins/u60pro-devui/ui'
adb push ui/*.html ui/*.css /data/plugins/u60pro-devui/ui/        # 仅 litehtml 版需要
adb push u60pro-devui.stripped /data/plugins/u60pro-devui/u60pro-devui
adb shell '/etc/init.d/zte_topsw_devui stop; sleep 1;
           chmod 755 /data/plugins/u60pro-devui/u60pro-devui;
           nohup /data/plugins/u60pro-devui/u60pro-devui >/tmp/devui.log 2>&1 &'
```

开机自启：把二进制放 `/data/plugins/u60pro-devui/`、后端 `zwrt-datad` 放
`/data/plugins/zwrt-datad/`，再跑 `scripts/install-autostart.sh` —— 保留原厂 `zte_topsw_devui`
做早期屏幕/触摸 bring-up，`rc.local -> start.sh legacy` 晚接管。细节见
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)。

> **新构建先旁路文件名验证，稳定后再换正式位置**——见 manager 仓库 CLAUDE.md 的部署安全说明；
> 一个启动即崩溃的构建直接覆盖正式位置，会把整机拖进重启循环。

## 自定义界面（litehtml 版）

程序本身不内置画面，运行时读 `/data/plugins/u60pro-devui/ui` 下的 HTML/CSS 渲染到屏幕。改 HTML、
推到设备，约 1 秒内自动生效，不用重新编译。教程见 [docs/UI-GUIDE.md](docs/UI-GUIDE.md)。

`ui/` 下每个顶层 `NN-名字.html` 一页，`style.css` 共享样式，`ui/subpages/` 是二级页面，
`ui/functions/` 放自定义功能页。HTML 里 `{{令牌}}` 由程序替换成实时数据，`href="act:xxx"` 触发交互。
自带四个顶层页：信号、更多功能、图表、系统设置。

## 功能

- **CHILL**：内建 ShellCrash / mihomo 控制页——内核状态、实时速率、代理组切换、选节点、测延迟、
  重启内核。命名和原因见 [docs/CHILL.md](docs/CHILL.md)（屏幕上统一叫 CHILL，不叫 RELAX）。
- **eSIM**：通过 lpac 管理设备内 eUICC 卡的 profile。
- **Tailscale**：状态卡片和开关。
- **测速**：可选后端，支持循环测速。
- **corner-wake**：屏幕熄灭时角落轻触唤醒的辅助进程，识别 `/proc/<pid>/comm` 时对 devui 进程名做前缀匹配（兼容旁路测试文件名）。

## 文档

- [docs/UI-GUIDE.md](docs/UI-GUIDE.md) — litehtml 版自定义界面教程
- [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) — 架构、构建、数据模型、两套渲染器的踩坑记录
- [docs/CHILL.md](docs/CHILL.md) — CHILL 面板设计与命名
- [docs/SIGNAL-CARDS.md](docs/SIGNAL-CARDS.md) / [docs/modem.md](docs/modem.md) — 信号页与信令页字段口径
- [docs/SPEEDTEST.md](docs/SPEEDTEST.md) / [docs/ESIM.md](docs/ESIM.md) / [docs/DEVUI-IPC.md](docs/DEVUI-IPC.md)
- [docs/HARDWARE.md](docs/HARDWARE.md) — 设备硬件接口
- [CHANGELOG.md](CHANGELOG.md) — 更新日志

后端 `zwrt-datad`（轮询 `ubus`，提供 `GET /state` + SSE `/events`）: [33333s/zwrt-datad](https://github.com/33333s/zwrt-datad)。

## 许可证

[MIT](LICENSE)。litehtml（BSD）、LVGL（MIT）、FreeType（FTL/GPL 双授权）、stb（public domain）按各自
许可证引入。仓库不打包任何 ZTE 字体（运行时从设备加载），也不包含 vendor blobs。
