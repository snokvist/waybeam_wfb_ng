# Init scripts

On-device init scripts for the patched `wfb_tx` / `wfb_rx` stack from
`poc/`. One file per platform/role:

| File | Role | Platform |
|---|---|---|
| `S99wfb` | Vehicle (broadcaster) | SigmaStar Infinity6E, OpenIPC, classic SysV `/etc/init.d/` |
| `wfb-ng.sh` | Ground station (CPE510) | TP-Link CPE510 v3, OpenWrt + procd |

## `S99wfb` — opt-in WFB-NG broadcaster mode

Toggles a vehicle between its default WiFi AP (`/usr/bin/adapter`
managing hostapd) and **WFB-NG broadcaster mode** (wlan0 in monitor,
`wfb_tx` reading the venc SHM ring, `wfb_rx` for uplink, `link_controller`
managing FEC + bitrate).

### Install

```bash
scp -O poc/init/S99wfb root@192.168.1.13:/etc/init.d/S99wfb
ssh root@192.168.1.13 'chmod +x /etc/init.d/S99wfb'
```

Order is `S99` so it runs after `S95venc` (SHM ring exists) and
`S97waybeam-hub`.

### Usage

```bash
# Persistent — next boot starts as broadcaster
fw_setenv wfbmode 1 ; reboot

# Persistent — next boot stays in AP mode (default)
fw_setenv wfbmode 0 ; reboot

# Manual one-shot (requires wfbmode=1)
/etc/init.d/S99wfb start
/etc/init.d/S99wfb stop      # always returns wlan0 to AP via 'adapter start'
/etc/init.d/S99wfb status
/etc/init.d/S99wfb restart

tail -f /tmp/wfb.log         # combined daemon output
```

When enabled, `start` does:

1. `adapter stop` — kills hostapd and the adapter watchdog, flushes wlan0
2. `iw dev wlan0 set monitor otherbss` + bring link up
3. `iw dev wlan0 set channel <chan> <htmode>` (default 161 / HT20)
4. `iw dev wlan0 set txpower fixed <mBm>` (default 2000 = 20 dBm)
5. `wfb_tx -H local_shm ... -M 2 -k 8 -n 12 -P 1 -Q -S 1 -L 1 -T 16 -x`
6. `wfb_rx -i 208 -u 5801` (uplink data → 127.0.0.1:5801)
7. `link_controller --safe-startup-bitrate 4096`

`link_controller` writes `video0.bitrate=4096` and
`outgoing.maxPayloadSize=1400` to venc on its first tick — an MCS2-safe
floor — and then runs adaptive FEC k/n sizing from the venc sidecar.
WebUI on `:8765` is reachable via eth0.

### Tunables

Edit the constants at the top of the script:

| Variable | Default | Notes |
|---|---|---|
| `WFB_CHANNEL` | `161` | wlan channel |
| `WFB_HTMODE` | `HT20` | `HT20` or `HT40+/-` |
| `WFB_TXPOWER` | `2000` | mBm; 2000 = 20 dBm |
| `WFB_MCS` | `2` | MCS index passed to `wfb_tx -M` |
| `WFB_K` / `WFB_N` | `8` / `12` | initial FEC; link_controller adapts after first frame |
| `WFB_TX_LINK` / `WFB_RX_LINK` | `207` / `208` | radio link IDs |
| `WFB_CTRL` | `8000` | wfb_tx control port |
| `WFB_RX_FWD_PORT` | `5801` | wfb_rx decoded-data destination |
| `SAFE_BITRATE` | `4096` | kbps; passed as `--safe-startup-bitrate` |
| `SHM_RING` | `local_shm` | matches `outgoing.server: shm://local_shm` in venc.json |
| `KEY` | `/etc/drone.key` | wfb-ng key |

### Known gap

Stock 2024.02.10-release `wfb_rx` does not have the `-Y` JSON push
patch — only `wfb_tx` is patched here. So `link_controller`'s MCS
subsystem won't see `rx_ant` data on the vehicle and stays in
failsafe-watch. FEC adaptation is unaffected. Once the `-Y` patch is
ported to `wfb_rx`, swap the `wfb_rx -u 5801` line for
`wfb_rx -Y 127.0.0.1:5801` and pass `--stats 127.0.0.1:5801` to
`link_controller`.

