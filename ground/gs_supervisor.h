/*
 * gs_supervisor.h — internal header shared by gs_supervisor.c,
 * gs_supervisor_csa.c, gs_supervisor_scan.c, and gs_supervisor_http.c.
 *
 * Not a public API. Layout, sizes, and field meanings are tied to the
 * ground supervisor's process-lifetime singletons; nothing outside the
 * ground/ build is allowed to depend on this.
 */
#ifndef GS_SUPERVISOR_H
#define GS_SUPERVISOR_H

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "wfb_logger.h"   /* WfbLogConfig (telemetry logger, Phase 5) */

/* ---------- compile-time limits (small, predictable) ----------------- */

#define GS_MAX_TUNNELS       8
#define GS_MAX_IFACES        4
#define GS_MAX_EXTRA_ARGS   16
#define GS_MAX_SYSTEM_CMDS  32
#define GS_NAME_MAX         32
#define GS_PATH_MAX        256
#define GS_ARG_MAX         128
#define GS_ARGV_MAX         64
#define GS_ARGV_BUF       1536

#define API_MAX_CLIENTS      8
#define API_BUF_BYTES     2048
/* Slow-client deadline: a TCP peer that opens a connection but never
 * sends a complete header (i.e. no `\r\n\r\n` reached) gets closed after
 * this idle window.  Without it, eight half-open `nc` connections lock
 * out every other operator until the supervisor restarts. */
#define API_CLIENT_IDLE_US  (5ULL * 1000000ULL)

#define GS_DEFAULT_HTTP_PORT 9080
/* Must match the deployed path in ground/init/S46gs_supervisor and
 * ground/init/README.md (the init script passes this explicitly; this default
 * is what a bare `wfb-gs supervisor` falls back to). */
#define GS_DEFAULT_CONFIG    "/etc/gs_supervisor.json"

/* respawn backoff schedule (ms). */
extern const int GS_BACKOFF_MS[7];
#define GS_BACKOFF_LEN 7

/* SIGTERM-then-SIGKILL grace period when stopping a child. */
#define GS_STOP_GRACE_MS  1500

/* iface state cache */
#define GS_MAX_GLOBAL_IFACES (GS_MAX_TUNNELS * GS_MAX_IFACES)
#define GS_IFACE_REFRESH_US  2000000ULL

/* CSA / scan limits */
#define GS_CSA_MAX_IFACES 4
#define SCAN_MAX_STEPS    32

/* Default per-call deadline for the iw / ip-link forks driven by api
 * handlers. */
#define GS_FORK_DEADLINE_MS    1000
#define GS_SYSTEM_CMD_DEADLINE_MS 10000
#define GS_SYSTEM_CMD_MAX_ARGV    32

/* WCMD on-the-wire constants — kept here (not in shared/) because they're
 * scoped to the GS supervisor; the vehicle side parses these via
 * shared/wcmd_proto.h. */
#define WCMD_MAGIC          0x57434D44u   /* "WCMD" big-endian */
#define WCMD_VERSION        1
#define WCMD_MSG_REQ        1
#define WCMD_KEY_BITRATE_KBPS    1
#define WCMD_KEY_FPS             2
#define WCMD_KEY_PAYLOAD_BYTES   3
#define WCMD_KEY_FORCE_IDR       4
#define WCMD_KEY_WFB_FEC_K       5
#define WCMD_KEY_WFB_FEC_N       6
#define WCMD_KEY_WFB_MCS         7
#define WCMD_KEY_WFB_BANDWIDTH   8
#define WCMD_KEY_WFB_LDPC        9
#define WCMD_KEY_WFB_STBC       10
#define WCMD_KEY_WFB_SHORT_GI   11
#define WCMD_KEY_FEC_ENABLED    12
#define WCMD_KEY_MCS_ENABLED    13
#define WCMD_KEY_WFB_TXPOWER    14
#define WCMD_KEY_RECORD         15
#define WCMD_KEY_PEEK_ENABLED      16
/* 17 reserved (was WCMD_KEY_PEEK_DROP_ENABLED) — do not reuse */
#define WCMD_KEY_MAX            16   /* highest OPERATOR key (/api/v1/cmd) */
/* Logging-sync marker (infra, not an operator command — see
 * shared/wcmd_proto.h).  Emitted by logsync_emit() on its own ~10 s timer,
 * NOT through the operator /api/v1/cmd path, so it lives above WCMD_KEY_MAX
 * and never indexes the per-operator-key rate-limit array. */
