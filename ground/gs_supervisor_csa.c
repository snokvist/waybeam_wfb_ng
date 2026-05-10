/*
 * gs_supervisor_csa.c — channel switch announcement orchestrator.
 *
 * One-shot synchronized channel hop coordinated with vehicle's csa_feed
 * state machine in vehicle/csa/csa.c. Wire format is documented in
 * vehicle/csa/PROTOCOL.md (newline-terminated JSON `csa_commit` frames).
 *
 * Flow:
 *   1. POST /api/v1/system/csa records sess++/T_switch and queues N=5
 *      csa_commit frames at 20 ms cadence into the uplink wfb_tx UDP
 *      input port (same hop that WCMDs use).
 *   2. supervisor_tick → csa_tick drains the queue and, at T_switch,
 *      fork+execs `iw dev <iface> set channel <chan> <ht>` on the
 *      GS-side iface.
 *   3. On revert deadline T_switch+t_revert_ms: if no rx_ant pkts
 *      arrived since T_switch on any rx tunnel, fork+exec a revert iw.
 *      Mirrors vehicle csa_tick's verify→revert behavior.
 *
 * One CSA in flight at a time; a new POST while phase!=IDLE returns 409.
 */

#include "gs_supervisor.h"

CsaState g_csa = { .phase = CSA_IDLE, .sess = 0 };
uint32_t g_csa_seq_in_burst = 0;

/* Persistent UDP socket for csa_send_commit_frame().  Opened lazily on
 * first send; closed only at process exit (kernel cleanup).  Avoids the
 * old socket()/close() per-frame that burned 5 syscalls × 2 × per CSA
 * burst (10 ifd churns in 100 ms when CSA fires). */
int g_csa_send_fd = -1;

/* Send one csa_commit JSON frame to the uplink wfb_tx UDP input port.
 * dt_to_switch_ms is filled in fresh at send time so all frames in the
 * burst converge on the same absolute T_switch on the receiver. */
int csa_send_commit_frame(const Config *c)
{
	const Tunnel *up = NULL;
	for (int i = 0; i < c->tunnel_count; i++) {
		if (!strcmp(c->tunnels[i].name, c->venc_cmd_uplink) &&
		    !strcmp(c->tunnels[i].role, "tx")) {
			up = &c->tunnels[i];
			break;
		}
	}
	if (!up || up->udp_in_port <= 0) return -1;

	uint64_t now = now_us();
	int dt_ms = (g_csa.t_switch_us > now)
	    ? (int)((g_csa.t_switch_us - now) / 1000ULL) : 0;
	int t_revert_ms = (int)((g_csa.t_revert_us - g_csa.t_switch_us) / 1000ULL);

	char buf[320];
	int n = snprintf(buf, sizeof(buf),
		"{\"type\":\"csa_commit\",\"ver\":1,\"sess\":%u,\"seq\":%u,"
		"\"src\":\"ground\",\"target_chan\":%d,\"target_ht\":\"%s\","
		"\"dt_to_switch_ms\":%d,\"t_revert_ms\":%d,"
		"\"prev_chan\":%d,\"prev_ht\":\"%s\"}\n",
		g_csa.sess, g_csa_seq_in_burst++,
		g_csa.target_chan, g_csa.target_ht,
		dt_ms, t_revert_ms,
		g_csa.prev_chan, g_csa.prev_ht);
	if (n < 0 || (size_t)n >= sizeof(buf)) return -1;

	if (g_csa_send_fd < 0) {
		g_csa_send_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		if (g_csa_send_fd < 0) return errno;
	}
	struct sockaddr_in dst = {
		.sin_family = AF_INET,
		.sin_port   = htons((uint16_t)up->udp_in_port),
	};
	dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	ssize_t w = sendto(g_csa_send_fd, buf, (size_t)n, 0,
	                   (struct sockaddr *)&dst, sizeof(dst));
	return (w == (ssize_t)n) ? 0 : errno;
}

