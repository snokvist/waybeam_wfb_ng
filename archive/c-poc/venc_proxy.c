/*
 * venc_proxy — ground-side waybeam_venc HTTP -> WCMD/UDP shim.
 *
 * Runs on the ground host alongside wfb_tx_native. Accepts the same
 * HTTP surface the vehicle's waybeam_venc exposes (a small whitelisted
 * subset, see below), translates each call into a WCMD binary frame,
 * and fires it at 127.0.0.1:<upstream-port> where wfb_tx_native picks
 * it up and forwards it across the radio uplink. On the vehicle the
 * link_controller demuxes WCMD off its rx_ant socket and replays the
 * request as a real HTTP GET against venc.
 *
 *   caller (curl / GCS) ─HTTP─▶ venc_proxy ─UDP─▶ wfb_tx_native ─radio─▶
 *      ─▶ wfb_rx ─UDP─▶ link_controller ─HTTP─▶ waybeam_venc
 *
 * The radio uplink is one-way at the wire level (see CMD_PROXY.md
 * "Topologies"), so no WCMD_RESP ever gets back to this proxy. Replies
 * are therefore synthetic: we mirror venc's success-shape JSON
 * (`{"ok":true}` / `{"idr":true}`) so existing GCS code that pokes the
 * waybeam_venc API keeps working unchanged.
 *
 * Whitelisted surface:
 *   GET /api/v1/set?video0.bitrate=<kbps>
 *   GET /api/v1/set?video0.fps=<fps>
 *   GET /api/v1/set?outgoing.maxPayloadSize=<bytes>
 *     (multiple keys may be combined in one query — one WCMD frame per key)
 *   GET /request/idr
 *   GET /health           (local liveness probe)
 *
 * Anything else returns 404. Validation, range clamps, and rate
 * limiting are enforced on the vehicle side by the WCMD proxy in
 * link_controller — this binary is intentionally a dumb translator.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "wcmd_proto.h"

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

static uint16_t g_seq = 1;

static int wcmd_send(int udp_fd, const struct sockaddr_in *dst,
                     uint8_t key, int32_t value)
{
	WcmdReq req;
	memset(&req, 0, sizeof(req));
	req.magic    = htonl(WCMD_MAGIC);
	req.version  = WCMD_VERSION;
	req.msg_type = WCMD_MSG_REQ;
	req.seq      = htons(g_seq++);
	req.key      = key;
	req.value    = (int32_t)htonl((uint32_t)value);
	ssize_t n = sendto(udp_fd, &req, sizeof(req), 0,
	                   (const struct sockaddr *)dst, sizeof(*dst));
	return (n == (ssize_t)sizeof(req)) ? 0 : -1;
}

static void urldecode(char *s)
{
	char *o = s;
	while (*s) {
		if (*s == '%' && isxdigit((unsigned char)s[1])
		    && isxdigit((unsigned char)s[2])) {
			char hex[3] = { s[1], s[2], 0 };
			*o++ = (char)strtol(hex, NULL, 16);
			s += 3;
		} else if (*s == '+') {
			*o++ = ' '; s++;
		} else {
			*o++ = *s++;
		}
	}
	*o = 0;
}

/* Read the request line + headers into buf until "\r\n\r\n" or buf is
 * full. Returns total bytes read, or -1 on error. The body (if any) is
 * ignored — we only consume GETs. */
static int read_http_head(int fd, char *buf, size_t buf_sz)
{
	size_t got = 0;
	while (got + 1 < buf_sz) {
		ssize_t n = recv(fd, buf + got, buf_sz - 1 - got, 0);
		if (n <= 0) return -1;
		got += (size_t)n;
		buf[got] = 0;
		if (strstr(buf, "\r\n\r\n")) return (int)got;
	}
	return -1;
}

static void send_http_response(int fd, int code, const char *status,
                               const char *body)
{
	char hdr[256];
	int n = snprintf(hdr, sizeof(hdr),
	    "HTTP/1.1 %d %s\r\n"
	    "Content-Type: application/json\r\n"
	    "Content-Length: %zu\r\n"
	    "Connection: close\r\n"
	    "Cache-Control: no-store\r\n"
	    "\r\n",
	    code, status, strlen(body));
	if (n <= 0) return;
	(void)!send(fd, hdr,  (size_t)n,         MSG_NOSIGNAL);
	(void)!send(fd, body, strlen(body),      MSG_NOSIGNAL);
}

/* Split "k1=v1&k2=v2" in-place. Pairs missing '=' are skipped. Returns
 * the number of pairs found. Values are URL-decoded; keys are not (venc
 * keys are dotted ASCII so '%' is not expected). */
static int parse_query(char *q, char *keys[], char *vals[], int max_pairs)
{
	int n = 0;
	while (q && *q && n < max_pairs) {
		char *amp = strchr(q, '&');
		if (amp) *amp = 0;
		char *eq = strchr(q, '=');
		if (eq) {
			*eq = 0;
			keys[n] = q;
			vals[n] = eq + 1;
			urldecode(vals[n]);
			n++;
		}
		if (!amp) break;
		q = amp + 1;
	}
	return n;
}

