# Airtime-aware pps guard (per-MCS "airslot" ceiling)

## Problem

The adaptive payload sizer (`PAYLOAD_TABLE` + `payload_safe_bitrate_kbps`)
bounds the **host packet rate** against a roughly flat `~1100 pps` ceiling.
That is the correct limit at **high MCS**, where a packet costs almost no
airtime and the SoC softirq / driver TX-ring is the wall — verified by the
incident that motivated the static cap: `MCS7 + 800 B + 21 Mbps → ~3300 pps →
kernel watchdog reboot`.

At **low MCS** that flat ceiling is the *wrong* limit. The binding constraint
there is **channel airtime**, not packet count. A ~1050 B frame at MCS0
(6.5 Mbps PHY, HT20) occupies ~1.3 ms on air, so the radio physically tops out
near **~750 pps** — 3300 pps is impossible regardless of CPU. Two ceilings, and
which one binds flips with the rung:

| MCS | PHY (HT20) | airtime/pkt (~1050 B) | airtime pps ceiling | binds on |
|----|-----------|----------------------|---------------------|----------|
| 0 | 6.5 | ~1.33 ms | ~750 | **airtime** |
| 1 | 13 | ~0.68 ms | ~1460 | **airtime** |
| 2 | 19.5 | ~0.47 ms | ~2140 | **airtime** |
| 3 | 26 | ~0.36 ms | ~2780 | airtime ≈ CPU |
| 4 | 39 | ~0.25 ms | ~3970 | CPU (~3300) |
| 5–7 | 52–65 | ~0.17–0.20 ms | ~5000–5950 | CPU (~3300) |

The static table never *models* the low-MCS airtime limit; today the low rungs
stay safe only because `bitrate_min` couples low MCS to low bitrate. That is
safety by coincidence, with thin margin (MCS0 at the 2800 floor already runs
~60–66% airtime), and no guard if parity rises or the floor is raised.

## Mechanism

`airtime_safe_bitrate_kbps(phy_mbps, payload, k, n, max_pct, preamble_us)`
caps the target bitrate so total on-air airtime stays under `max_pct` of the
channel, using the live `phy_mbps` the MCS selector already tracks:

```
airtime = bitrate_bps * (n/k) * [ 1/phy_bps + T_pre/(8*payload) ]
          \____ coded data airtime ____/   \__ per-packet preamble __/
```

- **Data term** dominates and needs no payload — it is just the FEC-coded
  bitrate over the PHY rate. This is what makes low MCS tight.
- **Preamble term** is the per-packet PHY overhead (`T_pre ≈ 36–40 µs`,
  HT-mixed 1SS); it is what the *pps* count actually costs in airtime. Small
  (1–4 %) but real, and the only airtime cost that scales with packet count.

Solving `airtime = max_pct` for bitrate gives the cap. It is applied in
`bitrate_assert()` after the existing static pps cap; **airtime is a hard
physical limit, so the clamp is honoured even below `bitrate_min`** (a
throttled-but-flowing link beats a wedged one).

## Why it is safe to ship ON by default (80 %)

At `airtime_max_pct = 80` the cap is **non-binding at every rung's normal
operating point** — the cap sits well above the actual target everywhere:

| MCS | operating bitrate | airtime cap @80 % | binds? |
|----|------------------|-------------------|--------|
| 0 | 2800 (floor) | ~3360 | no |
| 1 | ~3900 | ~6500 | no |
| 2 | ~6500 | ~10300 | no |
| 5 | ~17300 | ~26800 | no |
| 6 | ~21300 | ~30900 | no |

So enabling it changes **no** current bitrates. It only engages if a
low-MCS / small-payload / heavy-parity combination would otherwise push past
80 % airtime — exactly the wedge it exists to prevent. It also generalises the
old `payload_safe_bitrate_kbps` (which only fired when the operator pinned
`payload_max` below the tier): the airtime guard is phy-aware and always
applies.

## Config & observability

- `fec.airtime_max_pct` (default **80**, `0` = off) — live via `/set`, public
  in `/schema` (operator policy). Lower it to trade video bitrate for airtime
  headroom at the low rungs.
- `fec.airtime_preamble_us` (default **40**) — expert; the per-packet PHY
  preamble used in the estimate.
- `/status` `fec` block gains an `airtime` object:
  `{max_pct, est_pct, cap_kbps, capped}` — `est_pct` is the channel occupancy
  the current operating point implies (so the low-rung load is finally
  visible, not just inferred from pps), `cap_kbps` the ceiling, `capped` true
  when it is actively clamping.

Verbose FEC log gains `airtime_cap=<kbps>` alongside `pps_cap`.

## Not changed

- The `PAYLOAD_TABLE` and `payload_safe_bitrate_kbps` static caps stay — they
  remain the right host-pps limit at high MCS. The airtime guard composes with
  them (target takes the lower of all caps).
- FEC parity is still MCS-driven, not loss-reactive (unchanged).
