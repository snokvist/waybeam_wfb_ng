/*
 * gs_supervisor — ground-side wfb supervisor.
 *
 * Single native binary that fork/execs N×wfb_rx and N×wfb_tx children
 * from one JSON config and exposes a REST API for lifecycle control.
 * Replaces the wfb-ng.sh / S99wfb / wbmode / wfb_cmd sprawl on the
 * ground side. See GS_SUPERVISOR.md for the full design.
 *
 * Asymmetric to vehicle by design — the vehicle keeps link_controller
 * as the single venc-driven control loop; the ground gets dumb pipes.
 *
 * Compilation units:
 *   gs_supervisor.c       — entry, signal/log, JSON parser, config,
 *                           tunnel lifecycle, stats listener, iface
 *                           cache, wfb_cmd round-trip, WCMD emit,
 *                           system commands, supervisor_tick orchestrator
 *   gs_supervisor_csa.c   — CSA orchestrator (g_csa, csa_tick)
 *   gs_supervisor_scan.c  — passive channel scanner (g_scan, scan_tick)
 *   gs_supervisor_http.c  — REST API + WebUI (api_handle and helpers)
 *
 * Style matches link_controller.c: single-thread poll() loop, libc only,
 * GET-based API. No pthreads, no external deps.
 */

#include "gs_supervisor.h"

const int GS_BACKOFF_MS[GS_BACKOFF_LEN] = { 500, 1000, 2000, 4000, 8000, 16000, 30000 };

/* ---------- tiny utils ----------------------------------------------- */

volatile sig_atomic_t g_shutdown = 0;
volatile sig_atomic_t g_sigchld  = 0;
int                   g_verbose  = 0;   /* -v: full info + inherit child stdout/stderr */

static void on_signal(int sig)
{
	if (sig == SIGCHLD) g_sigchld = 1;
	else                g_shutdown = 1;
}

uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}

uint64_t now_ms(void) { return now_us() / 1000; }

void logf_(const char *level, const char *fmt, ...)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	struct tm tm;
	localtime_r(&ts.tv_sec, &tm);
	char hdr[64];
	strftime(hdr, sizeof(hdr), "%Y-%m-%d %H:%M:%S", &tm);
	fprintf(stderr, "%s.%03ld [gs/%s] ", hdr, ts.tv_nsec / 1000000, level);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

int write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	while (len) {
		ssize_t w = write(fd, p, len);
		if (w < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		p   += w;
		len -= (size_t)w;
	}
	return 0;
}

/* JSON parser (json_parse/jeq/jskip/jfind/jstr/jint/jbool) now lives in the
 * shared header-only shared/wfb_json.h, included via gs_supervisor.h. */

/* ---------- config + tunnel model -----------------------------------
 *
 * Tunnel, Config, IfaceState, and TunnelState live in gs_supervisor.h
 * so the CSA / scan / HTTP modules can share them without a tangle of
 * forward declarations. */

IfaceState g_iface_state[GS_MAX_GLOBAL_IFACES];
int        g_iface_state_count = 0;

const char *tunnel_state_name(TunnelState s)
{
	switch (s) {
	case TS_STOPPED:  return "stopped";
	case TS_STARTING: return "starting";
	case TS_RUNNING:  return "running";
	case TS_BACKOFF:  return "backoff";
	case TS_FAILED:   return "failed";
	}
	return "?";
}

/* ---------- config loader -------------------------------------------- */

static void tunnel_init_defaults(Tunnel *t)
{
	memset(t, 0, sizeof(*t));
	t->mcs_index = -1; t->stbc = -1; t->ldpc = -1;
	t->short_gi = -1; t->vht_mode = -1; t->vht_nss = -1;
	t->fec_timeout_ms = -1;
	t->stats_local_fd = -1;
	t->st_rssi_best = INT_MIN;
	t->autostart = true;
}

int cfg_parse_tunnel(const char *js, JTok *toks, int n, int t_idx, Tunnel *t)
{
	tunnel_init_defaults(t);

	int v;
	long lv;
	bool bv;
	char tmp[GS_ARG_MAX];

	if ((v = jfind(js, toks, n, t_idx, "name")) < 0) return -1;
	if (jstr(js, &toks[v], t->name, sizeof(t->name)) < 0) return -1;

	if ((v = jfind(js, toks, n, t_idx, "role")) < 0) return -1;
	if (jstr(js, &toks[v], t->role, sizeof(t->role)) < 0) return -1;
	if (strcmp(t->role, "rx") && strcmp(t->role, "tx")) return -1;

	if ((v = jfind(js, toks, n, t_idx, "link_id")) < 0) return -1;
	if (jint(js, &toks[v], &lv) < 0) return -1;
	t->link_id = (int)lv;

	if ((v = jfind(js, toks, n, t_idx, "radio_port")) >= 0) {
		if (jint(js, &toks[v], &lv) < 0) return -1;
		t->radio_port = (int)lv;
	}

	v = jfind(js, toks, n, t_idx, "interfaces");
	if (v < 0 || toks[v].type != JT_ARR) return -1;
	int kids = toks[v].size;
	if (kids < 1 || kids > GS_MAX_IFACES) return -1;
	int ci = v + 1;
	for (int k = 0; k < kids; k++) {
		if (jstr(js, &toks[ci], t->ifaces[k], IFNAMSIZ) < 0) return -1;
		ci = jskip(toks, n, ci);
	}
	t->iface_count = kids;

	if ((v = jfind(js, toks, n, t_idx, "extra_args")) >= 0 && toks[v].type == JT_ARR) {
		kids = toks[v].size;
		if (kids > GS_MAX_EXTRA_ARGS) return -1;
		ci = v + 1;
		for (int k = 0; k < kids; k++) {
			if (jstr(js, &toks[ci], t->extra_args[k], GS_ARG_MAX) < 0) return -1;
			ci = jskip(toks, n, ci);
		}
		t->extra_arg_count = kids;
	}

	if ((v = jfind(js, toks, n, t_idx, "autostart")) >= 0) {
		if (jbool(js, &toks[v], &bv) < 0) return -1;
		t->autostart = bv;
	}

	if ((v = jfind(js, toks, n, t_idx, "binary")) >= 0) {
		if (jstr(js, &toks[v], t->binary, sizeof(t->binary)) < 0) return -1;
	}

	if (!strcmp(t->role, "rx")) {
		/* udp_out is optional — wfb_rx defaults to 127.0.0.1:5600. */
		if ((v = jfind(js, toks, n, t_idx, "udp_out")) >= 0) {
			if (jstr(js, &toks[v], tmp, sizeof(tmp)) < 0) return -1;
			char *colon = strchr(tmp, ':');
			if (!colon) return -1;
			*colon = 0;
			snprintf(t->udp_out_ip, sizeof(t->udp_out_ip), "%s", tmp);
			t->udp_out_port = atoi(colon + 1);
			if (t->udp_out_port <= 0 || t->udp_out_port > 65535) return -1;
		}

		if ((v = jfind(js, toks, n, t_idx, "stats_out")) >= 0) {
			if (jstr(js, &toks[v], t->stats_out, sizeof(t->stats_out)) < 0)
				return -1;
		}

		if ((v = jfind(js, toks, n, t_idx, "stats_tap")) >= 0) {
			if (jstr(js, &toks[v], t->stats_tap, sizeof(t->stats_tap)) < 0)
				return -1;
		}

		/* Boundary-probe PER producer (see PROBE_PER_SPEC.md). */
		if ((v = jfind(js, toks, n, t_idx, "probe")) >= 0) {
			if (jbool(js, &toks[v], &bv) < 0) return -1;
			t->probe = bv;
		}
		t->probe_window_ms = 500;
		if ((v = jfind(js, toks, n, t_idx, "probe_window_ms")) >= 0) {
			if (jint(js, &toks[v], &lv) < 0) return -1;
			if (lv < 100 || lv > 10000) return -1;
			t->probe_window_ms = (int)lv;
		}
	} else {
		if ((v = jfind(js, toks, n, t_idx, "udp_in_port")) < 0) return -1;
		if (jint(js, &toks[v], &lv) < 0) return -1;
		t->udp_in_port = (int)lv;

		if ((v = jfind(js, toks, n, t_idx, "control_port")) >= 0) {
			if (jint(js, &toks[v], &lv) < 0) return -1;
			t->control_port = (int)lv;
		}

		int fec_idx = jfind(js, toks, n, t_idx, "fec");
		if (fec_idx >= 0 && toks[fec_idx].type == JT_OBJ) {
			if ((v = jfind(js, toks, n, fec_idx, "k")) >= 0) {
				if (jint(js, &toks[v], &lv) < 0) return -1;
				t->fec_k = (int)lv;
			}
			if ((v = jfind(js, toks, n, fec_idx, "n")) >= 0) {
				if (jint(js, &toks[v], &lv) < 0) return -1;
				t->fec_n = (int)lv;
			}
		}

		int radio_idx = jfind(js, toks, n, t_idx, "radio");
		if (radio_idx >= 0 && toks[radio_idx].type == JT_OBJ) {
			if ((v = jfind(js, toks, n, radio_idx, "bandwidth_mhz")) >= 0) {
				if (jint(js, &toks[v], &lv) < 0) return -1;
				t->bandwidth_mhz = (int)lv;
			}
			if ((v = jfind(js, toks, n, radio_idx, "mcs_index")) >= 0) {
				if (jint(js, &toks[v], &lv) < 0) return -1;
				t->mcs_index = (int)lv;
			}
			if ((v = jfind(js, toks, n, radio_idx, "stbc")) >= 0) {
				if (jint(js, &toks[v], &lv) < 0) return -1;
				t->stbc = (int)lv;
			}
			if ((v = jfind(js, toks, n, radio_idx, "ldpc")) >= 0) {
				if (jint(js, &toks[v], &lv) < 0) return -1;
				t->ldpc = (int)lv;
			}
		}
	}

	t->state = TS_STOPPED;
	t->pid   = -1;
	return 0;
}

/* ---------- profile expansion ----------------------------------------
 *
 * "profile" synthesizes the canonical 3-tunnel topology — video rx
 * (link 207), boundary-probe rx (link 50, PER producer), uplink tx
 * (link 208, WCMD back-channel) — plus the adapter bring-up/down
 * command blocks, from a handful of deployment facts:
 *
 *   "profile": {
 *     "ifaces": ["wlxA", "wlxB"],      // monitor-mode adapters (1..4)
 *     "uplink_iface": "wlxB",          // TX adapter (default: last)
 *     "channel": 161,
 *     "ht": "HT20",                    // optional (HT20/HT40+/HT40-)
 *     "txpower_mbm": 2000,             // optional; absent = leave alone
 *     "wfb_bin_dir": "/usr/local/bin"  // optional; absent = $PATH
 *   }
 *
 * Link ids/ports must match the vehicle's S99wfb, so they are not
 * configurable here by design; a hand-written "tunnels" array remains
 * available as the advanced override (mutually exclusive with
 * "profile"). Synthesis also makes the probe-forward foot-gun
 * impossible: the probe tunnel always lands on a dead udp_out port,
 * never 5600 (RTP video).
 *
 * Composition with an explicit "system" block: synthesized iface
 * bring-up runs first, then the config's system.up entries (e.g. a
 * walkout logger); on the way down the config's system.down entries
 * run first, then the synthesized iface take-down. */

#define PROFILE_VIDEO_LINK   207
#define PROFILE_PROBE_LINK   50
#define PROFILE_PROBE_PORT   50
#define PROFILE_UPLINK_LINK  208
#define PROFILE_STATS_FANIN  "127.0.0.1:6600"
#define PROFILE_UPLINK_UDP_IN 6600
#define PROFILE_UPLINK_CTRL  8000
#define PROFILE_PROBE_SINK_IP   "127.0.0.1"  /* dead port: never 5600 */
#define PROFILE_PROBE_SINK_PORT 5751

static int profile_push_cmd(char cmds[][GS_PATH_MAX], int *count,
                            const char *fmt, ...)
{
	if (*count >= GS_MAX_SYSTEM_CMDS) {
		LOG_ERR("profile: synthesized system block exceeds %d "
		        "commands — reduce ifaces or explicit system entries",
		        GS_MAX_SYSTEM_CMDS);
		return -1;
	}
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(cmds[*count], GS_PATH_MAX, fmt, ap);
	va_end(ap);
	if (n < 0 || n >= GS_PATH_MAX) return -1;
	(*count)++;
	return 0;
}

static void profile_set_binary(Tunnel *t, const char *bin_dir,
                               const char *name)
{
	if (bin_dir[0])
		snprintf(t->binary, sizeof(t->binary), "%s/%s", bin_dir, name);
	/* else: empty binary → build_argv_* falls back to $PATH lookup */
}

