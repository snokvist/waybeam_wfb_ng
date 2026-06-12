# Slimming the config surface for public distribution

Status: PROPOSAL (audit of the post-PR#58 tree — unified PER-probe +
RSSI-guard law, bucket FSM removed)

PR #58 already deleted the bucket FSM, `mcs.mode`, 15 tunables and 13
CLI flags.  This doc audits what is *left* on both daemons and proposes
the public-distribution surface, on the principle that going all-in on
the PER+RSSI law means the law owns the radio: every knob that exists
only to second-guess it is a support liability, not a feature.

## 1. Inventory (current)

| Surface | Count | Where |
|---|---|---|
| vehicle live tunables (`/set`, `/schema`, WebUI Tune tab) | **77** (3 common + 29 fec + 25 mcs + 20 cmd) | `link_controller.c` `TUNABLES[]` |
| vehicle CLI flags | **~50** | `usage()` |
| vehicle WebUI Tune tab | all 77 (auto-rendered from `/schema`) | `vehicle/webui/index.html` |
| S99wfb shell vars + fw_env | 20 vars + 3 env (`wfbmode`/`wfbprobe`/`wfblog`) | `vehicle/init/S99wfb` |
| GS config JSON | ~15 tunnel keys × N tunnels + system.up/down free-form shell | `ground/config/*.json` (89 lines for the reference host) |
| GS WebUI | 4 tabs; Link tab alone has 2 manual radio/FEC editors + 7 quick-fire rows | `ground/webui/gs.html` |

Most of the 77 tunables are **algorithm mechanics** (EWMA alphas,
dwells, cooldowns, hysteresis, pacing) that were necessary while the
law was being designed and bench-tuned.  Post-PR#58 they have
device-validated defaults and no remaining operator story.

## 2. Classification policy

Three buckets:

- **PUBLIC** — a flyer with no knowledge of the law has a reason to
  touch it (policy, not mechanics).
- **EXPERT** — needed during the test cycle / for cross-hardware
  bring-up; hidden from the default UI but still reachable.
- **FREEZE** — compile-time constant; delete the registry row.

Recommended mechanism for EXPERT: add one `expert` bit to
`TunableDesc`.  `/schema` returns PUBLIC rows only; `/schema?all=1`
returns everything.  The WebUI Tune tab auto-slims with zero JS work,
`/set` still accepts every key (walkout logs + curl keep full access),
and nothing is deleted mid-test-cycle.  Hard-FREEZE comes *after* the
public test cycle tells us which expert knobs nobody touched.

## 3. Vehicle tunables: 77 → 8 public (+4 calibration-pending)

### PUBLIC (8)

| Key | Why an operator touches it |
|---|---|
| `fec.enabled` | pin manual FEC |
| `fec.bitrate_min_kbps` / `fec.bitrate_max_kbps` | power/heat/recording budget — pure policy |
| `fec.safety_margin` | the one "aggressiveness" knob (fraction of PHY rate usable) |
| `mcs.enabled` | pin manual MCS |
| `mcs.mcs_min` / `mcs.mcs_max` | hardware/regulatory rate envelope |
| `mcs.failsafe_timeout_s` | deployment-dependent (uplink parity ratio — see S99wfb comment); cannot be frozen |

### Calibration-pending (4) — PUBLIC until the walkout cycle, then revisit

`mcs.rssi_floor_dbm`, `mcs.rssi_fade_db_per_s`, `mcs.rssi_fade_arm_dbm`,
`mcs.rssi_floor_hyst_db` — PR #58's open checkbox: bench RSSI ≈ −9 dBm
never armed the guards, so the defaults are *uncalibrated*.  These are
exactly what the walkout loggers exist to calibrate.  Keep them visible
through the test cycle; freeze (or collapse into a single
near/medium/far sensitivity preset) once field data lands.

### EXPERT (everything else), notable members

- **Probe-law thresholds** `probe_clean_milli` / `probe_fail_milli` /
  `demote_per_milli`, `promote_dwell_s` — the heart of the law, but
  device-tuned on one combo (Infinity6E + RTL88x2).  A public test
  cycle across other adapters may need them — that argues EXPERT, not
  FREEZE.
