# gs_supervisor / link_controller — deferred polish

PR #45 (`feature/gs-supervisor-host-x86` → `claude/simplify-wfb-deployment-CkSHL`)
landed the GS supervisor, WCMD/CSA/scan support, the WCMD `record` key, and
seq-burst dedup.  This file captures items pulled out of that review for a
future cleanup pass — none are blocking, but each has a clear deliverable.

The first batch (everything ticked below) shipped in
`feature/followups-batch-1`; the open list is what's still outstanding.

## WCMD / link_controller

- **Spaced WCMD burst across FEC blocks.** `wcmd_emit()` currently sends
  `WCMD_BURST_FRAMES=3` copies back-to-back through a single socket.  All
  three land in one `wfb_tx` FEC block, so a single block loss still drops
  the command.  Move to a `supervisor_tick`-driven scheduler at ~20 ms
  cadence (mirror the CSA burst pattern in `gs_supervisor.c:1271-1287`)
  so the copies span multiple blocks.
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

- ~~Stale CSA reference in comment.~~  Updated to point at
  `vehicle/csa/csa.c` and `vehicle/csa/PROTOCOL.md`.
- **Drain swallows packets that should re-emit downstream.**
  `scan_apply_step_drained()` reads the listener fd directly without
  re-emitting to `stats_fwd_addr`.  If `stats_out` is wired to push
  rx_ant to a downstream consumer, those samples are lost during scan
  dwells.
- ~~CSA opens a fresh UDP socket per burst frame.~~  Replaced with
  process-lifetime persistent socket (`g_csa_send_fd`).
- ~~`scan_apply_step_drained` clears `step_saw_traffic = false` twice.~~
  Dead second reset removed.
- ~~Scan response buffer `body2[480]` is tight for `SCAN_MAX_STEPS=32`
  with 4 long-named ifaces.~~  Bumped to 768.
- **`run_system_cmd` still uses `system()`** with strings from JSON
  config (existing TODO at `gs_supervisor.c:1378-1384`).  Convert to
  fork+exec like the iw helpers from this PR.
- ~~`-Wformat-truncation` on `g_scan.hts[i]` copy.~~  Replaced
  `snprintf("%s", src)` with explicit `strnlen`/`memcpy`.
- **gs_supervisor.c is approaching 4000 lines.** Split into
  `gs_supervisor_csa.c`, `gs_supervisor_scan.c`, `gs_supervisor_http.c`
  before adding more endpoints.
- ~~Per-iface txpower preset list (5/15/20/25 dBm) hardcoded twice.~~
  Lifted to `TXPOWER_PRESETS_MBM` JS constant; both rows
  (`renderQuickTxpowerRow` + `renderGsTxpowerRow`) consume it.

## WebUI (gs.html)

- **`populate*Select` rebuild on every refresh** — preserves selection
  but blows away listeners and DOM nodes wastefully.  Move to a diff
  rather than full rebuild.
- **`set_radio` form doesn't pre-populate from the live cache.**
  Operator has to read the cache and type.  Auto-fill the inputs from
  `t.radio` on tunnel selection.
- **No abort/timeout on `fetch()`.** Long-blocking calls (200 ms
  wfb_cmd, iw fork) can pile up if the user spams a button.
- ~~`csa-prev-ht` falls back silently if iface state reports `HT80`.~~
  Added HT80 option and a toast when iface state contains an unknown
  width (defaults to HT20 with operator notification).
- **Stats columns are still per-100 ms-interval, not running totals.**
  This PR fixed the misleading `kB` label by dropping it; if operators
  want a "total bytes sent this session" view, the supervisor needs to
  track running sums.

## Architectural

- **API handlers are synchronous.** `wfb_cmd` round-trips and `iw`
  forks block the entire event loop for 30–300 ms.  Unblock via either
  a worker thread or a fork-and-reply pattern.  Most painful with
  multiple operators on the WebUI hitting buttons concurrently.
- **No request-level timeout in `api_handle()`.** The slow-client
  deadline added in this PR closes idle TCP connections, but a client
  that sends a complete request and then triggers a slow `wfb_cmd`
  (200 ms) or `iw txpower` (~80 ms) handler is not bounded — relies on
  upstream `recv` timeouts.
