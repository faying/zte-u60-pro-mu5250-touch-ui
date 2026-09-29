# Speed test (optional backend)

**English** · [中文](SPEEDTEST.zh-CN.md)

The touch UI does not ship a speed test program; it only shows the speed test entry when `better-speedtest` is installed on the device (`src/speedtest.c`). Remove the backend and the entry disappears automatically.

## Paths on the device

- Binary: `/data/plugins/better-speedtest/better-speedtest`
- Log: `/tmp/better-speedtest.log`
- UI preferences: `st_src` / `st_dir` / `st_dur` in `/data/plugins/u60pro-devui/devui.conf` (the UI does not modify `better-speedtest`'s own `config.json`)
- Loop test markers: `/tmp/better-speedtest.loop`, `/tmp/better-speedtest.loop.pid`

## Behavior

- Starting a test runs `better-speedtest test --json`, with arguments from the selected source (`auto` / `cnspeed` / `ookla` / `cdn`), direction (both / download / upload) and duration (10 / 15 / 20 seconds, or 0 = loop until stopped manually).
- Stopping ends the process and resets the state.

## Install and uninstall

```sh
mkdir -p /data/plugins/better-speedtest
cat > /data/plugins/better-speedtest/better-speedtest    # piped in from the computer over ssh
chmod +x /data/plugins/better-speedtest/better-speedtest
```

Uninstall: `rm -f /data/plugins/better-speedtest/better-speedtest /tmp/better-speedtest.log`.

`better-speedtest` is a separate plugin, not in this repo; `zwrt-datad` takes no part in speed tests either.
