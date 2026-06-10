# Spec — MCS-ladder probe PER: measured link headroom

**Status:** PROPOSED / deferred. Not for now.
**Priority:** AFTER the current effort concludes — i.e. after we have a trained
Tier-1 inference model evaluated on real captured data. Probe PER is a strictly
*better* headroom signal that we fold in later; it does not block the current
RSSI/loss-based loop.
**Date:** 2026-06-07
**Inspiration:** https://gist.github.com/gilankpam/38e71e036e6b924ac88fe49afb46c231
(probe-link MCS sounding). Adopt its core; correct the probe *direction* for our
topology (see §3).

---

## 1. Why (motivation)

Our whole loop currently estimates "is the link healthy / can we change MCS" from
**signal-strength** features — effective RSSI (and SNR when available). The first
real walk-around capture proved that is a weak predictor of actual capacity:

- the link carried **full traffic with zero loss at −85 dBm** best-chain (MCS 3
  has huge margin), so RSSI thresholds (controller lo/hi −70/−50) are far more
  conservative than the link's real limit;
- the only "outage" was a TX dropout at −7 dBm, not a fade — i.e. RSSI told us
  nothing about the failure.

The referenced gist found the same thing from the other side: at the failure
cliff **"SNR collapses to garbage … PER is the only trustworthy signal."** Two
independent observations, one conclusion:

> **Only actively measuring packet-error-rate at a given MCS reveals real
> headroom. Signal-strength metrics do not predict the cliff.**

So instead of *inferring* headroom from RSSI, **measure it**: transmit a small,
throwaway probe stream at candidate MCS rungs and read the PER per rung.

## 2. Core idea (the ladder)

Send dedicated probe packets at MCS rungs around the current operating point and
measure PER per rung. Key properties (from the gist, to re-validate for us):

- **Monotonicity:** once a rung fails, all higher rungs fail; once one works, all
  lower work. ⟹ you only need to probe the **boundary**: `current+1` (and `+2`
  as confirmation), not the whole ladder. Keeps airtime tiny.
- **Sharp cliff:** transitions are abrupt (gist: MCS4 = 1% PER viable, MCS5 =
  100% dead, at the same instant). Treat each rung as go/no-go, not a gradient.
- **PER is the signal; per-rung RSSI/SNR are noisy and die at the cliff** — log
  them for context but do not gate on them.

## 3. Architecture — DIRECTION IS THE CRUX

The video flows **drone → ground**. To measure *video* headroom the probe must
travel **the same direction as the video (drone → ground)**, exactly as the gist
does. Do **not** infer video headroom from the **ground → drone uplink** probe:
the links are **not reciprocal** — different TX power/EIRP, the drone's small
antennas vs the ground diversity rig, and interference/noise floor that is *local
to each receiver*. Uplink PER-vs-MCS measures the reverse path and can mislead.

Two probes, two distinct purposes (run either or both):

| probe | direction | answers | consumer |
|-------|-----------|---------|----------|
| **video-headroom probe** | drone → ground | can the *video* go to MCS+1/+2? | the vehicle MCS controller |
| **uplink-capacity probe** | ground → drone | can the *uplink* carry a higher-rate event/telemetry stream? | the 20–50 Hz upstream plan |

The vehicle's `link_controller` already lives on the drone and already consumes
ground-side RX stats over the back-channel, so video-headroom probe PER (measured
at the GS) reports back up the **same existing channel** to where the decision is
made.

```
drone:  [wfb_tx port 0  MCS=video  FEC k_v/n_v]      <- live video, untouched
        [wfb_tx port 51 MCS=cur+1  FEC 1/1 off]      <- probe rung +1
        [wfb_tx port 52 MCS=cur+2  FEC 1/1 off]      <- probe rung +2  (confirm)
          | (air -> ground)
GS:     [wfb_rx port 0  -> video, never touched]
        [wfb_rx port 51 -> PER(cur+1)]  [wfb_rx port 52 -> PER(cur+2)]
          -> probe logger -> PER per rung -> back-channel up to controller + our loop
```

## 4. Probe stream design

- **One `wfb_tx` per probed MCS rung**, each on its **own `radio_port`** (e.g.
  50+rung_index), so the GS can separate rungs by port.
- **FEC OFF (`k=1, n=1`)** — expose raw packet loss unmasked by error correction.
  (Consequence: probe PER reads *harsher* than the FEC-protected video at the
  cliff — gist saw 100% vs 92%. Use the probe for go/no-go on a rung, NOT as a
  calibrated video-PER.)
- **MTU-sized packets (~1400 B)** to match video airtime; smaller packets read
  optimistically and lie about headroom.
- **~20 pps PER RUNG** (NOT 20 pps total). The gist's 20 pps/rung gave stable PER
  in a short window; ~7 pps (20 total / 3) is too sparse for a clean estimate.
  With boundary-only probing (1–2 rungs) total probe load stays ~20–40 pps.