static int cfg_expand_profile(const char *js, JTok *toks, int n,
                              int p_idx, Config *c)
{
	int v;
	long lv;

	char ifaces[GS_MAX_IFACES][IFNAMSIZ];
	int iface_count = 0;
	v = jfind(js, toks, n, p_idx, "ifaces");
	if (v < 0 || toks[v].type != JT_ARR ||
	    toks[v].size < 1 || toks[v].size > GS_MAX_IFACES) {
		LOG_ERR("profile: 'ifaces' must be an array of 1..%d names",
		        GS_MAX_IFACES);
		return -1;
	}
	int ci = v + 1;
	for (int k = 0; k < toks[v].size; k++) {
		if (jstr(js, &toks[ci], ifaces[k], IFNAMSIZ) < 0) return -1;
		ci = jskip(toks, n, ci);
		iface_count++;
	}

	char uplink[IFNAMSIZ];
	snprintf(uplink, sizeof(uplink), "%s", ifaces[iface_count - 1]);
	if ((v = jfind(js, toks, n, p_idx, "uplink_iface")) >= 0) {
		if (jstr(js, &toks[v], uplink, sizeof(uplink)) < 0) return -1;
		bool member = false;
		for (int k = 0; k < iface_count; k++)
			if (!strcmp(uplink, ifaces[k])) member = true;
		if (!member) {
			LOG_ERR("profile: uplink_iface '%s' not in ifaces[]",
			        uplink);
			return -1;
		}
	}

	if ((v = jfind(js, toks, n, p_idx, "channel")) < 0 ||
	    jint(js, &toks[v], &lv) < 0 || lv < 1 || lv > 200) {
		LOG_ERR("profile: 'channel' (1..200) is required");
		return -1;
	}
	int channel = (int)lv;

	char ht[8] = "HT20";
	if ((v = jfind(js, toks, n, p_idx, "ht")) >= 0) {
		if (jstr(js, &toks[v], ht, sizeof(ht)) < 0) return -1;
		if (strcmp(ht, "HT20") && strcmp(ht, "HT40+") &&
		    strcmp(ht, "HT40-")) {
			LOG_ERR("profile: ht must be HT20/HT40+/HT40-");
			return -1;
		}
	}

	long txpower_mbm = 0;
	if ((v = jfind(js, toks, n, p_idx, "txpower_mbm")) >= 0) {
		if (jint(js, &toks[v], &txpower_mbm) < 0 ||
		    txpower_mbm < 0 || txpower_mbm > 4000) {
			LOG_ERR("profile: txpower_mbm out of range [0, 4000]");
			return -1;
		}
	}

	char bin_dir[GS_PATH_MAX] = "";
	if ((v = jfind(js, toks, n, p_idx, "wfb_bin_dir")) >= 0) {
		if (jstr(js, &toks[v], bin_dir, sizeof(bin_dir)) < 0) return -1;
	}

	/* system.up: iface bring-up first, explicit entries after. The
	 * sleeps match the reference config — monitor-mode flips race
	 * driver state without them. */
	char up[GS_MAX_SYSTEM_CMDS][GS_PATH_MAX];
	int up_count = 0;
	for (int k = 0; k < iface_count; k++) {
		const char *ifc = ifaces[k];
		if (profile_push_cmd(up, &up_count, "ip link set %s down", ifc) < 0 ||
		    profile_push_cmd(up, &up_count, "iw dev %s set monitor otherbss", ifc) < 0 ||
		    profile_push_cmd(up, &up_count, "sleep 0.5") < 0 ||
		    profile_push_cmd(up, &up_count, "ip link set %s up", ifc) < 0 ||
		    profile_push_cmd(up, &up_count, "sleep 0.5") < 0 ||
		    profile_push_cmd(up, &up_count, "iw dev %s set channel %d %s", ifc, channel, ht) < 0)
			return -1;
		if (txpower_mbm > 0 &&
		    (profile_push_cmd(up, &up_count, "sleep 0.5") < 0 ||
		     profile_push_cmd(up, &up_count, "iw dev %s set txpower fixed %ld", ifc, txpower_mbm) < 0))
			return -1;
	}
	for (int k = 0; k < c->system_up_count; k++)
		if (profile_push_cmd(up, &up_count, "%s", c->system_up[k]) < 0)
			return -1;
	memcpy(c->system_up, up, sizeof(up));
	c->system_up_count = up_count;

	/* system.down: explicit entries first, iface take-down after. */
	for (int k = 0; k < iface_count; k++)
		if (profile_push_cmd(c->system_down, &c->system_down_count,
		                     "ip link set %s down", ifaces[k]) < 0)
			return -1;

	/* The three canonical tunnels. */
	Tunnel *t = &c->tunnels[0];
	tunnel_init_defaults(t);
	snprintf(t->name, sizeof(t->name), "video");
	snprintf(t->role, sizeof(t->role), "rx");
	profile_set_binary(t, bin_dir, "wfb_rx");
	t->link_id = PROFILE_VIDEO_LINK;
	t->radio_port = 0;
	t->iface_count = iface_count;
	for (int k = 0; k < iface_count; k++)
		memcpy(t->ifaces[k], ifaces[k], IFNAMSIZ);
	/* udp_out omitted → wfb_rx default 127.0.0.1:5600 (RTP video) */
	snprintf(t->stats_out, sizeof(t->stats_out), PROFILE_STATS_FANIN);
	t->probe_window_ms = 500;
	t->extra_arg_count = 3;
	snprintf(t->extra_args[0], GS_ARG_MAX, "-x");
	snprintf(t->extra_args[1], GS_ARG_MAX, "-l");
	snprintf(t->extra_args[2], GS_ARG_MAX, "100");

	t = &c->tunnels[1];
	tunnel_init_defaults(t);
	snprintf(t->name, sizeof(t->name), "probe");
	snprintf(t->role, sizeof(t->role), "rx");
	profile_set_binary(t, bin_dir, "wfb_rx");
	t->link_id = PROFILE_PROBE_LINK;
	t->radio_port = PROFILE_PROBE_PORT;
	t->iface_count = iface_count;
	for (int k = 0; k < iface_count; k++)
		memcpy(t->ifaces[k], ifaces[k], IFNAMSIZ);
	snprintf(t->udp_out_ip, sizeof(t->udp_out_ip), PROFILE_PROBE_SINK_IP);
	t->udp_out_port = PROFILE_PROBE_SINK_PORT;
	snprintf(t->stats_out, sizeof(t->stats_out), PROFILE_STATS_FANIN);
	t->probe = true;
	t->probe_window_ms = 500;
	t->extra_arg_count = 3;
	snprintf(t->extra_args[0], GS_ARG_MAX, "-x");
	snprintf(t->extra_args[1], GS_ARG_MAX, "-l");
	snprintf(t->extra_args[2], GS_ARG_MAX, "100");

	t = &c->tunnels[2];
	tunnel_init_defaults(t);
	snprintf(t->name, sizeof(t->name), "uplink");
	snprintf(t->role, sizeof(t->role), "tx");
	profile_set_binary(t, bin_dir, "wfb_tx");
	t->link_id = PROFILE_UPLINK_LINK;
	t->radio_port = 0;
	t->iface_count = 1;
	snprintf(t->ifaces[0], IFNAMSIZ, "%s", uplink);
	t->udp_in_port = PROFILE_UPLINK_UDP_IN;
	t->control_port = PROFILE_UPLINK_CTRL;
	t->fec_k = 1;
	t->fec_n = 2;
	/* -T 1: close FEC blocks at 1 ms idle so the 3 redundant WCMD
	 * copies span blocks (see CLAUDE.md "WCMD redundancy"). */
	t->extra_arg_count = 2;
	snprintf(t->extra_args[0], GS_ARG_MAX, "-T");
	snprintf(t->extra_args[1], GS_ARG_MAX, "1");

	c->tunnel_count = 3;

	/* WCMD emit path defaults on unless the config said otherwise. */
	if (!c->venc_cmd_uplink[0]) {
		c->venc_cmd_enabled = true;
		snprintf(c->venc_cmd_uplink, sizeof(c->venc_cmd_uplink),
		         "uplink");
	}

	LOG_INFO("profile: synthesized video/probe/uplink tunnels "
	         "(%d iface(s), uplink=%s, ch=%d %s)",
	         iface_count, uplink, channel, ht);
	return 0;
}

int cfg_load(const char *path, Config *c)
{
	memset(c, 0, sizeof(*c));
	snprintf(c->http_bind, sizeof(c->http_bind), "0.0.0.0");
	c->http_port = GS_DEFAULT_HTTP_PORT;
	c->venc_cmd_rate_limit_ms = 50;
	wfb_logger_defaults(&c->telemetry);   /* DISABLED by default; 127.0.0.1:6700, wfb.sqlite, 1200 s. Opt in via telemetry.enabled or wfb-link log.enabled. */

	int fd = open(path, O_RDONLY);
	if (fd < 0) { LOG_ERR("config open(%s): %s", path, strerror(errno)); return -1; }
	struct stat st;
	if (fstat(fd, &st) < 0) { close(fd); return -1; }
	if (st.st_size <= 0 || st.st_size > 256 * 1024) { close(fd); return -1; }
	char *buf = malloc((size_t)st.st_size + 1);
	if (!buf) { close(fd); return -1; }
	ssize_t r = read(fd, buf, (size_t)st.st_size);
	close(fd);
	if (r != st.st_size) { free(buf); return -1; }
	buf[r] = 0;

	JTok toks[JSON_MAX_TOKS];
	int n = json_parse(buf, (int)r, toks, JSON_MAX_TOKS);
	if (n < 1 || toks[0].type != JT_OBJ) {
		LOG_ERR("config parse failed (n=%d)", n);
		free(buf); return -1;
	}

	int v;
	long lv;
	if ((v = jfind(buf, toks, n, 0, "key_file")) >= 0)
		jstr(buf, &toks[v], c->key_file, sizeof(c->key_file));

	int http_idx = jfind(buf, toks, n, 0, "http");
	if (http_idx >= 0 && toks[http_idx].type == JT_OBJ) {
		if ((v = jfind(buf, toks, n, http_idx, "bind")) >= 0)
			jstr(buf, &toks[v], c->http_bind, sizeof(c->http_bind));
		if ((v = jfind(buf, toks, n, http_idx, "port")) >= 0 &&
		    jint(buf, &toks[v], &lv) == 0)
			c->http_port = (uint16_t)lv;
	}

	int sys_idx = jfind(buf, toks, n, 0, "system");
	if (sys_idx >= 0 && toks[sys_idx].type == JT_OBJ) {
		int up = jfind(buf, toks, n, sys_idx, "up");
		if (up >= 0 && toks[up].type == JT_ARR) {
			int kids = toks[up].size;
			if (kids > GS_MAX_SYSTEM_CMDS) {
				LOG_ERR("config: system.up has %d entries, max %d",
				        kids, GS_MAX_SYSTEM_CMDS);
				free(buf); return -1;
			}
			int ci = up + 1;
			for (int k = 0; k < kids; k++) {
				jstr(buf, &toks[ci], c->system_up[k], GS_PATH_MAX);
				ci = jskip(toks, n, ci);
			}
			c->system_up_count = kids;
		}
		int dn = jfind(buf, toks, n, sys_idx, "down");
		if (dn >= 0 && toks[dn].type == JT_ARR) {
			int kids = toks[dn].size;
			if (kids > GS_MAX_SYSTEM_CMDS) {
				LOG_ERR("config: system.down has %d entries, max %d",
				        kids, GS_MAX_SYSTEM_CMDS);
				free(buf); return -1;
			}
			int ci = dn + 1;
			for (int k = 0; k < kids; k++) {
				jstr(buf, &toks[ci], c->system_down[k], GS_PATH_MAX);
				ci = jskip(toks, n, ci);
			}
			c->system_down_count = kids;
		}
	}

	int vc_idx = jfind(buf, toks, n, 0, "venc_cmd");
	if (vc_idx >= 0 && toks[vc_idx].type == JT_OBJ) {
		bool bv;
		if ((v = jfind(buf, toks, n, vc_idx, "enabled")) >= 0 &&
		    jbool(buf, &toks[v], &bv) == 0)
			c->venc_cmd_enabled = bv;
		if ((v = jfind(buf, toks, n, vc_idx, "uplink_tunnel")) >= 0)
			jstr(buf, &toks[v], c->venc_cmd_uplink, sizeof(c->venc_cmd_uplink));
		if ((v = jfind(buf, toks, n, vc_idx, "rate_limit_ms")) >= 0 &&
		    jint(buf, &toks[v], &lv) == 0)
			c->venc_cmd_rate_limit_ms = (int)lv;
	}

	/* telemetry logger (Phase 5): in-process udp->sqlite capture, replacing the
	 * retired Python ingester. DISABLED unless opted in (enabled=false default,
	 * set above) so a config with no `telemetry` block never writes to CWD; any
	 * present field overrides the defaults. */
	int tel_idx = jfind(buf, toks, n, 0, "telemetry");
	if (tel_idx >= 0 && toks[tel_idx].type == JT_OBJ) {
		bool bv;
		if ((v = jfind(buf, toks, n, tel_idx, "enabled")) >= 0 && jbool(buf, &toks[v], &bv) == 0)
			c->telemetry.enabled = bv;
		if ((v = jfind(buf, toks, n, tel_idx, "db")) >= 0)
			jstr(buf, &toks[v], c->telemetry.db, sizeof(c->telemetry.db));
		if ((v = jfind(buf, toks, n, tel_idx, "bind")) >= 0)
			jstr(buf, &toks[v], c->telemetry.bind, sizeof(c->telemetry.bind));
		if ((v = jfind(buf, toks, n, tel_idx, "listen")) >= 0 && jint(buf, &toks[v], &lv) == 0)
			c->telemetry.listen_port = (int)lv;
		if ((v = jfind(buf, toks, n, tel_idx, "max_duration")) >= 0 && jint(buf, &toks[v], &lv) == 0)
			c->telemetry.max_duration = (int)lv;
		if ((v = jfind(buf, toks, n, tel_idx, "source")) >= 0)
			jstr(buf, &toks[v], c->telemetry.source, sizeof(c->telemetry.source));
		if ((v = jfind(buf, toks, n, tel_idx, "channel")) >= 0 && jint(buf, &toks[v], &lv) == 0)
			c->telemetry.channel = (int)lv;
		if ((v = jfind(buf, toks, n, tel_idx, "tx_power")) >= 0)
			jstr(buf, &toks[v], c->telemetry.tx_power, sizeof(c->telemetry.tx_power));
		if ((v = jfind(buf, toks, n, tel_idx, "antenna_cfg")) >= 0)
			jstr(buf, &toks[v], c->telemetry.antenna_cfg, sizeof(c->telemetry.antenna_cfg));
	}

	int prof_idx = jfind(buf, toks, n, 0, "profile");
	int tarr = jfind(buf, toks, n, 0, "tunnels");
	if (prof_idx >= 0 && tarr >= 0) {
		LOG_ERR("config: 'profile' and 'tunnels' are mutually "
		        "exclusive — drop one");
		free(buf); return -1;
	}
	if (prof_idx >= 0) {
		if (toks[prof_idx].type != JT_OBJ ||
		    cfg_expand_profile(buf, toks, n, prof_idx, c) < 0) {
			free(buf); return -1;
		}
	} else {
		if (tarr < 0 || toks[tarr].type != JT_ARR) {
			LOG_ERR("config: missing 'tunnels' array (or 'profile')");
			free(buf); return -1;
		}
		int kids = toks[tarr].size;
		if (kids < 1 || kids > GS_MAX_TUNNELS) {
			LOG_ERR("config: tunnel count %d out of range [1..%d]",
			        kids, GS_MAX_TUNNELS);
			free(buf); return -1;
		}
		int ci = tarr + 1;
		for (int k = 0; k < kids; k++) {
			if (cfg_parse_tunnel(buf, toks, n, ci, &c->tunnels[k]) < 0) {
				LOG_ERR("config: tunnel #%d parse failed", k);
				free(buf); return -1;
			}
			ci = jskip(toks, n, ci);
		}
		c->tunnel_count = kids;
	}
	free(buf);

	/* Forward-port safety (the 2026-06-10 "video flap" class): wfb_rx
	 * defaults its decoded-payload forward to 127.0.0.1:5600 — the RTP
	 * video port. A probe tunnel forwarding there injects probe payloads
	 * straight into the H.265 decoder. Reject that outright, and warn on
	 * any two rx tunnels sharing an effective forward port (an omitted
	 * udp_out counts as 5600). */
	for (int k = 0; k < c->tunnel_count; k++) {
		Tunnel *t = &c->tunnels[k];
		if (strcmp(t->role, "rx") != 0) continue;
		int eff = t->udp_out_ip[0] ? t->udp_out_port : 5600;
		if (t->probe && (!t->udp_out_ip[0] || eff == 5600)) {
			LOG_ERR("config: probe tunnel '%s' must set an explicit "
			        "udp_out on a dead port (never 5600 = RTP video)",
			        t->name);
			return -1;
		}
		for (int j = 0; j < k; j++) {
			Tunnel *o = &c->tunnels[j];
			if (strcmp(o->role, "rx") != 0) continue;
			int oeff = o->udp_out_ip[0] ? o->udp_out_port : 5600;
			if (oeff == eff)
				LOG_WARN("config: rx tunnels '%s' and '%s' both "
				         "forward to udp:%d (omitted udp_out = "
				         "5600) — decoded payloads will interleave",
				         o->name, t->name, eff);
		}
	}

	return 0;
}

/* ---------- /etc/wfb-link.json overlay (Phase 3b) --------------------- *
 *
 * Sparse overlay of the unified link preset onto the already-loaded
 * gs_supervisor.json: ONLY fields present in wfb-link.json override; absent
 * fields keep their gs_supervisor.json value. This lets one file tune the
 * cross-cutting, must-match-both-ends identifiers (key, link ids) plus the
 * tx-side fec/mcs/bw, while the GS keeps its richer tunnel + system.up model.
 * No-op (logged) when the file is absent or unparseable. Same parser cfg_load
 * uses — no libsodium, so this is unconditional (standalone + mega).
 *
 * Mapping: key.file -> key_file (global); links.{video,uplink,probe} -> the
 * link_id of the tunnel named video/uplink/probe; fec.k/n, mcs.boot, radio.bw
 * -> every tx-role tunnel. */
static Tunnel *cfg_tunnel_by_name(Config *c, const char *name)
{
	for (int i = 0; i < c->tunnel_count; i++)
		if (!strcmp(c->tunnels[i].name, name)) return &c->tunnels[i];
	return NULL;
}

