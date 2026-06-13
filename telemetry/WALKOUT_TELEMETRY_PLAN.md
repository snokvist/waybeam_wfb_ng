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
- [x] **Phase 2b+2c — periodic LOG_SYNC markers (sync + identify in one).**
      DONE + device-validated on a real mini-walk (31 markers, fit slope
      1.00002 = ~19 ppm GS↔vehicle clock skew, clean). The GS emits
      `WCMD_KEY_LOG_SYNC{seq, gs_unix_seconds}` upstream every 10 s
      (`logsync_emit` in gs_supervisor, own counter so operator cmd stats stay
      clean), riding the existing WCMD uplink 3×-redundant. The vehicle's
      link_controller records `(seq, gs_unix_s, recv_uptime_s)` into `/status`
      as a `logsync` object (own burst-dedup so 3 copies → 1 marker; bypasses
      allow_keys_mask as infra; no HTTP/wfb side effect); the walkout logger
      mirrors it to SD at 1 Hz. Post-walk `import_vehicle_session.py` fits the
      `(recv_uptime_s ↔ gs_unix_s)` pairs into a line, stamps `gs_unix_ms` into
      every vehicle record's raw_json, and identifies the GS session by overlap
      (latest-started among overlappers — robust to orphaned open sessions).
      The record axis switched to `status.uptime_s` (sub-second, marker-aligned)
      from the 1 Hz outer `up`.
      - **Wire:** additive `WCMD_KEY_LOG_SYNC 18` in `shared/wcmd_proto.h` +
        vendored `ground/gs_supervisor.h`; `WCMD_NUM_KEYS` 17→18 in
        link_controller. `make test` (76) green — additive key, no struct change.
      - **Multi-session span (validated):** the GS realtime clock is continuous
        across an ingester roll, so the line fit (and every `gs_unix_ms`) stays
        correct even when a walk spans several GS sessions — e.g. a mid-walk
        "New capture session" press. Overlay should key on `gs_unix_ms`; the
        single-GS-session label is just the midpoint's. For the cleanest 1:1
        label, press "New capture session" BEFORE starting a walk.
- [ ] **Capture the probe (downlink PER) into the DB too?** Only the video
      tunnel raw rx_ant is tapped today. The probe stream is `{type:probe}` —
      decide: ingest as its own session/source, or leave to the back-channel.
- [ ] **Flask UI autostart (optional).** Launched by hand today
      (`telemetry/.venv/bin/python webui/webapp.py --db wfb.sqlite --port 8090`).
      Wire into a systemd unit / gs_supervisor hook if we want it always-on.
- [ ] **PR #59 scope check.** Branch now spans peek + both walkout loggers +
      monotonic naming + 1 Hz sync + `stats_tap` + ingester + webui + vehicle
      importer. Decide: land as one, or split telemetry-capture into its own PR.

## Done this session (review fixes + improvements)

- [x] **Live webui pause + zoom-safe refresh** (user request). The session view
      has a `live` toggle (pause) and HOLDS the 2 s poll while a label-select OR
      a manual zoom is active (`ZOOMED` flag via a setScale hook) — refresh never
      snaps a zoomed/selected region back. (`6063041`, `1e43337`.)
- [x] **Phantom ANT 4-7 fixed** (user report). link_controller deduped rx_ant by
      antenna id; ant_count is now distinct antennas (4), not entry count.
      Unit-tested + device-verified. (`06dcae3`.)
- [x] **C1 (critical)** `/series` 500 on malformed `ant[]` fixed.  **D** vehicle
      sessions show MCS via a `current_mcs` fallback.  **F** session-list
      auto-refresh.  **N2** stats_tap gated `!probe`.  **E** auto-filled session
      metadata (channel/tx-power/antenna).  (`1e43337`, `6ec36df`, capture commit.)
- [x] **Ingester shutdown + orphan reclaim (S2 + SIG_IGN).** Explicit signal
      handlers (a backgrounded ingester inherits SIG_IGN — `kill -INT` was a
      no-op, so sessions never closed and orphans were unkillable); `start` now
      reclaims the port and rolls a fresh session.

## Improvements (still queued)

- [ ] **git-ignore `wfb.sqlite` (it's now a runtime artifact).** The committed DB
      ships only the synthetic-demo session; live captures shouldn't churn it.
      Per DATASTORE.md "revisit git-ignore once it grows" — do it now: gitignore
      the DB + WAL/SHM, keep a `schema.sql`-built empty DB or a tiny seed.
- [ ] **Incremental `/series` (review N1).** The endpoint returns ALL rows and
      re-parses every `raw_json` for `mcs_dist` on every 2 s live poll — O(N) and
      rising (a 30-min walk re-parses ~18k rows each tick). Add `?since_id=` and
      append client-side, or cache mcs_dist per record.

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
