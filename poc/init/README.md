# Init scripts

On-device init scripts for SigmaStar Infinity6E (OpenIPC) vehicles
running the patched `wfb_tx` + `link_controller` stack from `poc/`.

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
5. `wfb_tx -H local_shm ... -M 2 -k 8 -n 12 -P 1 -Q -S 1 -L 1 -b 1 -x`
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