- **Mirror the video PHY** (bandwidth, STBC, LDPC, guard interval); vary **only
  MCS** so the probe isolates the MCS variable.
- **Packet format** (per gist `feeder.c`): magic `"PRB0"` + 64-bit big-endian
  sequence number + `0xA5` fill to size.
- **Probe RX MUST set an explicit forward port (`-u <dead port>` / `-U <sock>`).**
  `wfb_rx` defaults its decoded-payload forward to `127.0.0.1:5600` — the **RTP
  video port**. A probe `wfb_rx` left at the default injects accepted `PRB` packets
  straight into the live H.265 decoder and flaps the video. The probe needs only
  the `-Y` rx_ant stats; the decoded payload is unused, so send it to a dead local
  port. (Root-caused 2026-06-10: this — not adapter sharing or host CPU — was the
  Test-C "video on/off flap." `-x` flapped because plaintext probe packets were
  *accepted* and forwarded to 5600; AEAD-on dropped them, so no flap.)

## 5. Measurement

Two independent per-rung PER reads (cross-check; the gist found them in
agreement):

1. **Sequence-gap (ground truth):** logger tracks the `PRB0` 64-bit seq per port,
   `PER = 1 − received / (max_seq − min_seq + 1)` over a sliding window.
2. **wfb-ng native stats:** `wfb_rx -Y` reports `lost`/`data` per antenna keyed by
   `freq:mcs:bw` — already in our schema. Cross-check against (1).

Also capture per-rung RSSI/SNR/noise_floor (via `wfb_schema.best_chain`) for
context, with the caveat they go unreliable exactly at the cliff.

## 6. Decision logic

- **Promote:** if the `current+1` probe PER is clean (≪1% over the window),
  the video MCS may rise to it. Confirm with `+2` before a two-step jump.
- **Demote:** driven by the **video's own live PER** (not the probe) — if the real
  feed loses packets, step down to a known-good rung immediately. (Demotion stays
  reactive on the real stream; promotion is the thing the probe de-risks.)
- Keep the controller's existing hysteresis/cooldowns
  ([[link-controller-decision-logic]]); the probe only replaces the *"is there
  headroom to go up"* inference, which today is RSSI-based and too conservative.

## 7. How it plugs into the closed training loop

This is why it's worth doing for *this* project, not just for control:

1. **Better feature/label than RSSI.** Per-rung probe PER (`per_at_cur`,
   `per_at_cur+1`, `per_at_cur+2`, derived `headroom_rungs`) is measured ground
   truth for "margin". Add to `wfb_review_queue` events and as Tier-1 features;
   it should dominate the RSSI thresholds the synthetic model over-fit to.
2. **Bench-free loss-data generator (big one).** FEC-off probing at `+1/+2`
   *deliberately produces the loss cliff* the walk-around couldn't, **without
   risking the live video** (video stays at its safe MCS). This is exactly the
   `lost`/`fec` cliff data the supervisor flagged as entirely missing — obtainable
   without physically attenuating or flying to the edge.
3. **Cleaner labels.** The measured outcome gains a real `headroom` axis: a
   degraded-RSSI window with clean `+1` probe PER is *not* actually at risk; one
   with failing `+1` probe is. Resolves ambiguity the current outcome detector
   can't see.

## 8. Implementation — PROTOTYPE VALIDATED ON HARDWARE (2026-06-07)

A working prototype lives in `telemetry/probe/` and was validated end-to-end on
the real air unit (`root@192.168.1.13`, OpenIPC SSC338Q) + ground station, **with
the live video link untouched**. See memory `wfb-manual-probe-setup` for the full
bring-up log. Reuses stock binaries — no new RF code.

**Bench-validated rules (MUST hold or PER lies):**
- **Mirror the video PHY, vary ONLY MCS.** Video runs `stbc=1 ldpc=1 short_gi=0
  bw=20` (read live via `wfb_tx_cmd 8000 get_radio`). A probe WITHOUT LDPC read a
  false cliff (MCS7: 99.6% PER → **0.8%** once `-S 1 -L 1` was added). This is the
  single most important rule.
- **FEC OFF (`-k 1 -n 1`)** so raw loss isn't masked. With 1/1, every lost pkt is
  counted: `uniq + lost == sent` exactly (verified 250 sent → 202 uniq + 48 lost).
- **PACE the feed (≤20 pps/rung).** Overrunning `wfb_tx`'s UDP input silently
  drops packets there (no seq number → invisible to RX → PER understated). Always
  sanity-check `accounted == sent`.
- **Isolate by link_id** (`channel_id = link_id<<8 | radio_port`); keep off video
  (i=207/p0) and uplink (i=208/p0). Ground RX on the **RX-only** adapter.
- **AEAD off (`-x`)** both sides. Single shared `/etc/drone.key`.
- **MCS index ≠ monotonic ladder** (HT 0-7 = 1SS, 8-15 = 2SS): probe within
  video's spatial-stream regime (1SS, 0-7).

