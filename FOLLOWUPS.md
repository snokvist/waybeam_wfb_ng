# gs_supervisor / link_controller — deferred polish

PR #45 (`feature/gs-supervisor-host-x86` → `claude/simplify-wfb-deployment-CkSHL`)
landed the GS supervisor, WCMD/CSA/scan support, the WCMD `record` key, and
seq-burst dedup.  This file captures items pulled out of that review for a
future cleanup pass — none are blocking, but each has a clear deliverable.

## WCMD / link_controller

- **Spaced WCMD burst across FEC blocks.** `wcmd_emit()` currently sends
  `WCMD_BURST_FRAMES=3` copies back-to-back through a single socket.  All
  three land in one `wfb_tx` FEC block, so a single block loss still drops
  the command.  Move to a `supervisor_tick`-driven scheduler at ~20 ms
  cadence (mirror the CSA burst pattern in `gs_supervisor.c:1271-1287`)
  so the copies span multiple blocks.
- **Three sources of truth for `WFB_FEC_TIMEOUT_KEEP`.** Defined in
  `link_controller.c`, `shm-input.patch` (`tx_cmd.h`), and
  `fec_controller/wfb_control.py`.  Lift into a shared header — the
  comment at `link_controller.c:160-171` already calls this out.
- **`cmd.wfb_iface[16]` should use `IFNAMSIZ`.** Magic 16 at
  `link_controller.c:447`.
- **`wcmd_dispatch` blocks the rx_ant socket reader for ~30–80 ms (iw)
  or ~300 ms (wfb_get_*).** Worst case one rx_ant stats sample is
  dropped per WCMD.  Move long-running dispatches to a worker thread or
  fork-and-reply if WCMD volume grows.
- **`coalesced_burst` counter exposed in `/cmd/status` JSON only — not
  surfaced in the WebUI** ([gs.html](webui/gs.html)).  Add a row to the
  WCMD card so operators can see burst-dedup in action.

## gs_supervisor

- **Stale CSA reference in comment.**  `gs_supervisor.c:1024-1026` points
  at `poc/csa/csa.c` / `poc/csa/PROTOCOL.md`; the vehicle CSA actually
  lives in `link_controller`'s CSA subsystem.
- **Drain swallows packets that should re-emit downstream.**
  `scan_apply_step_drained()` at `gs_supervisor.c:1213` reads the
  listener fd directly without re-emitting to `stats_fwd_addr`.  If
  `stats_out` is wired to push rx_ant to a downstream consumer, those
  samples are lost during scan dwells.
- **CSA opens a fresh UDP socket per burst frame** —
  `csa_send_commit_frame()` at `gs_supervisor.c:1124`.  5 sockets in
  100 ms.  Reuse a persistent socket on the supervisor.
- **`scan_apply_step_drained` clears `step_saw_traffic = false` twice**
  (`gs_supervisor.c:1208,1220`).  The post-drain reset is dead code.
- **Scan response buffer `body2[480]` is tight** for `SCAN_MAX_STEPS=32`
  with 4 long-named ifaces.  `gs_supervisor.c:2877` — bump to 768 or
  check `bp` against cap.
- **`run_system_cmd` still uses `system()`** with strings from JSON
  config (existing TODO at `gs_supervisor.c:1378-1384`).  Convert to
  fork+exec like the iw helpers from this PR.
- **`-Wformat-truncation` on `g_scan.hts[i]` copy** at line ~2969
  (cross-build only, gcc 13 on aarch64 is stricter).  `hts[i]` is
  `char[8]` and the source is the same size, but the compiler can't
  prove it.  Add an explicit length guard before the copy.
- **gs_supervisor.c is approaching 4000 lines.** Split into
  `gs_supervisor_csa.c`, `gs_supervisor_scan.c`, `gs_supervisor_http.c`
  before adding more endpoints.
- **Per-iface txpower preset list (5/15/20/25 dBm) is hardcoded twice:**
  once in `cmdQuick(...)` quick-fire buttons (`gs.html:404-407`) and
  once in `renderGsTxpowerRow`'s `presets[]` (`gs.html:489`).  Lift to
  a JS constant.

## WebUI (gs.html)

- **`populate*Select` rebuild on every refresh** — preserves selection
  but blows away listeners and DOM nodes wastefully.  Move to a diff
  rather than full rebuild.
- **`set_radio` form doesn't pre-populate from the live cache.**
  Operator has to read the cache and type.  Auto-fill the inputs from
  `t.radio` on tunnel selection.
- **No abort/timeout on `fetch()`.** Long-blocking calls (200 ms
  wfb_cmd, iw fork) can pile up if the user spams a button.
- **`csa-prev-ht` falls back silently** if iface state reports e.g.
  `HT80` — the `<select>` only knows HT20/HT40+/HT40-.
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
