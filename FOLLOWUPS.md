# gs_supervisor / link_controller — deferred polish

PR #45 landed the GS supervisor, WCMD/CSA/scan support, the WCMD `record`
key, and seq-burst dedup.  PR #47 (batch 1) shipped the high-leverage
small-effort items.  This file is the running TODO for what's left.

When you're asked "what's next", pull from **Next batch — sorted by
gain/effort** below.  The list is ordered top-down: pick from the top,
working downward until the gain runs out or the effort budget is hit.

## Next batch — sorted by gain/effort (best ratio first)

Each row is `<title> — gain · effort · where`.  All open items below the
section dividers are mirrored here so this list is the only thing you
need to read when planning the next PR.

PR #49 shipped items #1–#3 (fetchT abort/timeout in gs.html,
fork-waitpid deadlines in `gs_supervisor.c`, set_radio auto-fill from
live cache).  PR #50 dropped "Spaced WCMD bursts" (operator confirmed
that wfb_tx `-T 1` already closes FEC blocks at 1 ms idle, so the 3
back-to-back copies span blocks naturally) and shipped `run_system_cmd`
fork+execvp + scan-drain re-emit.  PR #51 split `gs_supervisor.c` into
four units (`_csa.c` / `_scan.c` / `_http.c` + a slimmer main file plus
`gs_supervisor.h`) ahead of further endpoint additions.