**Prototype files (standalone — run alongside production, no integration needed):**
- `telemetry/probe/probe_drone.sh` — drone: per-rung probe `wfb_tx` (mirrors PHY,
  FEC 1/1) + paced shell `PRB`-seq feeder; trap-based cleanup (a probe TX must
  never leak into production).
- `telemetry/probe/probe_log.py` — ground: reads each rung's `-Y` rx_ant stream,
  emits windowed `{"type":"probe",ts_ms,radio_port,mcs,per,recv,lost,accounted,
  rssi,snr,window_s}` into the loop's JSONL — first-class alongside `wfb_review_queue`.
- `telemetry/probe/probe_ground.sh` — ground: per-rung `wfb_rx_native -Y` (RX-only
  adapter) + `probe_log`. (Under sudo, use ABSOLUTE binary paths — `$HOME=/root`.)

**IMPROVED architecture — single swept TX (preferred over N parallel TX):**
`wfb_tx_cmd <ctrl_port> set_radio [-B bw] [-G gi] [-S stbc] [-L ldpc] [-M mcs]
[-N nss] [-V]` changes a running `wfb_tx`'s rate LIVE (FLAG-based, NOT positional —
confirmed in `wfb-ng/build/wfb-ng/src/tx_cmd.c:232`; the exact mechanism
`link_controller` uses to actuate video MCS — `link_controller.c:2040`; wire format
in `tests/protocols/_proto/mcs_protocol.py` `RadioParams`). So instead of one
`wfb_tx` per rung (heavier; a wide 4-rung blast correlated with driver stress),
run **ONE** probe `wfb_tx -C <ctrl>` and **step its MCS** mirroring the video PHY:
`wfb_tx_cmd <ctrl> set_radio -B 20 -S 1 -L 1 -M <mcs>` (vary only `-M`; -G omitted
= long GI, -N defaults to 1 — cf. `RadioParams.with_mcs`), dwelling ~1–2 s per rung.
`probe_log` already buckets PER by the received `mcs` field, so a single swept TX +
single RX yields the whole ladder. Lighter footprint, boundary-friendly.

## 8a. Integration into waybeam_wfb_ng (LATER PR/spec — keep in lockstep)

The prototype runs standalone today (generates loss-cliff data for the gemma4 loop
without touching production). Productionizing is a **separate `waybeam_wfb_ng`
PR/spec** that must move three pieces in lockstep:
- **`S99wfb` (air unit):** add the probe `wfb_tx -C <ctrl>` (mirroring the video
  PHY) + the paced feeder, gated like `wfbmode`; the MCS sweep driven by
  `wfb_tx_cmd set_radio`.
- **`gs_supervisor` (ground):** spawn the probe `wfb_rx_native -Y` per rung (or one
  for the swept TX) and run `probe_log`, teeing probe records into the existing
  stats path (same place the `-Y` video tee goes). **The probe RX MUST be given an
  explicit forward port (`-u <dead port>` / `-U <sock>`) — never the `5600` default,
  which is the live RTP video sink (see §4).** Cleaner still: skip the second
  `wfb_rx` entirely and demux the probe `radio_port` inside the *existing* video
  `wfb_rx` (both diversity adapters already capture the probe frames in monitor
  mode), which never forwards probe payloads to the decoder at all. NB: the
  RX-only-adapter advice elsewhere was a red herring — adapter sharing was never the
  flap cause; the forward-port collision was.
- **`link_controller` (air unit):** consume probe PER over the existing
  back-channel for the **promote** decision (raise MCS only when `cur+1` probe PER
  is clean); **demotion stays reactive** on live video PER. Replaces the
  RSSI-threshold "headroom" inference (§6).
Fold probe-PER features into `wfb_link_score.py` and the Tier-1 retrain.

## 9. Risks / open questions (validate before trusting)

- **Reciprocity** — only an issue if someone shortcuts to uplink-only probing for
  video headroom. Don't (see §3).
- **Monotonicity under motion/multipath** — the gist validated it in a *single
  static session*. Frequency-selective fading can break strict monotonicity;
  re-test across motion and ranges before relying on boundary-only probing.
- **Airtime budget** — 60–75 pps was safe in the gist, but the safe ceiling vs.
  video bitrate is unmapped. Characterize before enabling in flight.
- **FEC-off vs FEC-protected calibration** — probe PER ≠ video PER at extreme
  loss; keep it go/no-go.
- **Generalization** — single card / one range pair so far; needs more sessions.

## 10. Relationship to current work

- Does **not** change the current plan: finish capturing real RF-stress data,
  build the holdout, train + evaluate the Tier-1 inference model on RSSI/loss
  features, conclude.
- Probe PER is the **next-generation headroom signal** layered on afterward,
  improving both the controller's promote decision and the loop's feature/label
  quality — and, usefully, giving us a bench-free way to capture loss-cliff data.
