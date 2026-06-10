# MCS strategy — RSSI→cliff prediction + lookahead probe (wfb_ng)

Design spec for per-MCS link adaptation on the wfb_ng FPV link. This is the
**decision logic** that sits on top of the Tier-1/Tier-2 telemetry plumbing
(`README.md`): Tier-1 carries the firehose and makes the real-time MCS call;
Tier-2 Gemma explains the rare escalation.

# ⇩ SHIPPED — Unified law: PER probe + RSSI guard-rails (2026-06-10)

**The boundary-probe law below is now THE control law** — the legacy
`bucket_from_rssi()` 3-bucket FSM, its `mcs_bucket_{0,1,2}` tunables, and the
`mcs.mode` knob were removed from `link_controller` on 2026-06-10. The RSSI
signal survives as two guard-rail demote rules folded into the same law
(floor at `rssi_floor_dbm`, fade at `rssi_fade_db_per_s` armed below
`rssi_fade_arm_dbm`) plus a promote guard — this subsumes the "RSSI fade-rate
augment" sketched as Phase 3 below. The probe producer is now MANDATORY when
MCS adaptation is enabled (`--probe`; S99wfb `wfbprobe=1`); `wfbprobe=0` runs
FEC-only. Spec: `specs/2026-06-10-unified-mcs-rssi-law/requirements.md`.

# ⇩ Boundary-probe control law (2026-06-08 pivot)

**This section is the live design and supersedes the model-centric framing in
the rest of the file** (kept below as background). The control loop is a
**relative boundary-probe ladder with a reactive-PER demote backstop, no ML in
the loop.** It replaces the shipped `bucket_from_rssi()` 3-bucket FSM and its
user-set `mcs_bucket_{0,1,2}` tunables — those went away (see above). The probe locates the
cliff directly, so the whole 0–5 video range is driven accurately with minimal
control logic and **no further data collection is needed to ship it**.

**Locked decisions (2026-06-08):** (1) **probe `V+2`-only** — no `V+1` probe (see
Ladder layout); (2) **`preempt_margin = +2` everywhere** — maintain a 2-rung
cushion at every video rung including the top (conservative; costs ~1 rung of
throughput, buys 1 rung of fade immunity); (3) **probe TX ownership split** —
`S99wfb` (air unit) owns the probe `wfb_tx -C` process lifecycle, `link_controller`
owns its MCS (drives `-M` over a local control port). Mirrors the existing
video-`wfb_tx` split and survives a controller restart. Implementation plan +
phasing tracked in roadmap Track A.

### Why no model
The cliff is **sharp and monotonic** (the gist and our own hardware probe both
confirm) — a comparator problem, not an ML problem. The probe *measures* the
cliff in situ; a model would only *predict* it, and worse (it must learn an
unseen per-environment noise floor that the probe sidesteps by measuring). You
don't train a net to locate a step function you can sound directly.

### Ladder layout
- **Video MCS ∈ 0–5** (1SS HT rungs; clamp, not user-set buckets).
- **Rungs 6,7 reserved as probe headroom** — never carry video. Their job is to
  guarantee that even at max video (V=5) there are two rungs above to sound.
- **Probe rung is RELATIVE and `V+2`-only** (not fixed at 6,7, and we do **not**
  probe `V+1`). Fixed top probes go blind the moment the cliff `C ≤ 5` — exactly
  the rate-limited regime where placement matters; relative probing tracks the
  boundary everywhere, and the 0–5 cap reserves the index room `V+2` needs at the
  top. **Why `V+2`-only is sufficient** (locked 2026-06-08): `+2 clean` → promote;
  `+2 fail while video healthy` → conservative demote-1. The only thing a `V+1`
  probe would add is the *magnitude* of a deep demote (state 3 vs 4) — and that is
  carried by reactive **video PER**, which can drop multiple rungs at once. So the
  `+1` column in the state table below is **analytical, not probed**; video PER
  substitutes for it on the demote side. Dropping `V+1` is the "minimal control
  logic" win.