static int venc_key_to_wcmd(const char *vkey, uint8_t *wkey_out)
{
	if (strcmp(vkey, "video0.bitrate") == 0) {
		*wkey_out = WCMD_KEY_BITRATE_KBPS;  return 0;
	}
	if (strcmp(vkey, "video0.fps") == 0) {
		*wkey_out = WCMD_KEY_FPS;           return 0;
	}
	if (strcmp(vkey, "outgoing.maxPayloadSize") == 0) {
		*wkey_out = WCMD_KEY_PAYLOAD_BYTES; return 0;
	}
	return -1;
}

/* Parse request line "METHOD <path> HTTP/x.y" out of the head buffer.
 * Splits path at '?' so *query is either NULL or a NUL-terminated query
 * string. Returns 0 on success. */
static int parse_request_line(char *buf, char **method, char **path,
                              char **query)
{
	char *eol = strstr(buf, "\r\n");
	if (!eol) return -1;
	*eol = 0;

	char *sp1 = strchr(buf, ' ');
	if (!sp1) return -1;
	*sp1 = 0;
	char *sp2 = strchr(sp1 + 1, ' ');
	if (!sp2) return -1;
	*sp2 = 0;

	*method = buf;
	*path   = sp1 + 1;
	*query  = NULL;
	char *q = strchr(*path, '?');
	if (q) { *q = 0; *query = q + 1; }
	return 0;
}

struct stats {
	uint64_t requests;
	uint64_t wcmd_sent;
	uint64_t errors_4xx;
};

static void handle_conn(int conn, int udp_fd, const struct sockaddr_in *dst,
                        bool dry_run, bool verbose, struct stats *stats)
{
	char buf[4096];
	if (read_http_head(conn, buf, sizeof(buf)) < 0) {
		stats->errors_4xx++;
		send_http_response(conn, 400, "Bad Request", "{\"ok\":false}");
		return;
	}

	char *method = NULL, *path = NULL, *query = NULL;
	if (parse_request_line(buf, &method, &path, &query) != 0) {
		stats->errors_4xx++;
		send_http_response(conn, 400, "Bad Request", "{\"ok\":false}");
		return;
	}
	stats->requests++;

	if (strcmp(method, "GET") != 0) {
		stats->errors_4xx++;
		send_http_response(conn, 405, "Method Not Allowed",
		                   "{\"ok\":false}");
		return;
	}

	if (strcmp(path, "/health") == 0) {
		send_http_response(conn, 200, "OK", "ok\n");
		return;
	}

	if (strcmp(path, "/request/idr") == 0) {
		if (!dry_run) {
			if (wcmd_send(udp_fd, dst, WCMD_KEY_FORCE_IDR, 0) == 0)
				stats->wcmd_sent++;
		}
		if (verbose)
			fprintf(stderr, "venc_proxy: IDR\n");
		send_http_response(conn, 200, "OK", "{\"idr\":true}");
		return;
	}

	if (strcmp(path, "/api/v1/set") == 0) {
		if (!query || !*query) {
			stats->errors_4xx++;
			send_http_response(conn, 400, "Bad Request",
			                   "{\"ok\":false}");
			return;
		}
		char *keys[16], *vals[16];
		int npairs = parse_query(query, keys, vals, 16);
		int sent = 0, bad = 0;
		for (int i = 0; i < npairs; i++) {
			uint8_t wkey;
			if (venc_key_to_wcmd(keys[i], &wkey) != 0) { bad++; continue; }
			char *endp = NULL;
			long v = strtol(vals[i], &endp, 10);
			if (!endp || *endp != 0) { bad++; continue; }
			if (v < INT32_MIN || v > INT32_MAX) { bad++; continue; }
			if (!dry_run) {
				if (wcmd_send(udp_fd, dst, wkey, (int32_t)v) == 0)
					sent++;
			} else {
				sent++;
			}
			if (verbose)
				fprintf(stderr,
				        "venc_proxy: set %s=%ld -> wcmd key=%u\n",
				        keys[i], v, wkey);
		}
		stats->wcmd_sent += (uint64_t)sent;
		if (sent == 0) {
			stats->errors_4xx++;
			send_http_response(conn, 400, "Bad Request",
			                   "{\"ok\":false}");
			return;
		}
		send_http_response(conn, 200, "OK", "{\"ok\":true}");
		return;
	}

	stats->errors_4xx++;
	send_http_response(conn, 404, "Not Found", "{\"ok\":false}");
}

