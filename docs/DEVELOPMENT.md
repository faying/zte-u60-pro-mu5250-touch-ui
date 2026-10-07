# Development notes

**English** · [中文](DEVELOPMENT.zh-CN.md)

The current touch UI is the **LVGL version** (the one `make` builds). The old litehtml version (`src/htmlmain.c`, `scripts/build.sh`) has been deleted
and is kept at git tag `legacy-litehtml`, together with its `ui/*.html` templates and docs.

## Code layout

| File | Purpose |
|---|---|
| `src/main.c` | Entry point: initializes DRM, touch, keys and backlight; main loop |
| `src/ui.c` + `src/ui_parts/*.c`, `src/ui_kit.c`, `src/ui_theme.c` | Layout of the 5 tabs and their subpages, shared widgets, light/dark themes and font loading. `ui.c` holds the shared parts (navigation, shared widgets, theme, subpage layer, status bar); each page is one file in `src/ui_parts/`, `#include`d by `ui.c` and compiled as a single unit (they share its static state and are not compiled separately) |
| `src/ui_logic.c`, `src/ui_exec.c` | Decision logic and "what happens after a tap", with no LVGL dependency; testable on their own |
| `src/data.c`, `src/json.c` | Read data from `zwrt-datad` (`/v2/state` + `/v2/events` on `127.0.0.1:9460`; the blocks are put back into the old `/state` shape for one parser) |
| `src/screen_feed.c`, `src/net_view.c` | The conclusions on the home signal card and status bar (smooth / slow: weak signal …, 5G-A / 4G+, carriers, roaming, logo) are computed by `zwrt-datad`'s `GET /v2/screen`; this code reads and parses it once per new snapshot. The rules themselves live in data-service `rust/src/project/screen.rs`; the C code no longer makes these decisions |
| `src/op_view.c`, `src/ui_parts/op.c` | Device writes in progress and their results (E4 write transactions): `zwrt-datad` sends them in `/v2/screen`'s `op` with every sentence in both languages; `op_view.c` parses it, `op.c` draws the transaction row under the title bar, the transaction page (revert / keep, got it, retry, restart) and hands the network-mode line datad's verdict. Writes themselves go through `data_control()` in `src/data.c`; the emergency script `scripts/u60-fallback.sh` runs only when datad cannot be reached |
| `src/http.c`, `src/agent_client.c` | Shared HTTP client (timeouts, chunked decoding, no SIGPIPE) and authenticated zte-agent requests (password, token, one re-login on 401); every feature module goes through it, except `data.c`, which has its own because of the stream semantics of `/v2/events` and `/control` |
| `src/esim.c`, `src/tailscale.c`, `src/speedtest.c`, `src/netinfo.c`, `src/scenario.c`, `src/alerts.c` | Feature backends: eSIM / APN and other write operations go through zte-agent (`127.0.0.1:9090`) |
| `src/drm_disp.c`, `src/touch_input.c`, `src/key_input.c`, `src/backlight.c` | Hardware interfaces, see [HARDWARE.md](HARDWARE.md) |
| `src/uid.c`, `src/uid_core.c` | Screen daemon `u60-uid` (decision logic in `uid_core.c`, testable on its own) |
| `include/devui_config.h` | Compile-time constants such as ports and paths |
| `patches/` | Small patch for LVGL v9.5.0 (synthetic bold in FreeType bitmap mode), applied automatically by `make` |
| `scripts/` | Device-side scripts: `supervise.sh` and `*.init` (procd supervision), `u60-guard.sh` (Wi-Fi fallback + alert SMS), `alert-lib.sh`, `doctor.sh`, `config-backup.sh`, `agent-auth.sh`, `chaos.sh`, etc. |

The UI design rules (hierarchy, color, tap feedback, confirmation style) are in the manager repo's `docs/DESIGN.md` §4.

## Build

