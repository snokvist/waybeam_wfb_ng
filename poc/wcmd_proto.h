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
