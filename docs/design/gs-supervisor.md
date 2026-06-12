# Ground-side wfb supervisor

Status: scaffolding landed (`gs_supervisor.c`). Lifecycle + REST work;
WCMD emit, `wfb_cmd` passthrough, system-up commands, and SSE are
stubbed for follow-up. Name `gs_supervisor` is provisional.

## Why

The ground station today is a pile of fragments:

- `poc/init/wfb-ng.sh` (procd shell wrapper, hard-coded args)
- `poc/init/S99wfb`, `poc/init/wbmode` (mode toggle + monitor-mode bring-up)
- ad-hoc helpers (`wfb_rx_to_backpack.py`, `ground_rssi_forwarder.py`)
- `wfb_cmd` invocations for runtime tuning
- a future cmd-injector for `WCMD` uplink frames (vehicle-side proxy
  already merged in #42)

The vehicle side already collapses its control loop into a single
native binary (`link_controller.c`). This doc proposes the ground-side
analogue: one thin native supervisor that fork/execs `wfb_rx` and
`wfb_tx`, exposes a REST API for lifecycle + control passthrough, and
emits `WCMD` frames in-process. Asymmetric on purpose — vehicle is
venc-driven and runs one process; ground orchestrates several.

## Scope

In:

- supervise N×`wfb_rx` and 1×`wfb_tx` defined in JSON config
- per-tunnel start / stop / restart over REST
- passthrough to each tunnel's `wfb_cmd` UDP control port
  (replaces shelling out to `wfb_cmd`)
- in-process `WCMD` emit toward the uplink `wfb_tx`'s UDP input
- whitelisted "system" commands for monitor-mode bring-up / tear-down
  (filled in on device)
- structured stdout/stderr capture per child (ring buffer + SSE)

Out:

- helper sidecars (backpack bridge, rssi forwarder) — stay external,
  systemd/procd manages them as today
- adaptive logic (FEC / MCS scoring) — vehicle's `link_controller`
  owns the loop; ground supervisor is dumb pipes only
- WCMD response correlation on ground (vehicle replies go back to the
  WCMD source IP via the rx_ant socket; the ground supervisor sends
  fire-and-forget)
- replacing `link_controller` on the vehicle

## Process model

```
gs_supervisor (single binary, single config)
├── HTTP/SSE server thread        (reuses link_controller's webui scaffold)
├── tunnel "video"   → child: wfb_rx ...
├── tunnel "telem"   → child: wfb_rx ...
├── tunnel "uplink"  → child: wfb_tx ...
└── wcmd emitter     (in-process, sends to uplink.udp_in_port)
```

- one POSIX thread per tunnel doing `fork`/`execvp` + `waitpid`
- stdout/stderr → pipes → per-tunnel ring buffer (last N KiB) + SSE fanout
- on child exit: if `autostart` and not stopped via REST, exponential
  backoff respawn (capped, like procd ratelimit)
- on `SIGTERM`/`SIGINT`: stop all children (SIGTERM, then SIGKILL after
  grace), run `system.monitor_mode_down`, exit

Reuse the embedded HTTP server from `link_controller.c` so the binary
stays small and the webui shell is consistent.

## Config (JSON)

Single file, default `/etc/waybeam/gs_supervisor.json`. Reload via
`POST /api/v1/reload` (only changed tunnels are stopped/started).

> **2026-06 addition — `profile` mode.** For the standard deployment a
> `"profile"` block (ifaces, uplink_iface, channel, ht, txpower_mbm,
> wfb_bin_dir) replaces the hand-written `tunnels[]` + `system{}` —
> the supervisor synthesizes the canonical video/probe/uplink tunnels
> with the link ids/ports that match the vehicle's S99wfb, plus the
> adapter bring-up/down commands. See
> `ground/config/profile_example.json`. `profile` and `tunnels` are
> mutually exclusive; the explicit form below remains the advanced
> override.

```json
{
  "key_file": "/etc/drone.key",

  "http": { "bind": "0.0.0.0", "port": 9080 },

  "system": {
    "up":   [ /* commands run before any tunnel starts; filled in on device */ ],
    "down": [ /* commands run on shutdown */ ]
  },

  "tunnels": [
    {
      "name": "video",
      "role": "rx",
      "interfaces": ["wlan0mon", "wlan1mon"],
      "link_id": 207,
      "radio_port": 0,
      "udp_out": "192.168.2.20:5600",
      "stats_out": "127.0.0.1:5801",
      "extra_args": ["-l", "100", "-x"],
      "autostart": true
    },
    {
      "name": "telem",
      "role": "rx",
      "interfaces": ["wlan0mon", "wlan1mon"],
      "link_id": 209,
      "radio_port": 0,
      "udp_out": "127.0.0.1:14550",
      "autostart": true
    },
    {
      "name": "uplink",
      "role": "tx",
      "interfaces": ["wlan0mon"],
      "link_id": 208,
      "radio_port": 0,
      "udp_in_port": 5801,
      "control_port": 8000,
      "fec": { "k": 1, "n": 2 },
      "radio": {
        "bandwidth_mhz": 20,
        "mcs_index": 1,
        "stbc": 1,
        "ldpc": 1
      },
      "extra_args": ["-Q", "-P", "1"],
      "autostart": true
    }
  ],

  "venc_cmd": {
    "enabled": true,
    "uplink_tunnel": "uplink",
    "rate_limit_ms": 50,
    "allow_keys": ["bitrate_kbps", "fps", "payload_bytes", "force_idr"]
  }
}
```

Mapping rules:

- `role: "rx"`  → `wfb_rx -K key_file -i link_id -p radio_port -c <ip> -u <port> [-Y <stats_out>] <extra_args...> iface1 iface2 ...`
- `role: "tx"`  → `wfb_tx -K key_file -i link_id -p radio_port -u udp_in_port -C control_port -k <fec.k> -n <fec.n> -B <bw> -M <mcs> -S <stbc> -L <ldpc> <extra_args...> iface1 iface2 ...`
- `interfaces` is a list — wfb-ng natively accepts multiple positional
  iface args. Use it for diversity rx (recommended whenever the ground
  has more than one adapter) and for `wfb_tx` mirror mode (uncommon —
  usually one tx adapter is enough). Different tunnels can share an
  iface or use different ones; the supervisor doesn't care.
- `extra_args` is appended verbatim — no parsing, lets us pass through
  any flag we haven't modeled yet
- unknown keys at top level are rejected at load time (typos surface
  immediately rather than silently ignored)

## REST surface

All under `/api/v1`. JSON in, JSON out. No auth in v1 (bind to
`127.0.0.1` or trust the LAN; matches link_controller).

### Status / inventory

```
GET  /api/v1/status                     → { uptime, iface, system_up, tunnels: [...] }
GET  /api/v1/tunnels                    → [{ name, role, state, pid, restarts, ... }]
GET  /api/v1/tunnels/{name}             → full detail incl. argv, last_exit, recent log
GET  /api/v1/tunnels/{name}/log?n=200   → tail of stdout/stderr ring buffer
```

### Lifecycle

```
POST /api/v1/tunnels/{name}/start       → no-op if running
POST /api/v1/tunnels/{name}/stop        → SIGTERM, grace, SIGKILL; clears autostart-on-exit
POST /api/v1/tunnels/{name}/restart     → stop + start
POST /api/v1/reload                     → re-read config, diff, apply
```

### wfb_cmd passthrough

Replaces shelling out to `wfb_cmd` from operator scripts.

```
POST /api/v1/tunnels/{name}/control
     { "cmd": "set_fec",   "args": [1, 2] }
     { "cmd": "set_radio", "args": [...] }
     { "cmd": "set_fec_timeout_ms", "args": [50] }   // per #43
   → { "ok": true, "raw_reply": "..." }
```

Implementation: build the wfb-ng control wire format and `sendto()` the
tunnel's `control_port`. Reply (if any) is read with a short timeout.
The wrapper does not interpret commands — it just forwards bytes — so
new wfb_cmd verbs work without a wrapper release.

### venc command (WCMD) emit

```
POST /api/v1/cmd
     { "key": "bitrate_kbps", "value": 4000 }
     { "key": "force_idr" }
   → { "ok": true, "seq": 1234 }   // fire-and-forget
```

Implementation: pack the 16-byte `WCMD_MSG_REQ` per `wcmd_proto.h`,
`sendto(127.0.0.1, venc_cmd.uplink_tunnel.udp_in_port)`. Rate-limit
per key per `rate_limit_ms`. `allow_keys` gates which keys are
acceptable. No response correlation on ground (vehicle's reply is
addressed back to the WCMD source IP via the rx_ant return path —
out of scope for v1).

### System commands

```
POST /api/v1/system/up                  → run system.up commands sequentially
POST /api/v1/system/down                → run system.down (also runs on shutdown)
```

Whitelist only — only commands listed in config can run. No arbitrary
shell over the network. Exact commands to be added on device once the
monitor-mode bring-up sequence is confirmed.

### Events

```
GET  /api/v1/events                     → SSE: state changes, child log lines, exits
```

## Tunnel state machine

```
stopped ──start──▶ starting ──exec ok──▶ running ──exit──▶ exited
   ▲                  │                     │                │
   │                  └──exec fail──┐       │                │
   │                                ▼       ▼                │
   └──────────── stop ◀──── failed ◀── crash (autostart) ────┘
                                         (backoff: 1s, 2s, 4s, … cap 30s)
```

`stop` via REST clears the autostart-on-exit flag so a stopped tunnel
stays stopped until explicitly started or `reload` re-enables it.

## File layout

```
poc/
  gs_supervisor.c              main: JSON parser, supervisor loop, REST
  Makefile.gs_supervisor       host + cross builds
  gs_supervisor.example.json   reference config matching the schema above
  GS_SUPERVISOR.md             this doc
  webui/
    gs.html                    later — minimal status/control page
```

## Build & smoke test

```bash
# Host build (debug):
cd poc/
make -f Makefile.gs_supervisor host

# Run against the example config (port 19080 to avoid clash):
./build/gs_supervisor.host --config gs_supervisor.example.json --port 19080

# Probe the API:
curl -s http://127.0.0.1:19080/api/v1/health
curl -s http://127.0.0.1:19080/api/v1/status   | jq
curl -s http://127.0.0.1:19080/api/v1/tunnels  | jq
curl -s http://127.0.0.1:19080/api/v1/tunnels/uplink/stop
curl -s http://127.0.0.1:19080/api/v1/tunnels/uplink/start

# Cross build (set CROSS_CC to the target toolchain gcc):
make -f Makefile.gs_supervisor cross \
     CROSS_CC=../toolchain/aarch64/bin/aarch64-openwrt-linux-gcc \
     OUT_CROSS=build/gs_supervisor.aarch64
```

If `wfb_rx` / `wfb_tx` aren't on `$PATH` (host development), children
exit 127 and the supervisor backs off — the REST API stays responsive,
which is the right behaviour to verify on host before deploying.

## Open questions before implementation

1. **Name** — `gs_supervisor`, `wfb_supervisor`, `ground_controller`?
2. **HTTP stack reuse** — link the same TU(s) `link_controller.c` uses,
   or copy/paste? Worth a small refactor to share if both binaries ship
   together.
3. **Monitor-mode commands** — to be filled in on device (user will
   confirm exact iw/ip incantations for the target ground hardware).
4. **WCMD return path** — leave as fire-and-forget for v1, or stand
   up a return-socket binding now? Recommend deferring.
