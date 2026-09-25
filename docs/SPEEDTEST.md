# 测速（可选后端）

触屏界面不带测速程序，只在设备上装了 `better-speedtest` 时显示测速入口（`src/speedtest.c`）。删掉后端，入口自动消失。

## 设备上的路径

- 程序：`/data/plugins/better-speedtest/better-speedtest`
- 日志：`/tmp/better-speedtest.log`
- 界面偏好：`/data/plugins/u60pro-devui/devui.conf` 里的 `st_src` / `st_dir` / `st_dur`（界面不改 `better-speedtest` 自己的 `config.json`）
- 循环测速标记：`/tmp/better-speedtest.loop`、`/tmp/better-speedtest.loop.pid`

## 行为

- 开始测速运行 `better-speedtest test --json`，参数来自所选的来源（`auto` / `cnspeed` / `ookla` / `cdn`）、方向（双向 / 下行 / 上行）和时长（10 / 15 / 20 秒，或 0 = 循环到手动停止）。
- 停止会结束进程并重置状态。

## 安装和卸载

```sh
mkdir -p /data/plugins/better-speedtest
cat > /data/plugins/better-speedtest/better-speedtest    # 从电脑经 ssh 管道传入
chmod +x /data/plugins/better-speedtest/better-speedtest
```

卸载：`rm -f /data/plugins/better-speedtest/better-speedtest /tmp/better-speedtest.log`。

`better-speedtest` 是独立的插件，不在本仓库里；`zwrt-datad` 也不参与测速。
