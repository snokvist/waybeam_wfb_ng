#ifndef WCMD_PROTO_H
#define WCMD_PROTO_H

/*
 * waybeam_venc command proxy — wire protocol.
 *
 * A small binary command channel multiplexed onto the rx_ant UDP listener
 * (default :5801, production :6600) of poc/link_controller. The ground
 * station already pushes wfb_rx -Y JSON to that port; this protocol uses
 * a 4-byte magic prefix ("WCMD") that does not collide with JSON, so the
 * link_controller dispatcher can tell binary command frames apart from
 * rx_ant JSON without parsing.
 *
 * link_controller acts as the proxy: it receives CMD_REQ datagrams,
 * validates and clamps the value, and translates the request to a single
 * HTTP GET against the local venc /api/v1 surface. The HTTP outcome is
 * returned to the originating socket as a CMD_RESP.
 *
 * Design parallels rtp_sidecar.h: 6-byte common header, network byte
 * order, #pragma pack(push,1) so on-the-wire size is fixed and stable.
 *
 *   ground-side cmd injector              vehicle link_controller
 *   ─────────────────────────             ─────────────────────────
 *      WCMD_REQ(key,value)  ──UDP──▶      multiplex with rx_ant JSON
 *                                         clamp + rate-limit + dispatch
 *                                         GET /api/v1/set?key=value
 *      WCMD_RESP(status)    ◀──UDP──      reply to recvfrom() peer
 *
 * Forward-compat: probes that don't recognise a future key/status enum
 * value treat it as UNKNOWN_KEY / OK respectively. Trailing bytes are
 * ignored; never bump the version for additive fields.
 */

#include <stdint.h>

#define WCMD_MAGIC          0x57434D44u  /* "WCMD" big-endian */
#define WCMD_VERSION        1

/* Message types */
#define WCMD_MSG_REQ        1   /* injector → proxy */
#define WCMD_MSG_RESP       2   /* proxy   → injector */

/*
 * Whitelisted command keys.
 *
 * The proxy refuses anything not in this enum. New keys may be added in
 * later versions without bumping WCMD_VERSION; older proxies reply with
 * WCMD_STATUS_UNKNOWN_KEY and the injector can fall back accordingly.
 */
#define WCMD_KEY_BITRATE_KBPS    1   /* video0.bitrate, units = kbps */
#define WCMD_KEY_FPS             2   /* video0.fps */
#define WCMD_KEY_PAYLOAD_BYTES   3   /* outgoing.maxPayloadSize */
#define WCMD_KEY_FORCE_IDR       4   /* GET /request/idr (value ignored) */

/*
 * Vehicle-side wfb_tx (video link) controls.  These tunnel a wfb_cmd
 * SET_FEC / SET_RADIO into the proxy → vehicle wfb_tx control_port.
 * The proxy reads current state via wfb_cmd GET_RADIO before mutating
 * just the requested field, so callers can twiddle one knob at a time
 * without losing the rest.  Caveat: link_controller's adaptive FEC and
 * MCS subsystems will overwrite these values on their next tick — set
 * fec.enabled=0 / mcs.enabled=0 first (via local /set?) for manual
 * control to stick.
 */
#define WCMD_KEY_WFB_FEC_K       5   /* wfb_tx FEC k (n preserved) */
#define WCMD_KEY_WFB_FEC_N       6   /* wfb_tx FEC n (k preserved) */
#define WCMD_KEY_WFB_MCS         7   /* radio mcs_index */
#define WCMD_KEY_WFB_BANDWIDTH   8   /* radio bandwidth (MHz) */
#define WCMD_KEY_WFB_LDPC        9   /* radio ldpc (0/1) */
#define WCMD_KEY_WFB_STBC       10   /* radio stbc */
#define WCMD_KEY_WFB_SHORT_GI   11   /* radio short_gi (0/1) */

/*
 * Adaptive subsystem master switches.  Toggle the FEC controller and the
 * MCS selector at runtime so an operator can pin a manual FEC k/n or MCS
 * via the keys above without restarting link_controller.  Re-enabling
 * resumes adaptation from the current state.
 */
#define WCMD_KEY_FEC_ENABLED    12   /* link_controller fec.enabled (0/1) */
#define WCMD_KEY_MCS_ENABLED    13   /* link_controller mcs.enabled (0/1) */

/*
 * Vehicle WLAN adapter TX power, in millibel-milliwatts (mBm), the unit
 * accepted by `iw dev <iface> set txpower fixed <N>` (100 mBm = 1 dBm).
 * Dispatched by link_controller as a fork+exec of `iw` against the iface
 * configured under cmd.wfb_iface (falling back to csa.iface when unset).
 * Honoured even when fec/mcs are off — txpower is unrelated to adaptive
 * subsystems and applies straight to the radio. Adaptive MCS does not
 * touch txpower today, so the value sticks until the next WCMD or
 * link_controller restart.
 */
