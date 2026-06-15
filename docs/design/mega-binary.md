# Mega binary — single-file deployment (busybox model)

Status: **implemented and running on hardware** (2026-06-15). Phases 0–3 are
in-tree; `wfb-gs` (x86) links + dispatches, and `wfb-air` cross-links to a
device-compatible armv7 binary that runs as the live wfb stack on the vehicle
(192.168.1.13) — all four roles (video-tx, uplink-rx, probe-tx, link) from the
one binary, risk #3 verified. Phase 4 packaging is the only remaining item.

## Motivation

Today each side deploys several executables:

- **Ground**: `gs_supervisor`, `wfb_rx`, `wfb_tx`, `wfb_tx_cmd`, `wfb_keygen`.
- **Vehicle**: `link_controller`, `wfb_tx` (video), `wfb_tx` (probe), `wfb_rx`
  (uplink), plus `wfb_keygen` for provisioning.

Shipping and version-matching that set of files is the deployment pain we want
to remove. The goal is **one file to copy/flash per side**, with the same
runtime behavior.

## Decision: multi-call binary, not a monolith

There are two readings of "single binary":

1. **Multi-call (busybox) binary** — one executable; the role is selected by
   `argv[0]` or a leading subcommand. Each role still runs as its **own
   process**; the supervisor keeps `fork()/exec()` but re-execs
   `/proc/self/exe <applet> …` instead of a separate binary name.
2. **True monolith** — everything in one address space, roles as threads,
   loopback UDP replaced by direct calls.

We choose **(1)**. Rationale:

- **No global-state collisions.** `rx.cpp` and `tx.cpp` both define identical
  file-static globals (`g_stats_udp_fd`, `g_stats_seq`, `g_stats_udp_dst`,
  `g_stats_udp_enabled`); the supervisor and CSA/scan code carry their own
  singletons (`g_csa`, `g_scan`, WCMD seq/counters). Separate processes keep
  these isolated for free.
- **Signal / SIGCHLD model is untouched.** The supervisor still reaps real
  children (`gs_supervisor.c` `supervisor_reap`, `waitpid`).
- **CSA's blocking `iw` calls** (50–300 ms) stay in their own process and
  cannot stall the live-video path.
- **The vendored wfb-ng fork stays a near-verbatim patch** — only `main()`
  gets renamed under an `#ifdef`, so `shm-input.patch` and the peek patch
  remain easy to rebase.
- The only thing the monolith would buy over this is eliminating localhost UDP
  hops, which are not a measured bottleneck.

### Scope: one binary per side, not one universal binary

The daemons are C (`cc`); the wfb-ng tools are C++ + libsodium (and libpcap for
`rx`). The two sides also target different architectures (vehicle armv7l vs.
ground x86_64/aarch64). The `rx`/`tx` **source** is shared, but a linked
artifact cannot span both architectures. So the deliverable is:

| Binary    | Target              | Applets                                    |
|-----------|---------------------|--------------------------------------------|
| `wfb-gs`  | ground x86 / aarch64| `supervisor`, `rx`, `tx`, `tx_cmd`, `keygen` |
| `wfb-air` | vehicle armv7l      | `link`, `tx`, `rx`, `tx_cmd`, `keygen`     |

## How dispatch works

A shared dispatcher (`multicall/wfb_multicall.c`, compiled into both binaries
with a per-side applet table) does:

```
main(argc, argv):
    name = applet token from argv[1], else basename(argv[0])
    shift argv past the applet token
    call <applet>_main(argc, argv)   # runs exactly one applet, then exit
    no match -> print usage (applet list)
```

Because each invocation runs exactly one `*_main()` in its own process,
`getopt`/`optind` and all file-static state start fresh every time — there is
no need to reset anything between applets.

The supervisor (`gs_supervisor.c` argv build at ~`:834-899`, binary lookup at
`:836`) and the vehicle `init/S99wfb` are changed to launch
`/proc/self/exe <applet> …` instead of separate binary names. Detection is by
build flag (`WFB_MULTICALL`) or by checking the `basename` of
`/proc/self/exe`.

## The `WFB_MULTICALL` contract

- With `WFB_MULTICALL` **unset** (the default), every source builds exactly as
  today and produces the existing separate binaries. The mega build is opt-in.
