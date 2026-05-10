/* shared/wfb_control.h — wfb_tx control protocol constants.
 *
 * Single source of truth for the wfb_tx control-socket opcodes, fixed
 * lengths, and sentinel values shared by:
 *   - vehicle/link_controller.c   (writer + reader)
 *   - ground/gs_supervisor.c      (writer + reader)
 *   - wfb-ng/shm-input.patch      (vendored fork — header file
 *                                  src/tx_cmd.h tracks these by hand)
 *
 * Wire formats (big-endian for multi-byte fields, host-endian for u8):
 *
 *   Request:  uint32_t req_id (NB); uint8_t cmd_id; <union body>
 *     set_fec    body (4 B):   k, n, fec_timeout_ms (NB)
 *     set_radio  body (7 B):   stbc, ldpc, short_gi, bw, mcs, vht_mode, vht_nss
 *     get_fec / get_radio:     no body
 *
 *   Response: uint32_t req_id (NB); uint32_t rc (NB); <union body>
 *     On error (rc != 0): only req_id+rc are sent (body omitted).
 *
 * If the value of WFB_FEC_TIMEOUT_KEEP ever changes, update tx_cmd.h in
 * the wfb-ng patch in lockstep — there's no shared header in the upstream
 * fork tree, so it must be carried by hand there.
 */
#ifndef WAYBEAM_WFB_CONTROL_H
#define WAYBEAM_WFB_CONTROL_H

/* ── opcodes ────────────────────────────────────────────────────────── */
#define WFB_CMD_SET_FEC    1
#define WFB_CMD_SET_RADIO  2
#define WFB_CMD_GET_FEC    3
#define WFB_CMD_GET_RADIO  4

/* ── fixed wire lengths ─────────────────────────────────────────────── */
#define WFB_CMD_REQ_HEADER         5            /* req_id(4) + cmd_id(1) */
#define WFB_CMD_REQ_GET_RADIO_LEN  WFB_CMD_REQ_HEADER
#define WFB_CMD_REQ_SET_FEC_LEN    (WFB_CMD_REQ_HEADER + 4)
#define WFB_CMD_REQ_SET_RADIO_LEN  (WFB_CMD_REQ_HEADER + 7)

/* ── sentinels ──────────────────────────────────────────────────────── */
/* set_fec.fec_timeout_ms = WFB_FEC_TIMEOUT_KEEP means "leave the running
 * value alone".  link_controller doesn't compute a frame-period-based
 * timeout today, so it always sends KEEP; the operator sets the timeout
 * via `wfb_tx -T` at boot.  Mirrored in shm-input.patch (src/tx_cmd.h). */
#define WFB_FEC_TIMEOUT_KEEP       0xFFFFu

/* ── timing ─────────────────────────────────────────────────────────── */
/* How long the GS supervisor waits for a wfb_tx control-socket reply
 * before treating the request as failed.  Pure UI guard — wfb_tx replies
 * are typically <2 ms on loopback. */
#define WFB_CMD_REPLY_TIMEOUT_MS   200

#endif /* WAYBEAM_WFB_CONTROL_H */
