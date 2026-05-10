/*
 * gs_supervisor_scan.c — passive channel scanner.
 *
 * Cycles the GS-side iface(s) through a channel/HT list, dwelling on each
 * combo for `dwell_ms`. After each dwell, the rx tunnel's pkt_all counter
 * is compared against the per-step baseline; an increase means traffic
 * was decoded on that channel — which is enough to declare we found the
 * vehicle. Useful when the operator doesn't know which channel the
 * vehicle is on (lost contact, fresh boot, regulator switch, etc.).
 *
 * No vehicle cooperation needed — purely a passive sweep on the GS.
 *
 * One scan in flight at a time; concurrent CSA blocks scan and vice
 * versa.
 */

#include "gs_supervisor.h"

ScanState g_scan = { .phase = SCAN_IDLE, .sess = 0 };

/* Hop the scan iface set to step index `i`. Skips the per-iface state
 * query (caller can do it post-scan) — every fork inside the dwell
 * window steals decode time and risks missing the vehicle's burst.
 *
 * After the hops, drains any rx-tunnel stats blobs that were already
 * queued by wfb_rx before we changed channels — those blobs reflect the
 * OLD channel's pkt.uniq and would falsely trip step_saw_traffic on the
 * very first dwell. */
void scan_apply_step_drained(Config *c, int i)
{
	if (i < 0 || i >= g_scan.step_count) return;
	int chan = g_scan.chans[i];
	const char *ht = g_scan.hts[i];
	for (int k = 0; k < g_scan.iface_count; k++) {
		int rc = run_iw_set_channel(g_scan.ifaces[k], chan, ht);
		if (rc != 0)
			LOG_WARN("scan: iw on %s rc=%d", g_scan.ifaces[k], rc);
	}
	g_scan.cur_step = i;
	g_scan.step_saw_traffic = false;
	if (g_scan.baseline_rx_idx >= 0) {
		Tunnel *rx = &c->tunnels[g_scan.baseline_rx_idx];
		if (rx->stats_local_fd >= 0) {
			/* Drain pending datagrams off the rx_ant socket so the
			 * fresh dwell starts with a clean baseline.  Re-emit each
			 * datagram to stats_fwd_addr if the tunnel was configured
			 * with `stats_out` — local consumption is intentionally
			 * skipped during scan dwells (counters are stale during
			 * channel hops) but downstream consumers should keep
			 * receiving samples without a hole every dwell. */
			char drain[4096];
			ssize_t got;
			while ((got = recv(rx->stats_local_fd, drain,
			                   sizeof(drain), MSG_DONTWAIT)) > 0) {
				if (rx->stats_fwd_active) {
					(void)sendto(rx->stats_local_fd,
					    drain, (size_t)got, 0,
					    (struct sockaddr *)&rx->stats_fwd_addr,
					    sizeof(rx->stats_fwd_addr));
				}
			}
		}
	}
	g_scan.step_started_us = now_us();
}

/* Scanner state machine: at each step deadline, decide whether the
 * dwell window saw any decryptable traffic. step_saw_traffic is
 * OR-set by stats_drain on every non-zero rx pkt.uniq sample — a
 * 100 ms interval count, not cumulative — so any "1" anywhere in the
 * window counts.  Called once per supervisor tick. */
void scan_tick(Config *c, uint64_t t_us)
{
	if (g_scan.phase != SCAN_RUNNING) return;
	if (t_us < g_scan.step_started_us + g_scan.step_dwell_us) return;

	bool found = g_scan.step_saw_traffic;
	g_scan.hops_done++;
	LOG_INFO("scan: step %d/%d ch %d %s — %s",
	         g_scan.cur_step + 1, g_scan.step_count,
	         g_scan.chans[g_scan.cur_step], g_scan.hts[g_scan.cur_step],
	         found ? "FOUND" : "no traffic");
	if (found) {
		g_scan.found_chan = g_scan.chans[g_scan.cur_step];
		snprintf(g_scan.found_ht, sizeof(g_scan.found_ht), "%s",
		         g_scan.hts[g_scan.cur_step]);
		LOG_INFO("scan: locked on chan %d %s after %d hop(s)",
		         g_scan.found_chan, g_scan.found_ht, g_scan.hops_done);
		g_scan.phase = SCAN_FOUND;
		for (int j = 0; j < g_scan.iface_count; j++) {
			IfaceState *ist = iface_state_find(g_scan.ifaces[j]);
			if (ist) (void)iface_state_query(ist);
		}
		return;
	}
	int next = g_scan.cur_step + 1;
	if (next >= g_scan.step_count) {
		LOG_WARN("scan: no traffic across %d step(s) — stopping",
		         g_scan.step_count);
		g_scan.phase = SCAN_STOPPED;
		for (int j = 0; j < g_scan.iface_count; j++) {
			IfaceState *ist = iface_state_find(g_scan.ifaces[j]);
			if (ist) (void)iface_state_query(ist);
		}
		return;
	}
	scan_apply_step_drained(c, next);
	g_scan.step_saw_traffic = false;
}