/* Pick the first rx tunnel — used to track post-switch traffic for
 * revert detection. Returns -1 if no rx tunnel exists. */
int csa_pick_rx_tunnel_idx(const Config *c)
{
	for (int i = 0; i < c->tunnel_count; i++)
		if (!strcmp(c->tunnels[i].role, "rx")) return i;
	return -1;
}

/* CSA state machine: drive burst frames, fire local iw at T_switch,
 * watch for revert.  Called once per supervisor tick. */
void csa_tick(Config *c, uint64_t t_us)
{
	if (g_csa.phase == CSA_BURST &&
	    t_us >= g_csa.next_frame_us &&
	    g_csa.frames_sent < g_csa.frames_total) {
		int rc = csa_send_commit_frame(c);
		if (rc != 0) {
			LOG_WARN("csa: send frame %d/%d failed (rc=%d) — aborting",
			         g_csa.frames_sent + 1, g_csa.frames_total, rc);
			g_csa.phase = CSA_IDLE;
		} else {
			g_csa.frames_sent++;
			g_csa.next_frame_us = t_us + 20000ULL; /* 20 ms cadence */
			if (g_csa.frames_sent >= g_csa.frames_total) {
				g_csa.phase = CSA_ARMED;
				LOG_INFO("csa: burst complete (%d frames), armed for "
				         "T_switch in %.0f ms",
				         g_csa.frames_total,
				         (double)(g_csa.t_switch_us - t_us) / 1000.0);
			}
		}
	}

	if (g_csa.phase == CSA_ARMED && t_us >= g_csa.t_switch_us) {
		LOG_INFO("csa: T_switch — iw set channel %d %s on %d iface(s)",
		         g_csa.target_chan, g_csa.target_ht, g_csa.iface_count);
		/* Snapshot the rx pkt counter so VERIFY can decide whether
		 * traffic resumed on the new channel. */
		int idx = csa_pick_rx_tunnel_idx(c);
		g_csa.baseline_rx_idx = idx;
		g_csa.baseline_pkt_all = (idx >= 0) ? c->tunnels[idx].st_pkt_all : 0;
		g_csa.baseline_pkt_us  = t_us;
		for (int i = 0; i < g_csa.iface_count; i++) {
			int rc = run_iw_set_channel(g_csa.ifaces[i],
			    g_csa.target_chan, g_csa.target_ht);
			if (rc != 0)
				LOG_WARN("csa: iw on %s rc=%d", g_csa.ifaces[i], rc);
			/* Refresh cache so /api/v1/ifaces reflects the hop
			 * before the periodic round-robin gets to it. */
			IfaceState *ist = iface_state_find(g_csa.ifaces[i]);
			if (ist) (void)iface_state_query(ist);
		}
		g_csa.phase = CSA_VERIFY;
	}

	if (g_csa.phase == CSA_VERIFY && t_us >= g_csa.t_revert_us) {
		bool alive = false;
		if (g_csa.baseline_rx_idx >= 0) {
			uint32_t now_pkt = c->tunnels[g_csa.baseline_rx_idx].st_pkt_all;
			alive = (now_pkt != g_csa.baseline_pkt_all);
		}
		if (alive || g_csa.no_revert) {
			LOG_INFO("csa: VERIFY ok (rx_alive=%d no_revert=%d) — committed",
			         (int)alive, (int)g_csa.no_revert);
		} else {
			LOG_WARN("csa: no rx traffic since T_switch — reverting "
			         "%d iface(s) to channel %d %s",
			         g_csa.iface_count, g_csa.prev_chan, g_csa.prev_ht);
			for (int i = 0; i < g_csa.iface_count; i++) {
				(void)run_iw_set_channel(g_csa.ifaces[i],
				    g_csa.prev_chan, g_csa.prev_ht);
				IfaceState *ist = iface_state_find(g_csa.ifaces[i]);
				if (ist) (void)iface_state_query(ist);
			}
		}
		g_csa.phase = CSA_IDLE;
	}
}