### Three signals, three axes (all free over the existing uplink)
RSSI is already forwarded up; probe PER demuxes off the **same** rx_ant tunnel
(ground `wfb_rx -Y` → ground `wfb_tx -u` → vehicle `wfb_rx` → `link_controller`
:6600; WCMD is just another tenant on that pipe). So carrying all three to the
selector costs no extra transport — productionizing is teeing probe JSON into
`wfb_tx -u` + demuxing a `"type":"probe"` record. No new back-channel protocol,
and no reciprocity bug (we already pick downlink MCS from ground-measured
*downlink* RSSI).

| signal | nature | job |
|---|---|---|
| **video PER @ V** | free, continuous, full-rate, FEC-calibrated | **fast/deep demote** — the probe-saturation floor (states 3–4) |
| **probe PER @ V+2** (only) | binary go/no-go, ~1–2 s window | **cliff location** — promote + conservative pre-emptive demote (states 1–2) |
| **RSSI (downlink)** | continuous *gradient* | **temporal glue** — fade-rate tells you if the 1–2 s-old probe is still trustworthy; triggers re-probe; fallback when the probe stream drops |

RSSI's unique contribution is *probe staleness / fade rate*, which neither the
binary probe nor the video PER provides. (Its **absolute** value stays
distrusted — the first walk-around carried full traffic at −85 dBm while the old
thresholds sat at −70/−50.)

### Adversarial state map (V = video rung, C = highest viable rung)
| state | +1 | +2 | truth | action | probe enough? |
|---|---|---|---|---|---|
| **1** `C ≥ V+2` | ✓ | ✓ | ≥2 rungs headroom | promote (climb toward V=C−2) | ✅ |
| **2** `C = V+1` | ✓ | ✗ | +1 margin, **video healthy** | pre-emptive demote 1 (restore +2 cushion) | ✅ |
| **3** `C = V` | ✗ | ✗ | current rung **at cliff** | demote now | ⚠️ video PER for urgency |
| **4** `C < V` (fast overshoot) | ✗ | ✗ | **already over, frames dropping** | demote ≥2 now | ❌ **probe blind — video PER only** |

States 3 and 4 read **identically** to the probe (`+1✗ +2✗`): the probe
saturates at its lower edge and cannot tell "just touched the cliff" from "blew
through it." State 4 — a fast multi-rung fade (yaw null, body block, TX dropout)
that drops `C` several rungs inside one probe window — is why **video PER stays
mandatory** as the demote backstop.

### Decision rules
- **Promote (slow, but probe-hastened).** A clean **+2** probe is ground truth
  that `V+1` is viable with a rung to spare → step up to `V+1`. Because the probe
  *measures* (vs. the noisy RSSI guess the old hysteresis was padding against),
  the long up-dwell can be **shortened** — keep a short dwell only to damp
  flapping, not to second-guess a clean probe. *(+2 clean ⟹ +1 clean by
  monotonicity, so the step is guaranteed safe.)*
- **Demote (quick, two triggers):**
  1. **Reactive** — video PER @ V spikes → demote immediately (state 3/4; fast,
     deep, FEC-calibrated). Unchanged from shipped behavior; this is the safety
     net the probe cannot replace.
  2. **Pre-emptive (conservative)** — **+2 fails while video is still healthy**
     (state 2) → demote one rung to restore the +2 cushion. This is the cheap
     conservatism: you give up ~1 rung of throughput to buy **one rung of fade
     absorption** (a sudden 1-rung `C` drop then leaves you still viable, zero
     frame loss) and to demote *while healthy* instead of *while bleeding*.
- Keep the existing **quick-to-demote / slow-to-promote** asymmetry; the probe
  only *hastens* promote, it does not invert the asymmetry.

### Cadence (locked 2026-06-08) — two clocks, decoupled
- **Reactive demote = rx_ant cadence, no floor.** Fires per video-`wfb_rx -Y`
  datagram (event-driven), so its latency = the video `wfb_rx -l` rate. Run `-l`
  fast (target ≤0.5 s; **20 Hz / 50 ms ideal** if the uplink carries it) — this is
  the only thing that catches *rapid* degradation (state 4). The probe window does
  **not** gate it.
- **Probe window / promote / pre-emptive-demote = 0.5 s** default (`probe_window_s`,
  tunable). Halving the window halves packets/window; pair with `probe_pps` (push to
  ~40 pps to keep ~20 samples per 0.5 s window if resolution suffers). The sharp
  cliff makes ~10 samples enough for go/no-go.