static void usage(const char *prog)
{
	fprintf(stderr,
"Usage: %s [--listen ADDR[:PORT]] [--upstream-port N] [--dry-run] [-v]\n"
"\n"
"Ground-side waybeam_venc HTTP -> WCMD/UDP proxy.\n"
"\n"
"Listens on TCP (default 0.0.0.0:80) for the venc HTTP surface and\n"
"translates each whitelisted call into a WCMD binary frame sent to\n"
"127.0.0.1:<upstream-port> (default 6600). Pair with wfb_tx_native\n"
"-u <upstream-port> on the ground host so the frames cross the radio\n"
"uplink and reach link_controller on the vehicle.\n"
"\n"
"Replies are synthetic — the radio uplink is one-way and never returns\n"
"a WCMD_RESP, so this proxy answers immediately with the venc-shape\n"
"JSON the GCS expects. Real validation/clamps/rate-limits live in the\n"
"on-vehicle WCMD proxy (see poc/CMD_PROXY.md).\n"
"\n"
"Options:\n"
"  --listen ADDR[:PORT]   bind address  (default 0.0.0.0:80)\n"
"  --upstream-port N      WCMD UDP dest (default 6600, host 127.0.0.1)\n"
"  --dry-run              parse & log but never sendto() the WCMD frame\n"
"  -v, --verbose          log each accepted request to stderr\n"
"  -h, --help             this help\n",
	prog);
}

int main(int argc, char **argv)
{
	const char *listen_addr = "0.0.0.0";
	int listen_port = 80;
	int upstream_port = 6600;
	bool dry_run = false;
	bool verbose = false;

	static const struct option opts[] = {
		{"listen",        required_argument, 0, 'l'},
		{"upstream-port", required_argument, 0, 'u'},
		{"dry-run",       no_argument,       0, 'n'},
		{"verbose",       no_argument,       0, 'v'},
		{"help",          no_argument,       0, 'h'},
		{0, 0, 0, 0}
	};

	char listen_buf[64] = {0};
	int c;
	while ((c = getopt_long(argc, argv, "l:u:nvh", opts, NULL)) != -1) {
		switch (c) {
		case 'l': {
			strncpy(listen_buf, optarg, sizeof(listen_buf) - 1);
			char *colon = strrchr(listen_buf, ':');
			if (colon) {
				*colon = 0;
				listen_addr = listen_buf;
				listen_port = atoi(colon + 1);
			} else {
				/* Bare number = port; keep default addr. */
				char *end = NULL;
				long p = strtol(listen_buf, &end, 10);
				if (end && *end == 0 && p > 0 && p <= 65535) {
					listen_port = (int)p;
				} else {
					listen_addr = listen_buf;
				}
			}
			break;
		}
		case 'u': upstream_port = atoi(optarg); break;
		case 'n': dry_run = true; break;
		case 'v': verbose = true; break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 2;
		}
	}

	if (listen_port <= 0 || listen_port > 65535) {
		fprintf(stderr, "venc_proxy: invalid listen port %d\n", listen_port);
		return 2;
	}
	if (upstream_port <= 0 || upstream_port > 65535) {
		fprintf(stderr, "venc_proxy: invalid upstream port %d\n",
		        upstream_port);
		return 2;
	}

	signal(SIGINT,  on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (udp_fd < 0) { perror("socket(udp)"); return 1; }
	struct sockaddr_in dst;
	memset(&dst, 0, sizeof(dst));
	dst.sin_family      = AF_INET;
	dst.sin_port        = htons((uint16_t)upstream_port);
	dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	int srv = socket(AF_INET, SOCK_STREAM, 0);
	if (srv < 0) { perror("socket(tcp)"); return 1; }
	int yes = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port   = htons((uint16_t)listen_port);
	if (inet_aton(listen_addr, &addr.sin_addr) == 0) {
		fprintf(stderr, "venc_proxy: bad listen address '%s'\n", listen_addr);
		return 2;
	}
	if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "venc_proxy: bind %s:%d: %s\n",
		        listen_addr, listen_port, strerror(errno));
		if (errno == EACCES && listen_port < 1024)
			fprintf(stderr,
			        "  (privileged port — run as root or "
			        "setcap 'cap_net_bind_service=+ep' venc_proxy)\n");
		return 1;
	}
	if (listen(srv, 16) < 0) { perror("listen"); return 1; }

	fprintf(stderr,
	        "venc_proxy: listening on %s:%d, WCMD -> 127.0.0.1:%d%s\n",
	        listen_addr, listen_port, upstream_port,
	        dry_run ? " (dry-run)" : "");

	struct stats stats = {0};
	while (!g_stop) {
		struct sockaddr_in peer;
		socklen_t plen = sizeof(peer);
		int conn = accept(srv, (struct sockaddr *)&peer, &plen);
		if (conn < 0) {
			if (errno == EINTR) continue;
			perror("accept");
			break;
		}
		struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
		setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(conn, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		handle_conn(conn, udp_fd, &dst, dry_run, verbose, &stats);
		close(conn);
	}

	if (verbose)
		fprintf(stderr,
		        "venc_proxy: shutting down (req=%llu wcmd=%llu 4xx=%llu)\n",
		        (unsigned long long)stats.requests,
		        (unsigned long long)stats.wcmd_sent,
		        (unsigned long long)stats.errors_4xx);

	close(srv);
	close(udp_fd);
	return 0;
}
