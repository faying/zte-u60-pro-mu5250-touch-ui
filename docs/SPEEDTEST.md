# Optional speedtest integration

Documents the optional `better-speedtest` integration in `u60pro-devui`.

## Current model

- DevUI does not bundle the `better-speedtest` binary; it ships a second-level template at `ui/subpages/speedtest.html`.
- When `/data/plugins/better-speedtest/better-speedtest` exists and is executable, "更多功能" shows a `网络测速` tile that opens the speedtest page (not a new top-level swipe page). Removing the backend hides the tile automatically.
- Locked preview mode reuses the first page but hides speedtest controls and native widgets.
- The old standalone `07-speedtest.html` swipe page has been removed; do not ship or install it.

## Runtime paths

- Binary: `/data/plugins/better-speedtest/better-speedtest`
- Log: `/tmp/better-speedtest.log`
- DevUI config: `/data/plugins/u60pro-devui/devui.conf` — stores only the `st_src`/`st_dir`/`st_dur` UI preferences; DevUI never rewrites `better-speedtest`'s own `config.json`.
- Loop flag / pid: `/tmp/better-speedtest.loop`, `/tmp/better-speedtest.loop.pid`

## Lock screen behavior

Screen-lock preview (`g_lock_state == 1`) keeps the first page as a read-only overview but must not expose speedtest controls:

- The speedtest tile is unreachable from locked preview; native gauge/chart drawing is skipped.
- `speedtest_poll()` keeps running so backend detection and any in-progress/finished test state stay fresh after unlock.

## Actions

- `act:stpage`: open `ui/subpages/speedtest.html` when the backend is available.
- `act:ststart`: run `better-speedtest test --json` with the selected source/direction/duration.
- `act:ststop`: kill the running process and reset speedtest state.
- `act:stsrc:<mode>`: select source — `auto`, `cnspeed`, `ookla`, or `cdn`.
- `act:stdir:<mode>`: select direction — `both`, `dl`, or `ul`.
- `act:stdur:<sec>`: select duration — `10`, `15`, `20`, or `0` (loop until the user taps stop).
- `act:sttoggle` and `act:stclose` may still be accepted for older-template compatibility; current UI should use the second-level page.

## Rendering notes

Speedtest visuals are hybrid-rendered: HTML/CSS lays out the second-level page, option buttons, loop warning, and chart/gauge placeholders; native drawing in `htmlmain.c` paints the circular gauge, pointer, live speed number, and two line charts.

- Native drawing is clipped below the 26px status bar so dragging the page can't paint over it.
- Horizontal swipe previews render pages at the current scroll position and draw native chart layers into the preview buffers, so charts don't disappear during page swipes.
- Chart under-fill follows the curve color in both dark and light themes.

## Install and uninstall

Install `better-speedtest` independently:

```sh
mkdir -p /data/plugins/better-speedtest
cp better-speedtest /data/plugins/better-speedtest/better-speedtest
chmod +x /data/plugins/better-speedtest/better-speedtest
```

No extra UI file copy is required — the DevUI binary and default UI templates already contain the optional entry point; it only appears when the backend binary is present.

To uninstall:

```sh
rm -f /data/plugins/better-speedtest/better-speedtest
rm -f /tmp/better-speedtest.log
```

If an old deployment still has standalone page files, remove them so the deleted page does not reappear:

```sh
rm -f /data/plugins/u60pro-devui/ui/07-speedtest.html
rm -f /data/plugins/u60pro-devui/ui/speedtest.css
```

## Public repository boundary

The DevUI repository may document and implement the optional integration, but it should not include private device logs, local SSH targets, local screenshots, framebuffer dumps, or the `better-speedtest` binary itself.

The `zwrt-datad` repository remains unrelated to speedtest execution. The speedtest backend is an independent optional plugin under `/data/plugins/better-speedtest/`.