- With `WFB_MULTICALL` **set**, each applet TU exposes `*_main()` and its real
  `main` is excluded:
  - **C daemons**: wrap `main` in `#ifndef WFB_MULTICALL`, provide
    `gs_supervisor_main()` / `link_controller_main()` otherwise.
    `link_controller.c` already has the `LC_TEST_NO_MAIN` guard (used by
    `tests/test_selector_law.c`) — generalize that pattern.
  - **wfb-ng C++ tools** (`rx.cpp`, `tx.cpp`, `tx_cmd.c`, `keygen.c`): these
    are cloned from upstream and patched at build time (`wfb-ng/build-*.sh`),
    not stored in-tree. Rather than maintain a third rebaseable patch just to
    rename `main`, the mega target renames it at the **compile line** with
    `-Dmain=wfb_rx_main` / `-Dmain=wfb_tx_main` / etc. This touches no upstream
    source and only applies to the mega build, so the existing
    separate-binary builds stay byte-identical.
- Applet entry points called from the C/C++ dispatcher boundary are declared
  `extern "C"`. The merged binary links with `g++`.

## Implementation phases

### Phase 0 — De-`main` the applets (mechanical, no behavior change)

Guard each tool's `main` and expose `*_main()` as above. Acceptance: with
`WFB_MULTICALL` unset, all existing build paths (`make`, `make vehicle`,
`make -C ground cross`, `wfb-ng/build-*.sh`) produce unchanged binaries.

**Status: in-tree C daemons done.**
- `vehicle/link_controller.c` — `WFB_MULTICALL` selects `link_controller_main`;
  default keeps `main`; the existing `LC_TEST_NO_MAIN` test path still excludes
  both. Verified by `nm` across all three modes; `test_selector_law` passes.
- `ground/gs_supervisor.c` — `WFB_MULTICALL` selects `gs_supervisor_main`;
  default keeps `main`. Verified by `nm` and a default link.

The C++ tools (`rx`/`tx`/`tx_cmd`/`keygen`) need no source change — they are
renamed via `-Dmain=` on the mega compile line in Phase 2/3.

### Phase 1 — Dispatcher

Add `multicall/wfb_multicall.c` + a per-side `applets.h` selecting the linked
`*_main` symbols. Dispatcher is C++ (so it can link the C++ TUs directly);
`extern "C"` on the C applet entries.

**Status: done.**
- `multicall/wfb_multicall.h` — `struct wfb_applet { name, alias, fn, help }`
  and the per-side `extern const struct wfb_applet wfb_applets[]` (NULL-term).
- `multicall/wfb_multicall.cpp` — generic `main()`; dispatches by
  `basename(argv[0])` (busybox-style symlink/exec name), then by leading
  subcommand, else prints the applet list (rc=2). Compiled as C++.
- `ground/gs_applets.cpp`, `vehicle/air_applets.cpp` — per-side tables. The
  C daemon entries (`gs_supervisor_main`, `link_controller_main`) are declared
  `extern "C"`; the wfb-ng C++ entries (`wfb_rx_main`, `wfb_tx_main`) as plain
  C++ (mangled name matches the `-Dmain=` rename); the wfb-ng C tools
  (`wfb_tx_cmd_main`, `wfb_keygen_main`) `extern "C"`. The wfb-ng entries are
  gated behind `WFB_WITH_WFBNG`, which Phase 2/3 defines once those objects are
  linked in — so Phase 1 builds and routes with just the in-tree daemons.

Verified: daemon-only `wfb-gs` / `wfb-air` link with `g++`; routing confirmed
for no-args/unknown (usage, rc=2), subcommand (`supervisor -h` rc=0,
`link --api-port 70000` rc=1), and `basename`-based symlink dispatch
(`gs_supervisor`, `link_controller`).

### Phase 2 — Ground binary (`wfb-gs`)

- New `ground/Makefile` targets (`make mega` / `make mega-cross`) linking the
  dispatcher + `gs_supervisor*.c` + `rx/tx/tx_cmd/keygen/zfex/wifibroadcast/`
  `radiotap/venc_ring/peek` objects with `-DWFB_MULTICALL`, libsodium +
  libpcap, via `g++`.
- Factor wfb-ng object compilation into a shared make fragment so flags aren't
  duplicated between `build-*.sh` and the ground Makefile.
- Change `tunnel_spawn` argv to prepend `/proc/self/exe rx|tx` in multicall
  mode.

Acceptance: `wfb-gs supervisor -c config.json` brings up tunnels that exec
`wfb-gs rx` / `wfb-gs tx`; REST API + stats fan-out identical to today;
`make test` (wire-format conformance) green. Fully validatable on x86 host.