#define WCMD_KEY_LOG_SYNC       18
/* SD-logger control (infra, like LOG_SYNC — see shared/wcmd_proto.h). Emitted
 * via /api/v1/logctl, NOT the operator /api/v1/cmd path, so it lives above
 * WCMD_KEY_MAX and never indexes the per-operator-key rate-limit array.
 * value: 1=start/roll a fresh vehicle SD log session, 0=stop. */
#define WCMD_KEY_LOG_CONTROL    19
#define WCMD_BURST_FRAMES        3

/* ---------- log helpers --------------------------------------------- */

extern volatile sig_atomic_t g_shutdown;
extern volatile sig_atomic_t g_sigchld;
extern int                   g_verbose;

void logf_(const char *level, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
uint64_t now_us(void);
uint64_t now_ms(void);
int      write_all(int fd, const void *buf, size_t len);

#define LOG_INFO(...)  logf_("info",  __VA_ARGS__)
#define LOG_WARN(...)  logf_("warn",  __VA_ARGS__)
#define LOG_ERR(...)   logf_("err",   __VA_ARGS__)
#define LOG_DEBUG(...) do { if (g_verbose) logf_("debug", __VA_ARGS__); } while (0)

/* ---------- minimal JSON parser (jsmn-shaped) ----------------------- *
 * Single source of truth in shared/wfb_json.h (header-only, static inline) —
 * provides JTokType / JTok / JSON_MAX_TOKS + json_parse/jeq/jskip/jfind/jstr/
 * jint/jbool, the same parser the multicall config-env applet uses. */
#include "wfb_json.h"

/* ---------- config + tunnel model ----------------------------------- */

typedef enum {
	TS_STOPPED = 0,
	TS_STARTING,
	TS_RUNNING,
	TS_BACKOFF,
	TS_FAILED,
} TunnelState;

typedef struct {
	char        name[GS_NAME_MAX];
	char        role[4];
	char        binary[GS_PATH_MAX];
	int         link_id;
	int         radio_port;

	int         iface_count;
	char        ifaces[GS_MAX_IFACES][IFNAMSIZ];

	/* rx-only */
	char        udp_out_ip[GS_ARG_MAX];
	int         udp_out_port;
	char        stats_out[GS_ARG_MAX];
	/* Optional fire-and-forget logging tap: a raw rx_ant copy is sent here
	 * (e.g. the SQLite telemetry ingester) IN ADDITION to stats_out. Never
	 * inline in the back-channel — stats_out is forwarded first and a
	 * dead/stalled tap consumer can never affect the uplink feed or the
	 * vehicle link. See DATASTORE.md. */
	char        stats_tap[GS_ARG_MAX];
	/* rx-only: boundary-probe PER producer ("probe": true).
	 * The tunnel's raw rx_ant is NOT re-emitted to stats_out (it would
	 * pollute the vehicle's video scorer); instead stats_drain()
	 * computes windowed per-received-MCS PER and sends compact
	 * {"type":"probe",...} records there. See PROBE_PER_SPEC.md. */
	bool        probe;
	int         probe_window_ms;           /* PER window (default 500) */

	/* tx-only */
	int         udp_in_port;
	int         control_port;
	int         fec_k, fec_n;
	int         bandwidth_mhz;
	int         mcs_index;
	int         stbc;
	int         ldpc;

	int         extra_arg_count;
	char        extra_args[GS_MAX_EXTRA_ARGS][GS_ARG_MAX];

	/* "keyless": true -> don't pass -K to this tunnel, even when a global
	 * key_file is set. For receiving an open (-xx) video downlink with no key
	 * (WFB_PACKET_SESSION_PLAIN); the global key still serves uplink/probe. */
	bool        keyless;

	bool        autostart;

	/* runtime state (mutable) */
	TunnelState state;
	pid_t       pid;
	uint64_t    started_us;
	uint64_t    exited_us;
	int         exit_code;
	int         restart_count;
	int         backoff_idx;
	uint64_t    next_start_ms;
	bool        autostart_on_exit;
	uint64_t    stop_deadline_ms;

	/* rx-only stats listener */
	int                 stats_local_fd;
	uint16_t            stats_local_port;
	struct sockaddr_in  stats_fwd_addr;
	int                 stats_fwd_active;
	struct sockaddr_in  stats_tap_addr;
	int                 stats_tap_active;
	char                stats_local_arg[GS_ARG_MAX];

	/* probe PER accumulator (rx + probe only). One bucket per received
	 * MCS so a window straddling a vehicle-side retune emits one clean
	 * record per MCS instead of mislabelling the whole window. */
	uint64_t    probe_win_start_us;
	struct {
		uint32_t uniq, lost;
		int      rssi;                 /* best avg dBm; INT_MIN = unset */
	} probe_bucket[16];
	uint32_t    probe_emit_count;
	uint32_t    probe_drop_count;          /* stale windows discarded */

	/* parsed rx_ant / tx_stats snapshot */
	uint64_t    st_first_us;
	uint64_t    st_last_us;
	uint32_t    st_interval_ms;
	uint32_t    st_msg_count;

	uint32_t    st_pkt_all;
	uint32_t    st_pkt_lost;
	uint32_t    st_pkt_fec;
	uint32_t    st_pkt_outgoing;
	uint32_t    st_pkt_dec_err;
	uint32_t    st_pkt_bytes;
	uint32_t    st_pkt_uniq;
	int         st_ant_count;
	int         st_rssi_best;
	/* Received-MCS histogram: per-ant `pkts` summed by the entry's
	 * received MCS index. Surfaces adaptive-MCS changes on the
	 * Tunnels tab. Diversity counts each adapter's copy, so this is
	 * per-antenna receptions, not deduped packets — the rung
	 * *distribution* is what matters.
	 *
	 * Kept as a 10×1 s sliding-window ring: slot i accumulates the second
	 * stamped in st_mcs_win_sec[i]; the API sums slots <10 s old to report
	 * the last-10 s distribution. Windowed (not cumulative) so the bars
	 * track what the link is doing now, not since boot. */
	uint32_t    st_mcs_win[10][16];
	uint64_t    st_mcs_win_sec[10];

	uint32_t    st_tx_pkts_in;
	uint32_t    st_tx_pkts_out;
	uint32_t    st_tx_bytes_in;
	uint32_t    st_tx_bytes_out;
	uint32_t    st_tx_drop;
	uint32_t    st_tx_trunc;
	uint32_t    st_tx_fec_timeouts;

	int         short_gi;
	int         vht_mode;
	int         vht_nss;
	int         fec_timeout_ms;
	int         radio_cache_have;
	int         fec_cache_have;
	uint64_t    radio_cache_us;
	uint64_t    tx_init_query_after_us;
} Tunnel;

/* Tri-state for the system.up/system.down lifecycle.
 *   SYS_DOWN      — adapters are released back to the host OS, no
 *                   tunnels are spawned. Either never brought up yet,
 *                   or explicitly taken down via /api/v1/system/down.
 *   SYS_UP        — system.up succeeded and iface readiness passed.
 *                   Tunnels are eligible for autostart / running.
 *   SYS_UP_FAILED — system.up was run but iface readiness timed out;
 *                   system.down was issued to roll back. Effectively
 *                   the same as SYS_DOWN for operations, but kept
 *                   distinct so the WebUI can warn "last bring-up
 *                   failed — fix the config or retry". */
typedef enum {
	SYS_DOWN = 0,
	SYS_UP,
	SYS_UP_FAILED,
} SystemState;

typedef struct {
	char     key_file[GS_PATH_MAX];
	char     http_bind[64];
	uint16_t http_port;

	int      system_up_count;
	char     system_up[GS_MAX_SYSTEM_CMDS][GS_PATH_MAX];
	int      system_down_count;
	char     system_down[GS_MAX_SYSTEM_CMDS][GS_PATH_MAX];

	int      tunnel_count;
	Tunnel   tunnels[GS_MAX_TUNNELS];

	bool     venc_cmd_enabled;
	char     venc_cmd_uplink[GS_NAME_MAX];
	int      venc_cmd_rate_limit_ms;

	WfbLogConfig telemetry;   /* in-process udp->sqlite logger (Phase 5) */

	SystemState system_state;
} Config;

const char *system_state_name(SystemState s);

/* Stop every tunnel and wait up to (GS_STOP_GRACE_MS + 500 ms) for the
 * children to exit, then SIGKILL any survivors. Shared by /system/down
 * and /system/reinit; both want a clean slate before touching the OS
 * adapter state. */
void supervisor_stop_all_tunnels(Config *c);

/* Stateless wrappers around the boot-time bring-up logic so the same
 * sequence runs from main() and from /api/v1/system/up. Each returns
 * 0 on success, -1 on failure (system.up command non-zero is logged
 * as a warning but not fatal; iface readiness timeout IS fatal).
 * supervisor_bring_up runs system.up + readiness + iface_state_init
 * and updates c->system_state. supervisor_take_down stops tunnels,
 * runs system.down, and updates c->system_state. */
int  supervisor_bring_up(Config *c);
int  supervisor_take_down(Config *c);

/* ---------- iface radio state cache --------------------------------- */

typedef struct {
	char     name[IFNAMSIZ];
	int      chan;
	int      freq_mhz;
	char     ht[8];
	int      txpower_mbm;
	uint64_t last_query_us;
	int      last_rc;
} IfaceState;

extern IfaceState g_iface_state[GS_MAX_GLOBAL_IFACES];
extern int        g_iface_state_count;

IfaceState *iface_state_find(const char *name);
IfaceState *iface_state_intern(const char *name);
int         iface_state_query(IfaceState *st);
void        iface_state_init(const Config *c);
void        iface_state_refresh_one(uint64_t now_us_arg);

/* ---------- argv composition ---------------------------------------- */

typedef struct {
	char  buf[GS_ARGV_BUF];
	size_t pos;
	char *argv[GS_ARGV_MAX];
	int   argc;
} ArgvBuilder;

int ab_push(ArgvBuilder *ab, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
int build_argv_rx(const Tunnel *t, const char *key_file, ArgvBuilder *ab);
int build_argv_tx(const Tunnel *t, const char *key_file, ArgvBuilder *ab);

/* ---------- tunnel lifecycle ---------------------------------------- */

const char *tunnel_state_name(TunnelState s);
int  cfg_parse_tunnel(const char *js, JTok *toks, int n, int t_idx, Tunnel *t);
int  cfg_load(const char *path, Config *c);
/* Sparse overlay of /etc/wfb-link.json onto a loaded Config (Phase 3b). Only
 * fields present in the overlay override; absent/missing-file is a no-op. */
void cfg_apply_wfb_link_overlay(Config *c, const char *path);
Tunnel *cfg_find_tunnel(Config *c, const char *name);

int  tunnel_spawn(Tunnel *t, const char *key_file);
void tunnel_request_stop(Tunnel *t);
void tunnel_on_exit(Tunnel *t, int wstatus);
void supervisor_reap(Config *c);

int  stats_listener_open(Tunnel *t);
void stats_listener_close(Tunnel *t);
void stats_drain(Tunnel *t);

/* ---------- iface validation + capture ------------------------------ */

int  parse_host_port(const char *s, struct sockaddr_in *out);
int  run_capture(char *const argv[], char *out, size_t cap);
int  iface_is_monitor(const char *iface);
int  iface_is_admin_up(const char *iface);
int  wait_iface_state(const Config *c, int timeout_ms, int interval_ms);

/* ---------- bounded fork helpers ------------------------------------ */

int waitpid_deadline(pid_t pid, int deadline_ms);
int run_iw_set_channel(const char *iface, int chan, const char *ht);

/* ---------- system commands ----------------------------------------- */

int  tokenize_argv(char *cmd, char **out_argv, int max_argv);
int  run_system_cmd(const char *cmd_const);
void run_system_block(const char *label, char cmds[][GS_PATH_MAX], int n);

/* ---------- wfb_cmd passthrough ------------------------------------- */

#include "wfb_control.h"

typedef struct {
	uint32_t req_id;
	uint8_t  cmd_id;
	uint8_t  k;
	uint8_t  n;
	uint16_t fec_timeout_ms;
} __attribute__((packed)) WfbCmdSetFecReq;

typedef struct {
	uint32_t req_id;
	uint8_t  cmd_id;
	uint8_t  stbc;
	uint8_t  ldpc;
	uint8_t  short_gi;
	uint8_t  bandwidth;
	uint8_t  mcs_index;
	uint8_t  vht_mode;
	uint8_t  vht_nss;
} __attribute__((packed)) WfbCmdSetRadioReq;

typedef struct {
	uint32_t req_id;
	uint8_t  cmd_id;
} __attribute__((packed)) WfbCmdGetReq;

typedef struct {
	uint32_t req_id;
	uint32_t rc;
	uint8_t  k;
	uint8_t  n;
	uint16_t fec_timeout_ms;
} __attribute__((packed)) WfbCmdGetFecResp;

typedef struct {
	uint32_t req_id;
	uint32_t rc;
	uint8_t  stbc;
	uint8_t  ldpc;
	uint8_t  short_gi;
	uint8_t  bandwidth;
	uint8_t  mcs_index;
	uint8_t  vht_mode;
	uint8_t  vht_nss;
} __attribute__((packed)) WfbCmdGetRadioResp;

uint32_t wfb_cmd_next_req_id(void);
int wfb_cmd_round_trip(int control_port,
                       const void *req, size_t req_len,
                       void *resp_buf, size_t resp_cap,
                       uint32_t *out_rc);
int wfb_cmd_refresh_radio(Tunnel *t);
int wfb_cmd_refresh_fec(Tunnel *t);

/* ---------- query string + WCMD ------------------------------------- */

const char *qs_get(const char *qs, const char *key, size_t *len_out);
int  qs_get_int(const char *qs, const char *key, int *out);

#pragma pack(push, 1)
typedef struct {
	uint32_t magic;
	uint8_t  version;
	uint8_t  msg_type;
	uint16_t seq;
	uint8_t  key;
	uint8_t  flags;
	uint16_t _pad;
	int32_t  value;
} WcmdReq;
#pragma pack(pop)

extern uint64_t g_wcmd_last_send_ms[WCMD_KEY_MAX + 1];
extern uint16_t g_wcmd_seq;
extern uint64_t g_wcmd_emit_total;
extern uint64_t g_wcmd_emit_frames;
extern uint64_t g_wcmd_emit_rate_limit;
extern uint64_t g_wcmd_emit_failed;

int         wcmd_key_from_str(const char *s, size_t n);
const char *wcmd_key_name(int key);
int         wcmd_emit(const Config *c, int key, int32_t value, uint16_t *seq_out);

/* ---------- CSA orchestrator ---------------------------------------- */

typedef enum {
	CSA_IDLE = 0,
	CSA_BURST,
	CSA_ARMED,
	CSA_VERIFY,
} CsaPhase;

typedef struct {
	CsaPhase phase;
	uint32_t sess;
	uint64_t t_switch_us;
	uint64_t t_revert_us;
	uint64_t next_frame_us;
	int      frames_sent;
	int      frames_total;
	int      target_chan;
	char     target_ht[8];
	int      prev_chan;
	char     prev_ht[8];
	char     ifaces[GS_CSA_MAX_IFACES][IFNAMSIZ];
	int      iface_count;
	uint64_t baseline_pkt_us;
	uint32_t baseline_pkt_all;
	int      baseline_rx_idx;
	bool     no_revert;
} CsaState;

extern CsaState g_csa;
extern uint32_t g_csa_seq_in_burst;
extern int      g_csa_send_fd;

int  csa_send_commit_frame(const Config *c);
int  csa_pick_rx_tunnel_idx(const Config *c);
void csa_tick(Config *c, uint64_t t_us);

/* ---------- Scanner -------------------------------------------------- */

typedef enum {
	SCAN_IDLE = 0,
	SCAN_RUNNING,
	SCAN_FOUND,
	SCAN_STOPPED,
} ScanPhase;

typedef struct {
	ScanPhase phase;
	uint32_t  sess;
	char      ifaces[GS_CSA_MAX_IFACES][IFNAMSIZ];
	int       iface_count;
	int       chans[SCAN_MAX_STEPS];
	char      hts[SCAN_MAX_STEPS][8];
	int       step_count;
	int       cur_step;
	uint64_t  step_started_us;
	uint64_t  step_dwell_us;
	int       baseline_rx_idx;
	bool      step_saw_traffic;
	int       found_chan;
	char      found_ht[8];
	int       hops_done;
} ScanState;

extern ScanState g_scan;

void scan_apply_step_drained(Config *c, int i);
void scan_tick(Config *c, uint64_t t_us);

/* ---------- HTTP API ------------------------------------------------- */

typedef struct {
	int      fd;
	uint64_t accepted_us;
	size_t   pos;
	char     buf[API_BUF_BYTES];
} ApiClient;

bool request_wants_html(const char *req);
extern const unsigned char gs_webui_html[];
extern const unsigned int  gs_webui_html_len;

int  api_listen_open(const char *bind_ip, uint16_t port);
void api_send(int fd, int code, const char *ctype, const char *body, int body_len);
void api_send_blob(int fd, const char *ctype,
                   const unsigned char *body, unsigned int len);
int  json_emit_tunnel(char *buf, size_t cap, const Tunnel *t, bool full);
int  json_emit_status(char *buf, size_t cap, const Config *c, uint64_t up_us);
void api_handle(ApiClient *cli, Config *c, uint64_t startup_us);

/* Telemetry dashboard + JSON API (gs_supervisor_telemetry.c, Phase 5).
 * Returns 1 if `path` was a telemetry route (response already sent), else 0. */
int  tele_route(ApiClient *cli, const char *path, const char *qstr);

#endif /* GS_SUPERVISOR_H */
