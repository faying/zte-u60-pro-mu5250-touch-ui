# Speed test

**English** · [中文](SPEEDTEST.zh-CN.md)

The touch UI's speed test page drives zte-agent's own speed test engine (manager repo, `zte-agent/src/speedtest.rs`, the same one the admin web page uses); the screen side is `src/speedtest.c` and `src/ui_parts/speedtest.c`. Nothing extra is installed on the device.

- Start / stop: `POST /api/speedtest/start`, `POST /api/speedtest/stop` (zte-agent runs the test in a background thread).
- Progress: `GET /api/speedtest/progress`, polled about once a second while the page is open.
- Servers: `GET /api/speedtest/servers` (zte-agent caches the list for 5 minutes); the first row is 自动 (zte-agent picks the best server).
- Every request carries zte-agent's Bearer token (`src/agent_client.c`). When zte-agent cannot be reached, the page says so instead of starting.

The old optional `better-speedtest` plugin (litehtml era) was never installed on real devices and is no longer supported; see tag `legacy-litehtml`.