void cfg_apply_wfb_link_overlay(Config *c, const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) { LOG_INFO("wfb-link overlay: %s absent — using gs_supervisor.json as-is", path); return; }
	struct stat st;
	if (fstat(fd, &st) < 0 || st.st_size <= 0 || st.st_size > 256 * 1024) { close(fd); return; }
	char *buf = malloc((size_t)st.st_size + 1);
	if (!buf) { close(fd); return; }
	ssize_t r = read(fd, buf, (size_t)st.st_size);
	close(fd);
	if (r != st.st_size) { free(buf); return; }
	buf[r] = 0;

	JTok toks[JSON_MAX_TOKS];
	int n = json_parse(buf, (int)r, toks, JSON_MAX_TOKS);
	if (n < 1 || toks[0].type != JT_OBJ) {
		LOG_WARN("wfb-link overlay: %s not a valid JSON object — ignored", path);
		free(buf); return;
	}

	int v, sec, applied = 0;
	long lv;

	/* key.file -> global key_file */
	if ((sec = jfind(buf, toks, n, 0, "key")) >= 0 && toks[sec].type == JT_OBJ) {
		if ((v = jfind(buf, toks, n, sec, "file")) >= 0 && toks[v].type == JT_STR) {
			jstr(buf, &toks[v], c->key_file, sizeof(c->key_file));
			LOG_INFO("wfb-link overlay: key_file -> %s", c->key_file); applied++;
		}
	}

	/* links.{video,uplink,probe} -> link_id of the same-named tunnel */
	if ((sec = jfind(buf, toks, n, 0, "links")) >= 0 && toks[sec].type == JT_OBJ) {
		static const char *names[] = { "video", "uplink", "probe" };
		for (int k = 0; k < 3; k++) {
			if ((v = jfind(buf, toks, n, sec, names[k])) >= 0 && jint(buf, &toks[v], &lv) == 0) {
				Tunnel *t = cfg_tunnel_by_name(c, names[k]);
				if (t) { t->link_id = (int)lv; LOG_INFO("wfb-link overlay: tunnel '%s' link_id -> %d", names[k], (int)lv); applied++; }
			}
		}
	}

	/* fec.k/n, mcs.boot, radio.bw -> every tx-role tunnel */
	int fec_k = -1, fec_n = -1, mcs = -1, bw = -1;
	if ((sec = jfind(buf, toks, n, 0, "fec")) >= 0 && toks[sec].type == JT_OBJ) {
		if ((v = jfind(buf, toks, n, sec, "k")) >= 0 && jint(buf, &toks[v], &lv) == 0) fec_k = (int)lv;
		if ((v = jfind(buf, toks, n, sec, "n")) >= 0 && jint(buf, &toks[v], &lv) == 0) fec_n = (int)lv;
	}
	if ((sec = jfind(buf, toks, n, 0, "mcs")) >= 0 && toks[sec].type == JT_OBJ &&
	    (v = jfind(buf, toks, n, sec, "boot")) >= 0 && jint(buf, &toks[v], &lv) == 0) mcs = (int)lv;
	if ((sec = jfind(buf, toks, n, 0, "radio")) >= 0 && toks[sec].type == JT_OBJ &&
	    (v = jfind(buf, toks, n, sec, "bw")) >= 0 && jint(buf, &toks[v], &lv) == 0) bw = (int)lv;
	if (fec_k >= 0 || fec_n >= 0 || mcs >= 0 || bw >= 0) {
		for (int i = 0; i < c->tunnel_count; i++) {
			Tunnel *t = &c->tunnels[i];
			if (strcmp(t->role, "tx") != 0) continue;
			if (fec_k >= 0) t->fec_k = fec_k;
			if (fec_n >= 0) t->fec_n = fec_n;
			if (mcs   >= 0) t->mcs_index = mcs;
			if (bw    >= 0) t->bandwidth_mhz = bw;
			LOG_INFO("wfb-link overlay: tx tunnel '%s' fec=%d/%d mcs=%d bw=%d",
			         t->name, t->fec_k, t->fec_n, t->mcs_index, t->bandwidth_mhz);
			applied++;
		}
	}

	/* log.enabled -> telemetry.enabled (parity with the air-side WFB_LOG env). */
	if ((sec = jfind(buf, toks, n, 0, "log")) >= 0 && toks[sec].type == JT_OBJ) {
		bool bv;
		if ((v = jfind(buf, toks, n, sec, "enabled")) >= 0 && jbool(buf, &toks[v], &bv) == 0) {
			c->telemetry.enabled = bv;
			LOG_INFO("wfb-link overlay: telemetry.enabled -> %s", bv ? "true" : "false");
			applied++;
		}
	}

	/* radio.htmode -> the `iw set channel` width in system.up. This is the iw
	 * channel width (RF), independent of radio.bw (the radiotap TX width above):
	 * a ground on a 40 MHz channel decodes both 20 and 40 MHz on its primary, so
	 * htmode=HT40+ with bw=20 is the canonical config while the air is HT20.
	 * Rewrites every `... set channel <chan> [old-ht]` line, replacing any
	 * existing width suffix. Runs before supervisor_bring_up(), so the rewritten
	 * commands are what actually execute. */
	if ((sec = jfind(buf, toks, n, 0, "radio")) >= 0 && toks[sec].type == JT_OBJ &&
	    (v = jfind(buf, toks, n, sec, "htmode")) >= 0 && toks[v].type == JT_STR) {
		char htmode[8] = "";
		jstr(buf, &toks[v], htmode, sizeof(htmode));
		if (strcmp(htmode, "HT20") && strcmp(htmode, "HT40+") &&
		    strcmp(htmode, "HT40-")) {
			LOG_WARN("wfb-link overlay: ignoring invalid radio.htmode '%s' "
			         "(want HT20/HT40+/HT40-)", htmode);
		} else {
			int rewritten = 0;
			for (int i = 0; i < c->system_up_count; i++) {
				char *cmd = c->system_up[i];
				char *p = strstr(cmd, "set channel ");
				if (!p) continue;
				p += strlen("set channel ");
				const char *q = p;               /* channel number token */
				while (*q && *q != ' ') q++;
				int clen = (int)(q - p);
				if (clen <= 0 || clen >= 8) continue;
				char chan[8];
				memcpy(chan, p, (size_t)clen);
				chan[clen] = 0;
				int prefix = (int)(p - cmd);     /* through "set channel " */
				char nb[GS_PATH_MAX];
				int rc = snprintf(nb, sizeof(nb), "%.*s%s %s",
				                  prefix, cmd, chan, htmode);
				if (rc > 0 && rc < (int)sizeof(nb)) {
					memcpy(cmd, nb, (size_t)rc + 1);
					rewritten++;
				}
			}
			if (rewritten) {
				LOG_INFO("wfb-link overlay: radio.htmode -> %s on %d iw "
				         "set-channel cmd(s)", htmode, rewritten);
				applied++;
			}
		}
	}

	LOG_INFO("wfb-link overlay: %d field group(s) applied from %s", applied, path);
	free(buf);
}

/* ---------- argv composition ----------------------------------------- */

int ab_push(ArgvBuilder *ab, const char *fmt, ...)
{
	if (ab->argc >= GS_ARGV_MAX - 1) return -1;
	if (ab->pos >= sizeof(ab->buf)) return -1;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(ab->buf + ab->pos, sizeof(ab->buf) - ab->pos, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= sizeof(ab->buf) - ab->pos) return -1;
	ab->argv[ab->argc++] = ab->buf + ab->pos;
	ab->pos += (size_t)n + 1;
	return 0;
}

int build_argv_rx(const Tunnel *t, const char *key_file, ArgvBuilder *ab)
{
	if (ab_push(ab, "%s", t->binary[0] ? t->binary : "wfb_rx") < 0) return -1;
	if (key_file && key_file[0] && (ab_push(ab, "-K") < 0 || ab_push(ab, "%s", key_file) < 0))
		return -1;
	if (t->udp_out_ip[0]) {
		if (ab_push(ab, "-c") < 0 || ab_push(ab, "%s", t->udp_out_ip) < 0) return -1;
		if (ab_push(ab, "-u") < 0 || ab_push(ab, "%d", t->udp_out_port) < 0) return -1;
	}
	if (ab_push(ab, "-i") < 0 || ab_push(ab, "%d", t->link_id) < 0) return -1;
	if (ab_push(ab, "-p") < 0 || ab_push(ab, "%d", t->radio_port) < 0) return -1;
	/* Always wire wfb_rx -Y into the supervisor's local listener so the
	 * Tunnels WebUI can show pkt counters / RSSI.  Forwarding to the user's
	 * configured stats_out happens in stats_drain(). */
	if (t->stats_local_arg[0]) {
		if (ab_push(ab, "-Y") < 0 || ab_push(ab, "%s", t->stats_local_arg) < 0) return -1;
	} else if (t->stats_out[0]) {
		if (ab_push(ab, "-Y") < 0 || ab_push(ab, "%s", t->stats_out) < 0) return -1;
	}
	for (int i = 0; i < t->extra_arg_count; i++)
		if (ab_push(ab, "%s", t->extra_args[i]) < 0) return -1;
	for (int i = 0; i < t->iface_count; i++)
		if (ab_push(ab, "%s", t->ifaces[i]) < 0) return -1;
	ab->argv[ab->argc] = NULL;
	return 0;
}

int build_argv_tx(const Tunnel *t, const char *key_file, ArgvBuilder *ab)
{
	if (ab_push(ab, "%s", t->binary[0] ? t->binary : "wfb_tx") < 0) return -1;
	if (key_file && key_file[0] && (ab_push(ab, "-K") < 0 || ab_push(ab, "%s", key_file) < 0))
		return -1;
	if (ab_push(ab, "-i") < 0 || ab_push(ab, "%d", t->link_id) < 0) return -1;
	if (ab_push(ab, "-p") < 0 || ab_push(ab, "%d", t->radio_port) < 0) return -1;
	if (ab_push(ab, "-u") < 0 || ab_push(ab, "%d", t->udp_in_port) < 0) return -1;
	if (t->control_port > 0) {
		if (ab_push(ab, "-C") < 0 || ab_push(ab, "%d", t->control_port) < 0) return -1;
	}
	if (t->fec_k > 0) {
		if (ab_push(ab, "-k") < 0 || ab_push(ab, "%d", t->fec_k) < 0) return -1;
	}
	if (t->fec_n > 0) {
		if (ab_push(ab, "-n") < 0 || ab_push(ab, "%d", t->fec_n) < 0) return -1;
	}
	if (t->bandwidth_mhz > 0) {
		if (ab_push(ab, "-B") < 0 || ab_push(ab, "%d", t->bandwidth_mhz) < 0) return -1;
	}
	if (t->mcs_index >= 0) {
		if (ab_push(ab, "-M") < 0 || ab_push(ab, "%d", t->mcs_index) < 0) return -1;
	}
	if (t->stbc >= 0) {
		if (ab_push(ab, "-S") < 0 || ab_push(ab, "%d", t->stbc) < 0) return -1;
	}
	if (t->ldpc >= 0) {
		if (ab_push(ab, "-L") < 0 || ab_push(ab, "%d", t->ldpc) < 0) return -1;
	}
	if (t->stats_local_arg[0]) {
		if (ab_push(ab, "-Y") < 0 || ab_push(ab, "%s", t->stats_local_arg) < 0) return -1;
	}
	for (int i = 0; i < t->extra_arg_count; i++)
		if (ab_push(ab, "%s", t->extra_args[i]) < 0) return -1;
	for (int i = 0; i < t->iface_count; i++)
		if (ab_push(ab, "%s", t->ifaces[i]) < 0) return -1;
	ab->argv[ab->argc] = NULL;
	return 0;
}

/* ---------- supervisor ----------------------------------------------- */

/* forward declarations: stats helpers live in the iface-validation block
 * further down to keep them grouped with the rest of the runtime/IPC code. */
int stats_listener_open(Tunnel *t);
void stats_listener_close(Tunnel *t);
void stats_drain(Tunnel *t);
int wfb_cmd_refresh_radio(Tunnel *t);
int wfb_cmd_refresh_fec(Tunnel *t);