See [README](../README.md#build). Key points:

- Use the image from `Dockerfile.build`; inside it, `HOME=/opt bash scripts/_build_freetype.sh` builds FreeType into `/opt/freetype-musl` (the Makefile's default `FT_DIR`).
- `make CROSS_COMPILE=aarch64-linux-` builds the UI; `make CROSS_COMPILE=aarch64-linux- u60-uid` builds the daemon.
- LVGL is not in the repo; if `make` can't find it, it tells you which version to clone.

## Fonts

The load order is in `ui_fonts_load()` in `src/ui_theme.c`:

| Use | First choice | Fallback |
|---|---|---|
| Chinese and body text | The device's own `/usr/ui/fonts/ZTEZhengYuan.ttf` (loaded at runtime, not in the repo) | `/data/plugins/u60pro-devui/fonts/u60-cjk-fallback.ttf` → LVGL's built-in Montserrat (no Chinese) |
| Numerals | `fonts/Nunito-600/700/800.ttf` (OFL) | The device's own Roboto → the Chinese font |

All bold weights are synthetic. The startup log line `ui: fonts cjk=… numerals=…` shows which set was actually used.
The Chinese fallback font is cut from Resource Han Rounded (OFL) by `scripts/fonts/build-cjk-fallback.sh <output dir>`.
No font files go into the repo; the install kit (`DEVUI_FONTS_DIR`) carries them to the device.

## Tests

```sh
scripts/test/docker.sh                 # device-side shell scripts, run in a busybox container with every command stubbed
make CROSS_COMPILE=aarch64-linux- uid-test ui-logic-test ui-exec-test   # pure-logic unit tests (static binaries, output in scripts/test/*/, run by docker.sh)
scripts/test/render/build.sh           # off-screen render tests: every scene × light/dark
U60_DEVICE_FONTS=<dir with ZTEZhengYuan.ttf and Roboto.ttf> U60_NUNITO_DIR=<Nunito dir> \
  scripts/test/render/render.sh [--png output-dir] [scene…]
```

The render tests check that every character has a glyph and that every page matches `tests/render/golden/` pixel for pixel; after an intentional UI change, update them with `--write-golden`.
Copy the device fonts from `/usr/ui/fonts/` on the device yourself; they don't go into the repo.

## Layout on the device

| Path | Contents |
|---|---|
| `/data/plugins/u60pro-devui/u60pro-devui` | UI binary (live slot) |
| `/data/plugins/u60pro-devui/u60-uid`, `/etc/init.d/u60-uid` | Screen daemon |
| `/data/plugins/u60pro-devui/fonts/` | Nunito and the Chinese fallback font |
| `/data/plugins/zwrt-datad/` | Data service |
| `/data/u60-guard/`, `/etc/init.d/{zte-agent,zwrt-datad,u60-guard}` | Supervision and watchdog scripts |

Autostart goes only through `/etc/init.d/<name> start` lines in `/etc/rc.local`; `enable` is not used.

Controlling `u60-uid`: `echo vendor > /tmp/u60-uid.ctl` hands the screen to the stock UI, `echo devui > /tmp/u60-uid.ctl` takes it back.
If it fails to start the UI twice in a row, it gives up and hands the screen back to the stock UI; the counters are in `/data/u60-uid/attempts` and `/data/u60-uid/gave-up`.

## Trying a new build on a real device

Two hard constraints: **a UI that crashes on startup gets escalated by the firmware into a whole-device reboot loop**; **if the screen has no UI at all for a few minutes, the firmware also reboots the whole device**.
So try a new build under a different file name first and only move it to the live slot once it is stable; two UIs also must not fight over `/dev/dri/card0`.

Regular users should just use the install kit's `./install.sh devui`, which does these checks. The manual trial procedure (do it all in one SSH session, without breaks):

```sh
cd /data/plugins/u60pro-devui
cat > u60pro-devui.test && chmod 755 u60pro-devui.test     # piped in from the computer over ssh
cp -p u60pro-devui u60pro-devui.known-good                 # back up the current live version
/etc/init.d/u60-uid stop                                   # stop the daemon; the UI itself keeps running
kill $(pidof u60pro-devui)                                 # stop the live UI...
nohup ./u60pro-devui.test > /tmp/devui-test.log 2>&1 &     # ...and immediately start the test build
sleep 20; pidof u60pro-devui.test && tail -n 20 /tmp/devui-test.log   # it passes only if still alive after 20 seconds
```

- **Pass**: `cp u60pro-devui.test u60pro-devui.tmp && mv u60pro-devui.tmp u60pro-devui` (atomic replace),
  then immediately `kill $(pidof u60pro-devui.test)`, `rm -f /data/u60-uid/attempts /data/u60-uid/gave-up`, `/etc/init.d/u60-uid start`.
- **Fail**: the live slot still holds the old version; just run `/etc/init.d/u60-uid start` to have it start the old version.

Notes:

- Find the test process with `pidof` (by process name), not `pgrep -f`: the latter matches the shell of this SSH command itself and kills it, so the remaining steps never run and the screen stays blank.
- Don't write to `/tmp/u60-uid.ctl` while `u60-uid` is not running; it will hang.
- If `u60-uid start` runs three times within 10 minutes, it assumes a crash loop and gives up; that's why the counters are cleared above.
- The device has no scp/sftp; transfer files with `ssh … 'cat > path' < local-file`.
