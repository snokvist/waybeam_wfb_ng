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

1. **`fetch()` abort/timeout in gs.html** — MED gain · LOW effort · webui.
   Current spam-the-button piles unbounded `wfb_cmd` (200 ms) + `iw` (~80 ms)
   handlers; an `AbortController`-backed `fetch()` wrapper plus a 1 s
   default timeout caps the operator-side queue.
2. **No request-level timeout in `api_handle()`** — MED gain · LOW effort
   · `gs_supervisor.c`.  Slow-client guard already times out idle TCP,
   but a client that sends a clean request and then waits for a slow
   `wfb_cmd`/`iw txpower` handler is currently unbounded.  Add a per-request
   deadline that returns `408` if the handler exceeds it.
3. **Auto-fill `set_radio` form from live cache** — MED gain · LOW-MED
   effort · `gs.html`.  Operator currently has to read `t.radio` and type.
   Pre-populate inputs on tunnel selection, leave the operator's
   subsequent edits sticky.
4. **Spaced WCMD bursts across FEC blocks** — HIGH gain · MED effort ·
   `gs_supervisor.c`.  3 redundant copies currently land in one FEC block;
   single block loss still drops the command.  Move to a `supervisor_tick`
   scheduler at ~20 ms cadence so copies span multiple blocks (mirror the
   CSA burst pattern).  Real RF resilience win.
5. **`run_system_cmd` → fork+exec** — MED gain · MED effort ·
   `gs_supervisor.c:1378-1384`.  `system()` over JSON-config strings is
   the last shell-injection surface in the daemon.  Convert to fork+exec
   like the iw helpers from PR #45.
6. **Drain re-emits to `stats_fwd_addr`** — LOW-MED gain · LOW-MED effort
   · `scan_apply_step_drained()`.  rx_ant samples are silently swallowed
   during scan dwells if `stats_out` is wired to a downstream consumer.
7. **`populate*Select` diff vs full rebuild** — LOW gain · MED effort ·
   `gs.html`.  Preserves selection but blows away listeners and DOM nodes
   on every 1 s tick.  Switch to keyed-diff updates.
8. **`wcmd_dispatch` async / worker** — MED gain · HIGH effort ·
   `link_controller.c`.  Long-running keys (`iw` 30–80 ms, `wfb_get_*`
   ~300 ms) block the rx_ant socket reader; one stats sample dropped per
   WCMD worst case.  Move to a worker thread or fork-and-reply.
9. **Running-totals stats columns** — LOW gain · MED effort · `gs.html`.
   Per-100-ms-interval values are fine for live tuning; running totals
   would let operators see "this session sent X MB."  Needs supervisor-side
   running sums + a UI toggle.
10. **Async API handlers (architectural)** — HIGH gain · HIGH effort ·
    `gs_supervisor.c`.  The big one — unblock the event loop end-to-end so
    multiple operators hitting buttons concurrently don't stall each
    other.  Fork-and-reply or worker pool; either way it's a rewrite of
    the api_handle dispatch.
11. **Split `gs_supervisor.c` (≈4000 LOC)** — MED gain · HIGH effort.
    Carve into `gs_supervisor_csa.c` / `_scan.c` / `_http.c` before adding
    more endpoints.  Pure maintainability play; no behaviour change.

## Notes for next session

The first batch (everything ticked below) shipped in PR #47
(`feature/followups-batch-1`); the descriptive lists below are kept
verbatim so the original wording, file:line references, and rationale
stay searchable.

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
