# Ground station init scripts (Buildroot / BusyBox)

Boot-persistence for `gs_supervisor` on Buildroot ground stations (RK3566 /
Radxa3e aarch64). BusyBox `rcS` runs `/etc/init.d/S??*` in numerical order.

## S46gs_supervisor — passive single-card video RX

Auto-starts `gs_supervisor` with `/etc/gs_supervisor.json` (the
`rk3566_passive.json` topology: one card in monitor mode, video RX link 207 →
`127.0.0.1:5600`, no uplink/probe). See `../config/rk3566_passive.json`.

### Install (on the target)

```bash
scp -O ground/init/S46gs_supervisor root@<gs>:/etc/init.d/S46gs_supervisor
ssh root@<gs> 'chmod +x /etc/init.d/S46gs_supervisor'
```

### Disable STA WiFi on this card (required for single-card passive nodes)

This host has **one** WiFi card and dedicates it to passive monitor-mode RX.
The stock `S45waybeam-wifi` STA manager (`WIFI_PRIMARY_IFACE="auto"`) would grab
that same card for `wpa_supplicant` association at boot — and a card running
wpa_supplicant in managed mode **cannot** be in monitor mode. Management/SSH is
over `eth0`, so STA WiFi is not needed here. Disable it by renaming it out of
the `S??*` boot glob (reversible):

```bash
ssh root@<gs> 'mv /etc/init.d/S45waybeam-wifi /etc/init.d/disabled-S45waybeam-wifi'
```

To re-enable STA WiFi later (and stop using this card as a passive RX), reverse
the rename and remove `S46gs_supervisor`.

This is the Buildroot analogue of `ground/setup/99-wfb-unmanaged.conf` (the
NetworkManager "unmanaged" drop-in used on x86/desktop ground stations) — both
keep the monitor-mode card away from the OS WiFi manager so gs_supervisor's
bring-up is deterministic.

### Control at runtime

```bash
/etc/init.d/S46gs_supervisor start|stop|restart
```

`stop` sends SIGTERM, letting gs_supervisor run its config `system.down` and
reap `wfb_rx` cleanly. PID file: `/var/run/gs_supervisor.pid`.

### Operational caveats (inherited from the passive config)

- **Channel must match the vehicle** (161/HT20 in the shipped config). A passive
  RX does not follow CSA hops — re-tune by hand or edit the config if the link
  hops.
- WebUI/stats: `http://<gs>/` (gs_supervisor wires `wfb_rx -Y` into its own
  listener even with no back-channel).