- The "20" in "20 pps probe" is a **packet** rate, not an emit rate — distinct from
  the 0.5 s decision window and from the rx_ant `-l` rate. Three independent clocks.

### What changes in `link_controller`
- **Remove** `bucket_from_rssi()` and the `mcs.mcs_bucket_{0,1,2}` /
  `rssi_thresh_low|high` selector tunables. RSSI is no longer a selector.
- **Add** the relative-probe consumer (`"type":"probe"` records → `per_at_V+1`,
  `per_at_V+2`), the promote rule (probe-gated, short dwell), and the
  pre-emptive-demote rule (+2-fail). Keep the reactive video-PER demote path.
- **Config** shrinks to: `mcs_min/mcs_max` (default 0/5), reserved probe rungs
  (6,7), promote dwell, demote hysteresis, and RSSI fade-rate threshold for
  probe-staleness/re-probe. No per-bucket MCS map to hand-tune.

### Data collection — now only validates the RSSI augment
Shipping the core law needs **no model and no deeper capture** — the probe is
self-sufficient for promote/demote. The remaining use for the telemetry
pipeline is narrow and offline: **calibrate the RSSI fade-rate / staleness
thresholds** (how fast an RSSI slope must be before a 1–2 s probe is treated as
stale) and **validate monotonicity under motion/multipath** (`§9`). The latter
is the one result that could force a change: if frequency-selective fading
breaks strict monotonicity, boundary-only probing needs widening — but the +2
conservative margin already hedges a single-rung violation. Tier-1 model + Gemma
Tier-2 are **not** in the control path; Gemma stays an optional decoupled
"why we dropped" WebUI annotator.

---

## The wfb_ng constraint that frames everything

wfb_ng injects raw 802.11 frames at whatever MCS the **TX (air unit)** writes
into the radiotap header; the ground `wfb_rx` only receives. There is no free
"peek at MCS+1" — to actually observe PER at a higher MCS you must *transmit*
some frames there, and on a single-stream video link every probe frame at a
too-high MCS is potentially lost video. That cost rules out steady-state
exploration schemes (Minstrel-style samplers spend ~10% of airtime probing —
far too much here). The strategy must therefore be **predict-first, probe only
to confirm/recalibrate at a decision boundary**.

## Core model: RSSI → likely MCS cliff

The "cliff" for an MCS is the RSSI below which that MCS's PER explodes
(uncorrectable loss). With enough data sessions we learn, per MCS, where that
cliff sits, and at runtime pick the **highest MCS whose predicted PER is under
budget** at the current RSSI.

### Why RSSI, and the role of SNR (privileged training signal)

The PER cliff is fundamentally an **SNR** phenomenon — a modulation needs a
certain signal-*to-noise* ratio to decode. RSSI (signal power) is only a proxy,
valid **as long as the noise floor matches what we saw in training**.

- **Live:** SNR is not reliably reported, so the deployed predictor runs on
  **RSSI** (plus packet counters and trend features).
- **Training:** SNR *is* available. We use it as **privileged information** —
  train *with* it, infer *without* it (a LUPI-style setup):
  1. **Label the true cliff cleanly.** SNR gives a sharp, low-scatter cliff per
     MCS; learn the RSSI→cliff mapping against those clean SNR-defined labels.
  2. **Size the safety margin.** The residual scatter of RSSI around the
     SNR-defined cliff *is* the noise-floor uncertainty. That number sets how
     much RSSI headroom to demand before trusting an MCS.
  3. **Sanity-check drift.** Where live SNR *is* present, compare to the
     RSSI-predicted SNR to estimate the current noise-floor offset.

So: **SNR teaches, RSSI decides, the probe corrects.**

### The noise-floor blind spot (why the probe is mandatory, not optional)

RSSI cannot see the noise floor. When live interference raises it, the same
RSSI sits at a lower effective SNR and the cliff arrives *earlier* than the map
predicts. Under RSSI-only this is the dominant error source — and the only thing
that closes it is active probing. The probe is therefore load-bearing here, not
a nice-to-have.

## The lookahead probe: confirm + online recalibration

Before committing a **step-up**, transmit a brief burst at the candidate MCS and
measure its PER. The probe does double duty:

1. **Confirm** the predicted MCS survives *right now* before committing.
2. **Recalibrate.** Each probe yields a fresh `(RSSI, MCS, pass/fail)` sample
   that says where the cliff *actually* is in the current environment — slide
   the whole RSSI→cliff map up/down to track the invisible noise-floor drift
   between training and now.

Probe budget is cost-aware: a short burst only when near a step-up boundary,
never a continuous fraction of airtime.

## Step-down vs step-up asymmetry

These two decisions are not symmetric and must not share a path:

| direction | trigger | data needed | latency |
|-----------|---------|-------------|---------|
| **step DOWN** | live PER at current MCS spikes (`pkt.lost`, `dec_err`, `fec_recovered` climbing) | none extra — current traffic already measures it | **fast / reactive** — link is dying |
| **step UP** | RSSI prior says headroom exists | RSSI map + confirm probe | **lazy** — headroom isn't urgent |

RSSI-only uncertainty is tolerable for the lazy up-decision; it is *not* what you
bet a falling link on. The down-decision needs neither RSSI nor a probe — the
current MCS's own PER already tells you.

## RSSI-specific requirements

- **Combine the chains.** Stats arrive per antenna (`ant.0/1/100/101`). With
  diversity, effective performance tracks the *best/combined* chain, not any
  single antenna. Model input = combined/max RSSI across chains + the spread —
  never raw per-antenna values a `0.0` fill could poison.
- **Hysteresis / dwell.** RSSI is a noisier predictor than SNR, so widen the
  margin and require dwell time at a level before switching, to stop MCS
  flapping around the cliff.

## Decision flow

```
            ┌───────────────── every record (~10 Hz, Tier-1, C) ─────────────────┐
            │                                                                     │
  rx_ant ──►│  combine chains → rssi_comb, rssi_spread                            │
            │  live PER at current MCS = f(pkt.lost, dec_err, fec_recovered)      │
            │                                                                     │
            │   PER spiking? ──yes──► STEP DOWN now (reactive, no probe)          │
            │        │no                                                          │
            │        ▼                                                            │
            │   RSSI map says MCS+k has margin (≥ safety)? ──no──► HOLD           │
            │        │yes                                                         │
            │        ▼                                                            │
            │   lookahead PROBE at MCS+1 ──fail──► HOLD + recalibrate map down    │
            │        │pass                                                        │
            │        ▼                                                            │
            │   STEP UP + recalibrate map                                         │
            └─────────────────────────────────────────────────────────────────────┘
                         │ only on rare escalations (<1% volume)
                         ▼
              Tier-2 Gemma (think=false): human-readable "why we dropped 2 steps"
```

## Data requirements (what the capture campaign must produce)

The model is only as good as the coverage of the cliffs:

- **Sweep RSSI/SNR across the full range at *each* MCS** — not just healthy-vs-bad
  at one rate. You need records around the *knee* of every MCS's PER curve.
- Each record must carry: `mcs`, per-chain `rssi` **and** `snr` (train-time),
  packet counters for PER (`all/uniq/lost/fec_recovered/dec_err`), and enough
  context to compute trends (RSSI slope, rolling loss-rate, time-since-loss —
  HANDOFF #2).
- **Label cliffs from SNR**, then learn RSSI→cliff against those labels.
- Capture across environments (noise floors) so recalibration is exercised, not
  just one clean range walk.

`gen_sample.py` currently hard-codes `mcs:3`; to support this it must emit
MCS-swept RSSI/SNR with realistic per-MCS cliffs, and SNR must be droppable to
simulate the RSSI-only inference path. (Tracked in HANDOFF.)

## Open questions (confirm before building the actuator)

- **Per-packet / fast MCS switching:** can the air unit change MCS quickly and
  cheaply? If a switch is disruptive/slow, lean harder on prediction and switch
  rarely; if cheap, occasional step-up probes are viable.
- **Command transport:** how does the ground decision reach the air unit
  (MAVLink? wfb tunnel? other)? The Tier-1 output vocabulary must match.
- **FEC coupling:** does FEC (k/n) change with MCS? If so, the "PER budget" is
  really a *post-FEC* budget and the cliff labels must account for it.
- **Probe mechanics:** how is a probe burst injected at a different MCS without
  disrupting the video stream, and what burst size gives a usable PER estimate?
