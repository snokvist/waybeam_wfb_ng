# Boundary-Probe MCS Control — Phase 4 (Productionize) — Requirements

Status: IMPLEMENTED + DEVICE-VERIFIED 2026-06-10 (§8 complete except operator-gated
induced-fade dynamic test)
Date: 2026-06-10
Branch: `feature/mcs-boundary-probe` (PR #58)
Depends on: Phase 1+2 (`link_controller` boundary-probe law, merged in this PR);
            forward-port flap fix (commit 88e02e8); mixed/parallel validation (cc1e5e6)

---

## 1. Background / what is already done

- **Phase 1+2 (shipped, device-validated):** `link_controller` ingests
  `{"type":"probe"}` records off the rx_ant uplink tunnel and drives video MCS
  0–5 under `mcs.mode=1` (boundary-probe law). Legacy RSSI-bucket FSM stays under
  `mcs.mode=0`. See `telemetry/MCS_STRATEGY.md` and `telemetry/PROBE_PER_SPEC.md`.
- **The "video on/off flap" was root-caused and fixed (2026-06-10):** a probe
  `wfb_rx` launched without `-u` inherits `127.0.0.1:5600` (the RTP video port) and
  injects probe payloads into the decoder. Not adapter sharing, not host CPU. All
  probe scripts now pass an explicit dead `-u`. See `PROBE_PER_SPEC.md` §4/§8a.
- **Steady-state soak PASSED (2026-06-10, Star6E imx335 @192.168.1.13):** with
  *mixed/parallel* fast probing (V+2 freshness 0.04–0.6 s, `mcs.probe_stale_age_s=1.0`,
  `probe_log --window-s 0.5`), `mcs.mode=1` converged to `mcs_max` and held —
  `commit_count` froze, `changes_in_window=0`, no oscillation, no failsafe, video
  clean. See `PROBE_PER_SPEC.md` §8b.
- **Today the probe rig is standalone prototype scripts** (`telemetry/probe/*.sh` +
  `probe_log.py` + `probe_bridge.py`). Phase 4 folds it into the production daemons.

This document specifies the production form.

---

## 2. Key architectural decision: single V+2 stream, link_controller-driven

The boundary-probe law reads **only `rung[current_mcs + 2]`** (`link_controller.c`
`selector_update_probe()`, the `int v2 = s->current_mcs + 2;` block — it does NOT
consult V+1). Therefore production needs to probe **exactly one MCS at any instant**:
`current_video_mcs + 2`.

This collapses the rig dramatically vs. the N-parallel-stream prototype:

- `wfb_rx -Y` keys stats by `freq:mcs:bw`, so **one** RX on **one** port reports a
  correct per-MCS PER bucket for a **single transmitter whose MCS varies over time**
  (seq numbers stay monotonic within the one session). This is valid precisely
  because we have one probe stream. (Multiple *concurrent* TX on the same link+port
  would collide seq counters and corrupt `lost`/`accounted` — explicitly out of scope;
  we never do that.)
- So: **1 probe `wfb_tx`, 1 feeder, 1 probe `wfb_rx`, no sweep, no parallel streams.**
  The probe TX is retuned to track `current+2` as video MCS moves (dynamic boundary
  tracking). At steady state the probe sits at a fixed MCS → continuous ~10 Hz freshness
  on the one rung the law reads.

Decisions locked with the maintainer (2026-06-10):
- GS per-rung PER computation: **ported into `gs_supervisor` (C)** — no python runtime
  dependency (works on aarch64 ground stations).
- Rung selection: **dynamic boundary tracking** (probe `current+2`, retune on MCS change).
- Probe RX: **single `wfb_rx`** on one port/link (valid per the single-stream argument
  above; no multi-port wfb_rx patch needed).
- Feeder: **integrated into `link_controller`** (internal UDP sender thread).

---

## 3. Component changes

### 3.1 `link_controller` (vehicle) — the probe brain  [primary new code]

`link_controller` already (a) knows the current committed video MCS, (b) drives the
video `wfb_tx` rate via `wfb_tx_cmd set_radio` on the control socket, and (c) consumes
probe records. Phase 4 adds the producer side:

- **Internal feeder thread:** generate `PRB`-magic + monotonic-seq + pad-to-MTU
  packets at a paced rate (start 20 pps; configurable) to the probe `wfb_tx` udp_in
  (loopback). Replaces the prototype shell `socat` feeder. Must be non-blocking and
  not perturb the FEC/MCS control loop timing.
- **Probe retune on commit:** in the MCS commit path (`selector_commit_mcs()`),
  whenever the committed video MCS changes, retune the probe `wfb_tx` to
  `min(committed + 2, 7)` via `wfb_tx_cmd set_radio` mirroring the video PHY
  (`-B/-S/-L`, same GI/NSS). If `committed + 2 > 7` (no 1SS headroom), park the probe
  (e.g. hold at 7, mark V+2 unavailable) — promotion above the corresponding video MCS
  is then simply not offered.
- **Retune ordering invariant (REVIEW THIS):** the probe must be retuned to the *new*
  V+2 **in the same commit** that changes video MCS, before the next probe-evaluation
  tick, so the law never reads a stale-high rung after a reactive demote. Define and
  test: on demote 5→4, probe retunes 7→6; the `rung[6]` reading must not be acted on
  until it reflects post-retune traffic (freshness via `last_us` already gates this —
  confirm the retune resets/ages the rung so a pre-retune `rung[6]` can't promote).
- **Config:** new `mcs.*` (or `probe.*`) tunables: feeder pps, probe link_id/port/ctrl,
  enable flag, PHY mirror source. Set **`mcs.probe_stale_age_s` default 1.0** (was 1.5).
- **Lifecycle:** link_controller controls + feeds the probe `wfb_tx` that `S99wfb`
  spawns (parallel to how it controls the video `wfb_tx` it does not spawn). On
  shutdown it stops feeding; `S99wfb stop` reaps the process.

### 3.2 `S99wfb` (vehicle init) — one extra process

`vehicle/init/S99wfb`, `start_wfb()`: after the video `wfb_tx` / uplink `wfb_rx` /
`link_controller`, conditionally spawn **one** probe `wfb_tx`:

```
wfb_tx -K $KEY -i $WFB_PROBE_LINK -p $WFB_PROBE_PORT -M <init_mcs> \
       -B $WFB_BW -S 1 -L 1 -k 1 -n 1 -x \
       -C $WFB_PROBE_CTRL -u $WFB_PROBE_FEED -l 1000 wlan0
```

- New env, gated like `wfbmode`: `wfbprobe` (0/1) + `WFB_PROBE_LINK` (50),
  `WFB_PROBE_PORT` (50), `WFB_PROBE_CTRL`, `WFB_PROBE_FEED`. Mirror the video PHY
  (`-B20 -S1 -L1`), FEC `1/1`, AEAD off (`-x`).
- `init_mcs` = best-effort `WFB_MCS + 2`; link_controller retunes immediately on first
  commit anyway.
- `stop_wfb()`: add the probe `wfb_tx` pidfile to the teardown loop (SIGTERM; the
  existing `killall` catch-all already covers `wfb_tx`).
- The probe stream is on `wlan0` → it follows CSA channel hops automatically. **Verify**
  the probe survives a CSA hop (it should — same iface/channel).

### 3.3 `host_x86.json` + `gs_supervisor` (ground) — one extra RX tunnel + PER calc

**`ground/config/host_x86.json`:** add one `rx` tunnel:

```json
{
  "name": "probe", "role": "rx",
  "binary": ".../wfb_rx_native",
  "interfaces": ["wlx40a5ef2f229b", "wlx40a5ef2f2308"],
  "link_id": 50, "radio_port": 50,
  "udp_out": "127.0.0.1:5751",          // DEAD port — NEVER 5600 (RTP video)
  "extra_args": ["-x", "-l", "100"],
  "autostart": true
}
```

- The probe RX runs alongside the video RX on the same diversity adapters (extra pcap
  handle — proven harmless; the flap was the forward port, not the 2nd pcap).
- **`udp_out` MUST be a dead port.** Add a guard in `gs_supervisor` config validation:
  reject / warn if a non-video tunnel forwards to `5600`.

**`gs_supervisor` (C):** port the windowed PER computation (today in `probe_log.py`)
into the supervisor:
- For the probe tunnel's rx_ant stream (already parsed in the rx stats listener,
  `gs_supervisor.c` ~line 1033+), maintain a sliding window (default **0.5 s**) per
  received MCS bucket (`freq:mcs:bw`), computing `per_milli = (lost*1000 + acc/2)/acc`
  with `accounted = lost + recv`.
- Emit `{"type":"probe","mcs":M,"accounted":A,"lost":L,...}` (schema = what
  `link_controller`'s demux already parses — `mcs`, `accounted`, `lost`) into the
  existing **`stats_drain` → `stats_out` (uplink `udp_in 6600`)** path, so it rides the
  same back-channel as video rx_ant to `link_controller :5801`. No new transport.
- Rate-cap the emitted probe records (a few/s) so they cannot overrun the uplink
  `wfb_tx` UDP input (documented silent-overrun limit).

### 3.4 `link_controller` consumer — already done

The rx_ant ingest seam already demuxes `"type":"probe"` and the law already acts on it.
No change beyond §3.1 (producer + stale default).

---

## 4. Wire format / schema (freeze + test)

Probe record (vehicle demux reads `mcs`, `accounted`, `lost`; `per_milli` derived):

```json
{"type":"probe","mcs":7,"accounted":20,"lost":0,"per":0.0,"recv":20,"window_s":0.5,"ts_ms":...}
```

- Add a conformance test under `tests/protocols/` for this record (producer =
  gs_supervisor, consumer = link_controller). Today only the rx_ant schema is tested.
- `per_milli` = `(lost*1000 + accounted/2) / accounted`, integer per-mille; `-1` when
  `accounted == 0` (treated as **invalid → hold**, never as fail).

---

## 5. Invariants / edge cases for the reviewer to scrutinize

1. **Retune ordering (§3.1):** a reactive video demote must retune the probe down
   *before* a stale high-rung reading can trigger a promote back up. This is the single
   most important correctness invariant — verify the freshness gate (`last_us` / age vs
   `probe_stale_age_s`) actually prevents acting on the pre-retune rung.
2. **Headroom clamp:** at video MCS 5 (`mcs_max=5`) the law cannot promote anyway, so
   probing rung 7 is for pre-empt-demote signal only. At video MCS = 6/7 (not used while
   `mcs_max=5`, but guard it) `current+2 > 7` → no 1SS rung → V+2 unavailable → hold.
3. **Invalid PER (`accounted==0`, `per_milli=-1`):** must map to **hold**, never demote.
   Observed real at the MCS-7 cliff in the soak. Confirm the law's branch order.
4. **Failsafe vs probe:** probe is on a separate link_id (50); rx_ant gap failsafe
   (forces `mcs_min`) is driven by the *video/uplink* path, independent of probe. A probe
   outage must NOT trip failsafe; it should just stale → hold. Verify.
5. **Airtime budget:** one probe stream at 20 pps × MTU mirrors a fraction of video
   airtime; document the budget and the max pps. (3 concurrent streams showed no 8812eu
   stress in the soak; a single stream is lighter.)
6. **Enable/disable:** `wfbprobe=0` (vehicle) and tunnel `autostart=false` (GS) must
   yield clean `mcs.mode=1`-with-no-probe behaviour: law holds at current MCS (no
   promote source, demote still reactive). Verify no failsafe/no drift.
7. **CSA hop:** probe TX (wlan0) and probe RX (monitor) must both follow a channel
   change; verify a CSA cycle doesn't strand the probe on the old channel.
8. **Feeder must not stall the control loop:** the in-`link_controller` feeder thread
   must be isolated from the selector/FEC timing (no shared lock held across send).

---

## 6. Tuning defaults (validated 2026-06-10)

| Param | Prototype | Phase 4 default | Note |
|---|---|---|---|
| `mcs.probe_stale_age_s` | 1.5 | **1.0** | matches ~10 Hz single-stream freshness |
| probe window (gs_supervisor) | — | **0.5 s** | window population = pps/2 packets; 0.3 s too short (invalid PER) |
| feeder pps (`mcs.probe_feed_pps`) | 20 | **40** | bring-up finding: 20 pps → 10-pkt windows → 100‰ per lost packet, too coarse (see fail threshold) |
| `mcs.probe_clean_milli` | 20 | 20 | ≤2% → promote |
| `mcs.probe_fail_milli` | 100 | **200** | bring-up finding (2026-06-10): at 100, ONE lost packet of a 10-pkt window sat exactly on the threshold → constant pre-empt-demote/promote bounce at a *marginal* (not failing) ceiling rung (~1/10 s, device-observed). A real cliff reads 500–1000‰ within a window, so 200 keeps the early warning. 40 pps + 200‰ device-verified: bounce stopped (0 demotes/90 s), marginal rung reads 100‰ → hold |
| `mcs.demote_per_milli` | 30 | 30 | ≥3% live video PER → reactive demote |
| `mcs.promote_dwell_s` | 0.5 | 0.5 | |
| `mcs.down_cooldown_s` | 0.2 | 0.2 | |

Note: the mode=1 promote path uses `promote_dwell_s` only — the oscillation
damper (`oscillation_*`) multiplies the *bucket-FSM* up-cooldown and does NOT
apply. If ceiling cycling ever reappears, the knobs are `probe_fail_milli` ↑,
`promote_dwell_s` ↑, or (code) a post-pre-empt clean-streak requirement.

---

## 7. Implementation order

1. `link_controller`: feeder thread + probe-retune-on-commit + stale default 1.0
   (host-build + unit-ish checks first).
2. `S99wfb`: spawn/teardown the gated probe `wfb_tx`.
3. `gs_supervisor`: probe-PER computation + drain tee + the 5600-forward guard.
4. `host_x86.json`: probe rx tunnel.
5. Wire-format test in `tests/protocols/`.
6. Device bring-up + verification (§8).
7. After soak: set `mcs.mode=1` default? (separate decision) and strip legacy bucket FSM.

Backward compatibility: every piece is gated (`wfbprobe`, tunnel `autostart`,
`mcs.mode`), so Phase 4 can deploy incrementally and roll back to `mcs.mode=0`.

---

## 8. Verification / bring-up checklist

- [x] Host build of `link_controller` + `gs_supervisor` clean (`-Wall -Wextra`).
- [x] `wfbprobe=0`: `mcs.mode=1` holds at current MCS, no failsafe, no drift.
      (Verified via the runtime-equivalent gate `mcs.probe_enabled=0`: feeder frozen,
      law holds, no failsafe, re-enable recovers <1 s. Cold boot with `wfbprobe=0`
      exercises the same `probe_on` gate plus no probe wfb_tx spawn.)
- [x] `wfbprobe=1`: probe `wfb_tx` up (link 50/p50), link_controller feeds it, probe
      records arrive at `:5801` (`/mcs/status` `probe.records` climbing, `parse_errors=0`).
- [x] `v2_age` < 1 s continuously at steady state; `v2_mcs == current_mcs + 2`.
      (Observed 0.04–0.5 s at 20 pps; 0.06–0.25 s typical after the 40 pps tuning.)
- [x] Video MCS commit retunes probe within one tick; `v2_mcs` tracks.
      (Live log: retune fires before the deferred video SET_RADIO commit.)
- [x] Reactive demote 5→4 retunes probe 7→6 and does NOT immediately re-promote on a
      stale rung[6] (the §5.1 invariant). (Gate host-proven; live demote→retune in 6 ms.)
- [x] gs_supervisor rejects/warns a non-video tunnel forwarding to 5600.
- [x] CSA hop: probe survives, records resume after hop. (2026-06-10, live 161→157→161
      round trip: vehicle COMMITTED 118 ms after switch; the hop's transient loss caused
      exactly one reactive demote 5→4 with probe retune 7→6 in 6 ms, then a clean rung
      read re-promoted to 5 within 0.6 s. GS probe tunnel `stale_dropped=0`, no
      failsafe, `retune_fails=0` across both hops.)
- [x] Steady-state soak (fixed distance): converges to ceiling, holds, no oscillation,
      video clean (re-confirm the Phase-pre result through the production path).
      (Post ceiling-bounce fix — 40 pps / `probe_fail_milli=200`: 2-minute window,
      0 demotes, held MCS 5 with `v2_acc=20, v2_per=0`.)
- [ ] Induced-fade dynamic test (operator): clean fast demote→re-promote, no flap at the
      boundary. (Deferred from the steady-state soak.)

---

## 9. Out of scope / deferred

- V+1 probing (the law only reads V+2 today). If Phase 3 wants V+1 confirmation, add a
  fast 2-rung micro-sweep on the same single stream.
- Multi-port `wfb_rx` patch (not needed for a single stream).
- ~~`mcs.mode=1` as the default + legacy bucket FSM removal~~ — DONE 2026-06-10,
  see `specs/2026-06-10-unified-mcs-rssi-law/` (mode knob removed; probe law is
  the only law, probe producer mandatory).
- ~~Phase 3 RSSI fade-rate/staleness augment~~ — DONE 2026-06-10 as the RSSI
  floor/fade guard-rails in the same unified-law change.
- **Probe downlink as a data side-channel** (maintainer idea, 2026-06-10): the PRB
  payload is pure padding today — the probe stream is a free ~28 kB/s vehicle→GS
  channel that already exists end-to-end (feeder → probe wfb_tx → probe wfb_rx →
  gs_supervisor). If the GS ever needs vehicle-originated data outside the video
  path (e.g. link_controller state snapshots, debug telemetry), it can ride inside
  the PRB packets after the 13-byte header with zero extra airtime. Caveats: AEAD
  is off (-x) so the channel is unauthenticated/plaintext, and it lives at V+2 so
  it degrades exactly when the link does — diagnostics-grade only, never control.

---

## 10. Implementation notes (2026-06-10, deviations from the draft — all tightening)

1. **§3.1 "feeder thread" → main-loop deadline tick.** `link_controller` is by
   design a single-threaded poll() loop; the feeder is a paced deadline tick in
   that loop (50 ms at 20 pps joins the poll-deadline computation), and the
   retune reconciler runs at loop-top. Satisfies §5.8 with zero locking.
2. **Retune is a reconciler, not a commit hook.** Desired probe MCS
   (`min(current+2, 7)`) plus the mirrored video PHY body is compared against
   the last acked body every loop iteration; any drift (MCS commit from ANY
   path — probe law, failsafe, realign, WCMD — or a PHY change like a WCMD
   bandwidth write) triggers one SET_RADIO. 1 s backoff while the probe wfb_tx
   is unreachable, so a missing probe can never stall the loop.
3. **§5.1 got a hard commit gate, not just the freshness check.**
   `ProbeState.gate_us` is stamped whenever the committed video MCS changes;
   `selector_update_probe()` refuses any rung record with `last_us < gate_us`.
   Host-verified: after a reactive demote, fresh-looking pre-demote rungs
   (well inside `probe_stale_age_s`) cannot re-promote. Combined with the GS
   bucketing by *received* MCS, a stale-high re-promote is impossible by
   construction.
4. **Probe tunnel must NOT raw-forward rx_ant (flap-class bug found in the
   draft).** §3.3 as drafted ("emit into the existing stats_drain → stats_out
   path") would have re-emitted the probe tunnel's raw rx_ant verbatim to 6600 —
   the vehicle ingests rx_ant as *video* score, so probe MCS-7 loss would have
   caused spurious reactive demotes. `"probe": true` suppresses the raw re-emit
   and forwards only the computed `{"type":"probe"}` records.
5. **GS guard (§3.3) semantics:** a probe tunnel without an explicit `udp_out`,
   or with `udp_out` port 5600, is REJECTED at config load; any two rx tunnels
   sharing an effective forward port (omitted = 5600) get a WARN.
6. **Producer never emits `accounted == 0`** — total-loss windows surface as
   staleness on the vehicle (hold), exactly matching the soak-observed MCS-7
   cliff behaviour. Stale partial windows (probe gap > 4 windows) are discarded,
   not emitted with overstated freshness.
7. **Schema frozen** in `tests/protocols/test_probe_protocol.py` +
   `_proto/probe_protocol.py` (18 tests): required keys `mcs`/`accounted`/`lost`,
   compact `"type":"probe"` demux token, per-mille round-half-up derivation,
   `accounted==0 → -1` invalid.
8. **Observability:** vehicle `/status` `mcs.probe` gains `configured`,
   `enabled`, `feed_seq`, `tuned_mcs`, `retune_fails`; GS tunnel status gains
   `probe:{window_ms, emitted, stale_dropped}`.
9. **Host-verified end-to-end** (fake wfb_tx control endpoints + fake wfb_rx):
   cold-start commit → retune to V+2; clean rung → promote + immediate
   re-retune; lossy video → reactive demote + down-retune; stale-rung hold
   (gate); retune history exactly one SET_RADIO per commit. GS: 20/2 window →
   `per:0.1000`, `window_s:0.501`, raw rx_ant fully suppressed.