- **Flap damping** `reentry_backoff_s` / `reentry_dwell_s`,
  `down_cooldown_s` — tuned in `fa5b95e`-era harness; mechanics.
- **Filters/pacing** `rssi_ewma_alpha`, `loss_ewma_alpha`,
  `rssi_aggregator`, `oscillation_window_s`, `probe_window_s`,
  `probe_stale_age_s`, `probe_feed_pps`, `probe_feed_bytes` — pure
  mechanics; `probe_feed_pps` was device-tuned in `507de12`.
- **FEC mechanics** (all of: `ewma_alpha`, `headroom_*`,
  `ppf_deadband_frac`, `k_hyst_up`, `cooldown_up_s`, `k_*_dwell_s`,
  `startup_grace_s`, `bitrate_tolerance/grace/lead`, `mcs_settle_s`,
  `boost_*`, `min/max_k`, `min/max_n`, `mtu`, `payload_min`,
  `skip_on_backpressure`, `backpressure_fill_pct`) — the AIMD gating
  design is settled (CLAUDE.md); nobody retunes this in the field.
- **`mcs.mcs_start`** — fold into `mcs_min`: boot-failsafe (`de33adf`)
  already drops to `mcs_min` when no GS is present, and the cold climb
  reaches cruise in <4 s, so a separate start rung buys nothing.
- **`mcs.probe_enabled`** — fold into `mcs.enabled`: post-PR#58 the
  probe is mandatory for MCS; a live "MCS on but probe off" state is a
  promote-starved trap, not a feature.
- **All 20 `cmd.*` keys** — WCMD guard bounds + allow mask.  Sanity
  rails, not operator policy.  EXPERT now, FREEZE candidates later.
- **`common.*`** — `api_port` is CLI-only in practice; `dry_run` /
  `verbose` are developer tools → EXPERT.

## 4. Vehicle CLI: ~50 → ~16 flags

Keep **wiring/topology only** (things that must be right before the
HTTP API exists); delete every flag that merely mirrors a tunable —
`/set` is the tuning path:

```
--wfb --api-port --dry-run -v --iface-mtu
--no-fec --sidecar --venc
--no-mcs --stats --probe --probe-feed --wfb-iface
--failsafe                      (deployment-dependent, set by S99wfb)
--safe-startup-bitrate          (inherited-venc-state bridge, per-camera)
--csa-iface
```

Deletions: the 13 FEC tuning flags (`--mtu --min-k --max-k
--ppf-deadband --k-hyst-up --cooldown-up --k-down-dwell --k-up-dwell
--startup-grace --safety --bitrate-* --boost-* --mcs-settle-s
--bitrate-lead-s`), the 6 MCS tuning flags (`--rssi-alpha --loss-alpha
--aggregator --down-cooldown --osc-window` + the 3 RSSI guard flags —
guards stay tunable via `/set`, the *flags* go), `--wfb-stats-port`,
`--max-payload`/`--no-safe-startup`/`--safe-startup-payload` (jumbo +
safe-startup details → compile defaults; revisit if a public user
actually runs jumbo), and the 4 CSA detail flags (`--csa-allowlist
--csa-bandwidth --csa-cooldown-ms --csa-no-revert` → frozen defaults;
the GS CSA UI is the operator surface).

Default `--probe 127.0.0.1:8001` / `--probe-feed 127.0.0.1:5750` /
`--stats 0.0.0.0:5801` so the S99wfb invocation shrinks to flags that
carry real information.

## 5. S99wfb: 20 vars → 6

Operator-meaningful, keep: `WFB_CHANNEL`, `WFB_HTMODE`, `WFB_BW`,
`WFB_TXPOWER`, `KEY`, `WFB_FAILSAFE_S`.

Freeze into the script body: `WFB_MCS`/`WFB_K`/`WFB_N` (boot
placeholders only — the controller rewrites all three within ~2 s, and
boot-failsafe covers the no-GS case), link ids, ports, probe
link/port/ctrl/feed, `SAFE_BITRATE`, pidfile paths.

fw_env knobs: keep `wfbmode` + `wfblog`; flip `wfbprobe` default to
**1** for public distribution (probe = the law's promote path; FEC-only
boot stays available as the opt-out).

## 6. GS supervisor