1. **`populate*Select` diff vs full rebuild** — LOW gain · MED effort ·
   `gs.html`.  Preserves selection but blows away listeners and DOM nodes
   on every 1 s tick.  Switch to keyed-diff updates.  (Also applies to
   `populateIfaceChecks` from PR #48.)
2. **`wcmd_dispatch` async / worker** — MED gain · HIGH effort ·
   `link_controller.c`.  Long-running keys (`iw` 30–80 ms, `wfb_get_*`
   ~300 ms) block the rx_ant socket reader; one stats sample dropped per
   WCMD worst case.  Move to a worker thread or fork-and-reply.
3. **Running-totals stats columns** — LOW gain · MED effort · `gs.html`.
   Per-100-ms-interval values are fine for live tuning; running totals
   would let operators see "this session sent X MB."  Needs supervisor-side
   running sums + a UI toggle.
4. **Async API handlers (architectural)** — HIGH gain · HIGH effort ·
   `gs_supervisor_http.c`.  The big one — unblock the event loop
   end-to-end so multiple operators hitting buttons concurrently don't
   stall each other.  Fork-and-reply or worker pool; either way it's a
   rewrite of the api_handle dispatch.  Now that HTTP is its own .c, the
   refactor is contained to that unit.
5. ~~**Split `gs_supervisor.c` (≈4000 LOC)**~~ — PR #51.  `_csa.c` /
   `_scan.c` / `_http.c` extracted; main file slimmed from 3998 to
   ~2170 LOC, with `gs_supervisor.h` carrying types + extern globals +
   cross-module prototypes.

## Notes for next session

  (The peek `--peek-short-tail` proportional-parity follow-up was retired: that
  mode, plus the NAL-aware idr/refpred profiles, was removed in PR #76. Peek now
  ships gate-close only.)

The first batch (everything ticked below) shipped in PR #47
(`feature/followups-batch-1`); the descriptive lists below are kept
verbatim so the original wording, file:line references, and rationale
stay searchable.

## link_controller (vehicle)

- ~~No first-class way to set the wlan iface MTU.~~  PR #48 adds
  `--iface-mtu N` (range [576, 9000]) which fork+execvp's
  `ip link set dev <iface> mtu N` against `{csa.iface, cmd.wfb_iface}`
  at startup.  Skipped under `--dry-run`.
- **Failsafe watchdog has no CSA-hop coupling (latent, same class as the
  backpressure spiral).** `selector_tick_no_data` demotes to `mcs_min` on
  rx_ant staleness > `failsafe_timeout_s`, but a CSA channel hop is a
  *deliberate* self-induced rx_ant dead-zone. Nothing gates the watchdog
  against an in-progress hop, so a slow re-acquisition would force the
  floor mid-hop (and the spurious SET_RADIO could race the channel
  switch). NOT currently biting: deployed `failsafe_timeout_s = 2.0` and
  measured hops re-acquire in ~118 ms (2026-06-10), and it self-recovers
  (re-climbs, not a lock-up) — so this is a robustness hardening, not the
  death-spiral the backpressure fix addressed. Fix: suppress the failsafe
  demote while `csa_st ∈ {ARMED, VERIFY}` (treat `t_switch`→`t_revert` as
  a watchdog grace window) and bump `last_datagram_us` on confirmed
  post-hop link-alive. Found by the 2026-06-13 feedback-trap sweep.
- **Post-CSA-hop RSSI step can spuriously arm the fade-demote (minor,
  mostly guarded).** The first well-spaced sample after a hop differences
  RSSI across the channel change → large false negative slope. Already
  blunted by the `dt > 2 s` slope reset + the `FADE_PERSIST_SAMPLES = 3`
  streak filter, leaving only the 0.5–2 s hop window marginally exposed.
  Cure with the same fix: force a slope re-baseline on the CSA `COMMITTED`
  transition. Found by the 2026-06-13 sweep.

## WCMD / link_controller

- ~~Spaced WCMD burst across FEC blocks.~~  Dropped May 2026: operator
  confirmed wfb_tx `-T 1` already closes a FEC block at 1 ms idle, so
  the 3 back-to-back copies span multiple blocks naturally.  No
  scheduler work needed.
- ~~Three sources of truth for `WFB_FEC_TIMEOUT_KEEP`.~~  Now lifted into
  `shared/wfb_control.h`; `link_controller.c` and `gs_supervisor.c` both
  pull it (and the opcodes) from there.  `shm-input.patch` (vendored
  fork) still carries its own copy by design.
- ~~`cmd.wfb_iface[16]` should use `IFNAMSIZ`.~~  Done.
- **`wcmd_dispatch` blocks the rx_ant socket reader for ~30–80 ms (iw)
  or ~300 ms (wfb_get_*).** Worst case one rx_ant stats sample is
  dropped per WCMD.  Move long-running dispatches to a worker thread or
  fork-and-reply if WCMD volume grows.
- ~~`coalesced_burst` counter exposed in `/cmd/status` JSON only — not
  surfaced in the WebUI.~~  Followup re-scoped: GS-side emit counters
  (`emit_total`, `emit_frames`, `rate_limited`, `emit_failed`) now live
  in `/api/v1/status` and are rendered in the gs.html WCMD card.  The
  vehicle-side coalesced counter is still vehicle-local (operator polls
  vehicle:8765/cmd/status directly).

## gs_supervisor

- ~~Empty `system.up`/`system.down` arrays were undocumented as the
  "trust host OS" mode.~~  PR #48 adds an explicit startup log line
  ("trusting host OS for iface bring-up …") and a `_comment_system`
  hint in `ground/config/example.json`.
- ~~Stale CSA reference in comment.~~  Updated to point at
  `vehicle/csa/csa.c` and `vehicle/csa/PROTOCOL.md`.
- ~~Drain swallows packets that should re-emit downstream.~~
  PR #50 mirrors the `stats_drain` re-emit pattern inside
  `scan_apply_step_drained()`: each pending datagram is re-sent to
  `stats_fwd_addr` if `stats_fwd_active` before being dropped locally.
  Local-side state is still skipped during dwells (channel-hopping
  counters are stale by definition) but downstream consumers no longer
  see a hole every step.
- ~~CSA opens a fresh UDP socket per burst frame.~~  Replaced with
  process-lifetime persistent socket (`g_csa_send_fd`).
- ~~`scan_apply_step_drained` clears `step_saw_traffic = false` twice.~~
  Dead second reset removed.
- ~~Scan response buffer `body2[480]` is tight for `SCAN_MAX_STEPS=32`
  with 4 long-named ifaces.~~  Bumped to 768.
- ~~`run_system_cmd` still uses `system()`~~  PR #50 replaces the
  `sh -c <cmd>` path with whitespace tokenization + fork+execvp, bounded
  by `GS_SYSTEM_CMD_DEADLINE_MS = 10000`.  Shell metacharacters
  (`;|&<>$`(){}[]*?'"\\`) are refused with an operator-friendly warning
  (wrap such commands in a script).  Existing host_x86 system.up
  commands all tokenize cleanly.
- ~~`-Wformat-truncation` on `g_scan.hts[i]` copy.~~  Replaced
  `snprintf("%s", src)` with explicit `strnlen`/`memcpy`.
- ~~**gs_supervisor.c is approaching 4000 lines.**~~  PR #51 splits it
  into `gs_supervisor.c` (entry, signal/log, JSON, config, tunnel
  lifecycle, stats, iface cache, wfb_cmd, WCMD, system commands,
  supervisor_tick), `gs_supervisor_csa.c`, `gs_supervisor_scan.c`, and
  `gs_supervisor_http.c`, with `gs_supervisor.h` carrying types, extern
  globals, and cross-module prototypes.  Main file dropped from 3998 to
  ~2170 LOC; HTTP unit is now ~1190 LOC with all 32 routes contained.
- ~~Per-iface txpower preset list (5/15/20/25 dBm) hardcoded twice.~~
  Lifted to `TXPOWER_PRESETS_MBM` JS constant; both rows
  (`renderQuickTxpowerRow` + `renderGsTxpowerRow`) consume it.
- ~~**`ant_count` is rung-inflated, not physical antennas.**~~  The `ant[]`
  walk counted every `{...}` object, but wfb-ng keys `antenna_stat` by
  `(freq, mcs_index, bandwidth, antenna_id)` (`wfb-ng rx.cpp:599`), so each
  physical chain appeared once per MCS rung present that window — `st_ant_count`
  floated 2/4/6 on a 2-chain single card as the rung mix shifted (downlink
  carries up to 3 at once: MCS5 bulk + MCS3 peek-PROTECT + MCS4 transitional).
  Same root cause already fixed on the link_controller scorer; gs_supervisor was
  never deduped.  Fixed: new `json_pick_str` helper + dedup the `ant[]` walk on
  the per-entry `"id"` (= `wlan_idx<<8|chain`), with a fallback to the raw object
  count if a pre-`id` `-Y` stream is seen.  `mcs_hist` still carries the per-rung
  breakdown.  Device-verified on 192.168.2.20 (2026-06-13): ant_count 4→**2**
  with 3 distinct rungs live.

## WebUI (gs.html)

- **`populate*Select` rebuild on every refresh** — preserves selection
  but blows away listeners and DOM nodes wastefully.  Move to a diff
  rather than full rebuild.  (Note: `populateIfaceChecks` from PR #48
  inherits the same full-rebuild pattern; same diff treatment applies.)
- ~~`set_radio` form doesn't pre-populate from the live cache.~~
  PR #49 adds `populateRadioFormFromTunnel()` driven from `t.radio` on
  tunnel-select change, with a `lastRadioFilled` guard so subsequent
  refreshes don't clobber operator edits.
- ~~No abort/timeout on `fetch()`.~~  PR #49 adds `fetchT()` —
  AbortController-backed wrapper with a 1 s default deadline; all 14
  call sites swapped.
- ~~`csa-prev-ht` falls back silently if iface state reports `HT80`.~~
  Added HT80 option and a toast when iface state contains an unknown
  width (defaults to HT20 with operator notification).
- ~~`<select multiple>` iface pickers need Ctrl+click and don't work
  on touch devices.~~  Replaced with checkbox lists (`.iface-checks`)
  in PR #48 — tap-friendly, ≥36 px row targets, multi-select.
- **Stats columns are still per-100 ms-interval, not running totals.**
  This PR fixed the misleading `kB` label by dropping it; if operators
  want a "total bytes sent this session" view, the supervisor needs to
  track running sums.

## Architectural

- **API handlers are synchronous.** `wfb_cmd` round-trips and `iw`
  forks block the entire event loop for 30–300 ms.  Unblock via either
  a worker thread or a fork-and-reply pattern.  Most painful with
  multiple operators on the WebUI hitting buttons concurrently.
- ~~No request-level timeout in `api_handle()`.~~  PR #49 bounds the
  three blocking `waitpid()` call sites driven by api_handle paths
  (`run_iw_set_channel`, `run_capture` (used by /api/v1/ifaces), and the
  inline `/system/txpower` iw fork) via `waitpid_deadline()` with a
  1 s default deadline.  `run_capture` also gained a `poll()`-driven
  read loop so the read side is bounded too.  `wfb_cmd` was already
  bounded by its own 200 ms recv timeout.