#define WCMD_KEY_WFB_TXPOWER    14   /* iw set txpower fixed <mBm> */

/*
 * venc recorder start/stop.  Single key with binary value: 1 → HTTP GET
 * /api/v1/record/start, 0 → /api/v1/record/stop.  Uses the same fire-
 * and-forget path as the other venc HTTP keys; venc replies 200 with
 * `{"ok":true}` on success.  The vehicle's seq-dedup window prevents a
 * triple-burst from triggering three start/stop cycles.
 */
#define WCMD_KEY_RECORD         15   /* /api/v1/record/{start,stop} */

/*
 * Vehicle wfb_tx peek (per-frame FEC close) master switch.  Tunnels a wfb_cmd
 * CMD_SET_PEEK into the proxy → vehicle wfb_tx control_port, mirroring the
 * FEC/MCS enable switches above.  Value is binary (1 = on, 0 = off).  When on,
 * wfb_tx closes each video frame's FEC block at the RTP marker (per-frame loss
 * isolation + tighter delivery timing).
 *
 * Key 17 (PEEK_DROP_ENABLED) is RETIRED: the NAL-aware PROTECT/DROP peek modes
 * were removed.  The number stays reserved so 18/19 keep their wire values; do
 * not reuse it.
 */
#define WCMD_KEY_PEEK_ENABLED      16   /* wfb_tx peek enabled (0/1) */
/* 17 reserved (was WCMD_KEY_PEEK_DROP_ENABLED) — do not reuse */

/*
 * Logging sync marker — GS → vehicle, for post-walk log time-alignment.
 * NOT an operator command: it carries no venc/wfb action and does NOT touch
 * the radio.  The GS emits one every ~10 s with `value` = gs_unix_seconds
 * (CLOCK_REALTIME) and the WCMD `seq` as the marker id.  link_controller
 * records (seq, gs_unix_s, its own uptime at receipt) into /status as a
 * "logsync" object; the walkout logger mirrors that to the SD card.  Post-
 * walk the importer fits the (gs_unix_s ↔ vehicle uptime) pairs into a line
 * and remaps the vehicle records onto the GS wall-clock — and the gs_unix
 * span identifies which GS capture session the walk belongs to (so a GS
 * "new session" mid-walk re-attributes the vehicle log automatically).
 *
 * Dispatch differs from operator keys: the proxy bypasses cmd.allow_keys_mask
 * (this is infrastructure, not an operator-gated action) but still honours
 * cmd.enabled, since it shares the uplink command path.  It is the one key
 * with no clamp window and no HTTP/wfb_cmd side effect.
 *
 * Y2038: `value` is int32, so the seconds stamp truncates after 2038-01-19.
 * Benign here — the importer fits deltas (slope≈1, offset absorbs the base),
 * and a single walk never straddles the rollover, so relative alignment holds.
 */
#define WCMD_KEY_LOG_SYNC       18   /* value = gs_unix_seconds; seq = marker id */

/*
 * SD-card logger control — GS → vehicle, infrastructure (like LOG_SYNC, NOT an
 * operator command).  Lets the GS "New capture session" button roll a fresh,
 * time-aligned vehicle log to match the new GS session instead of relying on a
 * mid-walk LOG_SYNC re-attribution.  `value`: 1 = start/roll a new SD session,
 * 0 = stop the current one.  link_controller's in-process logger handles it.
 * Same dispatch rules as LOG_SYNC: bypasses cmd.allow_keys_mask (infrastructure),
 * still honours cmd.enabled; no clamp window, no venc/wfb side effect.  Like
 * LOG_SYNC it sits above WCMD_KEY_MAX (the highest operator key) so the operator
 * /api/v1/cmd path neither lists nor gates it.
 */
#define WCMD_KEY_LOG_CONTROL    19   /* value: 1=start/roll new SD log, 0=stop */

/*
 * Authenticated Return-to-APFPV — an operator command on the KEYED uplink
 * (unlike the keyless key-64 backdoor below).  Maskable via cmd.allow_keys_mask
 * (bit 19); the GS emits it with wcmd_emit() (3-frame burst) from
 * /api/v1/cmd?key=return_apfpv.  The vehicle dispatches it through the normal
 * keyed path (allow_keys_mask gate + burst-dedup) and routes it to the SAME
 * recovery hook as key 64 (recovery.apfpv_cmd → arm wfbmode=0 + reboot), but
 * over the authenticated control link instead of the open -xx channel.  `value`
 * is ignored.
 *
 * Numbered 20, NOT 17: 17 is reserved (retired PEEK_DROP) and 18/19 are infra.
 * It sits ABOVE the GS WCMD_KEY_MAX (so it is not GS-rate-limited and never
 * indexes the per-operator-key array) but WITHIN the vehicle WCMD_NUM_KEYS
 * keyed-path bound, so wcmd_dispatch() accepts and masks it.
 */
