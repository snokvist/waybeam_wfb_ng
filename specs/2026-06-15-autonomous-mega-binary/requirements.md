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

**Phase 3b — ✅ VERIFIED 2026-06-15 (host).** `gs_supervisor` applies a SPARSE
`/etc/wfb-link.json` overlay onto the loaded `gs_supervisor.json` right after
`cfg_load` (`cfg_apply_wfb_link_overlay`, uses the in-file JSON parser — no
sodium, so unconditional in standalone + mega; path overridable via
`WFB_LINK_CONF` env, default `/etc/wfb-link.json`). Only fields present
override; absent/missing-file is a logged no-op. Mapping: `key.file`→`key_file`;
`links.{video,uplink,probe}`→the `link_id` of the same-named tunnel;
`fec.k/n`+`mcs.boot`+`radio.bw`→every tx-role tunnel. Channel/system.up stay in
`gs_supervisor.json` (the GS's richer native model). Host-verified via
`/api/v1/tunnels` + `/api/v1/keys`: overlay file changed link_id 207→199 /
208→218, tx fec 8/12→4/6, mcs 2→5, bw 20→40, key→over.key; no file ⇒ base
unchanged.

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

### Phase 5 — Fold SQLite logger into `wfb-gs` (C + static libsqlite3) — ✅ DONE 2026-06-15 (host)
Built in four device-verifiable sub-phases:
- **5a — logger core.** `ground/wfb_logger.{c,h}`: udp:6700 listener pthread,
  schema + WAL, 20 rec / 2 s commit cadence, `max_duration` 1200 s rollover,
  clean flag+join stop (final commit + `ended_at`). `derive_columns` ported
  **bit-identical** to `wfb_store.py` (device-checked against a Python-written
  row). SQLite shipped as the **vendored 3.48.0 amalgamation**
  (`ground/vendor/sqlite`, version-matched to the RK3566 buildroot) compiled to
  `sqlite3.o` (`THREADSAFE=2` + size omits) and linked into both standalone +
  mega (+`pthread`+`m`) — *not* a separate `libsqlite3.a` (simpler, no
  host/sysroot dep; same static-link outcome). NOT sodium-gated. New
  ground `telemetry` config block; started after API bind, stopped cleanly on
  shutdown. **Opt-in:** disabled by default — a config with no `telemetry` block
  (e.g. `rk3566_passive.json`) never auto-creates `wfb.sqlite` in CWD (respects
  the no-persisted-logs-on-overlay rule). Enable via the gs_supervisor.json
  `telemetry` block with an explicit non-overlay `db` path. The air-side
  wfb-link `log.enabled` flag is deliberately NOT wired to the ground logger (it
  drives only the air link_controller SD logger, which is SD-bound and
  self-disabling); auto-enabling the ground sqlite logger from it would persist
  a growing DB on the overlay root.
- **5b — read API + dashboard.** `ground/gs_supervisor_telemetry.c` serves the
  dashboard (embedded `ground/webui/telemetry/*` via the `webui` xxd target) at
  `/telemetry[/static/*]` and GET JSON under `/api/v1/telemetry/`
  (`sessions`, `session?id`, `series?id`, `labels?id`, `capture`). Read handlers
  use their own short-lived WAL connection (`wfb_logger_open`). `series`
  reproduces `webapp.py` byte-for-byte (t + 8 cols + per-rung `mcs_dist` from
  `raw_json`, label overlays, `tier1_state` all-null). GS-side charts only;
  2 s polling; dark palette unified with `gs.html`.
- **5c — write API.** GET+query mutations (matching the GET-only supervisor
  API), x-www-form-urlencoded decode for text: `labels?action=add|del`,
  `meta`, `session?action=delete` (refuses the active live capture, 409),
  `capture?action=roll`. Label/meta via short-lived WAL conns; roll signals the
  capture thread. **Ad-hoc capture control:** `capture?action=start|stop`
  (`wfb_logger_runtime_start`/`_stop`) spawn/seal the capture thread at runtime,
  so the logger ships disabled (`telemetry.enabled=false`) and is toggled from
  the dashboard Start/Stop buttons; `/capture` status carries `started` (thread
  exists) + `running` (loop ingesting) + the `db` write target. Idempotent and
  serialised by the single-threaded API loop (also closes the prior latent
  double-start gap).
- **5d — retire Python + wiring.** `wfb-gs telemetry-import <file.jsonl>` applet
  (reuses the logger insert path) for old logs. Replaced runtime Python
  (`wfb_capture.py`, `wfb_ingest.py`, `webui/`, `webui_session.sh`,
  `capture_session.sh`) moved to `archive/python-telemetry/`; `webui_session.sh`
  dropped from `host_x86.json` `system.up`/`down` (+ a `telemetry` block added).
  `wfb_store.py` + the offline ML/import tools stay in `telemetry/` (still
  imported by `import_vehicle_session.py`/`wfb_active.py`). `walkout_logger.sh`
  (bash/jq, separate concern) left untouched. **Deferred** (offline Python in
  `archive/`): the vehicle-uplink overlay page, ML `tier1_state` bands, and the
  SCP `vehicle/fetch` import.

Verified (host, 0 warnings on host standalone + host mega + aarch64 cross): a
300-record real capture → `wfb.sqlite` accrues with a schema + derived row
matching Python; all read endpoints serve correct JSON; label add/del + meta +
roll work; active-session delete → 409; SIGTERM seals the session; the import
applet ingests `real_wfb.jsonl` (325 rec). Live dual-adapter browser render +
RK3566 deploy = the remaining on-device gate.

### Phase 6 — Autonomy hardening + ship — ✅ DONE 2026-06-15
- **Parser dedup**: `gs_supervisor`'s in-file jsmn parser removed; both sides now
  share `shared/wfb_json.h` (header-only). Host + mega rebuilt clean; config +
  overlay parse verified on a scratch instance (link/fec overlay applied).
- **Strip parity**: ground mega now stripped via `MEGA_STRIP` (cross-strip when
  `CROSS_CC` set, else host `strip`) — 378 KB → 342 KB.
- **Air-side respawn — decision: keep S99wfb as-is (no new watchdog).** A crashed
  `wfb_tx`/`wfb_rx` on the SigmaStar is rare; `S99wfb {restart}` (or the Key-tab
  apply path) brings the stack back, and adding a watchdog to `link_controller`
  would duplicate the supervisor role it deliberately doesn't own. Revisit only
  if field data shows mid-flight applet crashes.
- **`S99wfb.prev` footgun** (memory `wfb_ng_initd_prev_backup_reexec`): documented
  — back up init scripts OUTSIDE `/etc/init.d` (BusyBox runs every `S*`).
- **venc SIGHUP**: resolved in Phase 1 (clean respawn re-reads config; only-if-changed).
- **Pre-existing upstream warnings** (`tx_cmd.c` unused-result, `radiotap.c`
  packed-member): left as-is — vendored wfb-ng, not touched by this work.
- Gate: build-verify all four targets, focused review of the new C, on-hardware
  (air `.13` across phases; ground dual-adapter live on this host), then merge #79.

Original open items:
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