int tunnel_spawn(Tunnel *t, const char *key_file)
{
	/* Lazy-open the per-tunnel stats listener.  rx tunnels emit rx_ant,
	 * tx tunnels emit tx_stats — same -Y wire format, parsed by the same
	 * stats_drain().  Port stays stable across respawns so the argv is
	 * cacheable. */
	if (t->stats_local_fd < 0) {
		(void)stats_listener_open(t);
	}

	ArgvBuilder ab = {0};
	int rc = !strcmp(t->role, "rx")
	       ? build_argv_rx(t, key_file, &ab)
	       : build_argv_tx(t, key_file, &ab);
	if (rc < 0) {
		LOG_ERR("tunnel '%s': argv build failed", t->name);
		t->state = TS_FAILED;
		return -1;
	}

	pid_t pid = fork();
	if (pid < 0) {
		LOG_ERR("tunnel '%s': fork: %s", t->name, strerror(errno));
		t->state = TS_FAILED;
		return -1;
	}
	if (pid == 0) {
		/* child — restore default signal handling, exec */
		signal(SIGCHLD, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGINT,  SIG_DFL);
		signal(SIGHUP,  SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		/* Silence wfb_rx/wfb_tx PKT spam unless -v was passed.  Keep
		 * stderr open just before exec so any exec failure message
		 * still reaches the operator; redirect after the (failed)
		 * exec is impossible, so we silence both fds up-front and
		 * write the exec-failure message to a fresh fd 2 via /dev/tty
		 * fallback — too fragile.  Instead leave stderr alone so the
		 * exec error bubbles up; only stdout gets nulled. */
		if (!g_verbose) {
			int devnull = open("/dev/null", O_WRONLY);
			if (devnull >= 0) {
				dup2(devnull, STDOUT_FILENO);
				dup2(devnull, STDERR_FILENO);
				close(devnull);
			}
		}
#ifdef WFB_MULTICALL
		/* Mega binary: re-exec ourselves as the rx/tx applet instead of
		 * relying on separate wfb_rx/wfb_tx binaries on PATH.  Override
		 * argv[0] with the applet alias so the dispatcher's basename match
		 * routes correctly (any configured t->binary is ignored — we are
		 * the binary).  See docs/design/mega-binary.md.
		 * Map the validated role explicitly — roles are constrained to
		 * "rx"/"tx" at config parse, so an unrecognized one means a new role
		 * slipped through; fail loudly rather than silently launching tx. */
		const char *applet = !strcmp(t->role, "rx") ? "wfb_rx"
		                   : !strcmp(t->role, "tx") ? "wfb_tx" : NULL;
		if (!applet) {
			fprintf(stderr, "[gs] mega: no applet for tunnel role '%s'\n", t->role);
			_exit(127);
		}
		ab.argv[0] = (char *)applet;
		execv("/proc/self/exe", ab.argv);
#else
		execvp(ab.argv[0], ab.argv);
#endif
		/* exec failed — best-effort error to whatever fd 2 points to. */
		fprintf(stderr, "[gs] exec %s: %s\n", ab.argv[0], strerror(errno));
		_exit(127);
	}

	t->pid           = pid;
	t->state         = TS_STARTING;
	t->started_us    = now_us();
	t->autostart_on_exit = true;
	t->stop_deadline_ms  = 0;

	/* For tx tunnels with a control_port, schedule a one-shot
	 * get_radio + get_fec ~500 ms after spawn to populate the live cache.
	 * wfb_tx needs a moment to bind its control socket. */
	if (!strcmp(t->role, "tx") && t->control_port > 0)
		t->tx_init_query_after_us = t->started_us + 500000ull;

	/* Reset stats counters — wfb_rx/wfb_tx restart their own monotonic
	 * counters on respawn, so old values would lie. */
	t->st_msg_count = 0;
	t->st_interval_ms = 0;
	t->st_first_us = t->st_last_us = 0;
	if (!strcmp(t->role, "rx")) {
		t->st_pkt_all = t->st_pkt_lost = t->st_pkt_fec = 0;
		t->st_pkt_outgoing = t->st_pkt_dec_err = t->st_pkt_bytes = 0;
		t->st_pkt_uniq = 0;
		t->st_ant_count = 0;
		t->st_rssi_best = INT_MIN;
		memset(t->st_mcs_win, 0, sizeof(t->st_mcs_win));
		memset(t->st_mcs_win_sec, 0, sizeof(t->st_mcs_win_sec));
	} else {
		t->st_tx_pkts_in = t->st_tx_pkts_out = 0;
		t->st_tx_bytes_in = t->st_tx_bytes_out = 0;
		t->st_tx_drop = t->st_tx_trunc = t->st_tx_fec_timeouts = 0;
	}

	/* Promote to RUNNING after a short settle window in supervisor_tick;
	 * for now treat exec as success unless SIGCHLD says otherwise. */
	t->state = TS_RUNNING;
	t->restart_count++;

	/* log composed argv for diagnosability */
	char joined[512];
	size_t pos = 0;
	for (int i = 0; i < ab.argc && pos < sizeof(joined) - 2; i++) {
		int w = snprintf(joined + pos, sizeof(joined) - pos,
		                 "%s%s", i ? " " : "", ab.argv[i]);
		if (w < 0) break;
		pos += (size_t)w;
	}
	LOG_INFO("tunnel '%s' started pid=%d argv: %s", t->name, pid, joined);
	return 0;
}

void tunnel_request_stop(Tunnel *t)
{
	if (t->pid <= 0) {
		t->state = TS_STOPPED;
		t->autostart_on_exit = false;
		return;
	}
	t->autostart_on_exit = false;
	t->stop_deadline_ms  = now_ms() + GS_STOP_GRACE_MS;
	if (kill(t->pid, SIGTERM) < 0 && errno != ESRCH)
		LOG_WARN("tunnel '%s': SIGTERM pid=%d: %s",
		         t->name, t->pid, strerror(errno));
	else
		LOG_INFO("tunnel '%s' stopping (SIGTERM pid=%d)", t->name, t->pid);
}

void tunnel_on_exit(Tunnel *t, int wstatus)
{
	t->exited_us = now_us();
	if (WIFEXITED(wstatus))         t->exit_code = WEXITSTATUS(wstatus);
	else if (WIFSIGNALED(wstatus))  t->exit_code = -WTERMSIG(wstatus);
	else                            t->exit_code = -1;
	LOG_INFO("tunnel '%s' exited pid=%d code=%d (autostart_on_exit=%d)",
	         t->name, t->pid, t->exit_code, t->autostart_on_exit);
	t->pid = -1;
	t->stop_deadline_ms = 0;

	if (!t->autostart_on_exit) {
		t->state = TS_STOPPED;
		t->backoff_idx = 0;
		return;
	}
	int idx = t->backoff_idx;
	if (idx >= GS_BACKOFF_LEN) idx = GS_BACKOFF_LEN - 1;
	t->next_start_ms = now_ms() + (uint64_t)GS_BACKOFF_MS[idx];
	if (t->backoff_idx < GS_BACKOFF_LEN - 1) t->backoff_idx++;
	t->state = TS_BACKOFF;
}

void supervisor_reap(Config *c)
{
	for (;;) {
		int wstatus = 0;
		pid_t pid = waitpid(-1, &wstatus, WNOHANG);
		if (pid <= 0) break;
		bool matched = false;
		for (int i = 0; i < c->tunnel_count; i++) {
			if (c->tunnels[i].pid == pid) {
				tunnel_on_exit(&c->tunnels[i], wstatus);
				matched = true;
				break;
			}
		}
		if (!matched) LOG_WARN("reaped unknown pid=%d", pid);
	}
}

/* ---------- bounded fork helpers ------------------------------------ *
 *
 * Used by both api_handle paths (iw set channel, system commands) and
 * the CSA / scan tick handlers in gs_supervisor_csa.c / _scan.c. */

/* Bounded waitpid: like waitpid(pid, NULL, 0) but returns by deadline_ms.
 * If the child doesn't exit before the deadline, sends SIGKILL and waits
 * up to ~250 ms for the kernel to deliver and reap.  Used to bound the
 * synchronous fork+exec helpers that an api_handle path drives — without
 * this a wedged USB driver or stuck `iw` could block the supervisor's
 * event loop for tens of seconds.
 *
 * Returns:
 *   >=0  WEXITSTATUS of normally-exited child
 *   -1   child crashed/signaled (no clean exit status)
 *   -2   deadline expired (SIGKILL sent; child reaped)
 *   -3   waitpid error (errno preserved)
 */
int waitpid_deadline(pid_t pid, int deadline_ms)
{
	uint64_t end = now_ms() + (uint64_t)deadline_ms;
	const struct timespec sleep_5ms = { .tv_sec = 0, .tv_nsec = 5 * 1000 * 1000 };
	for (;;) {
		int st = 0;
		pid_t r = waitpid(pid, &st, WNOHANG);
		if (r == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
		if (r < 0) return -3;
		if (now_ms() >= end) {
			kill(pid, SIGKILL);
			for (int i = 0; i < 50; i++) {
				pid_t r2 = waitpid(pid, &st, WNOHANG);
				if (r2 == pid || r2 < 0) break;
				nanosleep(&sleep_5ms, NULL);
			}
			return -2;
		}
		nanosleep(&sleep_5ms, NULL);
	}
}

/* fork+exec `iw dev <iface> set channel <chan> <ht>`. */
int run_iw_set_channel(const char *iface, int chan, const char *ht)
{
	if (!iface || !iface[0]) return -1;
	char cs[16];
	snprintf(cs, sizeof(cs), "%d", chan);
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		execl("/usr/sbin/iw", "iw", "dev", iface, "set", "channel",
		      cs, ht, (char*)NULL);
		execlp("iw", "iw", "dev", iface, "set", "channel",
		       cs, ht, (char*)NULL);
		_exit(127);
	}
	int rc = waitpid_deadline(pid, GS_FORK_DEADLINE_MS);
	if (rc == -2) LOG_WARN("iw set channel %s: deadline %d ms — SIGKILL'd",
	                       iface, GS_FORK_DEADLINE_MS);
	return rc;
}

/* LOG_SYNC marker cadence (ms) — see logsync_emit() / WCMD_KEY_LOG_SYNC. */
#define GS_LOGSYNC_INTERVAL_MS 10000
static void logsync_emit(const Config *c);

/* Periodic tick: handle SIGKILL escalation + backoff respawn, plus
 * deferred TX state queries (~500 ms after spawn so wfb_tx has bound
 * its control socket).  CSA + scan state machines are driven from
 * gs_supervisor_csa.c / gs_supervisor_scan.c. */
static void supervisor_tick(Config *c)
{
	uint64_t t_ms = now_ms();
	uint64_t t_us = now_us();

	/* Round-robin one iface refresh per tick so the /api/v1/ifaces
	 * cache stays current without forking iw on every loop. */
	iface_state_refresh_one(t_us);
	for (int i = 0; i < c->tunnel_count; i++) {
		Tunnel *t = &c->tunnels[i];

		if (t->stop_deadline_ms && t_ms >= t->stop_deadline_ms && t->pid > 0) {
			LOG_WARN("tunnel '%s' SIGKILL pid=%d (grace expired)", t->name, t->pid);
			kill(t->pid, SIGKILL);
			t->stop_deadline_ms = 0;
		}

		if (t->state == TS_BACKOFF && t_ms >= t->next_start_ms) {
			LOG_INFO("tunnel '%s' respawning (attempt %d)",
			         t->name, t->restart_count + 1);
			tunnel_spawn(t, c->key_file);
		}

		if (t->tx_init_query_after_us && t_us >= t->tx_init_query_after_us &&
		    t->pid > 0 && t->state == TS_RUNNING) {
			t->tx_init_query_after_us = 0;
			if (wfb_cmd_refresh_radio(t) == 0) {
				LOG_INFO("tunnel '%s' radio cache: bw=%d mcs=%d stbc=%d "
				         "ldpc=%d sgi=%d vht_mode=%d vht_nss=%d",
				         t->name, t->bandwidth_mhz, t->mcs_index, t->stbc,
				         t->ldpc, t->short_gi, t->vht_mode, t->vht_nss);
			} else {
				/* Likely too soon — retry once 1 s later. */
				t->tx_init_query_after_us = t_us + 1000000ull;
			}
			(void)wfb_cmd_refresh_fec(t);
		}
	}

	csa_tick(c, t_us);
	scan_tick(c, t_us);

	/* Logging-sync heartbeat: one LOG_SYNC marker up the WCMD uplink every
	 * GS_LOGSYNC_INTERVAL_MS so post-walk the vehicle log can be fit onto the
	 * GS wall-clock and attributed to this GS capture session.  First tick
	 * fires immediately (gate starts at 0) so an early anchor lands even on a
	 * short run. */
	static uint64_t logsync_next_ms = 0;
	if (t_ms >= logsync_next_ms) {
		logsync_next_ms = t_ms + GS_LOGSYNC_INTERVAL_MS;
		logsync_emit(c);
	}
}

/* ---------- system commands ------------------------------------------ */

/* Per-command deadline for system.up / system.down.  Generous enough to
 * cover a `sleep 0.5` plus the surrounding `iw` calls without nuisance
 * timeouts; operators running heavyweight bring-up (e.g. `wifi restart`)
 * may hit this and need to wrap the heavy work in a separate script. */
#define GS_SYSTEM_CMD_DEADLINE_MS 10000
#define GS_SYSTEM_CMD_MAX_ARGV    32

/* Tokenize a single-line command string into an argv-style array, in
 * place (NUL-terminators inserted between tokens).  Splits on
 * ASCII whitespace.  Refuses tokens that would change meaning under
 * `sh -c` interpretation — operators using shell features (||, $(),
 * redirection, env-var assignment) need a wrapper script.  argv[argc]
 * is set to NULL on success.
 *
 * Returns argc on success, -1 on shell-metachar reject, -2 on overflow. */
int tokenize_argv(char *cmd, char **out_argv, int max_argv)
{
	static const char metachars[] = ";|&<>$`(){}[]*?'\"\\";
	if (strpbrk(cmd, metachars)) return -1;
	int argc = 0;
	char *save = NULL;
	for (char *tok = strtok_r(cmd, " \t", &save); tok;
	     tok = strtok_r(NULL, " \t", &save)) {
		if (argc + 1 >= max_argv) return -2;
		out_argv[argc++] = tok;
	}
	out_argv[argc] = NULL;
	return argc;
}

/* Run one system.{up,down} command.  Replaces an earlier system() that
 * funneled JSON-config strings through `sh -c` — convert to a tokenized
 * fork+execvp so a typo'd iface name can no longer accidentally invoke
 * shell features.  Bounded by GS_SYSTEM_CMD_DEADLINE_MS. */
int run_system_cmd(const char *cmd_const)
{
	LOG_INFO("system: %s", cmd_const);
	char cmd[GS_PATH_MAX];
	snprintf(cmd, sizeof(cmd), "%s", cmd_const);
	char *argv[GS_SYSTEM_CMD_MAX_ARGV];
	int argc = tokenize_argv(cmd, argv, GS_SYSTEM_CMD_MAX_ARGV);
	if (argc < 0) {
		const char *why = (argc == -1)
		    ? "shell metachar — strip features or wrap in a script"
		    : "argv overflow";
		LOG_WARN("system: '%s' refused (%s)", cmd_const, why);
		return -1;
	}
	if (argc == 0) return 0;
	pid_t pid = fork();
	if (pid < 0) {
		LOG_WARN("system: '%s' fork failed: %s", cmd_const, strerror(errno));
		return -1;
	}
	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}
	int rc = waitpid_deadline(pid, GS_SYSTEM_CMD_DEADLINE_MS);
	if (rc == -2) {
		LOG_WARN("system: '%s' deadline %d ms — SIGKILL'd",
		         cmd_const, GS_SYSTEM_CMD_DEADLINE_MS);
		return -1;
	}
	if (rc != 0) LOG_WARN("system: '%s' rc=%d", cmd_const, rc);
	return rc;
}

void run_system_block(const char *label, char cmds[][GS_PATH_MAX], int n)
{
	if (n <= 0) return;
	LOG_INFO("running %s commands (%d)", label, n);
	for (int i = 0; i < n; i++) {
		if (cmds[i][0]) (void)run_system_cmd(cmds[i]);
	}
}

/* ---------- system lifecycle helpers --------------------------------- *
 *
 * Three callers share this code: boot-time main(), /api/v1/system/up
 * (manual retry after a failed boot), /api/v1/system/down, and
 * /api/v1/system/reinit. The boot path used to inline the sequence
 * and exit on iface-readiness failure; lifting it into helpers lets
 * the supervisor stay alive (HTTP listening) when bring-up fails, so
 * the operator can still drive a retry from the WebUI.
 */

const char *system_state_name(SystemState s)
{
	switch (s) {
	case SYS_DOWN:       return "down";
	case SYS_UP:         return "up";
	case SYS_UP_FAILED:  return "up_failed";
	}
	return "?";
}

void supervisor_stop_all_tunnels(Config *c)
{
	for (int i = 0; i < c->tunnel_count; i++) {
		Tunnel *t = &c->tunnels[i];
		if (t->pid > 0) {
			t->autostart_on_exit = false;
			if (kill(t->pid, SIGTERM) == 0)
				t->stop_deadline_ms = now_ms() + GS_STOP_GRACE_MS;
		}
	}
	uint64_t deadline = now_ms() + GS_STOP_GRACE_MS + 500;
	while (now_ms() < deadline) {
		bool any = false;
		for (int i = 0; i < c->tunnel_count; i++)
			if (c->tunnels[i].pid > 0) any = true;
		if (!any) break;
		supervisor_reap(c);
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}
	for (int i = 0; i < c->tunnel_count; i++) {
		if (c->tunnels[i].pid > 0) {
			kill(c->tunnels[i].pid, SIGKILL);
			waitpid(c->tunnels[i].pid, NULL, 0);
			c->tunnels[i].pid = -1;
		}
	}
}

int supervisor_bring_up(Config *c)
{
	run_system_block("system.up", c->system_up, c->system_up_count);
	if (wait_iface_state(c, 5000, 100) != 0) {
		LOG_WARN("system.up: iface readiness timed out — rolling back "
		         "via system.down. HTTP stays up so /api/v1/system/up "
		         "can retry after the operator fixes the bring-up "
		         "sequence (typo'd iface, missing module, "
		         "NetworkManager still holding the device, etc.)");
		run_system_block("system.down", c->system_down, c->system_down_count);
		c->system_state = SYS_UP_FAILED;
		return -1;
	}
	iface_state_init(c);
	c->system_state = SYS_UP;
	return 0;
}

int supervisor_take_down(Config *c)
{
	supervisor_stop_all_tunnels(c);
	run_system_block("system.down", c->system_down, c->system_down_count);
	c->system_state = SYS_DOWN;
	return 0;
}

/* ---------- rx stats listener + rx_ant parser ------------------------ *
 *
 * One UDP socket per rx tunnel.  wfb_rx is launched with `-Y` pointing at
 * this socket; the parser extracts pkt counters and antenna RSSI for the
 * Tunnels WebUI.  If the user originally configured `stats_out` to a
 * different host:port (e.g. forwarding rx_ant to the uplink TX so the
 * vehicle's link_controller sees ground RSSI), the supervisor re-emits
 * each datagram verbatim — the local consumption is purely additive.
 */