#define WCMD_KEY_RETURN_APFPV   20   /* keyed uplink: authenticated arm-APFPV + reboot */

/*
 * RECOVERY BACKDOOR key — NOT an operator command and NOT carried on the keyed
 * uplink.  This key travels ONLY on the separate keyless/open (-xx) recovery
 * link (its own link_id + its own link_controller UDP listener) and is handled
 * by a dedicated recovery_dispatch() that accepts this key and nothing else.
 *
 * It is deliberately numbered ABOVE WCMD_NUM_KEYS (the keyed-path bound, 20) so
 * the two command spaces are mutually exclusive by construction:
 *   - a recovery frame that somehow reached the keyed rx_ant listener is
 *     rejected by wcmd_dispatch() as UNKNOWN_KEY (key > WCMD_NUM_KEYS), and
 *   - an operator key sent on the open recovery listener is rejected by the
 *     recovery whitelist.
 * Because this channel is UNAUTHENTICATED (-xx: no key, no session crypto), it
 * may only trigger a SAFE, DEFERRED action — arm boot-into-APFPV on the NEXT
 * reboot (vehicle-side `recovery.apfpv_cmd` hook).  value is ignored.
 *
 * Never add this to cmd.allow_keys_mask; never dispatch it from wcmd_dispatch.
 */
#define WCMD_KEY_RECOVERY_APFPV 64   /* open recovery channel: arm boot-APFPV next reboot */

/* Status codes returned in CMD_RESP */
#define WCMD_STATUS_OK           0
#define WCMD_STATUS_DISABLED     1   /* cmd subsystem off (cmd.enabled=false) */
#define WCMD_STATUS_UNKNOWN_KEY  2   /* key not recognised by this version  */
#define WCMD_STATUS_KEY_BLOCKED  3   /* key disabled in cmd.allow_keys_mask */
#define WCMD_STATUS_OUT_OF_RANGE 4   /* value clamped or rejected           */
#define WCMD_STATUS_RATE_LIMITED 5   /* per-key min interval not elapsed    */
#define WCMD_STATUS_HTTP_ERROR   6   /* venc unreachable or returned !ok    */
#define WCMD_STATUS_BAD_FORMAT   7   /* reserved: malformed datagram (today
                                      * such frames are dropped silently
                                      * by the demux without dispatching)  */
#define WCMD_STATUS_NOT_PERMITTED 8  /* peer rejected (loopback-only mode)  */

#pragma pack(push, 1)

/**
 * Command request — injector → proxy, 16 bytes.
 *
 * The proxy never replies on the rx_ant socket if the magic check fails,
 * so a stray JSON datagram is silently passed through to the JSON
 * parsers downstream (current parsers reject non-rx_ant frames already).
 */
typedef struct {
	uint32_t magic;        /* WCMD_MAGIC                                  */
	uint8_t  version;      /* WCMD_VERSION                                */
	uint8_t  msg_type;     /* WCMD_MSG_REQ                                */
	uint16_t seq;          /* injector correlation id; echoed in resp    */
	uint8_t  key;          /* WCMD_KEY_*                                  */
	uint8_t  flags;        /* reserved, must be 0                         */
	uint16_t _pad;         /* reserved, must be 0                         */
	int32_t  value;        /* signed; ignored for FORCE_IDR               */
} WcmdReq;                 /* 16 bytes */

/**
 * Command response — proxy → injector, 16 bytes.
 *
 * Sent to the recvfrom() source address of the request. status reflects
 * proxy validation; http_status is the parsed numeric HTTP status code
 * from venc's response (e.g. 200, 400, 500) or 0 if no HTTP request was
 * issued (request rejected before dispatch). applied_value is the
 * post-clamp value the proxy applied to venc — or, on RATE_LIMITED, the
 * value the proxy *would* have applied if the rate-limit window had
 * elapsed. 0 for FORCE_IDR (value field is unused for that key).
 *
 * Clients must branch on `status` first; `http_status` is observability,
 * not a contract. venc returns HTTP 200 even when it rejects a /set
 * (body carries `"ok":false`), so http_status==200 + status==HTTP_ERROR
 * means "venc reachable but rejected the value".
 */
typedef struct {
	uint32_t magic;        /* WCMD_MAGIC                                  */
	uint8_t  version;      /* WCMD_VERSION                                */
	uint8_t  msg_type;     /* WCMD_MSG_RESP                               */
	uint16_t seq;          /* echo of WcmdReq.seq                         */
	uint8_t  status;       /* WCMD_STATUS_*                               */
	uint8_t  key;          /* echo of WcmdReq.key                         */
	uint16_t http_status;  /* venc HTTP response code, or 0               */
	int32_t  applied_value;/* post-clamp value; on RATE_LIMITED == "would
	                        * have applied"; 0 for FORCE_IDR              */
} WcmdResp;                /* 16 bytes */

#pragma pack(pop)

#endif /* WCMD_PROTO_H */
