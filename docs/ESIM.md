# eSIM switching page

**English** · [中文](ESIM.zh-CN.md)

On the touch screen, 「蜂窝 → SIM 与 eSIM」 (Cellular → SIM & eSIM) lists the profiles on the removable eUICC card in the SIM slot (5ber, eSTK.me and similar); tap twice to switch.
**It only switches**; downloading, deleting and renaming are done in the admin web.

## Data flow: call zte-agent, don't run lpac directly

```
lpac (qmi_qrtr) ◀── zte-agent (esim.rs) ──HTTP 127.0.0.1:9090──▶ src/esim.c ──▶ UI
```

Endpoints used: `GET /api/esim/profiles`, `POST /api/esim/switch`, `GET /api/esim/job` (switch progress).

The switch itself is a single `lpac profile enable`; the hard part is getting ZTE's stack to recognize the new card: it reads the SIM only once when its daemon starts,
so the agent power-cycles the UIM, restarts `zte_topsw_mdm` and waits for the IMSI to change, usually within 10 seconds. All of this happens in the agent; the UI only shows progress and the result.

Prerequisite: the eSIM component is installed on the device (lpac in `/data/esim` + a zte-agent with `/api/esim/*`, via the install kit's `./install.sh esim`); if it's missing, the page says so.

## Login

The UI logs in to the agent with the admin password. The password is read from `ZTE_AGENT_PASSWORD` in `/data/zte-agent.env` (written by the install kit), so normally there is nothing to configure.
To override it, write `/data/plugins/u60pro-devui/esim.conf`.

## Notes

- Some cards (e.g. eSTK.me) return `catBusy` after several operations in a short time: the agent has a 5-minute cooldown, the UI shows the reason; don't keep tapping.
- If you are operating remotely over this U60's own network, don't switch to a profile with no data; once switched, you can't connect back and have to switch back on the screen in front of the device.
