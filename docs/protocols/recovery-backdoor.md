# Recovery Backdoor (open/keyless APFPV channel)

A separate, **always-listening, unauthenticated** ground→vehicle command
channel whose sole purpose is to let a field operator recover a vehicle that
has become **uncommandable over the normal keyed uplink** — typically a
lost/mismatched `drone.key`, but also any keyed-uplink breakage. It can trigger
exactly **one** safe, deferred action: **arm boot-into-APFPV on the next
reboot**.

> **Planned change (mode-switch contract, lands in unified-WebUI FE Phase 2c).**
> Under the unified mode console (`waybeam-coordination`
> `protocols/mode-switch.md`, decision 3-1), the `apfpv_cmd` hook gains a
> trailing `reboot`: `fw_setenv wfbmode 0 && sync && reboot`. A confirmed
> recovery then returns the vehicle to APFPV **immediately**, instead of merely
> arming the next manual reboot. This **relaxes the "deferred action only"
> property** below — point 5 is re-justified for that case. **Today the hook is
> still arm-only** (`recovery-apfpv.sh.example` forbids rebooting); this section
> documents the agreed target, not yet-shipped behavior.

It is the mirror image of the `-xx` open *video downlink*: where that is a
keyless vehicle→ground TX that any `-x` RX can view, this is a keyless
ground→vehicle TX (`-xx`) decoded by a keyless (`-x`) vehicle RX.

```
 GS:      wfb_tx  -xx   link 209 port 0   ← gs_supervisor GET /api/v1/recovery
 Vehicle: wfb_rx  -x  (no -K)  link 209   → link_controller :5802 → recovery_dispatch()
                                            → fork+exec recovery.apfpv_cmd

 EXISTING (unchanged): keyed uplink link 208 (WCMD) · video 207 (-xx) · probe 50
```

## Why this is safe despite being unauthenticated

`-xx` carries no key and no session crypto, so anyone within RF range can
transmit recovery frames. The blast radius is contained **by construction**, not
by a secret:

1. **Separate listener + separate dispatcher.** Recovery frames arrive on their
   own `wfb_rx` → their own `link_controller` UDP port → `recovery_dispatch()`,
   which is *not* `wcmd_dispatch()`. The open path never calls the operator
   command code, so it can never reach MCS / channel / CSA / txpower / FEC /
   venc-HTTP — those all live in `wcmd_dispatch()`.
2. **Single-key whitelist.** `recovery_dispatch()` accepts only
   `WCMD_KEY_RECOVERY_APFPV` (64) and rejects everything else.
3. **Mutual key-space exclusion (defense in depth).** Key 64 is numbered above
   the keyed path's `WCMD_NUM_KEYS` (19), so a recovery frame that somehow hit
   the keyed rx_ant listener is rejected as `UNKNOWN_KEY`, and an operator key
   sent on the recovery listener is rejected by the whitelist. The two command
   spaces cannot cross-trigger.
4. **N-frame arming + seq-dedup + cooldown.** The vehicle requires
   `recovery.arm_count` (default 1) frames carrying the same `seq` within
   `recovery.arm_window_ms` (default 2000) before running the hook, then
   `recovery.cooldown_ms` (default 10000) gates re-execution. The default of 1
   fires on the first surviving frame — recovery is needed exactly when the RF
   link is worst, so a multi-frame arm gate fights the feature's own purpose, and
   `wfb_rx` already FEC/CRC-drops corrupt frames before dispatch (so the
   anti-stray value of a higher count is marginal). The cooldown still collapses
   the burst tail to a single action; raise `--recovery-arm-count` on hardened
   builds that prefer a stricter gate over worst-case reachability. The GS emits a
   heavy `WCMD_RECOVERY_BURST_FRAMES` (8) burst (one `seq`) to ride out
   loss on the open, ARQ-less link.
