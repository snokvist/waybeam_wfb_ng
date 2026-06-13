# Walkout telemetry + link-tuning — open actions

Working tracker for the range-walk telemetry effort (PR #59 track). Not a
roadmap — tactical list so nothing gets lost between sessions. Tick + date
items as they land; fold finished design into memory/specs.

## Build

- [ ] **Phase 2 — vehicle↔GS log correlation (lightweight).** No live log
      stream (it would encumber the link). Vehicle *announces its session
      filename* (now monotonic, e.g. `000003`) **+ its `uptime_s`** over the
      back-channel; GS records it, then **pulls the vehicle session dir over
      the management link post-walk at close range** and imports it into
      `wfb.sqlite` as a sibling `vehicle-uplink` session keyed by that filename.
      Time-align via a single `(gs_wallclock ↔ vehicle_up)` anchor captured at
      announce time → solves the vehicle RTC-reset (21:14) problem with no
      shared clock. Payoff: see **uplink (vehicle log, weak GS TX) vs downlink
      (GS capture, strong vehicle TX)** asymmetry directly.
- [ ] **Capture the probe (downlink PER) into the DB too?** Today only the
      video tunnel raw rx_ant is tapped (`stats_tap`). The probe stream is a
      different shape (`{type:probe}`) — decide whether to ingest it (own
      session/source) or leave it to the back-channel only.
- [ ] **Flask UI autostart (optional).** Currently launched by hand
      (`telemetry/.venv/bin/python webui/webapp.py --db wfb.sqlite --port 8090`).
      Wire into a systemd unit / gs_supervisor hook if we want it always-on.
- [ ] **PR #59 scope check.** Branch now spans peek + both walkout loggers +
      monotonic naming + 1 Hz sync + `stats_tap` + SQLite ingester. Decide:
      land as one, or split telemetry-capture into its own PR.

## Field (more walks — different kinds)

- [ ] **"Dwell at the edge" walk.** Hold at ~−80 dBm (marginal-but-alive) for
      30–60 s instead of walking straight through. Tests PER-probe
      promote/demote settling in the lossy band + whether **peek PROTECT keeps
      key/param frames alive** while bulk drops — watch the GS `mcs_hist` for
      protected frames landing on lower rungs.
- [ ] **Viewer-recovery observation (UNANSWERED).** After each blackout, does
      video recover cleanly (peek IDR protection + FEC_CLOSE) or stall waiting
      for a keyframe? Determines whether the diversity-reorder needs a
      viewer-side jitter-buffer mitigation.
- [ ] **Investigate the diversity collapse at the edge.** On walk `000002`,
      `adapter_count` dropped 2→1 right at the −87 dBm craters (−3 dB lost
      exactly when it mattered). Find why one GS adapter drops first
      (antenna/placement/orientation) — recovering edge diversity is the
      cheapest range win.

## Analysis / ML

- [ ] **Label real captures in the UI.** Once walk sessions land in
      `wfb.sqlite`, use the label tools to mark cliffs/events (e.g. "true cliff
      for MCS5", "should have stepped here") → feeds the Tier-1 active-learning
      loop (`wfb_active.py train|score|gemma`).

## Settled (reference — don't re-litigate)

- **Controller tuning: nothing to change.** Walk `000002` proved guards +
  pre-emptive demote + FEC boost + bitrate-vs-MCS all behaved correctly at the
  −85 floor (2.0 Mbps under a 6.5 Mbps MCS0 PHY ceiling). The blackout was a
  hard RF margin limit + diversity collapse, not a software fault. Range levers
  are **physical** (antennas, TX power), not controller knobs.
- **RSSI guard thresholds: leave as-is** (floor −85, fade arm −65, slope −10).
  They fired correctly for the first time on `000002`; revisit only on a
  false/late trigger.
- **SD durability:** 1 Hz `sync` shipped (vehicle). ext4 impossible (kernel has
  no ext support — would brick the mount); `-o sync` declined. Revisit only if
  we ever rebuild the kernel for journaling (low priority).
- **`stats_tap` is additive, never inline** — the DB is a passive logging tee;
  a dead ingester/DB can't affect `stats_out` or the vehicle link.