/* Parse "host:port" → sockaddr_in.  Returns 0 on success. */
int parse_host_port(const char *s, struct sockaddr_in *out)
{
	if (!s || !*s) return -1;
	const char *colon = strrchr(s, ':');
	if (!colon || colon == s) return -1;
	char host[64];
	size_t hl = (size_t)(colon - s);
	if (hl >= sizeof(host)) return -1;
	memcpy(host, s, hl);
	host[hl] = 0;
	int port = atoi(colon + 1);
	if (port <= 0 || port > 65535) return -1;
	memset(out, 0, sizeof(*out));
	out->sin_family = AF_INET;
	out->sin_port   = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &out->sin_addr) != 1) return -1;
	return 0;
}

/* Open a non-blocking UDP listener on 127.0.0.1:0 and stash the assigned
 * port in t->stats_local_port + a "127.0.0.1:port" string in
 * t->stats_local_arg (for handing to wfb_rx -Y). */
int stats_listener_open(Tunnel *t)
{
	if (t->stats_local_fd >= 0) return 0;
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		LOG_ERR("tunnel '%s': stats listener socket: %s", t->name, strerror(errno));
		return -1;
	}
	int rcvbuf = 256 * 1024;
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
	struct sockaddr_in sa = {0};
	sa.sin_family      = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port        = 0;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		LOG_ERR("tunnel '%s': stats bind 127.0.0.1:0: %s",
		        t->name, strerror(errno));
		close(fd);
		return -1;
	}
	socklen_t slen = sizeof(sa);
	if (getsockname(fd, (struct sockaddr *)&sa, &slen) < 0) {
		LOG_ERR("tunnel '%s': stats getsockname: %s",
		        t->name, strerror(errno));
		close(fd);
		return -1;
	}
	t->stats_local_fd   = fd;
	t->stats_local_port = ntohs(sa.sin_port);
	snprintf(t->stats_local_arg, sizeof(t->stats_local_arg),
	         "127.0.0.1:%u", (unsigned)t->stats_local_port);

	/* If user configured stats_out, parse for re-emit. */
	if (t->stats_out[0]) {
		if (parse_host_port(t->stats_out, &t->stats_fwd_addr) == 0) {
			t->stats_fwd_active = 1;
			LOG_INFO("tunnel '%s': stats listening on udp:%u (forward to %s)",
			         t->name, (unsigned)t->stats_local_port, t->stats_out);
		} else {
			t->stats_fwd_active = 0;
			LOG_WARN("tunnel '%s': stats_out '%s' unparseable; not forwarding",
			         t->name, t->stats_out);
		}
	} else {
		t->stats_fwd_active = 0;
		LOG_INFO("tunnel '%s': stats listening on udp:%u",
		         t->name, (unsigned)t->stats_local_port);
	}

	/* Optional fire-and-forget logging tap (e.g. the SQLite ingester).
	 * Parsed independently of stats_out: the tap is a pure copy emitted
	 * AFTER the back-channel forward, so a dead tap consumer never gates
	 * stats_out or the vehicle link. */
	t->stats_tap_active = 0;
	if (t->stats_tap[0]) {
		if (parse_host_port(t->stats_tap, &t->stats_tap_addr) == 0) {
			t->stats_tap_active = 1;
			LOG_INFO("tunnel '%s': stats tap -> %s",
			         t->name, t->stats_tap);
		} else {
			LOG_WARN("tunnel '%s': stats_tap '%s' unparseable; not tapping",
			         t->name, t->stats_tap);
		}
	}
	return 0;
}

/* Pull an unsigned-decimal field out of a pinned JSON window.  We
 * intentionally avoid a real parser — the rx_ant payload is generated
 * by a single producer with a stable shape and we just want the named
 * counters. */
static int json_pick_u32(const char *js, size_t jl, const char *key, uint32_t *out)
{
	size_t kl = strlen(key);
	for (size_t i = 0; i + kl + 2 < jl; i++) {
		if (js[i] != '"') continue;
		if (memcmp(js + i + 1, key, kl) != 0) continue;
		size_t p = i + 1 + kl;
		if (p >= jl || js[p] != '"') continue;
		p++;
		while (p < jl && (js[p] == ' ' || js[p] == ':')) p++;
		if (p >= jl) continue;
		if (js[p] == '-') return -1; /* refuse negative for u32 */
		uint64_t v = 0;
		int seen = 0;
		while (p < jl && js[p] >= '0' && js[p] <= '9') {
			v = v * 10u + (uint32_t)(js[p] - '0');
			p++;
			seen = 1;
			if (v > 0xFFFFFFFFull) return -1;
		}
		if (!seen) continue;
		*out = (uint32_t)v;
		return 0;
	}
	return -1;
}

/* Pull a signed integer field. */
static int json_pick_i32(const char *js, size_t jl, const char *key, int32_t *out)
{
	size_t kl = strlen(key);
	for (size_t i = 0; i + kl + 2 < jl; i++) {
		if (js[i] != '"') continue;
		if (memcmp(js + i + 1, key, kl) != 0) continue;
		size_t p = i + 1 + kl;
		if (p >= jl || js[p] != '"') continue;
		p++;
		while (p < jl && (js[p] == ' ' || js[p] == ':')) p++;
		if (p >= jl) continue;
		int neg = 0;
		if (js[p] == '-') { neg = 1; p++; }
		int64_t v = 0;
		int seen = 0;
		while (p < jl && js[p] >= '0' && js[p] <= '9') {
			v = v * 10 + (js[p] - '0');
			p++;
			seen = 1;
		}
		if (!seen) continue;
		*out = (int32_t)(neg ? -v : v);
		return 0;
	}
	return -1;
}

/* Pull a string field value ("key":"value") into out_buf, NUL-terminated and
 * truncated to out_sz.  Used to read the per-antenna "id" (hex antenna_id) so
 * ant_count can dedup on physical chain rather than (chain × MCS-rung). */
static int json_pick_str(const char *js, size_t jl, const char *key,
                         char *out_buf, size_t out_sz)
{
	if (out_sz == 0) return -1;
	size_t kl = strlen(key);
	for (size_t i = 0; i + kl + 2 < jl; i++) {
		if (js[i] != '"') continue;
		if (memcmp(js + i + 1, key, kl) != 0) continue;
		size_t p = i + 1 + kl;
		if (p >= jl || js[p] != '"') continue;   /* close of the key quote */
		p++;
		while (p < jl && (js[p] == ' ' || js[p] == ':')) p++;
		if (p >= jl || js[p] != '"') continue;    /* value must be a string */
		p++;
		size_t o = 0;
		while (p < jl && js[p] != '"' && o + 1 < out_sz) out_buf[o++] = js[p++];
		out_buf[o] = '\0';
		return 0;
	}
	return -1;
}

/* Slice a JSON value by key — returns pointer + length of the substring
 * starting at the value (object, array, or scalar).  We only need this to
 * carve out the "pkt":{...} block so json_pick_u32 ranges stay scoped. */
static int json_slice_object(const char *js, size_t jl, const char *key,
                              const char **out_start, size_t *out_len)
{
	size_t kl = strlen(key);
	for (size_t i = 0; i + kl + 3 < jl; i++) {
		if (js[i] != '"') continue;
		if (memcmp(js + i + 1, key, kl) != 0) continue;
		size_t p = i + 1 + kl;
		if (p >= jl || js[p] != '"') continue;
		p++;
		while (p < jl && (js[p] == ' ' || js[p] == ':')) p++;
		if (p >= jl) continue;
		char open  = js[p];
		char close = (open == '{') ? '}' : (open == '[') ? ']' : 0;
		if (!close) return -1;
		int depth = 0;
		size_t start = p;
		for (; p < jl; p++) {
			if (js[p] == open)  depth++;
			if (js[p] == close) depth--;
			if (depth == 0) {
				*out_start = js + start;
				*out_len   = (p - start) + 1;
				return 0;
			}
		}
		return -1;
	}
	return -1;
}

/* ---------- boundary-probe PER producer ------------------------------ *
 *
 * For a tunnel with "probe": true, stats_drain() accumulates each rx_ant
 * record's pkt counters into a bucket keyed by the *received* MCS (from
 * the ant[] block) and flushes one compact {"type":"probe"} record per
 * non-empty bucket per window to the tunnel's stats_out. Bucketing by
 * received MCS means a window straddling a vehicle-side probe retune
 * emits one clean record per MCS — pre-retune traffic can never land in
 * the new rung. Port of telemetry/probe/probe_log.py --by-mcs (the
 * device-validated prototype); schema frozen in
 * tests/protocols/test_probe_protocol.py. */

static void probe_buckets_reset(Tunnel *t, uint64_t now)
{
	memset(t->probe_bucket, 0, sizeof(t->probe_bucket));
	for (int i = 0; i < 16; i++) t->probe_bucket[i].rssi = INT_MIN;
	t->probe_win_start_us = now;
}

static void probe_window_flush(Tunnel *t, uint64_t now)
{
	double win_s = (double)(now - t->probe_win_start_us) / 1e6;
	struct timespec rt;
	clock_gettime(CLOCK_REALTIME, &rt);
	unsigned long long ts_ms = (unsigned long long)rt.tv_sec * 1000ULL +
	                           (unsigned long long)rt.tv_nsec / 1000000ULL;
	for (int m = 0; m < 16; m++) {
		uint32_t u = t->probe_bucket[m].uniq;
		uint32_t l = t->probe_bucket[m].lost;
		uint32_t acc = u + l;
		if (acc == 0) continue;
		char rssi_s[16] = "null";
		if (t->probe_bucket[m].rssi != INT_MIN)
			snprintf(rssi_s, sizeof(rssi_s), "%d",
			         t->probe_bucket[m].rssi);
		char rec[256];
		int len = snprintf(rec, sizeof(rec),
		    "{\"type\":\"probe\",\"ts_ms\":%llu,\"radio_port\":%d,"
		    "\"mcs\":%d,\"per\":%.4f,\"recv\":%u,\"lost\":%u,"
		    "\"accounted\":%u,\"rssi\":%s,\"window_s\":%.3f}\n",
		    ts_ms, t->radio_port, m,
		    (double)l / (double)acc, u, l, acc, rssi_s, win_s);
		if (len > 0 && len < (int)sizeof(rec) && t->stats_fwd_active)
			(void)sendto(t->stats_local_fd, rec, (size_t)len, 0,
			             (struct sockaddr *)&t->stats_fwd_addr,
			             sizeof(t->stats_fwd_addr));
		t->probe_emit_count++;
	}
	probe_buckets_reset(t, now);
}

static void probe_accumulate(Tunnel *t, uint32_t uniq, uint32_t lost,
                             int mcs, int rssi, uint64_t now)
{
	uint64_t win_us = (uint64_t)t->probe_window_ms * 1000ULL;
	if (t->probe_win_start_us == 0)
		probe_buckets_reset(t, now);
	/* Stale partial window (probe traffic gap): the counts would span
	 * far more than the nominal window — discard rather than emit a
	 * record whose arrival time overstates its freshness. */
	if (now - t->probe_win_start_us > 4 * win_us) {
		probe_buckets_reset(t, now);
		t->probe_drop_count++;
	}
	if (mcs >= 0 && mcs < 16) {
		t->probe_bucket[mcs].uniq += uniq;
		t->probe_bucket[mcs].lost += lost;
		if (rssi != INT_MIN &&
		    (t->probe_bucket[mcs].rssi == INT_MIN ||
		     rssi > t->probe_bucket[mcs].rssi))
			t->probe_bucket[mcs].rssi = rssi;
	}
	if (now - t->probe_win_start_us >= win_us)
		probe_window_flush(t, now);
}

