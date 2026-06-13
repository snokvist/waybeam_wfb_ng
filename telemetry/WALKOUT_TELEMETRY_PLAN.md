# Walkout telemetry + link-tuning — open actions

Working tracker for the range-walk telemetry effort (PR #59 track). Not a
roadmap — tactical list so nothing gets lost between sessions. Tick + date
items as they land; fold finished design into memory/specs.

## Build

- [x] **Phase 1 — GS downlink → SQLite (`stats_tap`).** Additive passive tap;
      `capture_session.sh` ingester in `system.up/down`. Device-validated at
      10 Hz. (PR #59 `998973b`.)
- [x] **Phase 2a — vehicle log importer + SSH-pull (MVP).**
      `import_vehicle_session.py` maps a vehicle `status.jsonl` (1 Hz
      link_controller state) onto the `records` schema (ts_ms=`up`×1000 →
      RTC-immune; mcs/rssi/per from score+mcs; raw_json kept) as a
      `vehicle-uplink` session tagged `notes=vehicle=<seq>`.
      `import_vehicle_session.sh <ip> [seq|latest]` pulls the session dir over
      the mgmt link post-walk (zero flight burden) and imports it. Validated by
      importing walk `000002` (session 3, RSSI −88..0 incl. the blackout).
- [ ] **Phase 2b — in-band filename + uptime announce (precise alignment).**
      MVP correlates by filename but each session keeps its own relative
      timeline. To overlay uplink vs downlink on one time axis, have the vehicle
      announce `{session_seq, uptime_s}` over the back-channel (tiny; e.g. a
      WCMD GET or a field in the existing stream); GS stamps arrival wall-clock
      → one `(gs_ts ↔ vehicle_up)` anchor → exact map. **Touches the frozen
      WCMD wire protocol** (`shared/wcmd_proto.h`, `make test`, vendored
      copies) — scope as its own change.
- [ ] **Phase 2c — GS ingester session per walk.** The orphaned ingester
      currently spans gs_supervisor restarts as ONE `sessions` row (pidfile
      guard makes `start` a no-op). Roll the GS session when a new vehicle
      `session_seq` appears (ties GS↔vehicle 1:1), or restart the ingester on
      each `system.up`. (Confirm against review findings.)
- [ ] **Capture the probe (downlink PER) into the DB too?** Only the video
      tunnel raw rx_ant is tapped today. The probe stream is `{type:probe}` —
      decide: ingest as its own session/source, or leave to the back-channel.
- [ ] **Flask UI autostart (optional).** Launched by hand today
      (`telemetry/.venv/bin/python webui/webapp.py --db wfb.sqlite --port 8090`).
      Wire into a systemd unit / gs_supervisor hook if we want it always-on.
- [ ] **PR #59 scope check.** Branch now spans peek + both walkout loggers +
      monotonic naming + 1 Hz sync + `stats_tap` + ingester + webui + vehicle
      importer. Decide: land as one, or split telemetry-capture into its own PR.

## Improvements (queued — some pending the integration review)

- [ ] **Auto-fill objective session metadata.** GS sessions open blank; have
      `capture_session.sh` pass `--channel/--tx-power/--antenna-cfg` (from the
      GS config) to the ingester so each session is self-describing. Subjective
      fields (scenario/location/weather/notes) stay manual via the UI editor.
- [ ] **MCS chart fallback for vehicle sessions.** The stacked MCS bar is fed by
      `raw_json.ant[]` (downlink only); vehicle-uplink records have no `ant`, so
      their `mcs` (current_mcs) doesn't render. In `/series`, when `ant` is
      absent fall back to a single-rung count from the denormalised `mcs` so the
      bar shows the vehicle's active MCS over time.
- [ ] **Session-list auto-refresh.** `/` (index) record counts are static —
      add a light poll so a growing live capture updates without a manual reload.

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
