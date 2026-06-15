/*
 * gs_supervisor_http.c — REST API + WebUI serving.
 *
 * Single-threaded poll() acceptor with API_MAX_CLIENTS slots; api_handle
 * dispatches one request synchronously per client and closes the
 * connection.  Routes are hand-rolled (no router framework) and live
 * under /api/v1/.
 *
 * The accept loop and per-tick poll() are still in the main supervisor;
 * this file only owns the dispatch + response side.
 */

#include "gs_supervisor.h"
#ifdef WFB_WITH_WFBNG
#include <unistd.h>
#include "wfb_keyseed.h"   /* key-management endpoint — mega build only (needs libsodium) */
#endif

/* ---------- Embedded WebUI ------------------------------------------- *
 *
 * webui/gs.html is xxd-i embedded by Makefile.gs_supervisor's `webui`
 * target as a const byte array. Served at `/` when the request carries
 * Accept: text/html (browser path); curl with no overrides gets the
 * /api/v1/health-style help text instead. */
#if __has_include("gs_assets.h")
#  include "gs_assets.h"
#else
   const unsigned char gs_webui_html[] =
       "<!doctype html><h1>gs_assets.h missing — run "
       "`make webui` from ground/.</h1>";
   const unsigned int  gs_webui_html_len = sizeof(gs_webui_html) - 1;
#endif

bool request_wants_html(const char *req)
{
	const char *eol = strchr(req, '\n');
	const char *headers = eol ? eol + 1 : req;
	return strstr(headers, "Accept: text/html") != NULL ||
	       strstr(headers, "accept: text/html") != NULL;
}

/* ---------- HTTP API ------------------------------------------------- */

int api_listen_open(const char *bind_ip, uint16_t port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
	if (!bind_ip || !*bind_ip) a.sin_addr.s_addr = htonl(INADDR_ANY);
	else if (inet_pton(AF_INET, bind_ip, &a.sin_addr) != 1) {
		close(fd); return -1;
	}
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
	if (listen(fd, 4) < 0) { close(fd); return -1; }
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	return fd;
}

void api_send(int fd, int code, const char *ctype, const char *body, int body_len)
{
	if (body_len < 0) body_len = (int)strlen(body);
	const char *reason = (code == 200) ? "OK" :
	                     (code == 400) ? "Bad Request" :
	                     (code == 404) ? "Not Found" :
	                     (code == 405) ? "Method Not Allowed" :
	                                     "Error";
	char hdr[256];
	int hl = snprintf(hdr, sizeof(hdr),
		"HTTP/1.0 %d %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %d\r\n"
		"Cache-Control: no-store\r\n"
		"Connection: close\r\n\r\n",
		code, reason, ctype, body_len);
	if (hl > 0 && write_all(fd, hdr, (size_t)hl) == 0)
		(void)write_all(fd, body, (size_t)body_len);
}

/* Send an arbitrary byte blob (e.g. embedded HTML). */
void api_send_blob(int fd, const char *ctype,
                          const unsigned char *body, unsigned int len)
{
	char hdr[256];
	int hl = snprintf(hdr, sizeof(hdr),
		"HTTP/1.0 200 OK\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %u\r\n"
		"Cache-Control: no-store\r\n"
		"Connection: close\r\n\r\n",
		ctype, len);
	if (hl > 0 && write_all(fd, hdr, (size_t)hl) == 0)
		(void)write_all(fd, body, (size_t)len);
}

int json_emit_tunnel(char *buf, size_t cap, const Tunnel *t, bool full)
{
	int p = 0;
	#define APP(...) do { int _w = snprintf(buf+p, cap-p, __VA_ARGS__); \
	                      if (_w < 0 || (size_t)_w >= cap-(size_t)p) return -1; \
	                      p += _w; } while (0)
	APP("{\"name\":\"%s\",\"role\":\"%s\",\"state\":\"%s\","
	    "\"pid\":%d,\"restarts\":%d,\"link_id\":%d,\"radio_port\":%d",
	    t->name, t->role, tunnel_state_name(t->state),
	    (int)t->pid, t->restart_count, t->link_id, t->radio_port);
	APP(",\"interfaces\":[");
	for (int i = 0; i < t->iface_count; i++)
		APP("%s\"%s\"", i ? "," : "", t->ifaces[i]);
	APP("]");
	if (t->started_us)
		APP(",\"uptime_s\":%.1f",
		    t->pid > 0 ? (double)(now_us() - t->started_us) / 1e6 : 0.0);
	if (t->exit_code || t->exited_us)
		APP(",\"last_exit\":%d", t->exit_code);
	if (full) {
		if (t->binary[0]) APP(",\"binary\":\"%s\"", t->binary);
		if (!strcmp(t->role, "rx")) {
			if (t->udp_out_ip[0])
				APP(",\"udp_out\":\"%s:%d\"", t->udp_out_ip, t->udp_out_port);
			if (t->stats_out[0]) APP(",\"stats_out\":\"%s\"", t->stats_out);
		} else {
			APP(",\"udp_in_port\":%d,\"control_port\":%d,"
			    "\"fec\":{\"k\":%d,\"n\":%d,\"timeout_ms\":%d,\"have\":%s},"
			    "\"radio\":{\"bw\":%d,\"mcs\":%d,\"stbc\":%d,\"ldpc\":%d,"
			    "\"short_gi\":%d,\"vht_mode\":%d,\"vht_nss\":%d,\"have\":%s}",
			    t->udp_in_port, t->control_port,
			    t->fec_k, t->fec_n, t->fec_timeout_ms,
			    t->fec_cache_have ? "true" : "false",
			    t->bandwidth_mhz, t->mcs_index, t->stbc, t->ldpc,
			    t->short_gi, t->vht_mode, t->vht_nss,
			    t->radio_cache_have ? "true" : "false");
		}
		APP(",\"autostart\":%s", t->autostart ? "true" : "false");
	}
	/* Always include stats for both roles — even if empty, the WebUI
	 * uses the presence of `msg_count` to decide whether to render the
	 * stats panel.  age_ms tells the UI how stale the snapshot is. */
	{
		uint64_t age_ms = 0;
		if (t->st_last_us) {
			uint64_t now_u = now_us();
			age_ms = (now_u > t->st_last_us) ? (now_u - t->st_last_us) / 1000 : 0;
		}
		APP(",\"stats\":{"
		    "\"msg_count\":%u,\"interval_ms\":%u",
		    t->st_msg_count, t->st_interval_ms);
		if (!strcmp(t->role, "rx")) {
			APP(",\"pkt_all\":%u,\"pkt_lost\":%u,\"pkt_fec_recovered\":%u,"
			    "\"pkt_outgoing\":%u,\"pkt_dec_err\":%u,\"pkt_uniq\":%u,"
			    "\"outgoing_bytes\":%u,\"ant_count\":%d",
			    t->st_pkt_all, t->st_pkt_lost, t->st_pkt_fec,
			    t->st_pkt_outgoing, t->st_pkt_dec_err, t->st_pkt_uniq,
			    t->st_pkt_bytes, t->st_ant_count);
			if (t->st_rssi_best != INT_MIN) APP(",\"rssi_best\":%d", t->st_rssi_best);
			/* Received-MCS histogram over the last 10 s (adaptive-MCS
			 * visibility). Sum the ring slots stamped
			 * within the window; only non-zero rungs; object omitted
			 * when there is no per-MCS data. */
			{
				uint64_t nsec = now_us() / 1000000ULL;
				uint32_t win[16] = {0};
				for (int sl = 0; sl < 10; sl++) {
					if (t->st_mcs_win_sec[sl] == 0) continue;
					if (nsec - t->st_mcs_win_sec[sl] >= 10) continue;
					for (int m = 0; m < 16; m++)
						win[m] += t->st_mcs_win[sl][m];
				}
				int mh_any = 0;
				for (int m = 0; m < 16; m++) {
					if (win[m] == 0) continue;
					APP("%s\"%d\":%u",
					    mh_any ? "," : ",\"mcs_hist\":{",
					    m, win[m]);
					mh_any = 1;
				}
				if (mh_any) APP("}");
			}
			if (t->probe)
				APP(",\"probe\":{\"window_ms\":%d,"
				    "\"emitted\":%u,\"stale_dropped\":%u}",
				    t->probe_window_ms,
				    t->probe_emit_count, t->probe_drop_count);
		} else {
			APP(",\"pkts_in\":%u,\"pkts_out\":%u,"
			    "\"bytes_in\":%u,\"bytes_out\":%u,"
			    "\"pkts_drop\":%u,\"pkts_trunc\":%u,\"fec_timeouts\":%u",
			    t->st_tx_pkts_in, t->st_tx_pkts_out,
			    t->st_tx_bytes_in, t->st_tx_bytes_out,
			    t->st_tx_drop, t->st_tx_trunc, t->st_tx_fec_timeouts);
		}
		APP(",\"age_ms\":%llu", (unsigned long long)age_ms);
		if (t->stats_local_port)
			APP(",\"listen_port\":%u", (unsigned)t->stats_local_port);
		APP("}");
	}
	APP("}");
	return p;
	#undef APP
}