/* Drain pending rx_ant datagrams off a single tunnel's stats fd. */
void stats_drain(Tunnel *t)
{
	for (;;) {
		char buf[4096];
		ssize_t got = recv(t->stats_local_fd, buf, sizeof(buf) - 1, 0);
		if (got < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) return;
			if (errno == EINTR) continue;
			LOG_WARN("tunnel '%s': stats recv: %s", t->name, strerror(errno));
			return;
		}
		if (got == 0) continue;
		buf[got] = 0;

		/* Re-emit to user's stats_out before parsing — keeps latency
		 * for the downstream consumer minimal. Probe tunnels do NOT
		 * re-emit raw rx_ant: forwarded up the uplink it would be
		 * ingested by the vehicle's link_controller as *video* score
		 * (probe loss → spurious reactive demotes). They forward the
		 * computed {"type":"probe"} records instead (below). */
		if (t->stats_fwd_active && !t->probe) {
			(void)sendto(t->stats_local_fd, buf, (size_t)got, 0,
			             (struct sockaddr *)&t->stats_fwd_addr,
			             sizeof(t->stats_fwd_addr));
		}

		/* Fire-and-forget logging tap: a raw rx_ant copy to e.g. the
		 * SQLite ingester, independent of and after the back-channel
		 * above. Best-effort — a dead tap consumer never affects
		 * stats_out or the vehicle link. Gated on !probe like the
		 * back-channel: a probe tunnel's raw rx_ant is not link quality
		 * (it carries {type:probe} records instead) and would mislead
		 * the store. */
		if (t->stats_tap_active && !t->probe) {
			(void)sendto(t->stats_local_fd, buf, (size_t)got, 0,
			             (struct sockaddr *)&t->stats_tap_addr,
			             sizeof(t->stats_tap_addr));
		}

		uint32_t iv;
		if (json_pick_u32(buf, (size_t)got, "interval_ms", &iv) == 0)
			t->st_interval_ms = iv;

		/* Dispatch on JSON type.  wfb_rx emits rx_ant; wfb_tx emits
		 * tx_stats.  We accept either on the same socket so a single
		 * supervisor port works for both tunnel roles. */
		bool is_rx_ant   = strstr(buf, "\"type\":\"rx_ant\"") != NULL;
		bool is_tx_stats = strstr(buf, "\"type\":\"tx_stats\"") != NULL;
		if (!is_rx_ant && !is_tx_stats) continue;

		if (is_rx_ant) {
			/* Record-local counters for the probe PER accumulator
			 * (st_pkt_* keep "latest seen" semantics for the UI). */
			uint32_t rec_uniq = 0, rec_lost = 0;
			int      rec_mcs  = -1;
			const char *pkt = NULL;
			size_t pkl = 0;
			if (json_slice_object(buf, (size_t)got, "pkt", &pkt, &pkl) == 0) {
				uint32_t u;
				if (json_pick_u32(pkt, pkl, "all",            &u) == 0) t->st_pkt_all      = u;
				if (json_pick_u32(pkt, pkl, "lost",           &u) == 0) { t->st_pkt_lost = u; rec_lost = u; }
				if (json_pick_u32(pkt, pkl, "fec_recovered",  &u) == 0) t->st_pkt_fec      = u;
				if (json_pick_u32(pkt, pkl, "outgoing",       &u) == 0) t->st_pkt_outgoing = u;
				if (json_pick_u32(pkt, pkl, "dec_err",        &u) == 0) t->st_pkt_dec_err  = u;
				if (json_pick_u32(pkt, pkl, "outgoing_bytes", &u) == 0) t->st_pkt_bytes    = u;
				if (json_pick_u32(pkt, pkl, "uniq",           &u) == 0) {
					t->st_pkt_uniq = u;
					rec_uniq = u;
					/* Scanner: any non-zero `uniq` sample during the
					 * current step's dwell counts as "we found the
					 * vehicle on this channel". `pkt.uniq` is the
					 * per-100ms-interval count (not cumulative), so a
					 * simple before/after diff doesn't work — sample
					 * continuously and OR-set a step-local flag.
					 *
					 * 200 ms post-hop grace: wfb_rx's own pcap kernel
					 * buffer can hold ~1 stats interval of pre-hop
					 * packets, which would falsely trip the flag on the
					 * very first sample. Skipping the first 200 ms of
					 * each dwell drops that "in-flight" blob without
					 * meaningfully shrinking the detection window. */
					if (u > 0 &&
					    g_scan.phase == SCAN_RUNNING &&
					    !strcmp(t->role, "rx") &&
					    now_us() >= g_scan.step_started_us + 200000ULL) {
						g_scan.step_saw_traffic = true;
					}
				}
				}

				/* Walk antenna list for best avg rssi.  rssi.avg is signed. */
			const char *ant = NULL;
			size_t al = 0;
			int ant_count = 0;            /* raw ant[] objects (fallback) */
			char seen_ids[16][24];        /* distinct antenna_id (physical chain) */
			int  n_seen = 0;
			int best_rssi = INT_MIN;
			uint32_t mcs_hist[16] = {0}; /* per-received-MCS pkt counts, this window */
			if (json_slice_object(buf, (size_t)got, "ant", &ant, &al) == 0 && al >= 2) {
				int depth = 0;
				size_t obj_start = 0;
				for (size_t i = 0; i < al; i++) {
					char ch = ant[i];
					if (ch == '{') {
						if (depth == 0) obj_start = i;
						depth++;
					} else if (ch == '}') {
						depth--;
						if (depth == 0) {
							const char *o = ant + obj_start;
							size_t ol = (i - obj_start) + 1;
							const char *rblock;
							size_t rbl;
							if (json_slice_object(o, ol, "rssi", &rblock, &rbl) == 0) {
								int32_t avg;
								if (json_pick_i32(rblock, rbl, "avg", &avg) == 0) {
									if (avg > best_rssi) best_rssi = avg;
								}
							}
							/* Received MCS of this ant entry (-Y keys
							 * entries by freq:mcs:bw). Last one wins —
							 * mirrors probe_log.py. */
							int32_t am;
							if (json_pick_i32(o, ol, "mcs", &am) == 0) {
								rec_mcs = am;
								/* Bucket this entry's pkt count by its
								 * received MCS for the Tunnels-tab
								 * histogram (peek PROTECT visibility). */
								int32_t ap;
								if (am >= 0 && am < 16 &&
								    json_pick_i32(o, ol, "pkts", &ap) == 0 &&
								    ap > 0)
									mcs_hist[am] += (uint32_t)ap;
							}
							ant_count++;
							/* ant_count should reflect PHYSICAL antenna
							 * chains, not raw ant[] entries: wfb-ng keys
							 * antenna_stat by (freq,mcs,bw,id), so one chain
							 * appears once per MCS rung present this window
							 * (downlink carries up to 3 at once: bulk +
							 * peek-PROTECT + transitional).  Dedup on "id"
							 * (= wlan_idx<<8|chain) so the count is hardware,
							 * not the rung mix.  mcs_hist carries per-rung. */
							char idbuf[24];
							if (json_pick_str(o, ol, "id", idbuf,
							                  sizeof idbuf) == 0) {
								int dup = 0;
								for (int s = 0; s < n_seen; s++)
									if (strcmp(seen_ids[s], idbuf) == 0) {
										dup = 1;
										break;
									}
								if (!dup && n_seen < (int)(sizeof seen_ids /
								                           sizeof seen_ids[0])) {
									snprintf(seen_ids[n_seen],
									         sizeof seen_ids[0], "%s", idbuf);
									n_seen++;
								}
							}
						}
					}
				}
			}
			/* Prefer the deduped physical-chain count; fall back to the raw
			 * object count if the -Y stream predates the per-ant "id" field. */
			t->st_ant_count = (n_seen > 0) ? n_seen : ant_count;
			if (best_rssi != INT_MIN) t->st_rssi_best = best_rssi;
			/* Fold this rx_ant window's per-MCS counts into the current
			 * 1 s ring slot. The API sums the last 10 slots, giving a
			 * 10 s sliding window: long enough that the brief per-GOP
			 * PROTECT burst is always captured, short enough that the
			 * bars track what the link is doing right now. */
			{
				uint64_t nsec  = now_us() / 1000000ULL;
				int      wslot = (int)(nsec % 10);
				if (t->st_mcs_win_sec[wslot] != nsec) {
					memset(t->st_mcs_win[wslot], 0,
					       sizeof(t->st_mcs_win[wslot]));
					t->st_mcs_win_sec[wslot] = nsec;
				}
				for (int m = 0; m < 16; m++)
					t->st_mcs_win[wslot][m] += mcs_hist[m];
			}

			/* Boundary-probe PER accumulation + window flush. */
			if (t->probe)
				probe_accumulate(t, rec_uniq, rec_lost, rec_mcs,
				                 best_rssi, now_us());
		}

		if (is_tx_stats) {
			const char *txb = NULL;
			size_t tbl = 0;
			if (json_slice_object(buf, (size_t)got, "tx", &txb, &tbl) == 0) {
				uint32_t u;
				int32_t  s;
				if (json_pick_u32(txb, tbl, "pkts_in",      &u) == 0) t->st_tx_pkts_in     = u;
				if (json_pick_u32(txb, tbl, "pkts_out",     &u) == 0) t->st_tx_pkts_out    = u;
				if (json_pick_u32(txb, tbl, "bytes_in",     &u) == 0) t->st_tx_bytes_in    = u;
				if (json_pick_u32(txb, tbl, "bytes_out",    &u) == 0) t->st_tx_bytes_out   = u;
				if (json_pick_u32(txb, tbl, "pkts_drop",    &u) == 0) t->st_tx_drop        = u;
				if (json_pick_u32(txb, tbl, "pkts_trunc",   &u) == 0) t->st_tx_trunc       = u;
				if (json_pick_u32(txb, tbl, "fec_timeouts", &u) == 0) t->st_tx_fec_timeouts = u;
				if (json_pick_i32(txb, tbl, "fec_k",        &s) == 0) {
					t->fec_k = s;
					t->fec_cache_have = 1;
				}
				if (json_pick_i32(txb, tbl, "fec_n",        &s) == 0) {
					t->fec_n = s;
					t->fec_cache_have = 1;
				}
			}
			/* radio block — refresh the live radio cache so the WebUI
			 * gets continuous updates without a wfb_cmd round-trip. */
			const char *rb = NULL;
			size_t rl = 0;
			if (json_slice_object(buf, (size_t)got, "radio", &rb, &rl) == 0) {
				int32_t s;
				if (json_pick_i32(rb, rl, "mcs",      &s) == 0) t->mcs_index     = s;
				if (json_pick_i32(rb, rl, "bw",       &s) == 0) t->bandwidth_mhz = s;
				if (json_pick_i32(rb, rl, "short_gi", &s) == 0) t->short_gi      = s;
				if (json_pick_i32(rb, rl, "stbc",     &s) == 0) t->stbc          = s;
				if (json_pick_i32(rb, rl, "ldpc",     &s) == 0) t->ldpc          = s;
				if (json_pick_i32(rb, rl, "vht_mode", &s) == 0) t->vht_mode      = s;
				if (json_pick_i32(rb, rl, "vht_nss",  &s) == 0) t->vht_nss       = s;
				t->radio_cache_have = 1;
				t->radio_cache_us   = now_us();
			}
		}

		uint64_t now = now_us();
		if (t->st_msg_count == 0) t->st_first_us = now;
		t->st_last_us = now;
		t->st_msg_count++;
	}
}

void stats_listener_close(Tunnel *t)
{
	if (t->stats_local_fd >= 0) {
		close(t->stats_local_fd);
		t->stats_local_fd = -1;
	}
}

/* ---------- iface state validation ----------------------------------- *
 *
 * Run AFTER system.up. Confirms each iface declared in any tunnel:
 *   - exists and is in monitor mode (via `iw dev <iface> info`)
 *   - is administratively UP (via /sys/class/net/<iface>/flags, IFF_UP bit)
 *
 * If any check fails, the supervisor refuses to spawn tunnels. Spawning
 * wfb_rx into a non-monitor iface produces opaque "pcap_activate failed"
 * loops that aren't actionable.
 */
#define IFF_UP_BIT 0x1u

/* fork+exec a command, capture up to cap-1 bytes of stdout, NUL-terminate.
 * Returns child exit status (0 on success), -1 on fork/pipe failure.
 *
 * Used to invoke `iw` without going through a shell — earlier versions
 * used popen()/system() which interpolated iface names into a /bin/sh -c
 * string.  iface names come from the operator's root-only config so it
 * was not a privilege boundary, but the shell-free path is cleaner and
 * removes one accidental-quoting footgun. */