5. **Bounded, idempotent action only.** *Arm-only hook (today):* worst case an
   attacker forces a *next-reboot* mode change — no immediate effect, no
   reboot-loop DoS. *Arm + reboot hook (planned, see callout above):* the worst
   case becomes an **immediate reboot into APFPV**, but the same gate bounds it:
   the 3-frame arm (same `seq`) + 2 s arm window admit only a deliberate
   operator burst, and the 10 s cooldown caps re-execution to **≤1 reboot per
   10 s** — a held button or hostile flood cannot reboot-loop the vehicle.
   APFPV is the **safe recovery state** (AP up, directly reachable), so even the
   abusive case lands the vehicle somewhere recoverable rather than bricked.
   **Blast radius (arm + reboot):** a confirmed recovery tap now reboots the
   vehicle — operators must treat the control as a reboot, and the GS UI
   confirm-gates it accordingly. Reboot is unconditional once armed; it does
   **not** depend on the keyed uplink, which is the point (it must work with a
   dead `drone.key`).

## Wire format

Reuses the 16-byte `WcmdReq` from `shared/wcmd_proto.h` verbatim, with
`key = WCMD_KEY_RECOVERY_APFPV` (64) and `value` ignored. `recovery_dispatch()`
fills a best-effort `WcmdResp` back to the peer for local observability; like
WCMD the GS does not bind the return path.

## The APFPV action (the `apfpv_cmd` hook)

`link_controller` does **not** know how the firmware selects APFPV at boot —
that lives in the OpenIPC/builder init layer. On arming it simply
`fork+exec`s `/bin/sh -c "<recovery.apfpv_cmd>"` (admin-configured, *not*
attacker-controlled). The hook is firmware-provided and wires to the real
boot-mode switch (e.g. `fw_setenv`). A reference stub ships at
`vehicle/scripts/recovery-apfpv.sh.example`. Keep `apfpv_cmd` a single token
(a script path); put any multi-step logic inside the script.

## Configuration

Vehicle (`/etc/wfb-link.json`, rendered to env by `config-env`):

```json
"links":    { "recovery": 209 },
"ports":    { "recovery": 5802 },
"recovery": { "enabled": true, "apfpv_cmd": "/etc/wfb/recovery-apfpv.sh" }
```

→ `WFB_RECOVERY`, `WFB_RECOVERY_LINK`, `WFB_RECOVERY_PORT`, `WFB_RECOVERY_CMD`,
consumed by `S99wfb`, which spawns the keyless recovery `wfb_rx` and passes
`--recovery-port` / `--recovery-cmd` (or `--no-recovery`) to `link_controller`.
`--recovery-arm-count` tunes the arming threshold; window/cooldown default in
`config_defaults()`.

Ground (`gs_supervisor.json`): a `tx` tunnel named `recovery` (link 209,
`extra_args: ["-xx"]`, robust low MCS) plus
`"recovery_cmd": { "enabled": true, "tunnel": "recovery" }`.

Enabled by default on both ends (field-recoverable out of the box); set
`recovery.enabled=false` (vehicle) or `recovery_cmd.enabled=false` (ground) on
hardened deployments to remove the open attack surface.

## Observability

- Vehicle `link_controller /status` → `"recovery": {recv, rejected, armed,
  executed, last_rc}`. `executed>0` means the hook ran; `last_rc` is its exit.
- Ground `gs_supervisor /api/v1/status` → `"recovery": {emit_total, emit_failed,
  burst_frames}`.

## Operator flow

GS WebUI → Vehicle Control → **recovery: arm boot-APFPV** (behind a `confirm()`
dialog) → `GET /api/v1/recovery`. With the arm-only hook the operator then
power-cycles / reboots the vehicle; with the planned arm + reboot hook the
vehicle reboots itself. Either way it comes up in AP-FPV, reachable directly
even though the wfb link/key was broken. Under the unified mode console this is
surfaced as the confirm-gated **"Return to APFPV"** control (decision 3-3,
manual-only — it never auto-fires).
