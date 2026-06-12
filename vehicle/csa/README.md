# CSA — graceful Channel Switch Announcement over wfb-ng

One-way Channel Switch Announcement piggybacked on the existing wfb-ng
ground→air uplink. Both the ground (cpe510) and vehicle radios hop to a new
channel/width simultaneously, with redundant frames so a single uplink loss
doesn't strand either side.

See [`PROTOCOL.md`](./PROTOCOL.md) for the wire format.

## Layout

| File | Role |
|---|---|
| `csa.h` / `csa.c` | Receiver state machine library — reused by `csa_agent` (standalone) and `link_controller` (integrated). |
| `csa_agent.c` | Standalone receiver shim — thin `main()` over `csa.c`. Built for cpe510 (mips24kc) and as a vehicle bench tool. |
| `csa_mux.py` | Host orchestrator — sends `csa_commit` frames to cpe510:5801 (over-air to vehicle) and cpe510:5802 (LAN to ground agent). |
| `udp_ts_recv.c` | ms-resolution UDP capture utility used by `csa_probe.py` to characterize uplink loss/jitter. |
| `csa_probe.py` | Loss/jitter probe: sends timestamped JSON over the same uplink. |
| `test_10_hops.sh` | Sequenced 10-hop consistency test. |

## Build

```sh
make armhf       # csa_agent.armhf + udp_ts_recv.armhf  (vehicle)
make mips24kc    # csa_agent.mips24kc                    (cpe510 / ath79)
```

## Bench-test workflow

### Round 2 — vehicle CSA folded into link_controller

On the **vehicle** (sigmastar Infinity6E), `link_controller` now multiplexes
CSA frames on the existing rx_ant listener (default 5801), so adaptive FEC +
MCS keep running across hops:

```sh
link_controller --wfb 127.0.0.1:8000 --venc 127.0.0.1:80 \
    --safe-startup-bitrate 4096 \
    --csa-iface wlan0
```

Off by default; set `--csa-iface` to enable. Requires the MCS subsystem
(default-on) since CSA shares the rx_ant socket. The receiver accepts
any channel/bandwidth with a 2 s cooldown and auto-revert on silence
(the former `--csa-allowlist/--csa-bandwidth/--csa-cooldown-ms/
--csa-no-revert` flags were frozen to these defaults in the
public-surface slimdown).

### Standalone agent (cpe510 ground relay, or vehicle without link_controller)

The standalone `csa_agent` is still useful: cpe510 doesn't run
link_controller, and the agent is handy for rapid bench iteration when the
vehicle's link_controller is stopped.

On the **cpe510** ground relay (ath79):

```sh
scp csa_agent.mips24kc root@<cpe510>:/tmp/csa_agent
ssh root@<cpe510> '/tmp/csa_agent --no-revert \
    --allowlist 149,153,157,161 --bandwidth HT20,HT40+ \
    5802 wlan0mon'
```

On the **vehicle**, only when running standalone (without link_controller):

```sh
scp csa_agent.armhf root@<vehicle>:/tmp/csa_agent
ssh root@<vehicle> 'killall link_controller; /tmp/csa_agent \
    --allowlist 149,153,157,161 --bandwidth HT20,HT40+ \
    5801 wlan0'
```

Flags (all optional, defaults are fully permissive):
- `--allowlist CH,...` — comma-separated channels accepted as targets
  (e.g. `149,153,157,161`). Unset = any channel, DFS included.
- `--bandwidth BW,...` — comma-separated bandwidths accepted as targets
  (e.g. `HT20,HT40+`). Unset = any bandwidth.
- `--cooldown-ms N` — minimum gap between channel changes (default 2000;
  0 disables; revert counts as a hop).

`--no-revert` on the ground side: cpe510 owns the channel; auto-reverting
on its own would be dangerous. Only the vehicle reverts if the link goes
silent post-switch.

From the **host**:

```sh
python3 csa_mux.py \
    --target-chan 149 --target-ht HT20 \
    --prev-chan 161   --prev-ht HT40+ \
    --lead-ms 1000 --t-revert-ms 5000
```

To run the 10-hop sweep:

```sh
CPE_HOST=192.168.2.2 VEH_HOST=192.168.1.13 ./test_10_hops.sh
```

After the test, restart link_controller on the vehicle:

```sh
ssh root@<vehicle> 'link_controller --wfb 127.0.0.1:8000 --venc 127.0.0.1:80 \
                    --safe-startup-bitrate 4096 &'
```

## Bench results (round 1)

Hardware: cpe510 (ath79, ch161 HT40+) ↔ vehicle (rtl8812-class on
sigmastar Infinity6E, ch161). RSSI ≈ −50 dBm.

| Test | Result |
|---|---|
| 200 × 100 ms uplink probe | 0 loss, 0 reorder, jitter stdev 2.4 ms |
| 5×10 burst @ 256 B | 0 loss, intra-burst jitter p95 ≤ 8 ms |
| Channel hop 161 HT40+ ↔ 149 HT20 | OK, vehicle confirmed link in 52 ms |
| 10-hop sweep (149/153/157/161, mixed widths) | 10/10 PASS, switch precision 0–5 ms, verify 31–245 ms (median ~85 ms) |

## Known gaps before production

1. Loss tolerance untested at degraded RSSI.
2. No bidirectional ack (downlink confirmation that vehicle followed).
3. No auth — anyone on the LAN can inject CSA on 5802. `sess` blocks replay
   within agent uptime, but a fresh forgery just picks `sess > current` and
   wins. `--allowlist` + `--bandwidth` + cooldown limit blast radius; HMAC
   closes the gap. See `PROTOCOL.md` "Replay-protection scope".
4. No persistent `sess` on the receiver — agent reboot resets the high-water
   mark; one captured frame can replay before the next legitimate session
   bumps `sess` past it. Cooldown limits damage to one hop per `cooldown_ms`.
5. CSA-aware MCS-down pause not implemented yet — a freshly-silent post-
   switch link could trigger a spurious bitrate drop while the controller
   is in VERIFY. (Not observed on the bench; logged as follow-up.)