**Status: DONE — full link + dispatch validated on the x86 dev host (2026-06-15).**
- `ground/gs_supervisor.c` `tunnel_spawn` — under `WFB_MULTICALL`, the child
  re-execs `/proc/self/exe` with argv[0] forced to `wfb_rx`/`wfb_tx` so the
  dispatcher routes by basename (any configured `t->binary` is ignored).
  Default builds keep `execvp`. Compiles clean in both modes.
- `ground/Makefile` `make mega` — links dispatcher + `gs_supervisor*.o` +
  wfb-ng objects (`rx/tx/tx_cmd/keygen` renamed via `-Dmain=`, plus
  `peek/zfex/wifibroadcast/radiotap/venc_ring`) into `wfb-gs` with `g++`,
  `-DWFB_MULTICALL -DWFB_WITH_WFBNG`, libsodium + libpcap.
- The single binary uses real libpcap throughout (rx needs it; tx never calls
  pcap, so the stub header is unnecessary in the merged build).

**Validated:** the host had `libsodium` 1.0.18, `libpcap`, and a patched wfb-ng
tree already at `wfb-ng/build/wfb-ng/src` (`venc_ring.{c,h}` present via sibling
`waybeam_venc`), so `make -C ground mega` linked `wfb-gs` (331 KB) directly.
The GATE (risk #1) passed with **no** `multiple definition` — the shm/peek
patch globals are correctly `static`. The one real failure was risk #2: see
below (fixed). Dispatch verified for all five applets: bare usage list,
`supervisor -h`, `rx`/`tx` usage (shows patched `-H` shm flag), `keygen`
(wrote drone.key/gs.key), and basename symlink (`wfb_tx_cmd` → version
`shm-patched-mega`).

### Phase 3 — Vehicle binary (`wfb-air`)

- New `vehicle/Makefile` target linking dispatcher + `link_controller.c
  csa/csa.c` + cross `rx/tx/tx_cmd/keygen/…` objects, `-DWFB_MULTICALL`,
  `-lm` + libsodium + cross libpcap, via cross `g++`. `tx_cmd` (the runtime
  control client) is included so on-device FEC/radio commands stay available
  without a second binary.
- Update `vehicle/init/S99wfb` to call `wfb-air tx …`, `wfb-air rx …`,
  `wfb-air link …`.
- Note: vehicle `tx` uses the stub pcap (inject-only) but `rx` (uplink) needs
  real cross libpcap (already built in `wfb-ng/build-armv7.sh`). The merged
  link must use real libpcap.

Acceptance: cross-build succeeds; `S99wfb` launches all four roles from one
binary; `make test` + `vehicle` host build + `test_selector_law` pass.

**Status: DONE — armv7 cross-link produced + device-compatible (2026-06-15); on-hardware run pending.**
- `vehicle/Makefile` `make mega` — mirrors ground: dispatcher + `link_controller.o`
  + `csa.o` + wfb-ng objects (renamed via `-Dmain=`) → `wfb-air`, linked with
  the cross `g++` (`CROSS_CXX`), `-lpcap -lsodium -lm`. The Makefile default
  `TOOLCHAIN ?= ../toolchain/toolchain.sigmastar-infinity6e` already resolves
  (through the `toolchain` symlink → sibling `waybeam_venc`) to the OpenIPC
  Buildroot gcc 13.3.0 `arm-openipc-linux-gnueabihf-g++` — no override needed.
- The cross libs that `build-armv7.sh` prepares are reused as-is:
  `wfb-ng/build/sodium-install` (libsodium 1.0.18, SONAME `.so.23`) and
  `wfb-ng/build/pcap-install` (libpcap 1.10.5, linked **static** — so `wfb-air`
  has no `libpcap` runtime dep).
- The cross link command (run from the repo root):
  ```bash
  make -C vehicle mega \
    MEGA_CFLAGS="-I../wfb-ng/build/sodium-install/include -I../wfb-ng/build/pcap-install/include" \
    MEGA_LDFLAGS="-L../wfb-ng/build/sodium-install/lib -L../wfb-ng/build/pcap-install/lib -lpcap -lsodium -lm"
  ```
  produced `vehicle/build/wfb-air` — ELF 32-bit ARM EABI5, stripped, 397 KB.
  `readelf -d` NEEDED = `libsodium.so.23, libstdc++.so.6, libm.so.6,
  libgcc_s.so.1, libc.so.6` — a **superset of the proven standalone `wfb_tx`**
  (adds only `libm`, which `link_controller` already links), so it runs
  wherever `wfb_tx`/`link_controller` already do. (A host-arch sanity build
  `CROSS_CC=gcc CROSS_CXX=g++` also links + routes all five applets, useful for
  testing dispatch logic off-target.)
- `vehicle/init/S99wfb` — adds a mega-detection block: when `wfb-air` is on
  PATH the launch tokens become `wfb-air tx|rx|link` and the `pidof`/`killall`
  process-name list collapses to `wfb-air` (all applets share that comm name);
  `status` reports the shared binary. Falls back to the standalone binaries and
  the original behavior when `wfb-air` is absent, so the change is a no-op for
  the multi-binary deployment. `sh -n` clean.

Same dependency caveat as Phase 2 for the final link (libsodium/libpcap +
`venc_ring` from the sibling repo). Note `wfb-air rx` (uplink) needs real
libpcap; `wfb-air tx` never calls pcap, so the single binary links real
libpcap once and the stub header is unnecessary.

### Phase 4 — Packaging & docs

- Top-level `make mega` builds both.
- Update `CLAUDE.md` (Repository layout + Build & test).
- Keep separate-binary builds as the default until the mega build is proven on
  hardware.

## Risks / watch-items

1. **C/C++ link & `extern "C"`** — the one real integration point; resolved by
   linking with `g++` and `extern "C"` on applet entry points.
2. **libpcap on the merged binary** — ground previously built `wfb_rx`
   *natively* against system libpcap while `wfb_tx` used a stub pcap header;
   the merged link must use one real libpcap. Confirm the cross libpcap covers
   `rx`'s capture path on each target.
3. **Patch-chain maintenance** — the rename patch must rebase cleanly on top of
   `shm-input.patch`; keep it minimal and `#ifdef`-gated.
4. **`/proc/self/exe` availability** — fine on the Linux targets here;
   documented as an assumption.

## Suggested sequencing

Phase 0 → 1 → 2 (ground first, x86 host — fastest to validate) → 3 (vehicle,
needs cross toolchain + hardware) → 4.

## Handoff — current state & how to finish (read this first)

Branch: `claude/hopeful-fermat-2rtbfd`.

### Done (in-tree, compile-verified)
- **Phase 0** — `main()` guarded behind `WFB_MULTICALL` in
  `vehicle/link_controller.c` (`link_controller_main`) and
  `ground/gs_supervisor.c` (`gs_supervisor_main`). Default builds unchanged
  (verified with `nm`); `test_selector_law` passes.
- **Phase 1** — `multicall/wfb_multicall.{h,cpp}` dispatcher +
  `ground/gs_applets.cpp` / `vehicle/air_applets.cpp` tables. Daemon-only
  `wfb-gs`/`wfb-air` link and route (usage / subcommand / symlink-basename).
- **Phase 2/3** — `make mega` in `ground/Makefile` and `vehicle/Makefile`;
  `ground/gs_supervisor.c` `tunnel_spawn` self-execs `/proc/self/exe` under
  `WFB_MULTICALL`; `vehicle/init/S99wfb` routes through `wfb-air` when present.
- **Full link validated (2026-06-15, x86 dev host).** `make -C ground mega`
  linked `wfb-gs` and `make -C vehicle mega CROSS_CC=gcc CROSS_CXX=g++` linked
  a host-arch `wfb-air`; both route all five applets. Default (non-mega) builds
  unchanged and `make test` green. The only fix needed was the rx/tx `-Dmain=`
  mangling mismatch (risk #2 below). Reproduce on this host:

  ```bash
  make -C ground  mega                                          # wfb-gs (x86)
  make -C vehicle mega CROSS_CC=gcc CROSS_CXX=g++ CROSS_STRIP=strip  # wfb-air (host-arch)
  ./ground/build/wfb-gs                # → usage listing applets
  ./ground/build/wfb-gs supervisor -h
  ```

### On-hardware run — DONE (2026-06-15, vehicle 192.168.1.13)
`wfb-air` (armv7) was installed to `/usr/bin`, the mega-aware `S99wfb` deployed,
and the stack restarted. Result: `pidof wfb-air` → 4 PIDs, all `comm=wfb-air`
(video-tx `-C 8000 -H local_shm`, uplink-rx `wlan0`, probe-tx `-C 8001`, link).
`S99wfb status` correctly enumerates the shared-comm-name PIDs; the restart
produced exactly 4 processes (the double-start guard's stop-first ran), so
**risk #3 holds**. `link_controller` HTTP (:8765) responds, SD logger active,
`wlan0` monitor TX live, peek `close` mode confirmed in `wfb.log`. The cross
link command that produced the deployed binary:

```bash
# Cross libs are already prepared under wfb-ng/build by build-armv7.sh; if
# absent on a fresh checkout, run it once first.
make -C vehicle mega \
  MEGA_CFLAGS="-I../wfb-ng/build/sodium-install/include -I../wfb-ng/build/pcap-install/include" \
  MEGA_LDFLAGS="-L../wfb-ng/build/sodium-install/lib -L../wfb-ng/build/pcap-install/lib -lpcap -lsodium -lm"
```

Note: `S99wfb` exposes only `wfbmode` as a boot-env knob; the peek profile,
probe, and SD-logger toggles are fixed script constants (`WFB_PEEK_PROFILE`,
`WFB_PROBE`, `WFB_LOG`) edited in the script, not `fw_setenv` overrides. (A
stale device `wfbpeek=1` env from before this change is simply ignored.)
Rollback is `rm /usr/bin/wfb-air` (S99wfb auto-falls back to the standalone
binaries, which remain in place) + restoring the `S99wfb` backup.

### Review risk checklist (verify in this order)
1. **(GATE) Link-time symbol collisions across `rx.o`/`tx.o`/tools.** ✅
   **PASSED (2026-06-15).** Merging two `main()`-bearing C++ programs into one
   binary fails to link if any **non-`static`** global or free function shares a
   name between `rx.cpp` and `tx.cpp` (the shm/peek patches add `g_stats_udp_fd`,
   `g_stats_seq`, `g_stats_udp_dst`, `g_stats_udp_enabled` to *both*). The
   ground+vehicle mega links produced **no** `multiple definition` — those
   globals are already `static` in the patch chain. No fix needed. If a future
   patch reintroduces a clash, mark the offending symbol `static` /
   anonymous-namespace **in the patch chain** (`shm-input.patch` / `peek.patch`),
   not the Makefile, then bump the vendored patch.
2. **`-Dmain=` mangling contract.** ⚠️ **HIT and FIXED (2026-06-15).** The
   pinned upstream declares `int main(int argc, char *const argv[])`, so the
   `-Dmain=`-renamed symbol mangles as `wfb_rx_main(int, char* const*)`
   (`_Z…iPKPc`) — **not** the `(int, char**)` (`…iPPc`) the dispatcher first
   assumed, giving `undefined reference to wfb_rx_main(int, char**)`. Fixed in
   `ground/gs_applets.cpp` / `vehicle/air_applets.cpp`: declare the rx/tx entries
   with their true `(int, char *const *)` signature and forward through a small
   thunk (`char** → char* const*` converts implicitly at the call) so they still
   fit the table's `(int, char**)` fn type — no upstream patch, no
   function-pointer cast/UB. The C tools (`tx_cmd`/`keygen`) are `extern "C"`, so
   their `char**` vs `char* const*` difference is irrelevant (no mangling).
3. **`S99wfb` process management under mega.** ✅ **VERIFIED (2026-06-15,
   .13).** All four applets share comm name `wfb-air`; `pidof wfb-air` returns
   all four PIDs and `S99wfb status` enumerates them correctly. The restart
   produced exactly four processes (not eight), so the double-start guard's
   stop-first path ran — busybox `pidof`/`killall` match comm as expected.
4. **Strip parity** — vehicle `make mega` strips, ground does not (cosmetic).

### Deferred cleanup (after the link is proven)
- The two `mega` Makefile blocks are ~40 near-identical lines; hoist the wfb-ng
  object rules into a shared `wfb-ng/mega.mk` included by both.
- **Phase 4 packaging** (not started): top-level `make mega` building both
  sides; update `CLAUDE.md` (Repository layout + Build & test) to mention
  `multicall/` and the mega binaries. Keep separate-binary builds the default
  until mega is proven on hardware.
- Deployment must install the `wfb-air`/`wfb-gs` binary; `S99wfb` already
  auto-routes when `wfb-air` is on PATH (no symlinks required, but busybox-style
  symlinks `wfb_rx`→`wfb-air` etc. also work via basename dispatch).
