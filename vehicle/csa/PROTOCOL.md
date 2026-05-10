# CSA-over-wfb-ng wire format (v0.1, MVP)

One-way Channel Switch Announcement piggybacked on the existing
ground→air uplink (`cpe510 wfb_tx -u 5801` → `vehicle wfb_rx -i 208 -u 5801`).

Each CSA frame is **one line of newline-terminated JSON**, written into the
same UDP stream that already carries `wfb_rx -Y` rx-stats blobs. The vehicle
agent (running on 5801) classifies each line by its `type` field:

- `type` starts with `csa_` → CSA handler
- anything else → forwarded to link_controller (round 2 will multiplex; round
  1 just stops link_controller during the test).

## Common fields

```
{
  "type":  "csa_<phase>",
  "ver":   1,
  "sess":  <uint32, monotonic per-orchestrator session>,
  "seq":   <uint32, per-session frame counter>,
  "src":   "ground"
}
```

`sess` lets the receiver dedupe and refuse stale repeats. `seq` is purely
for logging and correlation.

## Frames

### `csa_commit`

```
{
  "type":           "csa_commit",
  "ver":            1,
  "sess":           42,
  "seq":            <0..N-1>,
  "src":            "ground",
  "target_chan":    149,
  "target_ht":      "HT20",                 // "HT20" | "HT40+" | "HT40-"
  "dt_to_switch_ms": 500,                   // ms from THIS frame's recv to T_switch
  "t_revert_ms":    3000,                   // post-switch silence -> revert
  "prev_chan":      161,                    // for revert
  "prev_ht":        "HT40+"
}
```

Sender emits `N` copies (default 5) at 20 ms cadence, each with a
decremented `dt_to_switch_ms` so all copies resolve to the same absolute
local `T_switch = recv_monotonic + dt_to_switch_ms` on the receiver
(modulo per-frame jitter). The receiver's `T_switch` snaps to the FIRST
frame seen in a session; subsequent frames in the same session refresh
the `dt`-derived target if it lands within ±20 ms of the original
(otherwise dropped — likely reorder).

## Receiver guards

Every `csa_commit` is filtered by these checks before entering the state
machine. They are configured via `csa_agent` CLI flags. By default all
channels (DFS included) and all bandwidths are accepted; the only baseline
guard is a sanity range check on `target_chan`.

| Guard | Flag | Default |
|---|---|---|
| Range check (`target_chan ∈ [1,200]`) | always on | always on |
| Channel allowlist | `--allowlist 149,153,157,161` | unset (any channel) |
| Bandwidth allowlist | `--bandwidth HT20,HT40+` | unset (any bandwidth) |
| Cooldown between switches | `--cooldown-ms N` (0 disables) | 2000 ms |

`--allowlist` and `--bandwidth` are independent constraints: each acts as a
filter when set, or is permissive when unset. Both must pass when both are
configured. DFS is no longer special — exclude DFS channels by leaving them
out of the allowlist if they're not wanted.

A rejected frame is dropped on the wire and logged as
`REJECT … not in --allowlist` / `… not in --bandwidth` /
`… target_chan=X out of range` on the agent. Cooldown gates only **new**
sessions; same-session refresh frames are always allowed (they refine
`T_switch` within ±20 ms of the original). Cooldown is anchored on every
channel change, including auto-revert.

## Receiver state machine

```
IDLE
  └── on csa_commit{sess=S}:
        compute T_switch = now + dt_to_switch_ms
        prev = (prev_chan, prev_ht) from frame
        target = (target_chan, target_ht)
        → state = ARMED(S, T_switch, prev, target)

ARMED(S, T_switch, prev, target)
  ├── on csa_commit{sess=S, dt} with |refined_T - T_switch| <= 20ms:
  │     keep T_switch (or average)
  ├── at now == T_switch:
  │     iw set channel target
  │     T_alive_deadline = now + t_revert_ms
  │     → state = VERIFY(S, target, prev, T_alive_deadline)
  └── on csa_commit{sess=S', S' > S}:
        → ARMED(S', ...)   (latest session wins)

VERIFY(S, target, prev, T_alive_deadline)
  ├── on any UDP frame (CSA or stats) before deadline:
  │     → state = COMMITTED(target)
  └── at now == T_alive_deadline:
        iw set channel prev   (revert)
        → state = IDLE
```

`COMMITTED` is informational; the agent stays bound and forwards as before.

## Orchestrator (host) responsibilities

1. Pick `sess` (monotonic).
2. Compute `T_switch_cpe510 = cpe510_now_ms + lead_ms` via `ssh date +%s%3N`.
3. SSH-launch a deferred `iw` on cpe510 sleeping until `T_switch_cpe510`.
4. Inject `N=5` `csa_commit` frames into `cpe510:5801` at 20 ms cadence,
   timed so the last frame departs ≤ `lead_ms - 100` ms before `T_switch`.
5. After `lead_ms + t_revert_ms`, optionally probe vehicle reachability via
   ethernet (192.168.1.13) and report.

## Replay-protection scope

`sess` provides newest-wins replay protection **within an agent's uptime**.
A captured frame from a prior session has `sess <= a->sess` and is rejected.

What `sess` does NOT cover:

- **Agent reboot** resets the high-water mark; an old captured frame can be
  replayed once before the next legitimate session bumps `sess` past it. A
  persistent monotonic on disk would close this. Cooldown limits damage to
  one hop per `cooldown_ms`.
- **Fabrication on the LAN side** — anyone reachable on `cpe510:5802` (or
  the over-air uplink) can pick `sess > current` and inject a hop. This is
  not replay; only a MAC (HMAC + shared key) can stop it. `--allowlist` +
  `--bandwidth` + cooldown contain the blast radius if it happens.

`seq` is for logging and burst correlation only — not used for filtering.

## Out of scope for v0.1

- Bidirectional ack from vehicle (would need link_controller integration).
- Width changes without channel changes.
- Cancel / supersede mid-flight.
- HMAC + persistent monotonic session for cross-reboot replay protection
  and authenticity.
