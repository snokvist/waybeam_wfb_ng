# Autonomous mega-binary pair (wfb-gs / wfb-air)

Status: PLANNED 2026-06-15. Follows the merged mega-binary work (PR #78,
`8817a30`). Goal: make the `wfb-gs` / `wfb-air` pair boot into a working,
self-configuring link with **nothing but a startup script and an optional
config file** — no per-unit hand-seeding, no Python, no manual key copying.

## 1. Context

After PR #78, both sides ship as a single busybox-style binary:

- **air** (`wfb-air`, SigmaStar Infinity6E armv7l): `tx`/`rx`/`link`/`tx_cmd`/
  `keygen` applets, started by `vehicle/init/S99wfb`.
- **ground** (`wfb-gs`, RK3566 aarch64 / x86 dev host): `supervisor`/`rx`/
  `tx`/`tx_cmd`/`keygen`, started by `ground/init/S46gs_supervisor`.

Current autonomy gaps (verified during this spec's investigation):

| Gap | Today | Citation |
|---|---|---|
| Probe must be opted-in | `WFB_PROBE=1` script constant required for MCS adaptation | `vehicle/init/S99wfb:53` |
| venc sink not auto-wired | operator must pre-set `outgoing.server=shm://local_shm` | venc `outgoing.server` `MUT_LIVE`, `src/venc_api.c:399` |
| No shared config file | air is 100% S99wfb constants + CLI flags; `link_controller` reads **zero** config files; ground reads `/etc/waybeam/gs_supervisor.json` (hand-rolled parser, `gs_supervisor.c:81-178,665`) | — |
| Keys hand-seeded | `wfb_rx`/`wfb_tx` only *read* `-K` file (default CWD `rx.key`/`tx.key`); missing key = fatal throw | `rx.cpp:386`, `tx.cpp:226` |
| Two divergent startup scripts | air `S99wfb` vs ground `S46gs_supervisor` | — |
| SQLite logger is Python | `telemetry/wfb_{ingest,capture,store,stats_tee}.py` (~800 lines, stdlib) tap udp:6700 → `wfb.sqlite` | `telemetry/` |
| Telemetry UI separate | dashboard not in `gs_supervisor` WebUI | — |

**Key facts that shape the design:**

- `keygen.c:74-96` **already** derives a deterministic pair from a passphrase:
  Argon2i (fixed 16-byte salt `wifibroadcastkey`, `OPSLIMIT/MEMLIMIT_INTERACTIVE`)
  → 64-byte seed → `crypto_box_seed_keypair` ×2. So `wfb_keygen Waybeam` is
  bit-reproducible. `drone.key` = `drone_sk(32)‖gs_pk(32)`; `gs.key` =
  `gs_sk(32)‖drone_pk(32)`.
- venc `outgoing.server` is `MUT_LIVE`; set via
  `json_cli -s .outgoing.server "shm://local_shm" -i /etc/waybeam.json`. Whether
  SIGHUP re-applies it live is **contested** (memory `feedback_sighup_no_config_reread`
  says SIGHUP does not re-read `/etc/waybeam.json`; code path suggests reinit
  does). → device-verify gate, not an assumption (Phase 1).
- Ground already has a working hand-rolled JSON tokenizer
  (`gs_supervisor.c:81-178`) — reuse it, don't add cJSON.

## 2. Resolved design decisions (2026-06-15)

1. **Config consumption — `config-env` applet.** A new applet
   (`wfb-air config-env [file]` / `wfb-gs config-env [file]`) reads
   `/etc/wfb-link.json`, fills baked-in preset defaults for every missing
   field, and prints shell `KEY=value` lines. Startup scripts do
   `eval "$(wfb-* config-env /etc/wfb-link.json)"`. One C parser (the existing
   hand-rolled one, promoted to a shared file), defaults live in C, scripts stay
   thin, empty/missing/malformed file → today's working preset (with a warning).
2. **Telemetry logger — static libsqlite3 in C.** Port `wfb_capture`/`wfb_store`
   to a C module linked into `wfb-gs`, cross-building `libsqlite3.a` once
   (like libsodium/libpcap). Keep the exact schema (sessions/records/
   predictions/labels) + WAL so existing queries survive. Python removed.
3. **Key WebUI — seed + random + upload.** Unified key page on both sides:
   regenerate from passphrase seed (default `Waybeam`), generate fresh random
   pair, or upload an existing 64-byte key; shows current key fingerprint.

## 3. Unified config: `/etc/wfb-link.json`

Single shared **link preset**, consumed by both sides via `config-env`. Empty
`{}` (or absent file) ⇒ exactly today's link. All keys optional.

```json
{
  "radio":  { "channel": 161, "htmode": "HT20", "bw": 20, "txpower_mbm": 2000 },
  "key":    { "file": "/etc/drone.key", "seed": "Waybeam" },
  "links":  { "video": 207, "uplink": 208, "probe": 50 },
  "fec":    { "k": 8, "n": 12 },
  "mcs":    { "boot": 2, "min": 1, "max": 7 },
  "probe":  { "enabled": true },
  "peek":   { "profile": "close" },
  "log":    { "enabled": true },
  "venc":   { "shm": "local_shm", "auto_wire": true }
}
```

- `key.file` differs per side by default (`/etc/drone.key` air, `/etc/gs.key`
  ground); `key.seed` is the fallback passphrase (Phase 2).
- On **ground**, `config-env` emits the same env; the supervisor's tunnel
  *structure* stays in `gs_supervisor.json`, but link-layer values
  (channel/key/link_ids/fec/mcs/txpower) are overridden from `/etc/wfb-link.json`
  so one file tunes both ends. Exact override list pinned in Phase 3.

## 4. Phases (each independently on-hardware verifiable)

Devices: air `192.168.1.13` (Infinity6E bench), ground dev host (x86 +
RTL88x2 `wlx40a5ef2f229b`/`…2308`, ch161/5805 MHz) and/or `192.168.2.20`
(RK3566 passive RX). SigmaStar rule: **SIGTERM only, never SIGKILL**.

### Phase 1 — Quick autonomy wins (no new parser) — ✅ VERIFIED 2026-06-15 (.13)
Device finding: SIGHUP applies `outgoing.server` via a clean venc fork+exec
respawn (re-reads `/etc/waybeam.json`; pid changes, video resumes, no SoC
wedge) — so the only-if-changed guard is essential (never respawn a correct
encoder). Probe default-on, fragment sourcing, and the venc-shm no-op path all
confirmed live; ground detection/arg-resolution unit-verified both paths.

Scope:
- **Probe default-on.** Drop the `WFB_PROBE=1` requirement: probe runs unless
  explicitly disabled (`WFB_PROBE=0` or `probe.enabled=false`). `S99wfb:53,176`.
- **Auto-wire venc shm.** S99wfb reads `outgoing.server` with `json_cli`; if it
  isn't `shm://local_shm`, set it and signal venc; **device-verify** whether
  SIGHUP applies live or a venc restart is needed, and branch accordingly.
- **Unify startup scripts.** Extract shared logic into a sourced fragment
  `init/wfb-common.sh`; air `S99wfb` and a ground `S99wfb` (supersedes
  `S46gs_supervisor`, same boot slot) both become thin wrappers that source it
  and run the mega binary.

Verify: air `.13` cold-boots → link up, probe active by default, venc auto-set
to shm (`json_cli -g`); ground boots from the unified script → passive RX
decodes video; both `status` actions report per-role pidfiles.

### Phase 2 — Auto-keygen from seed `Waybeam` — ✅ VERIFIED 2026-06-15 (.13 + host)
Device-verified: deleting `/etc/drone.key` and restarting auto-seeded a key
**bit-identical** to `wfb-air keygen Waybeam` (sha `35c7d9c0…`); all 4 applets
started clean; the bench's real custom key was backed up and restored. Host:
both roles match the `wfb_keygen Waybeam` reference, deterministic under
`--force`, never overwrites an existing file, writes 0600.

Implemented as `multicall/wfb_keyseed.{c,h}` (Argon2i salt `wifibroadcastkey`
→ `crypto_box_seed_keypair`, atomic temp+rename), a `keygen-ensure` applet on
both sides (role defaulted by binary side via `WFB_SIDE_ROLE`), a backstop in
the `rx`/`tx` applet thunks, and `wfb_seed_key` in the fragment called by
`S99wfb` before launch (single writer). Ground relies on the thunk backstop
(role GS) — safe under the supervisor's concurrent spawns because the content
is deterministic and the write is atomic.

Scope:
- Shared `wfb_ensure_key(path, role, seed)` (port `keygen.c` Argon2i→seed_keypair):
  when the `-K` file is absent, derive the pair from `key.seed` (default
  `Waybeam`), write the role-correct 64-byte file, **log a loud INSECURE-DEFAULT
  warning**. Invoked explicitly by the startup script (`wfb-* keygen-ensure
  <KEY>`) and as a safety net in the `tx`/`rx` applet thunks before
  `wfb_{tx,rx}_main`.
- This is a bring-up fallback ONLY (file present ⇒ untouched); never overwrites
  an operator key.

Verify: `rm /etc/drone.key` on air + `/etc/gs.key` on ground → both auto-create;
fingerprints match `wfb_keygen Waybeam` reference; encrypted link establishes
0-loss. Then place a random pair → fallback does not touch it.

### Phase 3 — `/etc/wfb-link.json` + `config-env` applet — ✅ AIR VERIFIED 2026-06-15 (.13); ground = Phase 3b (deferred)
Done + device-verified on `.13`:
- `shared/wfb_json.h` — the gs_supervisor jsmn-style tokenizer extracted
  **header-only** (`static inline`), so config-env and any consumer share one
  parser with no link conflicts. (gs_supervisor keeps its identical in-file copy
  for now; folding it onto the header is a trivial Phase 6 cleanup — the code is
  byte-identical.)
- `config-env` applet on both sides (`multicall/wfb_configenv.{c,h}`): parses
  the file, applies §3 preset defaults (baked in C), emits shell `NAME=value`
  (string values single-quoted → `eval`-injection-safe, verified). Missing /
  empty `{}` / oversized / malformed ⇒ defaults + stderr warning, exit 0.
- Air `S99wfb` rewired to `eval "$(wfb-air config-env /etc/wfb-link.json)"`
  after the static-constant fallback block; `WFB_KEY_SEED`/`WFB_VENC_AUTOWIRE`
  threaded into the key-seed + venc-shm steps. `init/wfb-link.example.json`
  documents the schema.

Verified: no file & empty `{}` ⇒ identical link (ch161/20dBm/probe-on/peek-close,
host-confirmed byte-identical sans source comment); override file (txpower 1500,
probe off, peek off) → all applied (15 dBm, probe-tx gone, `link_controller
mcs.enabled=false`); remove ⇒ preset restored; malformed `{ broken ]` ⇒ defaults
+ warning, link still up; injection probe (`'`-laden seed) neutralized.

**Phase 3b (deferred — needs a ground bench):** have `gs_supervisor` apply
`/etc/wfb-link.json` link-layer overrides (channel/key/link-ids/fec/mcs) onto
its loaded `gs_supervisor.json` tunnels. Different mechanism (C-side; channel
lives in `system.up` shell strings, not a field) — the `config-env` applet is
already built into `wfb-gs`, just not yet consumed by the supervisor.

Original scope:

### Phase 4 — Unified key-management WebUI — ✅ VERIFIED 2026-06-15 (.13 + host)
Shared backend in `wfb_keyseed.{c,h}`: `wfb_key_fingerprint` (8-hex BLAKE2b of
the canonical `drone_pk‖gs_pk`, recovered via `crypto_scalarmult_base` — same on
a matched pair), `wfb_write_key_seed/random/raw`, `wfb_hex_to_key`. Both
endpoints gated on `WFB_WITH_WFBNG` (standalone daemons stay sodium-free).

- **Air**: `link_controller` gains `--key-file` (default `/etc/drone.key`,
  passed by `S99wfb`) and a GET `/keys` endpoint (status / seed / random /
  upload / restart). Apply = detached `S99wfb restart` (fork+setsid, **closes
  all inherited fds** so the `:8765` listen socket doesn't leak through exec —
  bug found & fixed on device). Key tab added to `vehicle/webui/index.html`.
- **Ground**: `gs_supervisor` gains GET `/api/v1/keys` (status / seed / random /
  upload); apply reuses the existing `/api/v1/system/reinit`. Key tab added to
  `ground/webui/gs.html` (shared dark palette — already identical).

Verified: air `.13` status/seed/random/upload/restart all work (seed
deterministic, upload restored the real custom key exactly, restart clean with
no socket leak, link healthy); ground host endpoint status/seed/random/upload +
malformed rejected; endpoint `gs.key` == `keygen Waybeam` reference. **Pair
fingerprint cross-check: Waybeam shows `000f5280` on BOTH air (drone) and ground
(gs)** — confirms the same-fingerprint-means-paired UX. Both WebUI pages serve
the Key tab (52 KB air / 67 KB ground, all controls present).

Scope:
- Key page on both WebUIs (seed regenerate / random / upload; fingerprint
  display). Backend: `GET` key status (fingerprint only, never secret),
  `POST` regenerate{seed|random|upload} → write key, signal reload (ground:
  restart tunnels; air: re-exec `tx`/`rx`). Reuse the already near-identical
  dark-theme CSS (JetBrains Mono / DM Sans) — unify into a shared snippet.

Verify: from air WebUI generate random pair; mirror to ground via upload (or
same seed); link re-establishes; fingerprints match across both UIs.

### Phase 5 — Fold SQLite logger into `wfb-gs` (C + static libsqlite3)
Scope:
- Cross-build `libsqlite3.a` (add to `wfb-ng/build-*.sh`).
- Port `wfb_capture`/`wfb_store` to a C logger module: udp:6700 listener
  thread, same schema + WAL, commit cadence (20 rec / 2 s), session rollover,
  `max_duration` 1200 s. Link into `wfb-gs`; gate on `log.enabled`.
- Integrate the telemetry dashboard as a tab/`/telemetry` route in
  `gs_supervisor_http.c`; unify look with the supervisor UI.
- Retire the Python files (keep a one-shot JSONL→sqlite import for old logs).

Verify: ground live link → `wfb.sqlite` accrues `records`; dashboard renders
live; `python` absent from the runtime path.

### Phase 6 — Autonomy hardening + ship
Open items to close:
- **Air-side respawn**: ground `gs_supervisor` already supervises tunnels; air
  has no supervisor for `wfb_tx`/`wfb_rx` if one dies. Decide: extend
  `link_controller` to respawn, or a minimal S99 watchdog. (Design in this
  phase; implement if low-risk.)
- **`S99wfb.prev` re-exec footgun** (memory `wfb_ng_initd_prev_backup_reexec`):
  document the "back up outside `/etc/init.d`" rule; optional self-guard.
- **Strip parity**: ground mega binary is currently unstripped — strip in
  `ground/Makefile`.
- Resolve the Phase-1 venc-SIGHUP finding in docs.
- Clear the two pre-existing upstream warnings (`tx_cmd.c` unused-result,
  `radiotap.c` packed-member) if touched.
- Gate: `build-verifier` (both targets, sizes), high-effort `code-review`,
  on-hardware verify on `.13` + ground, then PR (never commit to main directly).

## 5. Non-goals
- No ML in the link loop (telemetry capture only; predictions table preserved
  for offline use). See memory `wfb_ng_per_telemetry_track`.
- No change to the wfb-ng wire protocol, link IDs, or FEC/MCS control law.
- Not auto-unifying the *full* ground tunnel structure into `wfb-link.json` —
  only the shared link-layer preset (§3).

## 6. Risks
- venc SIGHUP-vs-restart ambiguity (Phase 1 gate).
- Deterministic `Waybeam` key is a **shared secret** — acceptable only as a
  labelled bring-up fallback; WebUI must make moving off it easy (Phase 4).
- `libsqlite3` cross-build for armv7l/aarch64 + binary-size growth (size is
  explicitly acceptable per maintainer).
- Re-exec on key change (air) must use SIGTERM teardown, never SIGKILL.