int run_capture(char *const argv[], char *out, size_t cap)
{
	if (cap == 0 || !argv || !argv[0]) return -1;
	int pipefd[2];
	if (pipe(pipefd) < 0) return -1;
	pid_t pid = fork();
	if (pid < 0) {
		close(pipefd[0]); close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		close(pipefd[0]);
		if (pipefd[1] != STDOUT_FILENO) {
			dup2(pipefd[1], STDOUT_FILENO);
			close(pipefd[1]);
		}
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	close(pipefd[1]);
	int rfd = pipefd[0];
	int flags = fcntl(rfd, F_GETFL, 0);
	if (flags >= 0) fcntl(rfd, F_SETFL, flags | O_NONBLOCK);
	size_t pos = 0;
	const uint64_t deadline = now_ms() + GS_FORK_DEADLINE_MS;
	bool timed_out = false;
	while (pos + 1 < cap) {
		uint64_t now = now_ms();
		if (now >= deadline) { timed_out = true; break; }
		struct pollfd p = { .fd = rfd, .events = POLLIN };
		int pr = poll(&p, 1, (int)(deadline - now));
		if (pr < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (pr == 0) { timed_out = true; break; }
		ssize_t r = read(rfd, out + pos, cap - 1 - pos);
		if (r < 0) {
			if (errno == EINTR || errno == EAGAIN) continue;
			break;
		}
		if (r == 0) break;
		pos += (size_t)r;
	}
	out[pos] = 0;
	close(rfd);
	if (timed_out) {
		kill(pid, SIGKILL);
		(void)waitpid_deadline(pid, 250);
		LOG_WARN("run_capture(%s): deadline %d ms — SIGKILL'd",
		         argv[0] ? argv[0] : "?", GS_FORK_DEADLINE_MS);
		return -1;
	}
	int rc = waitpid_deadline(pid, GS_FORK_DEADLINE_MS);
	if (rc == -2) LOG_WARN("run_capture(%s): waitpid deadline — SIGKILL'd",
	                       argv[0] ? argv[0] : "?");
	return (rc < 0) ? -1 : rc;
}

int iface_is_monitor(const char *iface)
{
	char *argv[] = { (char*)"iw", (char*)"dev",
	                 (char*)iface, (char*)"info", NULL };
	char out[1024];
	int rc = run_capture(argv, out, sizeof(out));
	if (rc != 0) return -1;
	return strstr(out, "type monitor") ? 0 : -1;
}

/* ---------- iface state cache: helpers ------------------------------ */

IfaceState *iface_state_find(const char *name)
{
	for (int i = 0; i < g_iface_state_count; i++)
		if (!strcmp(g_iface_state[i].name, name))
			return &g_iface_state[i];
	return NULL;
}

IfaceState *iface_state_intern(const char *name)
{
	IfaceState *st = iface_state_find(name);
	if (st) return st;
	if (g_iface_state_count >= GS_MAX_GLOBAL_IFACES) return NULL;
	st = &g_iface_state[g_iface_state_count++];
	memset(st, 0, sizeof(*st));
	snprintf(st->name, sizeof(st->name), "%s", name);
	st->chan = -1;
	st->freq_mhz = -1;
	st->txpower_mbm = -1;
	return st;
}

/* Parse one line out of `iw dev <iface> info` output.  Tolerates the
 * various phrasings iw uses across versions:
 *
 *   channel 161 (5805 MHz), width: 20 MHz, center1: 5805 MHz
 *   channel 36 (5180 MHz), width: 40 MHz (no HT), center1: 5190 MHz
 *   txpower 20.00 dBm
 *
 * Sets st->chan, st->freq_mhz, st->ht ("HT20" / "HT40+" / "HT40-"),
 * st->txpower_mbm where each appears.  Caller resets fields to "unknown"
 * before invoking. */
static void iface_state_absorb_line(IfaceState *st, const char *line)
{
	const char *p = strstr(line, "channel ");
	if (p) {
		int ch = -1, freq = -1;
		if (sscanf(p, "channel %d (%d MHz)", &ch, &freq) >= 1) {
			st->chan = ch;
			if (freq > 0) st->freq_mhz = freq;
		}
		const char *w = strstr(p, "width:");
		const char *cn = strstr(p, "center1:");
		if (w) {
			int width = 20;
			(void)sscanf(w, "width: %d MHz", &width);
			if (width <= 20) {
				snprintf(st->ht, sizeof(st->ht), "HT20");
			} else if (width == 40) {
				int center1 = 0;
				if (cn) (void)sscanf(cn, "center1: %d MHz", &center1);
				/* HT40+ if center1 > primary, HT40- if below. */
				if (center1 > 0 && st->freq_mhz > 0) {
					snprintf(st->ht, sizeof(st->ht),
					    center1 > st->freq_mhz ? "HT40+" : "HT40-");
				} else {
					snprintf(st->ht, sizeof(st->ht), "HT40+");
				}
			} else if (width > 0 && width < 1000) {
				snprintf(st->ht, sizeof(st->ht), "HT%d", width % 1000);
			}
		}
	}
	p = strstr(line, "txpower ");
	if (p) {
		double dbm = 0.0;
		if (sscanf(p, "txpower %lf", &dbm) == 1)
			st->txpower_mbm = (int)(dbm * 100.0 + 0.5);
	}
}

/* Pull a fresh snapshot via `iw dev <name> info`.  Synchronous fork+exec;
 * blocks ~30–80 ms on RTL88x2CU/EU.  Updates st->last_query_us regardless
 * of success so we don't hammer iw on a busted iface. */
int iface_state_query(IfaceState *st)
{
	st->last_query_us = now_us();
	st->chan = -1;
	st->freq_mhz = -1;
	st->ht[0] = 0;
	st->txpower_mbm = -1;

	char *argv[] = { (char*)"iw", (char*)"dev",
	                 st->name, (char*)"info", NULL };
	char out[1024];
	int rc = run_capture(argv, out, sizeof(out));
	st->last_rc = rc;
	if (rc != 0) return -1;
	/* Walk lines without strtok() so a missing trailing newline still
	 * absorbs the final line. */
	char *p = out;
	while (*p) {
		char *eol = strchr(p, '\n');
		if (eol) *eol = 0;
		iface_state_absorb_line(st, p);
		if (!eol) break;
		p = eol + 1;
	}
	return 0;
}

/* Walk every tunnel's iface list, intern each unique name, and run an
 * initial state query so the WebUI has truth on the first refresh.
 * Called once after system.up has finished. */
void iface_state_init(const Config *c)
{
	for (int i = 0; i < c->tunnel_count; i++) {
		for (int k = 0; k < c->tunnels[i].iface_count; k++) {
			IfaceState *st = iface_state_intern(c->tunnels[i].ifaces[k]);
			if (st) (void)iface_state_query(st);
		}
	}
}

/* Refresh one stale iface per tick (round-robin), so we don't fork iw
 * on every tick yet still keep the cache fresh. */
void iface_state_refresh_one(uint64_t now_us_arg)
{
	static int rr_idx = 0;
	if (g_iface_state_count == 0) return;
	for (int tries = 0; tries < g_iface_state_count; tries++) {
		IfaceState *st = &g_iface_state[rr_idx];
		rr_idx = (rr_idx + 1) % g_iface_state_count;
		if (now_us_arg - st->last_query_us >= GS_IFACE_REFRESH_US) {
			(void)iface_state_query(st);
			return;
		}
	}
}

int iface_is_admin_up(const char *iface)
{
	char path[128];
	int pn = snprintf(path, sizeof(path), "/sys/class/net/%s/flags", iface);
	if (pn < 0 || pn >= (int)sizeof(path)) return -1;
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	unsigned long flags = 0;
	int n = fscanf(f, "%lx", &flags);
	fclose(f);
	if (n != 1) return -1;
	return (flags & IFF_UP_BIT) ? 0 : -1;
}

/* Block up to `timeout_ms` waiting for every tunnel iface to be both
 * admin-UP and in monitor mode.  Polled every `interval_ms`.  This
 * absorbs the nondeterministic delay between `iw dev … set monitor` and
 * the iface becoming usable for pcap_open_live — a fixed `sleep` in the
 * config is fragile (driver/USB/load dependent).  Returns 0 if every
 * iface is ready before the deadline, -1 otherwise. */
int wait_iface_state(const Config *c, int timeout_ms, int interval_ms)
{
	uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
	int prev_bad = -1;
	for (;;) {
		int bad = 0;
		const char *first_bad = NULL;
		const char *first_reason = NULL;
		for (int i = 0; i < c->tunnel_count; i++) {
			const Tunnel *t = &c->tunnels[i];
			for (int k = 0; k < t->iface_count; k++) {
				const char *ifc = t->ifaces[k];
				if (iface_is_admin_up(ifc) != 0) {
					bad++;
					if (!first_bad) { first_bad = ifc; first_reason = "down"; }
					continue;
				}
				if (iface_is_monitor(ifc) != 0) {
					bad++;
					if (!first_bad) { first_bad = ifc; first_reason = "not-monitor"; }
				}
			}
		}
		if (bad == 0) {
			LOG_INFO("iface state OK on %d tunnel(s)", c->tunnel_count);
			return 0;
		}
		if (bad != prev_bad) {
			LOG_INFO("waiting for iface readiness — %d pending (first: %s %s)",
			         bad, first_bad ? first_bad : "?",
			         first_reason ? first_reason : "?");
			prev_bad = bad;
		}
		if (now_ms() >= deadline) {
			/* One last detailed report so the operator knows which iface
			 * timed out and why. */
			for (int i = 0; i < c->tunnel_count; i++) {
				const Tunnel *t = &c->tunnels[i];
				for (int k = 0; k < t->iface_count; k++) {
					const char *ifc = t->ifaces[k];
					if (iface_is_admin_up(ifc) != 0) {
						LOG_ERR("iface '%s' is not administratively UP "
						        "after %d ms — check 'ip link set %s up' "
						        "or extend the system.up sleep",
						        ifc, timeout_ms, ifc);
					} else if (iface_is_monitor(ifc) != 0) {
						LOG_ERR("iface '%s' is not in monitor mode "
						        "after %d ms — check 'iw dev %s set monitor'",
						        ifc, timeout_ms, ifc);
					}
				}
			}
			LOG_ERR("iface readiness timed out; refusing to spawn tunnels");
			return -1;
		}
		struct timespec ts = {
			.tv_sec  = interval_ms / 1000,
			.tv_nsec = (long)(interval_ms % 1000) * 1000000L,
		};
		nanosleep(&ts, NULL);
	}
}

/* ---------- wfb_cmd passthrough -------------------------------------- *
 *
 * Wire format mirrors poc/build/wfb-ng/src/tx_cmd.h (the patched wfb-ng
 * shipped via build_wfb_tx.sh). Keep the layout in sync if tx_cmd.h
 * changes — values are network byte order on the wire.
 *
 * Request:  uint32_t req_id; uint8_t cmd_id; <union body>
 *   set_fec    body (4 B):  k, n, fec_timeout_ms (NB)
 *   set_radio  body (7 B):  stbc, ldpc, short_gi, bw, mcs, vht_mode, vht_nss
 *   get_fec / get_radio:    no body
 *
 * Response: uint32_t req_id; uint32_t rc (NB); <union body>
 *   On error (rc != 0): only req_id+rc are sent (body omitted).
 *
 * wfb_tx binds its control socket to 127.0.0.1 only — sendto must come
 * from a 127.x address. */

/* WfbCmd*Req / WfbCmd*Resp typedefs and shared/wfb_control.h opcodes are
 * pulled in by gs_supervisor.h. */

/* Returns next req_id (32-bit rolling). Single-threaded => static is fine. */
uint32_t wfb_cmd_next_req_id(void)
{
	static uint32_t s_req_id = 0;
	return ++s_req_id;
}

/* Send a wfb_cmd request to 127.0.0.1:control_port and wait briefly for a
 * reply. Returns the number of bytes received (0 on timeout, -1 on send
 * failure). On a successful recv, *out_rc is set to the parsed rc field.
 *
 * `req` is the packed request payload; `req_len` its size on the wire.
 * `resp_buf` is a caller-provided buffer big enough for the largest
 * possible response (sizeof(WfbCmdGetRadioResp)).
 */
int wfb_cmd_round_trip(int control_port,
                              const void *req, size_t req_len,
                              void *resp_buf, size_t resp_cap,
                              uint32_t *out_rc)
{
	if (out_rc) *out_rc = 0xFFFFFFFFu;
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) return -1;
	struct timeval tv = {
		.tv_sec  = WFB_CMD_REPLY_TIMEOUT_MS / 1000,
		.tv_usec = (WFB_CMD_REPLY_TIMEOUT_MS % 1000) * 1000,
	};
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	struct sockaddr_in dst = {
		.sin_family = AF_INET,
		.sin_port   = htons((uint16_t)control_port),
	};
	dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	ssize_t w = sendto(s, req, req_len, 0,
	                   (struct sockaddr *)&dst, sizeof(dst));
	if (w != (ssize_t)req_len) {
		close(s);
		return -1;
	}
	ssize_t r = recv(s, resp_buf, resp_cap, 0);
	close(s);
	if (r <= 0) return 0;          /* timeout or peer error */
	if (out_rc && r >= 8) {
		uint32_t rc_nb;
		memcpy(&rc_nb, (char *)resp_buf + 4, 4);
		*out_rc = ntohl(rc_nb);
	}
	return (int)r;
}

/* Fetch live radio params via get_radio and stash into the tunnel's
 * runtime cache.  Single-shot (~200ms timeout); caller throttles. */
int wfb_cmd_refresh_radio(Tunnel *t)
{
	if (t->control_port <= 0) return -1;
	WfbCmdGetReq req = {
		.req_id = wfb_cmd_next_req_id(),
		.cmd_id = WFB_CMD_GET_RADIO,
	};
	char buf[64];
	uint32_t rc_out = 0xFFFFFFFFu;
	int n = wfb_cmd_round_trip(t->control_port, &req, sizeof(req),
	                           buf, sizeof(buf), &rc_out);
	if (n < (int)sizeof(WfbCmdGetRadioResp) || rc_out != 0) return -1;
	WfbCmdGetRadioResp gr;
	memcpy(&gr, buf, sizeof(gr));
	t->stbc           = gr.stbc;
	t->ldpc           = gr.ldpc;
	t->short_gi       = gr.short_gi;
	t->bandwidth_mhz  = gr.bandwidth;
	t->mcs_index      = gr.mcs_index;
	t->vht_mode       = gr.vht_mode;
	t->vht_nss        = gr.vht_nss;
	t->radio_cache_have = 1;
	t->radio_cache_us   = now_us();
	return 0;
}

int wfb_cmd_refresh_fec(Tunnel *t)
{
	if (t->control_port <= 0) return -1;
	WfbCmdGetReq req = {
		.req_id = wfb_cmd_next_req_id(),
		.cmd_id = WFB_CMD_GET_FEC,
	};
	char buf[64];
	uint32_t rc_out = 0xFFFFFFFFu;
	int n = wfb_cmd_round_trip(t->control_port, &req, sizeof(req),
	                           buf, sizeof(buf), &rc_out);
	if (n < (int)sizeof(WfbCmdGetFecResp) || rc_out != 0) return -1;
	WfbCmdGetFecResp gf;
	memcpy(&gf, buf, sizeof(gf));
	t->fec_k          = gf.k;
	t->fec_n          = gf.n;
	t->fec_timeout_ms = ntohs(gf.fec_timeout_ms);
	t->fec_cache_have = 1;
	return 0;
}

/* Tiny query-string scanner: find &key=...& and copy value (URL-decoded
 * for + and %xx is not needed for our integer args). On hit, returns
 * pointer into qs at start of value and *len_out is value length. NULL on
 * miss. qs is the part after '?', already de-prefixed. */
const char *qs_get(const char *qs, const char *key, size_t *len_out)
{
	if (!qs) return NULL;
	size_t klen = strlen(key);
	const char *p = qs;
	while (*p) {
		const char *eq = strchr(p, '=');
		const char *amp = strchr(p, '&');
		if (!eq) break;
		size_t kl = (size_t)(eq - p);
		const char *val = eq + 1;
		size_t vl = amp ? (size_t)(amp - val) : strlen(val);
		if (kl == klen && !strncmp(p, key, klen)) {
			if (len_out) *len_out = vl;
			return val;
		}
		if (!amp) break;
		p = amp + 1;
	}
	return NULL;
}

int qs_get_int(const char *qs, const char *key, int *out)
{
	size_t vl;
	const char *v = qs_get(qs, key, &vl);
	if (!v || vl == 0 || vl > 10) return -1;
	char buf[12];
	memcpy(buf, v, vl); buf[vl] = 0;
	char *end = NULL;
	long iv = strtol(buf, &end, 10);
	if (!end || *end) return -1;
	*out = (int)iv;
	return 0;
}

/* ---------- WCMD emit ------------------------------------------------ *
 *
 * 16-byte WCMD_MSG_REQ, layout per shared/wcmd_proto.h. Sent to the
 * uplink tunnel's udp_in_port (127.0.0.1) where wfb_tx forwards it over
 * the air.  The vehicle's link_controller demuxes WCMD frames from
 * rx_ant JSON via the WCMD_MAGIC ("WCMD" BE) prefix and proxies to the
 * venc HTTP API.
 *
 * Fire-and-forget on this side — link_controller's WcmdResp goes back
 * via the rx_ant return path, which the ground supervisor does not yet
 * bind.  Response correlation is deferred to v2 per the design doc.
 *
 * WCMD_* defines, WcmdReq layout, and WCMD_BURST_FRAMES live in
 * gs_supervisor.h. */

/* Per-key rate-limit state (single-process). Index = key id (1-based). */
uint64_t g_wcmd_last_send_ms[WCMD_KEY_MAX + 1] = { 0 };
uint16_t g_wcmd_seq = 0;

/* Emit-side observability — surfaced in /api/v1/status so the WebUI can
 * show burst-dedup health without polling the vehicle directly. */
uint64_t g_wcmd_emit_total      = 0; /* logical WCMDs emitted (all keys) */
uint64_t g_wcmd_emit_frames     = 0; /* wire frames sent (bursts × copies actually sent) */
uint64_t g_wcmd_emit_rate_limit = 0; /* /api/v1/cmd rejections by per-key window */
uint64_t g_wcmd_emit_failed     = 0; /* sendto failed entirely (no copy on the wire) */

/* Logging-sync marker emit state — kept separate from the operator WCMD
 * counters above so the WebUI's "commands emitted" stats aren't inflated by
 * the 10 s heartbeat.  Shares g_wcmd_seq so every marker still gets a wire-
 * unique seq (the vehicle dedups the 3-frame burst per (key, seq)). */
uint64_t g_logsync_emit_total = 0;   /* markers emitted (≥1 copy on the wire) */

int wcmd_key_from_str(const char *s, size_t n)
{
	if (n == 12 && !strncmp(s, "bitrate_kbps",  12)) return WCMD_KEY_BITRATE_KBPS;
	if (n ==  3 && !strncmp(s, "fps",            3)) return WCMD_KEY_FPS;
	if (n == 13 && !strncmp(s, "payload_bytes", 13)) return WCMD_KEY_PAYLOAD_BYTES;
	if (n ==  9 && !strncmp(s, "force_idr",      9)) return WCMD_KEY_FORCE_IDR;
	if (n ==  9 && !strncmp(s, "wfb_fec_k",      9)) return WCMD_KEY_WFB_FEC_K;
	if (n ==  9 && !strncmp(s, "wfb_fec_n",      9)) return WCMD_KEY_WFB_FEC_N;
	if (n ==  7 && !strncmp(s, "wfb_mcs",        7)) return WCMD_KEY_WFB_MCS;
	if (n == 13 && !strncmp(s, "wfb_bandwidth", 13)) return WCMD_KEY_WFB_BANDWIDTH;
	if (n ==  8 && !strncmp(s, "wfb_ldpc",       8)) return WCMD_KEY_WFB_LDPC;
	if (n ==  8 && !strncmp(s, "wfb_stbc",       8)) return WCMD_KEY_WFB_STBC;
	if (n == 12 && !strncmp(s, "wfb_short_gi", 12))  return WCMD_KEY_WFB_SHORT_GI;
	if (n == 11 && !strncmp(s, "fec_enabled", 11))   return WCMD_KEY_FEC_ENABLED;
	if (n == 11 && !strncmp(s, "mcs_enabled", 11))   return WCMD_KEY_MCS_ENABLED;
	if (n == 11 && !strncmp(s, "wfb_txpower", 11))   return WCMD_KEY_WFB_TXPOWER;
	if (n ==  6 && !strncmp(s, "record",       6))   return WCMD_KEY_RECORD;
	if (n == 12 && !strncmp(s, "peek_enabled", 12))  return WCMD_KEY_PEEK_ENABLED;
	return -1;
}

const char *wcmd_key_name(int key)
{
	switch (key) {
	case WCMD_KEY_BITRATE_KBPS:  return "bitrate_kbps";
	case WCMD_KEY_FPS:           return "fps";
	case WCMD_KEY_PAYLOAD_BYTES: return "payload_bytes";
	case WCMD_KEY_FORCE_IDR:     return "force_idr";
	case WCMD_KEY_WFB_FEC_K:     return "wfb_fec_k";
	case WCMD_KEY_WFB_FEC_N:     return "wfb_fec_n";
	case WCMD_KEY_WFB_MCS:       return "wfb_mcs";
	case WCMD_KEY_WFB_BANDWIDTH: return "wfb_bandwidth";
	case WCMD_KEY_WFB_LDPC:      return "wfb_ldpc";
	case WCMD_KEY_WFB_STBC:      return "wfb_stbc";
	case WCMD_KEY_WFB_SHORT_GI:  return "wfb_short_gi";
	case WCMD_KEY_FEC_ENABLED:   return "fec_enabled";
	case WCMD_KEY_MCS_ENABLED:   return "mcs_enabled";
	case WCMD_KEY_WFB_TXPOWER:   return "wfb_txpower";
	case WCMD_KEY_RECORD:        return "record";
	case WCMD_KEY_PEEK_ENABLED:      return "peek_enabled";
	}
	return "?";
}

/* Send a WCMD_MSG_REQ packet as a redundancy burst.  WCMD_BURST_FRAMES
 * copies of the same datagram (identical seq) go through one socket
 * back-to-back; the vehicle's seq-dedup window collapses them to a
 * single apply.  Returns 0 if at least one copy made it onto the wire,
 * an errno code (positive) if every send failed, or -1 if the uplink
 * tunnel is missing/disabled.
 *
 * Cost: 48 bytes on the air per command.  Benefit: single-FEC-block
 * loss on the uplink no longer drops the WCMD.  We don't space the
 * frames across FEC blocks here — back-to-back lands in one block, so
 * full-block loss still drops the command.  See gs.html roadmap for the
 * supervisor_tick-driven spaced-burst follow-up. */
int wcmd_emit(const Config *c, int key, int32_t value, uint16_t *seq_out)
{
	if (!c->venc_cmd_enabled) return -1;
	const Tunnel *up = NULL;
	for (int i = 0; i < c->tunnel_count; i++) {
		if (!strcmp(c->tunnels[i].name, c->venc_cmd_uplink) &&
		    !strcmp(c->tunnels[i].role, "tx")) {
			up = &c->tunnels[i];
			break;
		}
	}
	if (!up || up->udp_in_port <= 0) return -1;

	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) return errno;
	struct sockaddr_in dst = {
		.sin_family = AF_INET,
		.sin_port   = htons((uint16_t)up->udp_in_port),
	};
	dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	uint16_t seq = ++g_wcmd_seq;
	if (seq_out) *seq_out = seq;

	WcmdReq req = {
		.magic    = htonl(WCMD_MAGIC),
		.version  = WCMD_VERSION,
		.msg_type = WCMD_MSG_REQ,
		.seq      = htons(seq),
		.key      = (uint8_t)key,
		.flags    = 0,
		._pad     = 0,
		.value    = (int32_t)htonl((uint32_t)value),
	};
	int sent = 0, last_err = 0;
	for (int i = 0; i < WCMD_BURST_FRAMES; i++) {
		ssize_t w = sendto(s, &req, sizeof(req), 0,
		                   (struct sockaddr *)&dst, sizeof(dst));
		if (w == sizeof(req)) sent++;
		else                  last_err = errno;
	}
	close(s);
	if (sent > 0) {
		g_wcmd_emit_total++;
		g_wcmd_emit_frames += (uint64_t)sent;
		return 0;
	}
	g_wcmd_emit_failed++;
	return last_err ? last_err : -1;
}