### Config: synthesize the standard topology

Post-PR#58 every deployment runs the *same three tunnels* (video rx,
probe rx, uplink tx) whose link ids/ports must match S99wfb anyway.
Hand-writing them is 80 lines of foot-guns — the reference config
carries warning comments about NM races and the probe `udp_out`≠5600
trap precisely because humans keep stepping on them.

Proposal: a `"profile"` mode —

```json
{
  "key_file": "/etc/drone.key",
  "http": { "bind": "0.0.0.0", "port": 80 },
  "profile": {
    "ifaces": ["wlxA", "wlxB"],
    "uplink_iface": "wlxB",
    "channel": 161,
    "txpower_mbm": 2000,
    "wfb_bin_dir": "/usr/local/bin"
  }
}
```

→ supervisor synthesizes the three tunnels (canonical link ids 207/50/208,
probe `udp_out` on a dead port by construction, stats fan-in to the
uplink) **and** the `system.up`/`down` iw/ip sequences.  The raw
`tunnels[]` + `system{}` form stays as the advanced override — the
parser already exists; profile mode is a config-expander in front of it.
89 lines → ~12, and the channel lives in *one* place on the GS.

### WebUI: cut the Link tab down to switches

- **Tunnels / Channel / Encoder tabs: keep as-is.**  Lifecycle, CSA
  hop, scan, GS txpower, bitrate/fps/idr/record quick-fires are real
  operator workflows the law does not own.
- **Link tab — remove the manual radio editors.**  The "Uplink wfb_tx
  Get/Set FEC + Get/Set Radio" card (stbc/ldpc/sgi/vht/mcs/bw number
  grid) and the "Video wfb_tx" quick-fire grids (k/n/mcs/bw/ldpc/sgi/
  stbc rows) exist to hand-drive exactly what the unified law now
  drives.  Keep on the tab: the adaptive ON/OFF strip and the vehicle
  txpower row.  The generic-WCMD form on the Encoder tab already covers
  every removed key for emergencies (and B1 write-through means such
  writes are adopted, not reverted) — one escape hatch instead of
  twenty buttons.
- Net: gs.html loses ~250 lines of markup + the `ctrlGetFec/ctrlSetFec/
  ctrlGetRadio/ctrlSetRadio` JS handlers (~80 lines).

REST API: no removals needed — `/api/v1/tunnels/<t>/control` keeps
serving the generic form and scripts.  The API is not the liability;
the wall of buttons is.

## 7. What must NOT be slimmed yet

1. **RSSI guard knobs** — uncalibrated until the walkout/induced-fade
   cycle (PR #58's open item).  Freezing them now would bake in bench
   guesses.
2. **`mcs.failsafe_timeout_s` / `--failsafe`** — provably
   deployment-dependent (uplink parity vs rx_ant burst loss).
3. **Probe-law thresholds as EXPERT, not FREEZE** — single-device
   tuning so far; the public test cycle across adapters is exactly when
   they might need to move.
4. **Generic WCMD escape hatch + B1 write-through** — this pairing is
   what makes deleting the manual radio UI safe.

## 8. Phasing

**Phase 1 — before the public test cycle** (no behavior change, pure
surface reduction):
1. `expert` bit in `TunableDesc`; `/schema` filters, `/schema?all=1`
   full — vehicle Tune tab auto-slims 77 → 12 rows.
2. CLI cull to ~16 flags + port defaults; S99wfb prune; `wfbprobe`
   default 1.
3. Fold `mcs_start`→`mcs_min`, `probe_enabled`→`enabled`.
4. gs.html Link-tab cut.
5. GS profile-mode config synthesis.

**Phase 2 — after the walkout/field cycle:**
6. Freeze calibrated RSSI guard defaults (or collapse to a sensitivity
   preset).
7. Hard-delete EXPERT rows that the test cycle never touched; drop the
   matching `Config` fields.

Rough sizing for Phase 1: −200 LOC CLI parsing/usage, −330 LOC gs.html,
−77 lines reference config (+~150 LOC profile expander), S99wfb −40
lines of vars/comments.  The tunables registry itself barely changes in
Phase 1 (one bit per row) — by design, so the test cycle keeps full
reach while the public sees 12 knobs instead of 77.
