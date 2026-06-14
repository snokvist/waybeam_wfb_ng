# Design note: peek FEC_CLOSE without padding + proportional parity

Status: **SUPERSEDED — REMOVED in PR #76** (2026-06-14). The short-tail
proportional-parity close was device-verified (bench 192.168.1.13 + dev-host
`wfb_rx_native`) but found to re-introduce MCS-loop flap + fps drops on this
system, so it was removed; peek now ships the **gate close only**
(`--peek-profile off|close`). The `--peek-short-tail` flag and
`WFB_NONCE_SHORT_TAIL` no longer exist. Retained as design history.

Two corrections to this note's original assumptions, found in-tree during
implementation (the goal — proportional parity, no on-air padding — was
unaffected; only the mechanism changed):

1. **`fec_encode_simd` in this build has NO `block_nums` param** — it always
   computes the full `n-k` parity set into `fecs[0..n-k)`, where `fecs[i]` carries
   check-block id `k+i`. So the TX computes all parity (existing call, unchanged)
   and simply **injects only the first `p`** fragments on-air (ids `k..k+p-1` are a
   valid contiguous subset). Simpler than the note assumed.
2. **The note's recommended signal (a flag in the parity's `wpacket_hdr`) is
   impossible** — parity fragments are raw FEC bytes with no `wpacket_hdr` (the
   header is itself FEC-protected data). Replaced by: the **always-zero top bit of
   the 64-bit `data_nonce`** (`WFB_NONCE_SHORT_TAIL`, bit 63) set on a single
   boundary marker fragment at index `j`. The marker's `fragment_idx == j` conveys
   `j`; the bit is authenticated as part of the AEAD nonce/AAD. RX masks it off
   before deriving `block_idx`, synthesizes `[j+1, k)` as canonical FEC_ONLY
   zero-pad, and decodes. Completion test changed `== fec_k` -> `>= fec_k`
   (provably identical for full blocks; required because synthesis can jump
   `has_fragments` past `k`).

Security hardening (post embedded-c review):
- **RX validates the marker** before synthesizing: a genuine marker is FEC_ONLY,
  `packet_size==0`, at `1 <= j < fec_k`.  In plaintext (`-x`) mode there is no AEAD
  to reject a forged/corrupt fragment, so without this a single injected packet
  with the bit + an attacker-chosen `fragment_idx` could bulk-synthesize zero-pad
  into a live block and truncate a frame.  Invalid => treated as an ordinary
  fragment, no synthesis.  (rx.cpp, the `if (short_tail && ...)` guard.)
- **apply_fec bails gracefully** (`if (j >= fec_n) return;`) instead of
  `assert(j < fec_n)` if parity is ever insufficient — defends NDEBUG builds and
  the untrusted-RF path against an out-of-bounds read.  The `>= fec_k` completion
  test provably implies `present_parity >= missing_data` for legitimate traffic
  (synthesis fills the tail, so `has_fragments >= k` <=> parity suffices), so this
  never fires in practice; it is pure defense-in-depth.

Verification:
- Host FEC roundtrip (`wfb-ng/tests/peek_short_roundtrip.cpp`): **6700/6700**
  bit-exact recoveries across codes {8/12, 8/10, 4/6, 16/24, 37/51, 2/3}, payloads
  {32,512,1024,1448}, all `j in [1,k)`, all single-fragment drops, leading-data
  multi-drops up to `p`, and the over-loss non-decodability invariant.
- Device (real RF, peek ON, short-tail ON): `pkt_dec_err=0`, `pkt_lost=0`,
  smooth video. Backward-compat (new RX + old non-short TX) clean.