## `wfb-ng.sh` / `wfb-ng.init` / `wbmode` — CPE510 ground-station

Three-file set for a TP-Link CPE510 v3 (OpenWrt + procd) running the
cross-built `wfb_rx` / `wfb_tx` from PR
[#36](https://github.com/snokvist/waybeam_wfb_ng/pull/36):

| File | Installs to | Role |
|---|---|---|
| `wfb-ng.init` | `/etc/init.d/wfb-ng` | procd unit; gates on `/etc/waybeam/current` and only opens an instance when profile = `wfb` |
| `wfb-ng.sh` | `/usr/sbin/wfb-ng.sh` | foreground supervisor; runs `post-up.sh`, spawns `wfb_rx` + `wfb_tx`, traps signals for clean teardown |
| `wbmode` | `/usr/bin/wbmode` | mode switcher; copies `/etc/waybeam/<mode>/{network,wireless}` into uci, runs `post-up.sh`, then `/etc/init.d/wfb-ng restart` to re-evaluate the gate |

### What `wfb-ng.sh` does (when profile = `wfb`)

1. Invokes `/etc/waybeam/wfb/post-up.sh` to create `wlan0mon` on `phy0`
   (monitor, ch161 HT40+, txpower 2000 mBm by default; HT40+ primary on
   161 receives a vehicle HT20 ch161 signal cleanly).
2. Spawns `wfb_rx` (link 207, AEAD-cleartext data, decoded RTP forwarded
   to `192.168.2.20:5600`, stats pushed via `-Y` to `127.0.0.1:5801`).
3. Spawns `wfb_tx` (link 208, listens on UDP `5801`, control port `8000`,
   `M=1 k=1 n=2`).
4. Waits on both children. SIGTERM/SIGINT/SIGHUP → kill daemons +
   `iw dev wlan0mon del`.

The `127.0.0.1:5801` collision is **intentional** — `wfb_rx`'s `-Y`
JSON stats push lands on the same UDP port that `wfb_tx -u 5801` is
listening on. `wfb_tx` packs the JSON into the uplink stream on link
208, where the vehicle's `wfb_rx` forwards it to `link_controller`
upstream. This is the GS→vehicle feedback path.

### Install

```bash
scp -O poc/init/wfb-ng.sh   root@192.168.2.2:/usr/sbin/wfb-ng.sh
scp -O poc/init/wfb-ng.init root@192.168.2.2:/etc/init.d/wfb-ng
scp -O poc/init/wbmode      root@192.168.2.2:/usr/bin/wbmode
ssh root@192.168.2.2 '
  chmod +x /usr/sbin/wfb-ng.sh /etc/init.d/wfb-ng /usr/bin/wbmode
  /etc/init.d/wfb-ng enable
  /etc/init.d/wfb-ng restart
'
```

### Verify

```bash
ssh root@192.168.2.2 '
  iw dev                                       # wlan0mon, monitor, ch161
  ps w | grep -E "wfb_(rx|tx)" | grep -v grep
  ubus call service list | grep -A4 wfb-ng     # running:true
  logread | grep wfb-ng | tail
'
```

### Mode switch

`wbmode` is the supported way to flip modes — it stops services, swaps
the uci configs, restarts network, runs `post-up.sh`, then calls
`/etc/init.d/wfb-ng restart` so the procd gate re-evaluates the new
profile (starts daemons for `wfb`, leaves no process for `client`).

```bash
ssh root@192.168.2.2 'wbmode wfb'        # bring up broadcaster mode
ssh root@192.168.2.2 'wbmode client'     # back to managed-sta
ssh root@192.168.2.2 'wbmode status'
```

### Notes

- `active_profile()` uses `read -r p < /etc/waybeam/current` rather than
  `tr -d '[:space:]'` — BusyBox `tr` does **not** support POSIX class
  names, so `[:space:]` would be parsed as the literal char set
  `[ : s p a c e ]` and stripping `c` and `e` from `client` would
  silently produce `lint`.
- The procd gate (`wfb-ng.init`) is the reason the supervisor never
  hangs around in `client` mode; an earlier revision used an idle
  `while sleep 3600` loop inside the supervisor itself, which was both
  ugly and hard to interrupt cleanly.
