# archive/c-poc/ — superseded standalone C daemons

These were intermediate C ports of the Python `fec_controller/` and
`mcs_selector/` modules, plus a separate `venc_proxy` skeleton.  All
three were consolidated into `vehicle/link_controller.c` (single
binary, single config, single rx_ant socket).

Kept in-tree because the algorithms were stress-tested in their
standalone form before being merged; the merge changed structure but
not semantics, so these still serve as an algorithmic reference.

## What's here

| File | Original purpose | Subsumed by |
|---|---|---|
| `fec_controller.c` | Adaptive FEC sizing daemon (poc) | `vehicle/link_controller.c` (FEC subsystem) |
| `mcs_selector.c`  | Adaptive MCS daemon (poc) | `vehicle/link_controller.c` (MCS subsystem) |
| `venc_proxy.c` | Standalone WCMD → venc HTTP proxy | `vehicle/link_controller.c` (WCMD dispatcher) |
| `Makefile.{fec_controller,mcs_selector,venc_proxy}` | Cross-build for armv7l | `vehicle/Makefile` |
| `fec_controller.py` | Original Python implementation (kept here as a thin wrapper / launcher) | `archive/python/fec_controller/` |

## Building (historical)

The Makefiles still reference the toolchain at the repo root
(`../toolchain/...`) which works as long as the developer-machine
symlink at `<repo>/toolchain` is in place.  These builds aren't
maintained though — no CI, and protocol changes since the merge
will surface as missing symbols if you try.

## Algorithmic reference

If you're touching the C FEC / MCS / WCMD code in
`vehicle/link_controller.c` and want to understand the original
design intent:

- **FEC sizing**: `fec_controller.c` has the cleanest expression of
  the EWMA + bounded-headroom + hysteresis logic.
- **MCS scoring**: `mcs_selector.c` shows the score function in
  isolation, before it was wrapped in the rx_ant subscriber loop.
- **WCMD proxy**: `venc_proxy.c` shows the bare WCMD dispatcher with
  no rate limiting / clamping / dedup — a useful "what's the
  minimum?" reference.

## Don't deploy this

The vehicle runs `link_controller.c` (one binary, one process).
Running these alongside it would cause two daemons to fight over
wfb_tx control socket and the venc HTTP API.
