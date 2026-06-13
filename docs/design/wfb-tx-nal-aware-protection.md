# wfb_tx NAL-Aware Link Protection — Implementation Plan (waybeam_wfb_ng)

**Status:** Phase 1 plan, adapted for this repo.
**Upstream spec:** `waybeam_venc#143` —
`documentation/WFB_TX_NAL_AWARE_PROTECTION_SPEC.md`. That document is the
design rationale and the canonical match/action vocabulary; read it first.
**This document** re-targets that spec onto **`waybeam_wfb_ng`**, whose
`wfb-ng/shm-input.patch` already shifts the `wfb_tx` data path, control
protocol, and FEC-close mechanics away from stock `svpcom/wfb-ng`. Where the
spec and this repo disagree, **this document wins** for implementation.

> **Implementation status (this branch).** Phase 1 is implemented as
> `wfb-ng/peek.patch` (applies on top of `shm-input.patch`; grounded in the
> real cloned tree at `svpcom/wfb-ng` `af6ba85`). It adds `src/peek.{hpp,cpp}`
> (the classification engine), `src/test_peek.cpp` (26 host checks, all green),
> the `tx.cpp` PROTECT/DROP/FEC-close hooks on **both** the SHM and UDP paths,
> the per-level radiotap cache (rebuilt on `CMD_SET_RADIO`), and
> `CMD_SET_PEEK`/`CMD_GET_PEEK` + `wfb_tx_cmd peek` verbs. The three
> `wfb-ng/build-*.sh` scripts apply `peek.patch` and compile/link `peek.o`.
>
> **Operator surface (done):** the `-Y` tx_stats JSON carries a `peek` object
> (`enabled`, `drop`, `rules`, `pkts_dropped`, `fec_closes`). Two WCMD keys —
> `WCMD_KEY_PEEK_ENABLED` (16) and `WCMD_KEY_PEEK_DROP_ENABLED` (17),
> `shared/wcmd_proto.h` — tunnel ground→vehicle. `vehicle/link_controller.c`
> handles them via `wfb_set_peek()` (`CMD_SET_PEEK`, opcode 5 in
> `shared/wfb_control.h`) and logs peek activity from tx_stats.
> `ground/gs_supervisor.c` accepts `peek_enabled`/`peek_drop_enabled` on
> `/api/v1/cmd`, and the WebUI (Vehicle Control tab) has peek/drop ON/OFF
> buttons + dropdown entries.
>
> `CMD_GET_PEEK` reports `enabled`, `drop`, `n_rules`, `n_sig_rules`,
> `base_mcs`, `max_delta` (→ protect floor); `wfb_tx_cmd peek status` prints
> them and `link_controller` reads them back via `wfb_get_peek()` after each
> toggle. The `wfb-ng` clone is **pinned** to `af6ba85` in `build-armv7.sh` /
> `build-openwrt.sh` (shallow fetch-by-SHA).
>
> Verified on the host: peek unit test (26 checks) passes; `wfb_tx` +
> `wfb_tx_cmd` compile (`-Wall`) and **link** clean with `peek.o`;
> `link_controller` + `gs_supervisor` build (`-Wall -Wextra`); `make test`
> 58/58 green. Not yet done: on-target bench/loopback (§10.2-10.6) and the
> venc-repo `build_wfb_tx.sh` wiring.
>
> **Merge note:** the unmerged PR #58 rewrites `ground/webui/gs.html`,
> `ground/gs_supervisor.{c,h}`, and `vehicle/link_controller.c` (the same files
> this work touches). Expect conflicts; rebase this branch onto #58 once it
> lands rather than the reverse, since the peek edits are small and localized.

The design intent in `CLAUDE.md` — *"M-bit in wfb_tx aligns every frame's
final block, so block loss never contaminates adjacent frames"* — describes a
mechanism that **was implemented at one point and then removed**; the
`CLAUDE.md` and `docs/protocols/shm-input.md` references to it are **stale**.
What ships today in `shm-input.patch` is only the coarse `-T` idle timeout,
explicitly *"not a per-frame close"* (`docs/protocols/shm-input.md:164`). The
spec's **mandatory FEC_CLOSE signal re-introduces that per-frame close** as a
data-driven marker rule. Implementing this plan is what makes the `CLAUDE.md`
claim true again. (Stale-doc cleanup — the M-bit wording in `CLAUDE.md` and
`shm-input.md` — should land with the implementation, not before.)