- Airtime A/B at ~1560 incoming pkts/s: gate amp 1.94x (injected ~3061/s) vs
  short-tail amp 1.43x (~2216/s) = **~27% fewer on-air packets**, overhead roughly
  halved, AND per-frame isolation restored (60 closes/s = every frame, vs the
  gate's frame-size-dependent partial closes).

Original spec follows (for the record).

---

Follow-up to the shipped gate fix (PR #64, `fix/peek-fec-close-gate`).

## Why

peek's mandatory per-frame `FEC_CLOSE` (RTP marker bit) closes a FEC block on
every frame. `Transmitter::send_packet`'s `WFB_PACKET_FEC_ONLY` path zero-pads
the partial block up to `k` **on-air** (every padding fragment is injected, see
`send_block_fragment` at the close loop) and emits a **full `n−k` parity set**.
On a P-frame-heavy 60/120 fps stream that puts ~`fec_n` packets on air per frame
regardless of payload → ~62 % parity byte-overhead (device-measured) and ~100 %
packet-overhead.

The gate (PR #64) keeps the venc→wfb_tx ring under its backpressure threshold so
the **drops stop**, but the wasted airtime remains. This note reclaims it:
keep peek's per-frame flush + IDR PROTECT, but stop padding on-air and size the
parity to the actual data.

### Goal
For a partial close with `j (< k)` real data fragments: transmit **`j` data +
`p` parity**, where `p = max(1, round(j·(n−k)/k))` (preserve the configured code
rate), and **nothing else** — no zero-padding on air. Net airtime per frame ≈
the configured FEC rate instead of full `n−k`.

### Non-goals
- Changing the steady (full-block) path — blocks that fill to `k` are unchanged.
- Changing peek PROTECT (MCS-reduction) or the gate.
- CSA / WCMD / SHM transport.

## Feasibility (confirmed in-tree)
- **Proportional parity is producible from the existing `(k,n)` codec.**
  `fec_encode_simd` takes `block_nums` = the desired check-block ids (≥ k); pass
  only `p` of them to produce `p` parity. No `fec_new` per block.
  (`build/wfb-ng/src/zfex.h:43,55`.)
- **RX already FEC-recovers missing fragments** and creates the codec from the
  session `(k,n)` (`rx.cpp init_session` → `fec_new(fec_k, fec_n)`;
  `wsession_data_t.k/n`, `wifibroadcast.hpp`). So the codec is fixed per session;
  a short block needs the RX to know its real data-count `j`.
- **The RS math tolerates the implicit zeros.** Treat fragments `[j,k)` as
  known-zero (never transmitted). RX synthesizes them, then needs only
  `received_data + received_parity ≥ j` to recover the `j` real fragments. That
  is exactly proportional protection (`p` parity guards `j` data).

## The crux: signalling per-block `j` to the RX

The RX must learn, per block, that it ended early at `j` real data fragments with
`p` parity, so it (a) stops waiting for `k` data and (b) fills `[j,k)` with zeros
before decode. `wblock_hdr_t` is fixed (`packet_type` + `data_nonce =
block_idx<<8 | fragment_idx`) — no spare field. Options, recommended first:

1. **Recommended — carry `k_real=j` on the parity fragments via a `wpacket_hdr`
   flag + 1 trailing byte.** Parity fragments are the block-closers; tag each
   with a `WFB_FLAG_SHORT_BLOCK` bit in `wpacket_hdr.flags` and append `j` (1
   byte; `k ≤ 255`). RX reads `j` from the first parity it receives, fills
   `[j,k)` zeros, maps received parity ids to codec check-blocks `[k, k+p)`,
   decodes. Robust to losing some parity (every parity carries `j`). Smallest
   wire delta; no `wblock_hdr` change.
2. Encode `j` in the parity **fragment_idx**: place parity at `[j, j+p)` instead
   of `[k, k+p)`; RX infers `j` = first parity index. No header byte, but RX must
   distinguish "data idx" from "parity idx" without knowing `j` first — fragile
   when leading data is lost. Not recommended.
3. New `packet_type` for a short-block descriptor. Cleanest semantically, biggest
   wire change. Overkill.

## Changes

### TX — `tx.cpp`
- New flush path for a partial close (in `peek_emit` / `send_packet` FEC_ONLY):
  when `0 < fragment_idx < fec_k`, compute `j = fragment_idx`,
  `p = max(1, round(j*(fec_n-fec_k)/fec_k))`; zero-fill `[j,k)` **in the encode
  buffer only**, call `fec_encode_simd` requesting `p` check blocks
  `[fec_k, fec_k+p)`, inject the `p` parity (at `block_max_level`, Option A as
  today) tagged short-block with `j`. Do **not** `send_block_fragment` the
  `[j,k)` padding. Reset `fragment_idx`.
- Full blocks (`fragment_idx == fec_k`) unchanged.

### RX — `rx.cpp`
- On a fragment tagged `WFB_FLAG_SHORT_BLOCK`: set the block's effective data
  count `j` and parity `p`; synthesize `[j,k)` as zero fragments (present);
  recover among `j` real + received parity; emit only the `j` real data
  (their `wpacket_hdr.packet_size` already bounds payload; zeros have size 0 and
  are skipped). Adjust the "block complete?" test from `has k data` to
  `has j data (after zero-fill) recoverable`.

### Wire / rollout
- Bump a capability: add a `tags` TLV in `wsession_data_t` (or a `fec_type`
  value) = "short-block-v1" so a peer that lacks it is detectable. **Both ends
  are our fork — deploy TX+RX together.** Old RX + new TX must be prevented
  (old RX would mis-parse the short parity) → gate on the TLV: new TX only emits
  short blocks if the session negotiated the capability, else falls back to the
  gate behavior (PR #64). Mixed fleets stay safe.

## Simpler alternative (no wire change): close only on IDR (#3)
Change the mandatory `FEC_CLOSE` sig-rule from "RTP marker (every frame)" to
"IDR/keyframe boundary". P-frames then share blocks (fill to `k`, amortized
parity) → airtime back to ~configured rate; IDRs keep isolation + PROTECT.
- **Pro:** no wire change, ~10-line change, keeps RX untouched.
- **Con:** loses per-P-frame **flush** (a P-frame's tail waits for the next
  frame's data to fill the block — adds up to ~1 frame of latency; note `-T`
  idle-flush is `!use_shm`-gated so it won't bail us out on the SHM path) and
  per-P-frame loss isolation. For FPV the latency cost is the real question.

**Recommendation:** if the gate's ~12 % margin proves enough in field testing,
ship #3 (cheap, no wire risk). Pursue the full proportional-parity design only if
we need both the airtime reclaim **and** per-P-frame flush/isolation.

## Validation
1. Host unit test: encode a short block (`j < k`) with `p` proportional parity,
   drop `≤ p` fragments, assert exact recovery of the `j` real fragments. Extend
   `test_peek.cpp` or add a FEC-roundtrip test.
2. Device A/B (192.168.1.13, 120 fps, peek ON): byte-overhead before (~62 %) vs
   after (≈ configured rate); `fec_skip` stays 0; receiver decode integrity (no
   `dec_err`, no visual artifacts); airtime headroom at a forced-low MCS.
3. Mixed-version safety: old-RX + new-TX with capability off → identical to PR #64
   behavior (no short blocks emitted).

## Files / effort
`tx.cpp` (encode/inject), `rx.cpp` (decode/zero-fill/completion test),
`wifibroadcast.hpp` (flag + optional TLV), `test_peek.cpp` (roundtrip). All in
`wfb-ng/peek.patch` (regenerate via the shm-baseline diff method used for #64).
Medium effort; the RX completion/zero-fill logic is the risk area. Open
questions: exact `block_nums` mapping in `fec_decode_simd` for a relocated parity
set; whether to round `p` up or down at low `j`.