/* Emit one LOG_SYNC marker up the WCMD uplink (see WCMD_KEY_LOG_SYNC in
 * shared/wcmd_proto.h).  Mirrors wcmd_emit's socket/burst path but carries a
 * CLOCK_REALTIME wall-clock seconds stamp instead of an operator value, and
 * touches only g_logsync_emit_total so the operator cmd stats stay clean.
 * Fire-and-forget: a dropped marker just means one fewer alignment anchor —
 * the importer's line fit tolerates gaps.  No-op when the cmd subsystem or
 * uplink tunnel is unavailable (same gating as wcmd_emit). */
static void logsync_emit(const Config *c)
{
	if (!c->venc_cmd_enabled) return;
	const Tunnel *up = NULL;
	for (int i = 0; i < c->tunnel_count; i++) {
		if (!strcmp(c->tunnels[i].name, c->venc_cmd_uplink) &&
		    !strcmp(c->tunnels[i].role, "tx")) {
			up = &c->tunnels[i];
			break;
		}
	}
	if (!up || up->udp_in_port <= 0) return;

	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) return;
	struct sockaddr_in dst = {
		.sin_family = AF_INET,
		.sin_port   = htons((uint16_t)up->udp_in_port),
	};
	dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	uint16_t seq = ++g_wcmd_seq;
	int32_t  gs_unix_s = (int32_t)time(NULL);
	WcmdReq req = {
		.magic    = htonl(WCMD_MAGIC),
		.version  = WCMD_VERSION,
		.msg_type = WCMD_MSG_REQ,
		.seq      = htons(seq),
		.key      = WCMD_KEY_LOG_SYNC,
		.flags    = 0,
		._pad     = 0,
		.value    = (int32_t)htonl((uint32_t)gs_unix_s),
	};
	int sent = 0;
	for (int i = 0; i < WCMD_BURST_FRAMES; i++) {
		if (sendto(s, &req, sizeof(req), 0,
		           (struct sockaddr *)&dst, sizeof(dst)) == sizeof(req))
			sent++;
	}
	close(s);
	if (sent > 0) g_logsync_emit_total++;
}


/* ---------- main loop ------------------------------------------------ */

static void shutdown_all(Config *c)
{
	/* Shutdown teardown is byte-equivalent to a /system/down stop —
	 * SIGTERM, reap, SIGKILL, pid=-1. Share the helper so future
	 * tweaks (e.g. a longer grace, a stats-listener handoff) only
	 * have to happen in one place. */
	supervisor_stop_all_tunnels(c);
}

static void usage(const char *argv0)
{
	fprintf(stderr,
	    "usage: %s [--config PATH] [--port N] [-v]\n"
	    "  -c, --config PATH   JSON config (default %s)\n"
	    "  -p, --port N        override http.port from config\n"
	    "  -v, --verbose       inherit wfb_rx/wfb_tx stdout/stderr (default: /dev/null)\n",
	    argv0, GS_DEFAULT_CONFIG);
}

/* In the multi-call "mega binary" (see docs/design/mega-binary.md) the
 * dispatcher owns main() and invokes this as the "supervisor" applet.
 * Default builds (WFB_MULTICALL unset) keep main() unchanged. */
#ifdef WFB_MULTICALL
int gs_supervisor_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
	const char *cfg_path = GS_DEFAULT_CONFIG;
	int port_override = -1;

	static const struct option longopts[] = {
		{ "config",  required_argument, 0, 'c' },
		{ "port",    required_argument, 0, 'p' },
		{ "verbose", no_argument,       0, 'v' },
		{ "help",    no_argument,       0, 'h' },
		{ 0, 0, 0, 0 },
	};
	for (int o; (o = getopt_long(argc, argv, "c:p:vh", longopts, NULL)) != -1;) {
		switch (o) {
		case 'c': cfg_path = optarg; break;
		case 'p': port_override = atoi(optarg); break;
		case 'v': g_verbose = 1; break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 2;
		}
	}

	Config cfg;
	if (cfg_load(cfg_path, &cfg) < 0) return 1;
	{
		const char *wfb_link = getenv("WFB_LINK_CONF");
		cfg_apply_wfb_link_overlay(&cfg, (wfb_link && *wfb_link) ? wfb_link : "/etc/wfb-link.json");
	}
	if (port_override > 0 && port_override < 65536) cfg.http_port = (uint16_t)port_override;

	LOG_INFO("loaded config: %d tunnel(s), http=%s:%u",
	         cfg.tunnel_count, cfg.http_bind, cfg.http_port);
	for (int i = 0; i < cfg.tunnel_count; i++) {
		Tunnel *t = &cfg.tunnels[i];
		LOG_INFO("  [%d] %s role=%s link=%d port=%d ifaces=%d autostart=%d",
		         i, t->name, t->role, t->link_id, t->radio_port,
		         t->iface_count, (int)t->autostart);
	}

	struct sigaction sa = { .sa_handler = on_signal };
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigaction(SIGCHLD, &sa, NULL);
	sa.sa_flags = 0;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT,  &sa, NULL);
	sigaction(SIGHUP,  &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	if (cfg.system_up_count == 0 && cfg.system_down_count == 0) {
		LOG_INFO("system.up/down empty — trusting host OS for iface "
		         "bring-up (monitor mode, MTU, txpower) and skipping "
		         "managed link-layer setup");
	}

	/* Bring up adapters BUT do not exit on failure. If readiness times
	 * out, supervisor_bring_up rolls back via system.down and sets
	 * c->system_state = SYS_UP_FAILED; HTTP still starts so the
	 * operator can fix the bring-up sequence and retry via
	 * /api/v1/system/up from the WebUI. Tunnels do not autostart in
	 * that case (would just thrash against not-monitor ifaces). */
	bool boot_up_ok = (supervisor_bring_up(&cfg) == 0);

	int api_fd = api_listen_open(cfg.http_bind, cfg.http_port);
	if (api_fd < 0) {
		LOG_ERR("api listen %s:%u: %s", cfg.http_bind, cfg.http_port, strerror(errno));
		return 1;
	}
	LOG_INFO("REST API listening on http://%s:%u", cfg.http_bind, cfg.http_port);

	/* Telemetry logger (Phase 5): the in-process udp:6700 -> sqlite capture
	 * that replaced the Python ingester. No-op when telemetry.enabled is
	 * false. The rx tunnel's stats_tap must point at telemetry.listen. */
	if (wfb_logger_start(&cfg.telemetry) < 0)
		LOG_WARN("telemetry logger failed to start — continuing without it");

	ApiClient clients[API_MAX_CLIENTS];
	for (int i = 0; i < API_MAX_CLIENTS; i++) clients[i].fd = -1;

	uint64_t startup_us = now_us();

	if (boot_up_ok) {
		for (int i = 0; i < cfg.tunnel_count; i++) {
			if (cfg.tunnels[i].autostart)
				tunnel_spawn(&cfg.tunnels[i], cfg.key_file);
		}
	} else {
		LOG_WARN("tunnels NOT autostarted because boot bring-up failed "
		         "— issue /api/v1/system/up from the WebUI once the "
		         "underlying problem is fixed");
	}

	while (!g_shutdown) {
		/* pfd_slot encodes:
		 *   -1  : api listen socket (slot 0)
		 *   0..API_MAX_CLIENTS-1 : api client at clients[idx]
		 *   -1000-i : rx tunnel stats listener for tunnel i  */
		#define GS_SLOT_LISTEN  (-1)
		#define GS_SLOT_STATS_BASE (-1000)
		struct pollfd pfds[1 + API_MAX_CLIENTS + GS_MAX_TUNNELS];
		int           pfd_slot[1 + API_MAX_CLIENTS + GS_MAX_TUNNELS];
		int nfds = 0;
		pfds[nfds].fd = api_fd; pfds[nfds].events = POLLIN;
		pfd_slot[nfds] = GS_SLOT_LISTEN;
		nfds++;
		for (int i = 0; i < API_MAX_CLIENTS; i++) {
			if (clients[i].fd >= 0) {
				pfds[nfds].fd = clients[i].fd;
				pfds[nfds].events = POLLIN;
				pfd_slot[nfds] = i;
				nfds++;
			}
		}
		for (int i = 0; i < cfg.tunnel_count; i++) {
			if (cfg.tunnels[i].stats_local_fd >= 0) {
				pfds[nfds].fd = cfg.tunnels[i].stats_local_fd;
				pfds[nfds].events = POLLIN;
				pfd_slot[nfds] = GS_SLOT_STATS_BASE - i;
				nfds++;
			}
		}

		/* Default 250 ms tick for the supervisor. Tighten when a CSA
		 * burst/arm is active so frame cadence and the iw set channel
		 * fire with sub-10 ms accuracy against the vehicle's csa_tick. */
		int poll_to_ms = 250;
		if (g_csa.phase == CSA_BURST || g_csa.phase == CSA_ARMED) {
			uint64_t now = now_us();
			uint64_t next = (g_csa.phase == CSA_BURST &&
			                 g_csa.frames_sent < g_csa.frames_total)
			    ? g_csa.next_frame_us : g_csa.t_switch_us;
			int wait = (next > now)
			    ? (int)((next - now + 999ULL) / 1000ULL) : 1;
			if (wait < 5) wait = 5;
			if (wait < poll_to_ms) poll_to_ms = wait;
		} else if (g_csa.phase == CSA_VERIFY) {
			poll_to_ms = 50; /* tight enough for revert deadline */
		}
		if (g_scan.phase == SCAN_RUNNING) {
			uint64_t now = now_us();
			uint64_t next = g_scan.step_started_us + g_scan.step_dwell_us;
			int wait = (next > now) ? (int)((next - now + 999ULL) / 1000ULL) : 1;
			if (wait < 25) wait = 25;
			if (wait < poll_to_ms) poll_to_ms = wait;
		}
		int r = poll(pfds, nfds, poll_to_ms);
		/* Capture poll's errno before subsequent syscalls clobber it
		 * (waitpid in supervisor_reap leaves ECHILD on success). */
		int poll_err = (r < 0) ? errno : 0;

		if (g_sigchld) { g_sigchld = 0; supervisor_reap(&cfg); }
		supervisor_tick(&cfg);

		/* Reap idle clients that opened a TCP connection but never sent
		 * a complete header.  Without this, every API_MAX_CLIENTS slot
		 * could be squatted indefinitely by `nc 192.168.x.x 9080` and
		 * accept() would silently drop new connections. */
		{
			uint64_t now_u = now_us();
			for (int i = 0; i < API_MAX_CLIENTS; i++) {
				if (clients[i].fd < 0) continue;
				if (now_u - clients[i].accepted_us > API_CLIENT_IDLE_US) {
					LOG_DEBUG("api client slot %d idle, closing", i);
					close(clients[i].fd);
					clients[i].fd = -1;
				}
			}
		}

		if (r < 0) {
			if (poll_err == EINTR) continue;
			LOG_ERR("poll: %s", strerror(poll_err));
			break;
		}
		if (r == 0) continue;

		if (pfds[0].revents & POLLIN) {
			struct sockaddr_in peer; socklen_t plen = sizeof(peer);
			int cfd = accept(api_fd, (struct sockaddr *)&peer, &plen);
			if (cfd >= 0) {
				int slot = -1;
				for (int i = 0; i < API_MAX_CLIENTS; i++)
					if (clients[i].fd < 0) { slot = i; break; }
				if (slot < 0) {
					close(cfd);
				} else {
					int fl = fcntl(cfd, F_GETFL, 0);
					if (fl >= 0) fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
					clients[slot].fd = cfd;
					clients[slot].pos = 0;
					clients[slot].accepted_us = now_us();
				}
			}
		}
		/* Iterate over the pfds we actually polled, not over slot indices.
		 * A client just accepted in this iteration is NOT in pfds; reading
		 * pfds[idx] for it was an out-of-bounds garbage fetch that could
		 * trigger a spurious recv() returning EAGAIN, then close the fresh
		 * socket — symptom: "Empty reply from server". */
		for (int p = 1; p < nfds; p++) {
			int slot = pfd_slot[p];
			short re = pfds[p].revents;
			if (!(re & (POLLIN | POLLERR | POLLHUP))) continue;

			if (slot <= GS_SLOT_STATS_BASE) {
				int ti = GS_SLOT_STATS_BASE - slot;
				if (ti >= 0 && ti < cfg.tunnel_count)
					stats_drain(&cfg.tunnels[ti]);
				continue;
			}

			int i = slot;
			if (i < 0 || clients[i].fd < 0) continue;
			ssize_t got = recv(clients[i].fd,
			                   clients[i].buf + clients[i].pos,
			                   sizeof(clients[i].buf) - clients[i].pos - 1, 0);
			if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
			if (got <= 0) {
				close(clients[i].fd); clients[i].fd = -1; continue;
			}
			clients[i].pos += (size_t)got;
			clients[i].buf[clients[i].pos] = 0;
			if (strstr(clients[i].buf, "\r\n\r\n") || strstr(clients[i].buf, "\n\n")) {
				api_handle(&clients[i], &cfg, startup_us);
				close(clients[i].fd); clients[i].fd = -1;
			} else if (clients[i].pos >= sizeof(clients[i].buf) - 1) {
				api_send(clients[i].fd, 400, "text/plain", "request too long\n", -1);
				close(clients[i].fd); clients[i].fd = -1;
			}
		}
		#undef GS_SLOT_LISTEN
		#undef GS_SLOT_STATS_BASE
	}

	LOG_INFO("shutdown signal received");
	wfb_logger_stop();   /* clean flag+join: final commit + ended_at */
	for (int i = 0; i < API_MAX_CLIENTS; i++)
		if (clients[i].fd >= 0) close(clients[i].fd);
	close(api_fd);
	shutdown_all(&cfg);
	for (int i = 0; i < cfg.tunnel_count; i++)
		stats_listener_close(&cfg.tunnels[i]);
	/* Only run system.down on exit if we currently consider the system
	 * up — otherwise it was either already rolled back (SYS_UP_FAILED)
	 * or never brought up by us (operator took it down manually). */
	if (cfg.system_state == SYS_UP) {
		run_system_block("system.down", cfg.system_down, cfg.system_down_count);
	}
	LOG_INFO("bye");
	return 0;
}