---

## 0. TL;DR of the deltas vs the upstream spec

| Spec assumption (`svpcom/wfb-ng`) | This repo (`shm-input.patch`) | Impact |
|---|---|---|
| Peek runs after `recvmsg` in the UDP `data_source()` branch | **Vehicle video ingests from a SHM ring (`-H`)**; the hot path is the `venc_ring_read()` drain loop, not `recvmsg` | Peek hook moves into the SHM drain loop; factor a shared helper so the UDP branch (audio / non-SHM) gets it too |
| Frame-boundary FEC close is *new* but described against `-T` only | `-T` exists but is a **coarse idle timeout, not per-frame**; the per-frame "M-bit" close **was added then removed** (stale in `CLAUDE.md` / `shm-input.md`) | The mandatory FEC_CLOSE re-introduces the per-frame close; ship the stale-doc cleanup with it |
| `base_mcs` is a static CLI value | **`base_mcs` is dynamic** — link_controller's MCS selector writes `CMD_SET_RADIO` at runtime | PROTECT's prebuilt per-level radiotap cache must **rebuild on every `CMD_SET_RADIO`**, and `peek_cfg.base_mcs` must track it |
| `CMD_SET_FEC` union arm is `{k,n}` | This repo extended it to `{k,n,fec_timeout_ms}` (`tx_cmd.h`) | New `CMD_SET_PEEK=5`/`CMD_GET_PEEK=6` still slot in cleanly; mirror the `offsetof(...) + sizeof(arm)` send-sizing the repo already uses |
| "Add `peek.cpp` to Makefile/CMakeLists" | wfb_tx is built by **explicit per-file `g++` lines** in `wfb-ng/build-{armv7,aarch64,openwrt}.sh`, bypassing wfb-ng's own Makefile | Add a `peek.cpp` compile step + `src/peek.o` to the link line in **all three** build scripts; ship `peek.{hpp,cpp}` and the `tx.cpp`/`tx_cmd.*` edits inside `shm-input.patch` (or a companion `peek.patch` applied after it) |
| `--peek-rule` parsing in `peek.hpp` | same | unchanged |
| RTP marker is the frame-boundary signal | **Confirmed valid here** — SHM carries full RTP datagrams (`streamMode rtp`, slot = `maxPayloadSize + 12`, `docs/protocols/shm-input.md:98-99`) | marker-bit `BYTE_MASK{off=1,mask=0x80}` and NAL parsing apply byte-for-byte |
| `wfb_rx` needs no change | same — and the repo's RX already **suppresses synthetic FEC_ONLY slots** from `fec_recovered` (`shm-input.patch` rx.cpp hunk) | marker-close blocks are already RX-correct; nothing to do on the ground |
| waybeam encoder changes: none | **The `TRAIL_N`/`TRAIL_R` rewrite lives in the sibling `waybeam_venc` repo** (#142), not here | the `refpred` profile depends on that external rewrite being deployed; `idr` profile has no such dependency |

---

## 0.1 Verification status (read before implementing)

This plan mixes two kinds of claim. Treat them differently:

**Verified against this repo** (`shm-input.patch`, `vehicle/`, `docs/`,
`build-*.sh`, `shared/`):
- SHM drain loop shape, `SHM_DRAIN_MAX = 128`, the `t->send_packet(buf, rsize,
  0)` hook site (`shm-input.patch` SHM read path).
- `-T` is a coarse idle close, not per-frame (`docs/protocols/shm-input.md:164`).
- Existing FEC close via `while(t->send_packet(NULL,0,WFB_PACKET_FEC_ONLY))`
  (`CMD_SET_FEC`) and single close (`-T`); RX suppresses synthetic FEC_ONLY
  slots from `fec_recovered`. So RX already decodes early-closed blocks.
- `t->get_radiotap_header()` exists and returns the **live** header (the patch
  reads it for `-Y`; `shm-input.md` says it reflects applied `set_radio` values).
- `CMD_*` ids 1–4 in use; `cmd_set_fec`/`cmd_get_fec` already carry
  `fec_timeout_ms`; send-sizing uses `offsetof(cmd_*_t, u) + sizeof(arm)`.
- wfb_tx is linked by explicit `g++` lines in `build-*.sh` (no Makefile use).
- SHM carries full RTP datagrams (`streamMode rtp`, slot = `maxPayloadSize+12`).
- link_controller writes `CMD_SET_RADIO` **mcs_index-only**, other radio fields
  preserved from a startup `CMD_GET_RADIO` sync (`link_controller.c:5-16`).
- UDP video fallback is documented (`shm-input.md:242-256`).
- `-r` partial-block parity ratio exists (`shm-input.md:150`).
- `WCMD_KEY_FEC_ENABLED=12`/`WCMD_KEY_MCS_ENABLED=13` exist as the toggle precedent.

**Inherited from the upstream spec — NOT re-verified here.** `wfb-ng` source is
**not vendored**; `build-armv7.sh:184` does `git clone --depth 1
https://github.com/svpcom/wfb-ng.git`, so the following live in stock wfb-ng and
must be **re-confirmed against the actual cloned tree at implementation time**
(and `--depth 1` pins no revision — master can drift from what the spec read):
- the UDP `recvmsg` → `send_packet` call site (the *second* peek hook);
- the `CMD_SET_RADIO` runtime handler (where the radiotap rebuild hook lands);
- `init_radiotap_header(...)` signature, `radiotap_header_t` having no
  `MCS_IDX_OFF`, `inject_packet()` building `iovec[0]` from the header,
  `set_mark()` per-fragment carry, parity emit over `fragment_idx fec_k..fec_n`;
- the `waybeam_venc` `TRAIL_N`/`TRAIL_R` rewrite (#142) the `refpred` profile
  depends on (sibling repo, not this one).

**Action:** pin the wfb-ng clone to a known SHA in `build-*.sh` before
implementing, so the spec's symbol-level claims stay valid across rebuilds.

---

## 1. Where the peek actually runs (the big one)

In `shm-input.patch`, `data_source()` has **two** input paths:

- **UDP/recvmsg path** (`rc > 0` events branch) — used for audio and any
  non-SHM `-u`/`-U` instance.
- **SHM ring drain path** (`if (g_shm_ring) { ... }`) — the vehicle video
  hot path. It batches up to `SHM_DRAIN_MAX = 128` packets per outer
  iteration:

  ```c
  for(;;) {                                   // SHM drain loop
      int ret = venc_ring_read(g_shm_ring, buf, sizeof(buf), &out_len);
      if (ret != 0) break;
      ...
      t->select_output(mirror ? -1 : 0);
      if (cur_ts >= session_key_announce_ts) { t->send_session_key(); ... }
      t->send_packet(buf, rsize, 0);          // <-- PEEK HOOK GOES HERE
      ...
  }
  if (fec_timeout > 0) { /* -T fallback close */ }
  ```

**Plan:**

1. Add a free function in `peek.cpp`:
   `peek_outcome_t peek(const uint8_t *buf, size_t len, const peek_cfg_t&)`
   returning `{action, signals}` exactly as the spec §5 defines.
2. Add a small inline helper in `tx.cpp`,
   `emit_with_peek(Transmitter&, buf, rsize, cfg, &fec_close_ts, fec_timeout)`,
   that encapsulates: evaluate peek → on armed DROP `return` (skip send +
   skip signals) → else set the per-packet MCS level on the transmitter →
   `t->send_packet(buf, rsize, 0)` → if `signals & FEC_CLOSE`,
   `while(t->send_packet(NULL,0,WFB_PACKET_FEC_ONLY));` and
   `fec_close_ts = get_time_ms() + fec_timeout` (re-arm).
3. Call `emit_with_peek(...)` from **both** the SHM drain loop (replacing the
   verified `t->send_packet(buf, rsize, 0)` at the SHM hook site) **and** the
   UDP recvmsg branch (replacing its `t->send_packet(...)` — that call site is
   **stock wfb-ng**, below the `rc > 0: events detected` comment the patch
   references; confirm it against the cloned tree). One code path, two callers —
   mirrors how the spec keeps the engine transport-agnostic.

**Why hook both, not SHM-only.** In the *deployed* vehicle config only the SHM
path carries protectable video, so SHM-only would be functionally complete
today. But hook both anyway, because:

- it's **~one line** — both call sites are inside `data_source()` with the same
  `fec_close_ts`/`fec_timeout` locals in scope, and the peek logic is entirely
  in the shared helper;
- it's **zero-cost when off** — audio and the uplink/WCMD `wfb_tx` instances are
  separate processes running profile `off`; `enabled=false` skips the whole
  pipeline (spec §9), so they pay nothing;
- it **avoids a silent dead-zone** — `docs/protocols/shm-input.md` documents a
  UDP fallback for *video* ("Switching back to UDP mode" → `server:
  udp://…:5600`, `wfb_tx -u 5600`). SHM-only would make the feature a silent
  no-op in that mode — video on UDP, no protection, no error. The shared helper
  keeps parity for free.

The clean implementation **is** the shared helper called from both sites, not a
SHM-only special case.

**Per-packet vs per-batch:** the FEC_CLOSE must fire **inside** the drain
`for(;;)` loop, on the marked fragment, *not* at batch end — otherwise a
128-packet batch spanning several frames would collapse multiple AUs into one
block and defeat the single-class guarantee. The existing `-T` block after the
loop stays exactly as-is (fallback).

---

## 2. Control surface — extend the repo's already-extended protocol

`src/tx_cmd.h` in this repo currently defines `CMD_SET_FEC=1 … CMD_GET_RADIO=4`
and has **already grown** `cmd_set_fec`/`cmd_get_fec` to carry
`uint16_t fec_timeout_ms` (network byte order, with the `WFB_FEC_TIMEOUT_KEEP =
0xFFFF` sentinel). IDs 5/6 are free, so the spec's additions drop in:

```c
/* src/tx_cmd.h — append, do not renumber */
#define CMD_SET_PEEK 5
#define CMD_GET_PEEK 6

/* new arm in cmd_req_t.u */
struct { uint8_t enabled; uint8_t drop_enabled; }  /* 0/1, 0xFF = leave */
    __attribute__((packed)) cmd_set_peek;

/* new arm in cmd_resp_t.u */
struct { uint8_t enabled; uint8_t drop_enabled; uint8_t n_rules; uint8_t n_sig_rules; }
    __attribute__((packed)) cmd_get_peek;
```

- Send sizing in `tx_cmd.c` must follow the established
  `offsetof(cmd_req_t, u) + sizeof(req.u.cmd_set_peek)` pattern (see how
  `set_fec`/`get_fec` already do it in this repo).
- The `tx.cpp` control switch (same `switch (req.cmd_id)` that handles
  `CMD_SET_FEC`) gets two new `case`s. Unknown-cmd behavior already degrades
  cleanly, so an old `wfb_tx_cmd` against a new `wfb_tx` is safe (spec §7.3 holds).
- `wfb_tx_cmd` verbs: `peek on|off`, `peek drop on|off`, `peek status` — as spec §7.2.

### 2.1 Vehicle reachability — who flips the toggles

On the vehicle, **link_controller owns the wfb_tx control socket** (it already
issues `CMD_SET_FEC`/`CMD_SET_RADIO`). The ground operator therefore can't talk
to `wfb_tx` directly. Two options:

- **Phase 1 (minimal):** drive `peek on/off`/`drop on/off` locally on the
  vehicle via `wfb_tx_cmd`, or as startup args (`--peek-profile`). Good enough
  to bench the mechanism.
- **Phase 2 (operator control, recommended):** add WCMD keys mirroring the
  existing `WCMD_KEY_FEC_ENABLED`/`WCMD_KEY_MCS_ENABLED` —
  e.g. `WCMD_KEY_PEEK_ENABLED`, `WCMD_KEY_PEEK_DROP_ENABLED` in
  `shared/wcmd_proto.h` — so the ground → uplink → link_controller →
  `CMD_SET_PEEK` path lights up the toggles end-to-end. This is additive
  (new keys, no version bump per the wcmd_proto forward-compat rule) and needs
  a `make test` + a bump of the vendored proto copies under
  `tests/protocols/_proto/` per the repo `Rules`.

---

## 3. PROTECT under adaptive MCS (functional addition vs spec)

The spec prebuilds a `radiotap_header_t` per MCS level **once at init** from the
static `-M` value. This repo's `base_mcs` is **not static** — the MCS selector
in `vehicle/link_controller.c` writes `CMD_SET_RADIO` (mcs_index only) on its
adaptation tick. So PROTECT needs a cache that follows the radio:

1. `RawSocketTransmitter` keeps a small array of prebuilt headers
   `proto_hdr[0..PEEK_MAX_DELTA]`, where `proto_hdr[d]` is
   `init_radiotap_header(stbc, ldpc, short_gi, bandwidth, base_mcs - d, vht_mode, vht_nss)`
   (clamped at 0), reusing **the current radiotap params** — only `mcs_index`
   varies. There is no byte-poke; `init_radiotap_header` builds the bytes
   (spec §6 confirmed — the real struct has no `MCS_IDX_OFF`).
2. **Rebuild trigger:** the `CMD_SET_RADIO` handler is **stock wfb-ng** (not in
   `shm-input.patch`; confirm location in the cloned tree). After it applies the
   new radiotap header, add: (a) recompute `proto_hdr[]` from the new params and
   (b) set `peek_cfg.base_mcs = new mcs_index`. Do the same rebuild at startup
   once the initial header is built. Because link_controller changes
   **mcs_index only** (other radio fields are pinned from the startup
   `GET_RADIO` sync — `link_controller.c:5-16`), the rebuild can derive every
   `proto_hdr[d]` from `get_radiotap_header()`'s current fields, varying only
   the MCS — no need to track stbc/ldpc/gi/bw/vht separately.
3. `inject_packet()` selects `iovec[0]` from `proto_hdr[level]` for the current
   fragment, where `level` is the running per-packet PROTECT depth (carried the
   way `set_mark(uint32_t)` already carries per-fragment TX state — set on the
   transmitter immediately before `send_packet`).
4. **Parity = Option A** (most-robust level present in the block). Cheap here
   because marker-close makes each block single-class by construction (spec §6).

**Interaction note (call out in review):** the adaptive MCS selector and PROTECT
must not fight. PROTECT is *relative* (`base − Δ`), always derived from whatever
the selector last committed, so it tracks adaptation automatically. When the
selector parks at MCS0, PROTECT clamps to a no-op (spec §6 floor) — graceful.

---

## 4. `-x` plaintext mode — orthogonal, but verify

The recommended FPV config runs `-x` (plaintext data, `docs/protocols/shm-input.md:598`).
The peek runs at **ingestion, before FEC/encrypt**, and PROTECT/FEC_CLOSE both
act at the inject/block layer (`send_block_fragment` branches on `encrypt_data`
*after* the peek decision). So peek is independent of `-x` — but the test plan
must run **both** `-x` and AEAD to confirm the radiotap selection and FEC_ONLY
close are byte-identical under each (the only `-x` difference is the absence of
the 16-byte tag, which neither PROTECT nor FEC_CLOSE touches).

---

## 5. Build & packaging (repo-specific)

wfb_tx is **not** built via wfb-ng's Makefile here; `wfb-ng/build-armv7.sh`
(and `-aarch64.sh`, `-openwrt.sh`) compile each TU explicitly and link:

```sh
$CROSS_CXX $WFB_CFLAGS -std=gnu++11 -c -o src/tx.o   src/tx.cpp
$CROSS_CXX $WFB_CFLAGS -std=gnu++11 -c -o src/peek.o src/peek.cpp   # <-- ADD
...
$CROSS_CXX -o wfb_tx src/tx.o src/peek.o src/zfex.o src/wifibroadcast.o src/venc_ring.o $WFB_LDFLAGS
```

**Deliverables:**

1. Extend `wfb-ng/shm-input.patch` (or add a companion `wfb-ng/peek.patch`
   applied right after it) with: new `src/peek.hpp`, new `src/peek.cpp`, the
   `tx.cpp` hook + radiotap cache + control cases, and the `tx_cmd.{c,h}`
   additions. A companion patch keeps the SHM patch reviewable; the build
   script must `git apply` both, in order.
2. Add the `peek.o` compile + link lines to **all three** `build-*.sh`.
3. Bump the in-script `WFB_VERSION` string if you want the new build
   distinguishable (`shm-patched` → e.g. `shm-peek-patched`).

---

## 6. Tests

- **Wire-format conformance (`make test`) is unaffected.** Peek changes no wire
  struct: PASS is byte-identical, PROTECT only changes the radiotap *rate*
  (self-describing PHY, no payload change), FEC_CLOSE reuses the existing
  `WFB_PACKET_FEC_ONLY` path the RX already understands. So the frozen parsers
  under `tests/protocols/_proto/` need **no** change — *unless* you add the
  Phase-2 WCMD peek keys (§2.1), which do touch `shared/wcmd_proto.h` and then
  require the `make test` + vendored-copy bump per repo `Rules`.
- **`test_peek` host unit** (spec §10.1): table of crafted RTP+NAL byte
  sequences (single-NAL + FU-A, HEVC + H.264, truncated, marker set/clear) →
  assert `{action, signals}` per profile. Pure function. Add it as a native
  `g++` compile+run step in the build scripts (mirrors the existing
  `rx_native`/`tx_native` host builds), or as a standalone target under
  `probes/` so it runs without the cross toolchain.
- **Loopback / bench / drop / latency / A-B** (spec §10.2-10.6) unchanged, but
  run them against the **SHM** input path (`-H venc_wfb`), not `-u`, since that
  is the deployed configuration. The `-Y` tx_stats stream already exposes
  `fec_timeouts`, `fec_k/n`, and the live `radio.mcs` — use it to verify
  PROTECT is selecting the expected MCS per frame class without a pcap.

---

## 7. Open items / decisions for review

1. **Companion patch vs inline.** Recommend a separate `wfb-ng/peek.patch`
   (applied after `shm-input.patch`) so the two features stay independently
   reviewable and rebasable against upstream wfb-ng.
2. **Phase-2 WCMD keys.** Confirm whether operator-side toggling (§2.1) is in
   scope for the first cut or deferred. If deferred, the feature is
   vehicle-local only (startup arg or on-device `wfb_tx_cmd`).
3. **refpred external dependency.** The `refpred` profile is inert until the
   `waybeam_venc` `TRAIL_N`/`TRAIL_R` rewrite (#142) is on the deployed
   encoder. The `idr` profile works against any HEVC/H.264 GOP with no encoder
   dependency, so it's the lower-risk first target.
4. **Adaptive MCS + PROTECT airtime.** PROTECT stretches on-air time for matched
   frames (~1.3-1.5× at Δ1) and the adaptive MCS selector is unaware of that
   cost. Bench whether the selector needs a PROTECT-aware airtime margin, or
   whether keeping Δ small for the `refpred` base + bursty `idr` is enough
   (spec §9 airtime budget).
5. **Per-frame close × `-r` partial-block parity.** Marker-close makes most
   blocks *small* (a P-frame is often 1-3 packets « `k`), so each closes as a
   partial block whose parity count is governed by `-r` (`shm-input.md:150`),
   not by `n-k`. Confirm the protection/airtime trade-off of `-r` under
   one-block-per-frame: the removed M-bit implementation presumably tuned this,
   so check git history for the prior `-r` guidance before re-deriving.
6. **Pin the wfb-ng clone.** `build-*.sh` clones `--depth 1` master (§0.1).
   Pin a SHA so the spec's stock-symbol claims (`init_radiotap_header`,
   `inject_packet`, the `CMD_SET_RADIO` handler, the UDP send site) can't drift
   out from under the implementation between rebuilds.

---

## 8. Section-by-section map to the upstream spec

| Upstream spec § | Status in this repo | Action |
|---|---|---|
| §1 Motivation | unchanged | — |
| §2 Core idea | unchanged | — |
| §3 Match table struct | unchanged | ship in `src/peek.hpp` |
| §4 Profiles | unchanged | `refpred` gated on `waybeam_venc#142` |
| §5 Classification / peek | logic unchanged; **call site moves to SHM drain loop** | §1 above |
| §6 PROTECT | **header cache must rebuild on `CMD_SET_RADIO`** | §3 above |
| §6 Frame-boundary FEC close | **new mechanism** (current `-T` ≠ per-frame); this is the "M-bit" | §1 helper + per-packet close in drain loop |
| §6 DROP / PASS / coexistence | unchanged | — |
| §7 Control surface | extend repo's `tx_cmd.h` (cmd 5/6); optional Phase-2 WCMD keys | §2 above |
| §8 Integration points | re-mapped: SHM drain loop + `build-*.sh` instead of Makefile | §1, §5 |
| §9 Edge cases | unchanged | — |
| §10 Test plan | run against `-H` SHM path; `make test` unaffected | §6 above |
| §11 Non-goals | unchanged (`compact`/non-RTP still out of scope; SHM is RTP-only here) | — |
| §12 Future extensions | unchanged | — |