int json_emit_status(char *buf, size_t cap, const Config *c, uint64_t up_us)
{
	int p = 0;
	#define APP(...) do { int _w = snprintf(buf+p, cap-p, __VA_ARGS__); \
	                      if (_w < 0 || (size_t)_w >= cap-(size_t)p) return -1; \
	                      p += _w; } while (0)
	APP("{\"uptime_s\":%.1f,\"system_state\":\"%s\",\"wcmd\":{"
	    "\"emit_total\":%llu,\"emit_frames\":%llu,"
	    "\"rate_limited\":%llu,\"emit_failed\":%llu,"
	    "\"burst_frames\":%d},\"tunnels\":[",
	    (double)up_us / 1e6,
	    system_state_name(c->system_state),
	    (unsigned long long)g_wcmd_emit_total,
	    (unsigned long long)g_wcmd_emit_frames,
	    (unsigned long long)g_wcmd_emit_rate_limit,
	    (unsigned long long)g_wcmd_emit_failed,
	    WCMD_BURST_FRAMES);
	for (int i = 0; i < c->tunnel_count; i++) {
		if (i) APP(",");
		int n = json_emit_tunnel(buf + p, cap - p, &c->tunnels[i], false);
		if (n < 0) return -1;
		p += n;
	}
	APP("]}");
	return p;
	#undef APP
}

Tunnel *cfg_find_tunnel(Config *c, const char *name)
{
	for (int i = 0; i < c->tunnel_count; i++)
		if (!strcmp(c->tunnels[i].name, name)) return &c->tunnels[i];
	return NULL;
}

/* Routes:
 *   GET /api/v1/health
 *   GET /api/v1/status
 *   GET /api/v1/tunnels
 *   GET /api/v1/tunnels/{name}
 *   GET /api/v1/tunnels/{name}/start
 *   GET /api/v1/tunnels/{name}/stop
 *   GET /api/v1/tunnels/{name}/restart
 *   GET /api/v1/tunnels/{name}/control?cmd=set_fec&k=1&n=2[&fec_timeout_ms=50]
 *   GET /api/v1/tunnels/{name}/control?cmd=set_radio&stbc=&ldpc=&short_gi=&bandwidth=&mcs_index=&vht_mode=&vht_nss=
 *   GET /api/v1/tunnels/{name}/control?cmd=get_fec
 *   GET /api/v1/tunnels/{name}/control?cmd=get_radio
 */
