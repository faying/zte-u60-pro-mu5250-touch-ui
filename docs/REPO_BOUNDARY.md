# 仓库边界

这个仓库是公开的 DevUI 渲染器和 UI 模板工程。原则是保持可分享：源码、通用工具、公开文档和示例 UI 资源可以放在这里。

## 可以公开

- `src/`、`include/`、`scripts/`、`ui/` 下的渲染器源码、头文件、脚本和 UI 模板。
- 描述架构、UI 令牌、硬件假设和通用开发行为的公开文档。
- 不内置私有主机、密码、密钥路径或实机日志的通用辅助工具。

## 仅本地保留

下面这些内容不进入 git，并由 `.gitignore` 忽略：

- `.local-notes/`：私有 workflow、现场调试记录和上下文接续笔记。
- `agents.md`、`docs/LOCAL-CHANGES.md`、`docs/WORKFLOW-*.md`。
- 本地 SSH 目标、私钥路径、密码、编译机别名和实机日志。
- 预览图、framebuffer dump、临时二进制和 release 打包中间产物。

## 后端边界

DevUI 可以显示 modem/信令数据，但后端抓取、解码和回放逻辑属于配套 `zwrt-datad` 仓库。DevUI 文档只描述页面行为、展示字段和前端兜底规则。