void api_handle(ApiClient *cli, Config *c, uint64_t startup_us)
{
	cli->buf[cli->pos < API_BUF_BYTES ? cli->pos : API_BUF_BYTES - 1] = 0;

	if (strncmp(cli->buf, "GET ", 4) != 0) {
		api_send(cli->fd, 405, "text/plain", "expected GET\n", -1);
		return;
	}
	bool wants_html = request_wants_html(cli->buf);
	char *path = cli->buf + 4;
	char *sp = strchr(path, ' ');
	if (!sp) { api_send(cli->fd, 400, "text/plain", "bad request\n", -1); return; }
	*sp = 0;
	char *qs = strchr(path, '?');
	const char *qstr = NULL;
	if (qs) { *qs = 0; qstr = qs + 1; }

	/* venc-style alias: GET /request/idr → /api/v1/cmd?key=force_idr.
	 * Lets clients that already speak venc's URL hit the ground supervisor
	 * directly; the WCMD path, rate-limit, and response shape are unchanged. */
	if (!strcmp(path, "/request/idr")) {
		path = (char *)"/api/v1/cmd";
		qstr = "key=force_idr";
	}

	char body[8192];
	int n;

	if (!strcmp(path, "/")) {
		if (wants_html) {
			api_send_blob(cli->fd, "text/html; charset=utf-8",
			              gs_webui_html, gs_webui_html_len);
		} else {
			api_send(cli->fd, 200, "text/plain",
			    "gs_supervisor — see /api/v1/health|status|tunnels|cmd. "
			    "Open this URL in a browser for the WebUI.\n", -1);
		}
		return;
	}
	if (!strcmp(path, "/api/v1/health")) {
		api_send(cli->fd, 200, "text/plain", "ok\n", -1);
		return;
	}
	if (!strcmp(path, "/api/v1/ifaces")) {
		/* Per-iface current chan/ht/txpower snapshot.  Cached value is
		 * up to GS_IFACE_REFRESH_US (2 s) old by default; we report
		 * `age_ms` so the UI can grey out stale rows. */
		uint64_t now = now_us();
		int p = 0;
		p += snprintf(body + p, sizeof(body) - p, "[");
		for (int i = 0; i < g_iface_state_count; i++) {
			const IfaceState *st = &g_iface_state[i];
			double age_ms = st->last_query_us
			    ? (double)(now - st->last_query_us) / 1000.0 : -1.0;
			if (i) p += snprintf(body + p, sizeof(body) - p, ",");
			p += snprintf(body + p, sizeof(body) - p,
			    "{\"name\":\"%s\",\"chan\":%d,\"freq_mhz\":%d,"
			    "\"ht\":\"%s\",\"txpower_mbm\":%d,"
			    "\"age_ms\":%.0f,\"iw_rc\":%d}",
			    st->name, st->chan, st->freq_mhz, st->ht,
			    st->txpower_mbm, age_ms, st->last_rc);
		}
		p += snprintf(body + p, sizeof(body) - p, "]");
		api_send(cli->fd, 200, "application/json", body, p);
		return;
	}
	if (!strcmp(path, "/api/v1/status")) {
		n = json_emit_status(body, sizeof(body), c, now_us() - startup_us);
		if (n < 0) { api_send(cli->fd, 500, "text/plain", "overflow\n", -1); return; }
		api_send(cli->fd, 200, "application/json", body, n);
		return;
	}
	if (!strcmp(path, "/api/v1/tunnels")) {
		int p = 0;
		p += snprintf(body + p, sizeof(body) - p, "[");
		for (int i = 0; i < c->tunnel_count; i++) {
			if (i) p += snprintf(body + p, sizeof(body) - p, ",");
			int w = json_emit_tunnel(body + p, sizeof(body) - p,
			                         &c->tunnels[i], true);
			if (w < 0) { api_send(cli->fd, 500, "text/plain", "overflow\n", -1); return; }
			p += w;
		}
		p += snprintf(body + p, sizeof(body) - p, "]");
		api_send(cli->fd, 200, "application/json", body, p);
		return;
	}
	if (!strcmp(path, "/api/v1/system/reinit")) {
		/* Last-resort recovery: stop every tunnel, run system.down, then
		 * system.up + wait_iface_state, then respawn autostart tunnels.
		 * Unsticks adapters left in a bad post-monitor state
		 * (txpower=-100, NO-CARRIER while in monitor mode, etc.) —
		 * the symptoms seen when WCMD packets stopped getting on-air.
		 * Stats listeners stay open across reinit: wfb_rx/wfb_tx will
		 * be respawned with the same -Y target. */
		LOG_WARN("REST /system/reinit: tearing down all tunnels");
		supervisor_stop_all_tunnels(c);
		LOG_INFO("REST /system/reinit: running system.down + system.up");
		run_system_block("system.down", c->system_down, c->system_down_count);
		/* No need to set c->system_state = SYS_DOWN here —
		 * supervisor_bring_up writes the state unconditionally on both
		 * success (SYS_UP) and failure (SYS_UP_FAILED) paths. The
		 * inline system.down above only rolls back the adapters; the
		 * state field is owned by the bring-up call that follows. */
		if (supervisor_bring_up(c) != 0) {
			api_send(cli->fd, 503, "application/json",
			         "{\"ok\":false,\"error\":\"iface readiness timeout — "
			         "see supervisor log; state is up_failed\","
			         "\"system_state\":\"up_failed\"}", -1);
			return;
		}
		int spawned = 0;
		for (int i = 0; i < c->tunnel_count; i++) {
			Tunnel *t = &c->tunnels[i];
			/* pid <= 0 guard: tunnel_spawn doesn't check, so a manually
			 * started tunnel (POST /tunnels/<name>/start while system
			 * was down) would otherwise get re-forked here, orphaning
			 * the original child. The state machine doesn't formally
			 * enforce "no tunnels while not up", so be defensive. */
			if (t->autostart && t->pid <= 0) {
				t->backoff_idx = 0;
				t->next_start_ms = 0;
				if (tunnel_spawn(t, c->key_file) == 0) spawned++;
			}
		}
		char body[160];
		int p = snprintf(body, sizeof(body),
		                 "{\"ok\":true,\"reinit\":true,\"tunnels_spawned\":%d,"
		                 "\"system_state\":\"%s\"}",
		                 spawned, system_state_name(c->system_state));
		api_send(cli->fd, 200, "application/json", body, p);
		return;
	}
	if (!strcmp(path, "/api/v1/system/up")) {
		/* Run system.up + readiness wait, then autospawn tunnels.
		 * Refuses if system is already up — operator should use
		 * /system/reinit for a clean cycle. */
		if (c->system_state == SYS_UP) {
			api_send(cli->fd, 409, "application/json",
			         "{\"ok\":false,\"error\":\"already up — use "
			         "/api/v1/system/reinit to cycle\"}", -1);
			return;
		}
		LOG_WARN("REST /system/up: bringing up adapters");
		if (supervisor_bring_up(c) != 0) {
			api_send(cli->fd, 503, "application/json",
			         "{\"ok\":false,\"error\":\"iface readiness timeout — "
			         "see supervisor log; state is up_failed\","
			         "\"system_state\":\"up_failed\"}", -1);
			return;
		}
		int spawned = 0;
		for (int i = 0; i < c->tunnel_count; i++) {
			Tunnel *t = &c->tunnels[i];
			/* pid <= 0 guard: tunnel_spawn doesn't check, so a manually
			 * started tunnel (POST /tunnels/<name>/start while system
			 * was down) would otherwise get re-forked here, orphaning
			 * the original child. The state machine doesn't formally
			 * enforce "no tunnels while not up", so be defensive. */
			if (t->autostart && t->pid <= 0) {
				t->backoff_idx = 0;
				t->next_start_ms = 0;
				if (tunnel_spawn(t, c->key_file) == 0) spawned++;
			}
		}
		char body[160];
		int p = snprintf(body, sizeof(body),
		                 "{\"ok\":true,\"tunnels_spawned\":%d,"
		                 "\"system_state\":\"%s\"}",
		                 spawned, system_state_name(c->system_state));
		api_send(cli->fd, 200, "application/json", body, p);
		return;
	}
	if (!strcmp(path, "/api/v1/system/down")) {
		/* Stop every tunnel, then run system.down. Always safe to call:
		 * a no-op tunnel-stop is harmless, and running system.down
		 * against already-down adapters is fine (nmcli/iw will just
		 * report "already" and continue). */
		LOG_WARN("REST /system/down: stopping tunnels and running system.down");
		/* Count tunnels that were running at request time. We capture
		 * BEFORE supervisor_take_down because the helper clears pid
		 * as it reaps. Note this is "tunnels we asked to stop", not
		 * "tunnels confirmed dead" — SIGKILL is unblockable so the
		 * two are effectively equal in practice. */
		int running_before = 0;
		for (int i = 0; i < c->tunnel_count; i++)
			if (c->tunnels[i].pid > 0) running_before++;
		supervisor_take_down(c);
		char body[128];
		int p = snprintf(body, sizeof(body),
		                 "{\"ok\":true,\"tunnels_stopped\":%d,"
		                 "\"system_state\":\"%s\"}",
		                 running_before, system_state_name(c->system_state));
		api_send(cli->fd, 200, "application/json", body, p);
		return;
	}
	if (!strcmp(path, "/api/v1/cmd")) {
		size_t kl;
		const char *kstr = qs_get(qstr, "key", &kl);
		if (!kstr) {
			api_send(cli->fd, 400, "text/plain", "missing ?key=\n", -1);
			return;
		}
		int key = wcmd_key_from_str(kstr, kl);
		if (key < 0) {
			api_send(cli->fd, 400, "text/plain",
			         "key must be bitrate_kbps|fps|payload_bytes|force_idr|"
			         "wfb_fec_k|wfb_fec_n|wfb_mcs|wfb_bandwidth|"
			         "wfb_ldpc|wfb_stbc|wfb_short_gi|"
			         "fec_enabled|mcs_enabled|wfb_txpower\n", -1);
			return;
		}
		int32_t value = 0;
		int v;
		if (qs_get_int(qstr, "value", &v) == 0) value = (int32_t)v;
		else if (key != WCMD_KEY_FORCE_IDR) {
			api_send(cli->fd, 400, "text/plain",
			         "this key requires ?value=\n", -1);
			return;
		}
		/* Rate limit per key. */
		if (c->venc_cmd_rate_limit_ms > 0 && key >= 1 && key <= WCMD_KEY_MAX) {
			uint64_t t_ms = now_ms();
			uint64_t since = t_ms - g_wcmd_last_send_ms[key];
			if (g_wcmd_last_send_ms[key] != 0 &&
			    (int)since < c->venc_cmd_rate_limit_ms) {
				g_wcmd_emit_rate_limit++;
				char body[128];
				int p = snprintf(body, sizeof(body),
				                 "{\"ok\":false,\"error\":\"rate_limited\","
				                 "\"key\":\"%s\",\"retry_after_ms\":%d}",
				                 wcmd_key_name(key),
				                 c->venc_cmd_rate_limit_ms - (int)since);
				api_send(cli->fd, 429, "application/json", body, p);
				return;
			}
		}
		uint16_t seq = 0;
		int rc = wcmd_emit(c, key, value, &seq);
		if (rc == -1) {
			api_send(cli->fd, 503, "application/json",
			         "{\"ok\":false,\"error\":\"venc_cmd disabled or uplink missing\"}",
			         -1);
			return;
		}
		if (rc != 0) {
			char body[128];
			int p = snprintf(body, sizeof(body),
			                 "{\"ok\":false,\"error\":\"sendto: %s\"}",
			                 strerror(rc));
			api_send(cli->fd, 502, "application/json", body, p);
			return;
		}
		g_wcmd_last_send_ms[key] = now_ms();
		char body[128];
		int p;
		if (key == WCMD_KEY_FORCE_IDR) {
			p = snprintf(body, sizeof(body),
			             "{\"ok\":true,\"seq\":%u,\"key\":\"%s\"}",
			             (unsigned)seq, wcmd_key_name(key));
		} else {
			p = snprintf(body, sizeof(body),
			             "{\"ok\":true,\"seq\":%u,\"key\":\"%s\",\"value\":%d}",
			             (unsigned)seq, wcmd_key_name(key), (int)value);
		}
		api_send(cli->fd, 200, "application/json", body, p);
		return;
	}
	/* Roll the vehicle's SD telemetry log (WCMD_KEY_LOG_CONTROL=19, infra key
	 * like LOG_SYNC).  value=1 (default) starts/rolls a fresh session, 0 stops.
	 * Fired by the telemetry "New capture" button so a fresh GS session gets a
	 * matching, time-aligned vehicle log.  Bypasses the operator rate-limit/name
	 * table (the key sits above WCMD_KEY_MAX). */
	if (!strcmp(path, "/api/v1/logctl")) {
		int v = 1;
		(void)qs_get_int(qstr, "value", &v);
		int32_t value = (v != 0) ? 1 : 0;
		uint16_t seq = 0;
		int rc = wcmd_emit(c, WCMD_KEY_LOG_CONTROL, value, &seq);
		if (rc == -1) {
			api_send(cli->fd, 503, "application/json",
			         "{\"ok\":false,\"error\":\"venc_cmd disabled or uplink missing\"}", -1);
			return;
		}
		if (rc != 0) {
			char body[128];
			int p = snprintf(body, sizeof(body),
			                 "{\"ok\":false,\"error\":\"sendto: %s\"}", strerror(rc));
			api_send(cli->fd, 502, "application/json", body, p);
			return;
		}
		char body[96];
		int p = snprintf(body, sizeof(body),
		                 "{\"ok\":true,\"seq\":%u,\"key\":\"log_control\",\"value\":%d}",
		                 (unsigned)seq, (int)value);
		api_send(cli->fd, 200, "application/json", body, p);
		return;
	}
	if (!strcmp(path, "/api/v1/system/scan")) {
		/* Required: iface (comma-sep), chans (comma-sep). Optional: ht /
		 * hts (single HT for all steps, or comma-sep per step), dwell_ms
		 * (default 600). Refuses if a CSA hop or another scan is in flight. */
		if (g_csa.phase != CSA_IDLE) {
			api_send(cli->fd, 409, "application/json",
			    "{\"ok\":false,\"error\":\"csa active\"}", -1);
			return;
		}
		if (g_scan.phase == SCAN_RUNNING) {
			api_send(cli->fd, 409, "application/json",
			    "{\"ok\":false,\"error\":\"scan already running\"}", -1);
			return;
		}
		size_t il, cl, hl_ht, hl_hts;
		const char *istr = qs_get(qstr, "iface", &il);
		const char *cstr = qs_get(qstr, "chans", &cl);
		const char *htstr = qs_get(qstr, "ht", &hl_ht);
		const char *htsstr = qs_get(qstr, "hts", &hl_hts);
		if (!istr || il == 0 || !cstr || cl == 0) {
			api_send(cli->fd, 400, "application/json",
			    "{\"ok\":false,\"error\":\"need iface=&chans=\"}", -1);
			return;
		}
		/* parse iface list */
		char ifaces[GS_CSA_MAX_IFACES][IFNAMSIZ];
		int  iface_count = 0;
		const char *p = istr, *e = istr + il;
		while (p < e && iface_count < GS_CSA_MAX_IFACES) {
			const char *q = p;
			while (q < e && *q != ',') q++;
			size_t L = (size_t)(q - p);
			if (L == 0 || L >= IFNAMSIZ) {
				api_send(cli->fd, 400, "application/json",
				    "{\"ok\":false,\"error\":\"bad iface in list\"}", -1);
				return;
			}
			memcpy(ifaces[iface_count], p, L);
			ifaces[iface_count][L] = 0;
			iface_count++;
			p = q + 1;
		}
		/* If we hit the iface cap mid-list, p still has data left.  Reject
		 * loudly rather than silently dropping the operator's tail input. */
		if (p < e) {
			char b[96];
			int bp = snprintf(b, sizeof(b),
			    "{\"ok\":false,\"error\":\"too many ifaces (max %d)\"}",
			    GS_CSA_MAX_IFACES);
			api_send(cli->fd, 400, "application/json", b, bp);
			return;
		}
		if (iface_count == 0) {
			api_send(cli->fd, 400, "application/json",
			    "{\"ok\":false,\"error\":\"no iface\"}", -1);
			return;
		}
		for (int j = 0; j < iface_count; j++) {
			bool known = false;
			for (int i = 0; i < c->tunnel_count && !known; i++)
				for (int k = 0; k < c->tunnels[i].iface_count; k++)
					if (!strcmp(c->tunnels[i].ifaces[k], ifaces[j])) {
						known = true; break;
					}
			if (!known) {
				char b[96];
				int bp = snprintf(b, sizeof(b),
				    "{\"ok\":false,\"error\":\"iface '%s' not in any tunnel\"}",
				    ifaces[j]);
				api_send(cli->fd, 404, "application/json", b, bp);
				return;
			}
		}
		/* parse chan list */
		int  chans[SCAN_MAX_STEPS];
		int  chan_count = 0;
		p = cstr; e = cstr + cl;
		while (p < e && chan_count < SCAN_MAX_STEPS) {
			char buf[8]; int bn = 0;
			while (p < e && *p != ',' && bn < (int)sizeof(buf) - 1) buf[bn++] = *p++;
			buf[bn] = 0;
			if (bn == 0) {
				api_send(cli->fd, 400, "application/json",
				    "{\"ok\":false,\"error\":\"empty chan in list\"}", -1);
				return;
			}
			char *endp = NULL;
			long ch = strtol(buf, &endp, 10);
			if (endp == buf || ch < 1 || ch > 200) {
				api_send(cli->fd, 400, "application/json",
				    "{\"ok\":false,\"error\":\"chan out of range (1..200)\"}", -1);
				return;
			}
			chans[chan_count++] = (int)ch;
			if (p < e) p++;
		}
		if (p < e) {
			char b[96];
			int bp = snprintf(b, sizeof(b),
			    "{\"ok\":false,\"error\":\"too many chans (max %d)\"}",
			    SCAN_MAX_STEPS);
			api_send(cli->fd, 400, "application/json", b, bp);
			return;
		}
		if (chan_count == 0) {
			api_send(cli->fd, 400, "application/json",
			    "{\"ok\":false,\"error\":\"no chans\"}", -1);
			return;
		}
		/* parse hts: prefer 'hts=' (per-step) over 'ht=' (single).
		 * If neither given, default everything to HT20. */
		char hts[SCAN_MAX_STEPS][8];
		for (int i = 0; i < chan_count; i++) snprintf(hts[i], 8, "HT20");
		if (htsstr && hl_hts > 0) {
			int hi = 0;
			p = htsstr; e = htsstr + hl_hts;
			while (p < e && hi < SCAN_MAX_STEPS) {
				const char *q = p;
				while (q < e && *q != ',') q++;
				size_t L = (size_t)(q - p);
				if (L == 0 || L >= 8) {
					api_send(cli->fd, 400, "application/json",
					    "{\"ok\":false,\"error\":\"bad ht\"}", -1);
					return;
				}
				memcpy(hts[hi], p, L); hts[hi][L] = 0;
				hi++;
				p = q + 1;
			}
			if (hi == 1 && chan_count > 1) {
				/* repeat first across all */
				for (int i = 1; i < chan_count; i++)
					snprintf(hts[i], 8, "%s", hts[0]);
			} else if (hi != chan_count) {
				api_send(cli->fd, 400, "application/json",
				    "{\"ok\":false,\"error\":\"hts count != chans count\"}", -1);
				return;
			}
		} else if (htstr && hl_ht > 0 && hl_ht < 8) {
			char one[8];
			memcpy(one, htstr, hl_ht); one[hl_ht] = 0;
			for (int i = 0; i < chan_count; i++)
				snprintf(hts[i], 8, "%s", one);
		}
		for (int i = 0; i < chan_count; i++) {
			if (strcmp(hts[i], "HT20") &&
			    strcmp(hts[i], "HT40+") &&
			    strcmp(hts[i], "HT40-")) {
				api_send(cli->fd, 400, "application/json",
				    "{\"ok\":false,\"error\":\"ht must be HT20|HT40+|HT40-\"}", -1);
				return;
			}
		}
		int dwell_ms = 600;
		(void)qs_get_int(qstr, "dwell_ms", &dwell_ms);
		if (dwell_ms < 100)  dwell_ms = 100;
		if (dwell_ms > 10000) dwell_ms = 10000;

		/* Arm scan state. */
		memset(&g_scan, 0, sizeof(g_scan));
		g_scan.sess++;
		for (int j = 0; j < iface_count; j++)
			snprintf(g_scan.ifaces[j], sizeof(g_scan.ifaces[j]),
			    "%s", ifaces[j]);
		g_scan.iface_count = iface_count;
		for (int i = 0; i < chan_count; i++) {
			g_scan.chans[i] = chans[i];
			/* hts[i] is parsed/validated to ≤7 + NUL above (see L<8
			 * guard).  Use explicit length copy so gcc-13's stricter
			 * -Wformat-truncation doesn't trip on snprintf("%s", src)
			 * when src and dst are both char[8]. */
			size_t hl = strnlen(hts[i], sizeof(hts[i]) - 1);
			memcpy(g_scan.hts[i], hts[i], hl);
			g_scan.hts[i][hl] = '\0';
		}
		g_scan.step_count = chan_count;
		g_scan.step_dwell_us = (uint64_t)dwell_ms * 1000ULL;
		g_scan.baseline_rx_idx = csa_pick_rx_tunnel_idx(c);
		g_scan.step_saw_traffic = false;
		g_scan.found_chan = -1;
		g_scan.phase = SCAN_RUNNING;
		scan_apply_step_drained(c, 0);
		LOG_INFO("scan: armed sess=%u %d iface(s) %d step(s) dwell=%d ms",
		         g_scan.sess, iface_count, chan_count, dwell_ms);

		char buf[400];
		int bp = snprintf(buf, sizeof(buf),
		    "{\"ok\":true,\"sess\":%u,\"step_count\":%d,\"dwell_ms\":%d,"
		    "\"ifaces\":[",
		    g_scan.sess, chan_count, dwell_ms);
		for (int j = 0; j < iface_count; j++)
			bp += snprintf(buf + bp, sizeof(buf) - bp, "%s\"%s\"",
			    j ? "," : "", ifaces[j]);
		bp += snprintf(buf + bp, sizeof(buf) - bp, "]}");
		api_send(cli->fd, 200, "application/json", buf, bp);
		return;
	}
	if (!strcmp(path, "/api/v1/system/scan/cancel")) {
		if (g_scan.phase != SCAN_RUNNING) {
			api_send(cli->fd, 200, "application/json",
			    "{\"ok\":true,\"already_idle\":true}", -1);
			return;
		}
		LOG_INFO("scan: cancel requested at step %d/%d",
		         g_scan.cur_step + 1, g_scan.step_count);
		g_scan.phase = SCAN_STOPPED;
		api_send(cli->fd, 200, "application/json",
		    "{\"ok\":true,\"cancelled\":true}", -1);
		return;
	}
	if (!strcmp(path, "/api/v1/scan")) {
		uint64_t t_us = now_us();
		double dwell_left_ms = (g_scan.phase == SCAN_RUNNING)
		    ? (double)((int64_t)(g_scan.step_started_us + g_scan.step_dwell_us)
		               - (int64_t)t_us) / 1000.0
		    : 0.0;
		int cur_chan = (g_scan.cur_step < g_scan.step_count)
		    ? g_scan.chans[g_scan.cur_step] : -1;
		const char *cur_ht = (g_scan.cur_step < g_scan.step_count)
		    ? g_scan.hts[g_scan.cur_step] : "";
		/* Sized for worst case: 4 ifaces (≈19 chars each) + 32 chans
		 * (4 chars each) + JSON envelope ≈ 360 B; 768 leaves headroom. */
		char body2[768];
		int p = snprintf(body2, sizeof(body2),
		    "{\"phase\":%d,\"sess\":%u,\"cur_step\":%d,\"step_count\":%d,"
		    "\"hops_done\":%d,\"dwell_left_ms\":%.0f,"
		    "\"cur_chan\":%d,\"cur_ht\":\"%s\","
		    "\"found_chan\":%d,\"found_ht\":\"%s\","
		    "\"ifaces\":[",
		    (int)g_scan.phase, g_scan.sess, g_scan.cur_step, g_scan.step_count,
		    g_scan.hops_done, dwell_left_ms,
		    cur_chan, cur_ht,
		    g_scan.found_chan, g_scan.found_ht);
		for (int j = 0; j < g_scan.iface_count; j++)
			p += snprintf(body2 + p, sizeof(body2) - p, "%s\"%s\"",
			    j ? "," : "", g_scan.ifaces[j]);
		p += snprintf(body2 + p, sizeof(body2) - p, "],\"chans\":[");
		for (int i = 0; i < g_scan.step_count; i++)
			p += snprintf(body2 + p, sizeof(body2) - p, "%s%d",
			    i ? "," : "", g_scan.chans[i]);
		p += snprintf(body2 + p, sizeof(body2) - p, "]}");
		api_send(cli->fd, 200, "application/json", body2, p);
		return;
	}
	if (!strcmp(path, "/api/v1/system/csa")) {
		/* Synchronized channel hop. Required: chan, ht, iface. Optional:
		 * lead_ms (default 500), t_revert_ms (default 3000),
		 * prev_chan/prev_ht (defaults: assume current channel from the
		 * iface's last known state — operator override accepted),
		 * no_revert (1 disables auto-revert; default 0).
		 *
		 * Schedule: now → BURST (5 frames @ 20 ms) → ARMED → at T_switch
		 * fork iw set channel → VERIFY for t_revert_ms → revert if no
		 * traffic.  Vehicle's csa_tick mirrors the same revert deadline. */
		if (g_csa.phase != CSA_IDLE) {
			char body[160];
			int p = snprintf(body, sizeof(body),
			    "{\"ok\":false,\"error\":\"csa already active\","
			    "\"phase\":%d,\"sess\":%u}",
			    (int)g_csa.phase, g_csa.sess);
			api_send(cli->fd, 409, "application/json", body, p);
			return;
		}
		/* Refuse while a scan is dwelling — both subsystems drive iw on
		 * the same iface; interleaving would mis-time hops.  Operator
		 * cancels via /api/v1/system/scan/cancel first.  Mirrored on the
		 * scan handler so the lock is symmetric. */
		if (g_scan.phase == SCAN_RUNNING) {
			char body[160];
			int p = snprintf(body, sizeof(body),
			    "{\"ok\":false,\"error\":\"scan running — cancel first\","
			    "\"scan_sess\":%u}",
			    g_scan.sess);
			api_send(cli->fd, 409, "application/json", body, p);
			return;
		}
		size_t il, hl_ht;
		const char *istr = qs_get(qstr, "iface", &il);
		const char *htstr = qs_get(qstr, "ht", &hl_ht);
		int chan = -1;
		if (qs_get_int(qstr, "chan", &chan) != 0 || chan < 1 || chan > 200) {
			api_send(cli->fd, 400, "text/plain",
			    "missing/bad ?chan= (1..200)\n", -1);
			return;
		}
		if (!istr || il == 0 ||
		    !htstr || hl_ht == 0 || hl_ht >= 8) {
			api_send(cli->fd, 400, "text/plain",
			    "missing ?iface=name1,name2,…&ht=HT20|HT40+|HT40-\n", -1);
			return;
		}
		/* Accept comma-separated iface list so a diversity-RX setup
		 * (multiple GS adapters per video tunnel) can hop atomically. */
		char ifaces[GS_CSA_MAX_IFACES][IFNAMSIZ];
		int  iface_count = 0;
		const char *p = istr, *end = istr + il;
		while (p < end && iface_count < GS_CSA_MAX_IFACES) {
			const char *q = p;
			while (q < end && *q != ',') q++;
			size_t len = (size_t)(q - p);
			if (len == 0 || len >= IFNAMSIZ) {
				api_send(cli->fd, 400, "text/plain",
				    "bad iface in list\n", -1); return;
			}
			memcpy(ifaces[iface_count], p, len);
			ifaces[iface_count][len] = 0;
			iface_count++;
			p = q + 1;
		}
		if (p < end) {
			char body[96];
			int bp = snprintf(body, sizeof(body),
			    "too many ifaces (max %d)\n", GS_CSA_MAX_IFACES);
			api_send(cli->fd, 400, "text/plain", body, bp);
			return;
		}
		if (iface_count == 0) {
			api_send(cli->fd, 400, "text/plain", "no iface\n", -1);
			return;
		}
		for (int j = 0; j < iface_count; j++) {
			bool known = false;
			for (int i = 0; i < c->tunnel_count && !known; i++) {
				for (int k = 0; k < c->tunnels[i].iface_count; k++) {
					if (!strcmp(c->tunnels[i].ifaces[k], ifaces[j])) {
						known = true; break;
					}
				}
			}
			if (!known) {
				char body[96];
				int bp = snprintf(body, sizeof(body),
				    "iface '%s' is not in any tunnel\n", ifaces[j]);
				api_send(cli->fd, 404, "text/plain", body, bp);
				return;
			}
		}
		char ht[8];
		memcpy(ht, htstr, hl_ht); ht[hl_ht] = 0;
		if (strcmp(ht, "HT20") && strcmp(ht, "HT40+") && strcmp(ht, "HT40-")) {
			api_send(cli->fd, 400, "text/plain",
			    "ht must be HT20|HT40+|HT40-\n", -1);
			return;
		}
		int lead_ms = 500;
		int t_revert_ms = 3000;
		(void)qs_get_int(qstr, "lead_ms", &lead_ms);
		(void)qs_get_int(qstr, "t_revert_ms", &t_revert_ms);
		if (lead_ms < 100)  lead_ms = 100;
		if (lead_ms > 5000) lead_ms = 5000;
		if (t_revert_ms < 500)   t_revert_ms = 500;
		if (t_revert_ms > 30000) t_revert_ms = 30000;
		int prev_chan = chan;  /* fallback if operator omits */
		(void)qs_get_int(qstr, "prev_chan", &prev_chan);
		size_t pl = 0;
		const char *pstr = qs_get(qstr, "prev_ht", &pl);
		char prev_ht[8] = "HT20";
		if (pstr && pl > 0 && pl < 8) {
			memcpy(prev_ht, pstr, pl); prev_ht[pl] = 0;
		}
		int no_revert = 0;
		(void)qs_get_int(qstr, "no_revert", &no_revert);

		const Tunnel *up = NULL;
		for (int i = 0; i < c->tunnel_count; i++) {
			if (!strcmp(c->tunnels[i].name, c->venc_cmd_uplink) &&
			    !strcmp(c->tunnels[i].role, "tx")) {
				up = &c->tunnels[i]; break;
			}
		}
		if (!up || up->udp_in_port <= 0) {
			api_send(cli->fd, 503, "application/json",
			    "{\"ok\":false,\"error\":\"uplink tunnel missing\"}", -1);
			return;
		}

		/* Arm.  Frame burst is driven by supervisor_tick at 20 ms cadence,
		 * starting from the next tick. */
		uint64_t t_us = now_us();
		g_csa.sess += 1;
		g_csa.t_switch_us  = t_us + (uint64_t)lead_ms * 1000ULL;
		g_csa.t_revert_us  = g_csa.t_switch_us +
		                     (uint64_t)t_revert_ms * 1000ULL;
		g_csa.next_frame_us = t_us;
		g_csa.frames_sent  = 0;
		g_csa.frames_total = 5;
		g_csa.target_chan  = chan;
		snprintf(g_csa.target_ht, sizeof(g_csa.target_ht), "%s", ht);
		g_csa.prev_chan    = prev_chan;
		snprintf(g_csa.prev_ht,   sizeof(g_csa.prev_ht),   "%s", prev_ht);
		g_csa.iface_count = iface_count;
		for (int j = 0; j < iface_count; j++)
			snprintf(g_csa.ifaces[j], sizeof(g_csa.ifaces[j]),
			    "%s", ifaces[j]);
		g_csa.no_revert    = (no_revert != 0);
		g_csa_seq_in_burst = 0;
		g_csa.phase = CSA_BURST;
		LOG_INFO("csa: armed sess=%u ifaces=%d %d→%d (%s→%s) "
		         "lead=%d ms t_revert=%d ms",
		         g_csa.sess, g_csa.iface_count,
		         g_csa.prev_chan, g_csa.target_chan,
		         g_csa.prev_ht, g_csa.target_ht,
		         lead_ms, t_revert_ms);

		char body[400];
		int bp = snprintf(body, sizeof(body),
		    "{\"ok\":true,\"sess\":%u,\"chan\":%d,\"ht\":\"%s\","
		    "\"ifaces\":[",
		    g_csa.sess, chan, ht);
		for (int j = 0; j < iface_count; j++)
			bp += snprintf(body + bp, sizeof(body) - bp,
			    "%s\"%s\"", j ? "," : "", ifaces[j]);
		bp += snprintf(body + bp, sizeof(body) - bp,
		    "],\"lead_ms\":%d,\"t_revert_ms\":%d,"
		    "\"prev_chan\":%d,\"prev_ht\":\"%s\",\"no_revert\":%s}",
		    lead_ms, t_revert_ms, prev_chan, prev_ht,
		    no_revert ? "true" : "false");
		api_send(cli->fd, 200, "application/json", body, bp);
		return;
	}
	if (!strcmp(path, "/api/v1/csa")) {
		uint64_t t_us = now_us();
		double t_to_switch_ms = (g_csa.phase == CSA_BURST ||
		                         g_csa.phase == CSA_ARMED)
		    ? (double)((int64_t)g_csa.t_switch_us - (int64_t)t_us) / 1000.0
		    : 0.0;
		double t_to_revert_ms = (g_csa.phase == CSA_VERIFY)
		    ? (double)((int64_t)g_csa.t_revert_us - (int64_t)t_us) / 1000.0
		    : 0.0;
		char body[400];
		int bp = snprintf(body, sizeof(body),
		    "{\"phase\":%d,\"sess\":%u,\"frames_sent\":%d,"
		    "\"frames_total\":%d,\"target_chan\":%d,\"target_ht\":\"%s\","
		    "\"prev_chan\":%d,\"prev_ht\":\"%s\",\"ifaces\":[",
		    (int)g_csa.phase, g_csa.sess, g_csa.frames_sent,
		    g_csa.frames_total, g_csa.target_chan, g_csa.target_ht,
		    g_csa.prev_chan, g_csa.prev_ht);
		for (int j = 0; j < g_csa.iface_count; j++)
			bp += snprintf(body + bp, sizeof(body) - bp,
			    "%s\"%s\"", j ? "," : "", g_csa.ifaces[j]);
		bp += snprintf(body + bp, sizeof(body) - bp,
		    "],\"t_to_switch_ms\":%.1f,\"t_to_revert_ms\":%.1f,"
		    "\"no_revert\":%s}",
		    t_to_switch_ms, t_to_revert_ms,
		    g_csa.no_revert ? "true" : "false");
		api_send(cli->fd, 200, "application/json", body, bp);
		return;
	}
	if (!strcmp(path, "/api/v1/system/txpower")) {
		/* Per-iface `iw set txpower fixed <mBm>` on the GS host. We accept
		 * any iface that appears in some tunnel's iface list — anything
		 * else is a typo. The dispatch is synchronous (~30–80 ms on
		 * RTL88x2CU/EU); keep `mbm` in 50..3000 to match the WCMD clamp.
		 *
		 * Useful when the operator notices an adapter dropped to txpower=
		 * -100 dBm after a monitor-mode flip and wants to restore it
		 * without reinit, or when probing range vs throughput. */
		size_t il, ml;
		const char *istr = qs_get(qstr, "iface", &il);
		const char *mstr = qs_get(qstr, "mbm",   &ml);
		if (!istr || !mstr) {
			api_send(cli->fd, 400, "text/plain",
			         "missing ?iface=<name>&mbm=<50..3000>\n", -1);
			return;
		}
		char iface[IFNAMSIZ];
		if (il == 0 || il >= sizeof(iface)) {
			api_send(cli->fd, 400, "text/plain", "bad iface\n", -1); return;
		}
		memcpy(iface, istr, il); iface[il] = 0;
		bool known = false;
		for (int i = 0; i < c->tunnel_count && !known; i++) {
			for (int k = 0; k < c->tunnels[i].iface_count; k++) {
				if (!strcmp(c->tunnels[i].ifaces[k], iface)) {
					known = true; break;
				}
			}
		}
		if (!known) {
			api_send(cli->fd, 404, "text/plain",
			         "iface is not in any tunnel\n", -1);
			return;
		}
		int mbm;
		if (qs_get_int(qstr, "mbm", &mbm) != 0) {
			api_send(cli->fd, 400, "text/plain", "mbm not numeric\n", -1);
			return;
		}
		if (mbm < 50 || mbm > 3000) {
			api_send(cli->fd, 400, "text/plain",
			         "mbm out of range (50..3000)\n", -1);
			return;
		}
		char mbm_s[16];
		snprintf(mbm_s, sizeof(mbm_s), "%d", mbm);
		LOG_INFO("REST /system/txpower: iw dev %s set txpower fixed %s",
		         iface, mbm_s);
		pid_t pid = fork();
		int rc = -1;
		if (pid == 0) {
			execl("/usr/sbin/iw", "iw", "dev", iface, "set", "txpower",
			      "fixed", mbm_s, (char*)NULL);
			execlp("iw", "iw", "dev", iface, "set", "txpower",
			       "fixed", mbm_s, (char*)NULL);
			_exit(127);
		}
		if (pid > 0) {
			rc = waitpid_deadline(pid, GS_FORK_DEADLINE_MS);
			if (rc == -2) LOG_WARN("REST /system/txpower: iw dev %s "
			                       "deadline %d ms — SIGKILL'd",
			                       iface, GS_FORK_DEADLINE_MS);
		}
		IfaceState *ist = iface_state_find(iface);
		if (ist) (void)iface_state_query(ist);
		char body[160];
		int p = snprintf(body, sizeof(body),
		                 "{\"ok\":%s,\"iface\":\"%s\",\"mbm\":%d,\"iw_rc\":%d}",
		                 rc == 0 ? "true" : "false", iface, mbm, rc);
		api_send(cli->fd, rc == 0 ? 200 : 502, "application/json", body, p);
		return;
	}
	if (!strncmp(path, "/api/v1/tunnels/", 16)) {
		char name[GS_NAME_MAX];
		const char *rest = path + 16;
		const char *slash = strchr(rest, '/');
		size_t nlen = slash ? (size_t)(slash - rest) : strlen(rest);
		if (nlen == 0 || nlen >= sizeof(name)) {
			api_send(cli->fd, 400, "text/plain", "bad name\n", -1); return;
		}
		memcpy(name, rest, nlen); name[nlen] = 0;
		Tunnel *t = cfg_find_tunnel(c, name);
		if (!t) { api_send(cli->fd, 404, "text/plain", "no such tunnel\n", -1); return; }
		const char *action = slash ? slash + 1 : "";
		if (!*action) {
			n = json_emit_tunnel(body, sizeof(body), t, true);
			if (n < 0) { api_send(cli->fd, 500, "text/plain", "overflow\n", -1); return; }
			api_send(cli->fd, 200, "application/json", body, n);
			return;
		}
		if (!strcmp(action, "start")) {
			if (t->pid > 0) {
				api_send(cli->fd, 200, "application/json",
				         "{\"ok\":true,\"already\":true}", -1);
				return;
			}
			t->backoff_idx = 0;
			t->next_start_ms = 0;
			tunnel_spawn(t, c->key_file);
			api_send(cli->fd, 200, "application/json", "{\"ok\":true}", -1);
			return;
		}
		if (!strcmp(action, "stop")) {
			tunnel_request_stop(t);
			api_send(cli->fd, 200, "application/json", "{\"ok\":true}", -1);
			return;
		}
		if (!strcmp(action, "restart")) {
			tunnel_request_stop(t);
			t->autostart_on_exit = true;
			t->backoff_idx = 0;
			/* If the child was already gone (TS_BACKOFF or TS_STOPPED
			 * after tunnel_request_stop's early-return), queue an
			 * immediate respawn instead of leaving it parked. */
			if (t->pid <= 0) {
				t->state = TS_BACKOFF;
				t->next_start_ms = now_ms();
			}
			api_send(cli->fd, 200, "application/json", "{\"ok\":true}", -1);
			return;
		}
		if (!strcmp(action, "control")) {
			if (strcmp(t->role, "tx") != 0) {
				api_send(cli->fd, 400, "text/plain",
				         "control only valid on tx tunnels\n", -1);
				return;
			}
			if (t->control_port <= 0) {
				api_send(cli->fd, 409, "text/plain",
				         "tunnel has no control_port set; add -C in config\n", -1);
				return;
			}
			size_t cl;
			const char *cmd = qs_get(qstr, "cmd", &cl);
			if (!cmd) {
				api_send(cli->fd, 400, "text/plain", "missing ?cmd=\n", -1);
				return;
			}
			char respbuf[64];
			uint32_t rc_out = 0xFFFFFFFFu;
			int rlen = 0;

			if (cl == 7 && !strncmp(cmd, "set_fec", 7)) {
				int k = -1, nn = -1, to = WFB_FEC_TIMEOUT_KEEP;
				if (qs_get_int(qstr, "k", &k) < 0 ||
				    qs_get_int(qstr, "n", &nn) < 0 ||
				    k < 0 || k > 255 || nn < 0 || nn > 255) {
					api_send(cli->fd, 400, "text/plain",
					         "set_fec needs k,n in [0,255]\n", -1);
					return;
				}
				int to_arg;
				if (qs_get_int(qstr, "fec_timeout_ms", &to_arg) == 0) {
					if (to_arg < 0 || to_arg > 0xFFFE) {
						api_send(cli->fd, 400, "text/plain",
						         "fec_timeout_ms out of range\n", -1);
						return;
					}
					to = to_arg;
				}
				WfbCmdSetFecReq req = {
					.req_id         = wfb_cmd_next_req_id(),
					.cmd_id         = WFB_CMD_SET_FEC,
					.k              = (uint8_t)k,
					.n              = (uint8_t)nn,
					.fec_timeout_ms = htons((uint16_t)to),
				};
				rlen = wfb_cmd_round_trip(t->control_port, &req, sizeof(req),
				                          respbuf, sizeof(respbuf), &rc_out);
				if (rlen > 0 && rc_out == 0) {
					t->fec_k = k;
					t->fec_n = nn;
					if (to != WFB_FEC_TIMEOUT_KEEP) t->fec_timeout_ms = to;
					t->fec_cache_have = 1;
				}
			} else if (cl == 9 && !strncmp(cmd, "set_radio", 9)) {
				/* GET-merge-SET: an operator who passes only `mcs_index`
				 * via query string should not have stbc / ldpc / sgi /
				 * bandwidth / vht_* silently overwritten with our local
				 * defaults.  Read live values first; mutate only the
				 * fields the request explicitly carried.
				 *
				 * Falls back to the cached/configured values if GET_RADIO
				 * fails (e.g. wfb_tx hasn't bound its control port yet
				 * post-spawn) — better than refusing the request. */
				int stbc, ldpc, sgi, bw, mcs, vhtm, vhtn;
				if (wfb_cmd_refresh_radio(t) == 0) {
					stbc = t->stbc;
					ldpc = t->ldpc;
					sgi  = t->short_gi;
					bw   = t->bandwidth_mhz;
					mcs  = t->mcs_index;
					vhtm = t->vht_mode;
					vhtn = t->vht_nss;
				} else {
					stbc = (t->stbc        >= 0) ? t->stbc        : 0;
					ldpc = (t->ldpc        >= 0) ? t->ldpc        : 0;
					sgi  = (t->short_gi    >= 0) ? t->short_gi    : 0;
					bw   = (t->bandwidth_mhz > 0) ? t->bandwidth_mhz : 20;
					mcs  = (t->mcs_index   >= 0) ? t->mcs_index   : 1;
					vhtm = (t->vht_mode    >= 0) ? t->vht_mode    : 0;
					vhtn = (t->vht_nss     >  0) ? t->vht_nss     : 1;
				}
				/* Only override fields the caller actually passed. */
				(void)qs_get_int(qstr, "stbc",      &stbc);
				(void)qs_get_int(qstr, "ldpc",      &ldpc);
				(void)qs_get_int(qstr, "short_gi",  &sgi);
				(void)qs_get_int(qstr, "bandwidth", &bw);
				(void)qs_get_int(qstr, "mcs_index", &mcs);
				(void)qs_get_int(qstr, "vht_mode",  &vhtm);
				(void)qs_get_int(qstr, "vht_nss",   &vhtn);
				WfbCmdSetRadioReq req = {
					.req_id    = wfb_cmd_next_req_id(),
					.cmd_id    = WFB_CMD_SET_RADIO,
					.stbc      = (uint8_t)stbc,
					.ldpc      = (uint8_t)(ldpc != 0),
					.short_gi  = (uint8_t)(sgi  != 0),
					.bandwidth = (uint8_t)bw,
					.mcs_index = (uint8_t)mcs,
					.vht_mode  = (uint8_t)(vhtm != 0),
					.vht_nss   = (uint8_t)vhtn,
				};
				rlen = wfb_cmd_round_trip(t->control_port, &req, sizeof(req),
				                          respbuf, sizeof(respbuf), &rc_out);
				if (rlen > 0 && rc_out == 0) {
					t->stbc           = stbc;
					t->ldpc           = (ldpc != 0);
					t->short_gi       = (sgi  != 0);
					t->bandwidth_mhz  = bw;
					t->mcs_index      = mcs;
					t->vht_mode       = (vhtm != 0);
					t->vht_nss        = vhtn;
					t->radio_cache_have = 1;
					t->radio_cache_us   = now_us();
				}
			} else if ((cl == 7 && !strncmp(cmd, "get_fec",   7)) ||
			           (cl == 9 && !strncmp(cmd, "get_radio", 9))) {
				uint8_t cmd_id = (cl == 7) ? WFB_CMD_GET_FEC : WFB_CMD_GET_RADIO;
				WfbCmdGetReq req = {
					.req_id = wfb_cmd_next_req_id(),
					.cmd_id = cmd_id,
				};
				rlen = wfb_cmd_round_trip(t->control_port, &req, sizeof(req),
				                          respbuf, sizeof(respbuf), &rc_out);
			} else {
				api_send(cli->fd, 400, "text/plain",
				         "cmd must be set_fec|set_radio|get_fec|get_radio\n", -1);
				return;
			}

			if (rlen < 0) {
				api_send(cli->fd, 502, "application/json",
				         "{\"ok\":false,\"error\":\"sendto failed\"}", -1);
				return;
			}
			if (rlen == 0) {
				api_send(cli->fd, 504, "application/json",
				         "{\"ok\":false,\"error\":\"timeout\"}", -1);
				return;
			}
			/* JSON-emit the response. Body interpretation depends on cmd. */
			char body2[256];
			int p = snprintf(body2, sizeof(body2),
			                 "{\"ok\":%s,\"rc\":%u",
			                 rc_out == 0 ? "true" : "false", rc_out);
			if (rc_out == 0) {
				if (cl == 7 && !strncmp(cmd, "get_fec", 7) &&
				    rlen >= (int)sizeof(WfbCmdGetFecResp)) {
					WfbCmdGetFecResp gf;
					memcpy(&gf, respbuf, sizeof(gf));
					p += snprintf(body2 + p, sizeof(body2) - p,
					              ",\"k\":%u,\"n\":%u,\"fec_timeout_ms\":%u",
					              gf.k, gf.n, ntohs(gf.fec_timeout_ms));
					/* Refresh runtime cache on the way through. */
					t->fec_k          = gf.k;
					t->fec_n          = gf.n;
					t->fec_timeout_ms = ntohs(gf.fec_timeout_ms);
					t->fec_cache_have = 1;
				} else if (cl == 9 && !strncmp(cmd, "get_radio", 9) &&
				           rlen >= (int)sizeof(WfbCmdGetRadioResp)) {
					WfbCmdGetRadioResp gr;
					memcpy(&gr, respbuf, sizeof(gr));
					p += snprintf(body2 + p, sizeof(body2) - p,
					              ",\"stbc\":%u,\"ldpc\":%u,\"short_gi\":%u,"
					              "\"bandwidth\":%u,\"mcs_index\":%u,"
					              "\"vht_mode\":%u,\"vht_nss\":%u",
					              gr.stbc, gr.ldpc, gr.short_gi,
					              gr.bandwidth, gr.mcs_index,
					              gr.vht_mode, gr.vht_nss);
					t->stbc             = gr.stbc;
					t->ldpc             = gr.ldpc;
					t->short_gi         = gr.short_gi;
					t->bandwidth_mhz    = gr.bandwidth;
					t->mcs_index        = gr.mcs_index;
					t->vht_mode         = gr.vht_mode;
					t->vht_nss          = gr.vht_nss;
					t->radio_cache_have = 1;
					t->radio_cache_us   = now_us();
				}
			}
			snprintf(body2 + p, sizeof(body2) - p, "}");
			api_send(cli->fd, 200, "application/json", body2, -1);
			return;
		}
		api_send(cli->fd, 404, "text/plain", "unknown action\n", -1);
		return;
	}

	if (!strcmp(path, "/api/v1/keys")) {
#ifdef WFB_WITH_WFBNG
		/* GET /api/v1/keys                       -> status (role gs, fingerprint)
		 * GET /api/v1/keys?action=seed&seed=...  -> deterministic re-key
		 * GET /api/v1/keys?action=random         -> fresh random pair
		 * GET /api/v1/keys?action=upload&hex=...  -> verbatim 64-byte (128 hex) key
		 * Apply a change by calling /api/v1/system/reinit (restarts tunnels). */
		char action[32] = "";
		size_t al; const char *as = qs_get(qstr, "action", &al);
		if (as && al < sizeof(action)) { memcpy(action, as, al); action[al] = 0; }
		int rc = 0; const char *errmsg = NULL;
		if (action[0] == 0) {
			/* status only */
		} else if (!strcmp(action, "seed")) {
			char seed[128] = "Waybeam";
			size_t sl; const char *ss = qs_get(qstr, "seed", &sl);
			if (ss && sl && sl < sizeof(seed)) { memcpy(seed, ss, sl); seed[sl] = 0; }
			rc = wfb_write_key_seed(c->key_file, WFB_ROLE_GS, seed);
			if (rc) errmsg = "write failed";
		} else if (!strcmp(action, "random")) {
			rc = wfb_write_key_random(c->key_file, WFB_ROLE_GS);
			if (rc) errmsg = "write failed";
		} else if (!strcmp(action, "upload")) {
			char hex[160] = ""; unsigned char kb[64];
			size_t hl; const char *hs = qs_get(qstr, "hex", &hl);
			if (!hs || hl >= sizeof(hex)) { rc = -1; errmsg = "missing/long hex"; }
			else {
				memcpy(hex, hs, hl); hex[hl] = 0;
				if (wfb_hex_to_key(hex, kb) != 0) { rc = -1; errmsg = "bad key (need 128 hex chars)"; }
				else { rc = wfb_write_key_raw(c->key_file, kb); if (rc) errmsg = "write failed"; }
			}
		} else {
			rc = -1; errmsg = "unknown action";
		}
		char fp[16]; wfb_key_fingerprint(c->key_file, WFB_ROLE_GS, fp, sizeof(fp));
		int exists = (access(c->key_file, F_OK) == 0);
		char body[640]; char errbuf[80] = "";
		if (errmsg) snprintf(errbuf, sizeof(errbuf), "\"error\":\"%s\",", errmsg);
		int n = snprintf(body, sizeof(body),
			"{\"ok\":%s,%s\"role\":\"gs\",\"path\":\"%s\",\"exists\":%s,"
			"\"fingerprint\":\"%s\",\"changed\":%s,\"seed_default\":\"Waybeam\","
			"\"apply_hint\":\"/api/v1/system/reinit\"}",
			rc == 0 ? "true" : "false", errbuf, c->key_file,
			exists ? "true" : "false", fp, (rc == 0 && action[0]) ? "true" : "false");
		api_send(cli->fd, rc == 0 ? 200 : 400, "application/json", body, n);
#else
		api_send(cli->fd, 501, "application/json",
		         "{\"ok\":false,\"error\":\"key management requires the mega binary\"}", -1);
#endif
		return;
	}

	api_send(cli->fd, 404, "text/plain", "not found\n", -1);
}

