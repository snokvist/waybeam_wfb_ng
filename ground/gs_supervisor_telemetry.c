/*
 * gs_supervisor_telemetry.c — telemetry dashboard + JSON API (Phase 5).
 *
 * Serves the SQLite-backed telemetry UI that replaced the Flask app: the
 * embedded dashboard assets at /telemetry (and /telemetry/static/) and the JSON
 * API under /api/v1/telemetry/. Read endpoints open their own short-lived
 * SQLite connection over WAL (wfb_logger_open) so they never touch the capture
 * thread's write connection; the API is GET-only (mutations carry ?action=...),
 * matching the rest of the supervisor.
 *
 * Routes (this file owns everything tele_route() claims):
 *   GET /telemetry                          dashboard (session list) page
 *   GET /telemetry/session?id=N             session detail page
 *   GET /telemetry/static/<asset>           embedded js/css/uPlot
 *   GET /api/v1/telemetry/sessions          [READ] session list
 *   GET /api/v1/telemetry/session?id=N      [READ] one session row (full meta)
 *   GET /api/v1/telemetry/series?id=N       [READ] aligned uPlot arrays + mcs_dist
 *   GET /api/v1/telemetry/labels?id=N       [READ] labels (x-seconds)
 *   GET /api/v1/telemetry/capture           [READ] logger status
 *   (mutations: ?action=add|del|delete|roll + /meta — added in Phase 5c)
 */
#include "gs_supervisor.h"
#include "wfb_logger.h"
#include "sqlite3.h"

#include <stdarg.h>

/* ---- embedded dashboard assets (xxd -i via the Makefile `webui` target) ---- */
#if __has_include("telemetry_assets.h")
#  include "telemetry_assets.h"
#else
   static const unsigned char tele_index_html[] =
       "<!doctype html><h1>telemetry_assets.h missing — run `make webui` from ground/.</h1>";
   static const unsigned int  tele_index_html_len = sizeof(tele_index_html) - 1;
   static const unsigned char tele_session_html[] = "missing";
   static const unsigned int  tele_session_html_len = sizeof(tele_session_html) - 1;
   static const unsigned char tele_app_js[] = "";
   static const unsigned int  tele_app_js_len = 0;
   static const unsigned char tele_app_css[] = "";
   static const unsigned int  tele_app_css_len = 0;
   static const unsigned char tele_uplot_js[] = "";
   static const unsigned int  tele_uplot_js_len = 0;
   static const unsigned char tele_uplot_css[] = "";
   static const unsigned int  tele_uplot_css_len = 0;
#endif

/* ---- growable string buffer (large JSON responses) ------------------------ */
typedef struct { char *buf; size_t len, cap; int err; } SB;

static void sb_need(SB *s, size_t extra)
{
	if (s->err) return;
	if (s->len + extra + 1 <= s->cap) return;
	size_t nc = s->cap ? s->cap : 16384;
	while (nc < s->len + extra + 1) nc *= 2;
	char *nb = realloc(s->buf, nc);
	if (!nb) { s->err = 1; return; }
	s->buf = nb; s->cap = nc;
}
static void sb_putc(SB *s, char c) { sb_need(s, 1); if (!s->err) s->buf[s->len++] = c; }
static void sb_puts(SB *s, const char *str) { size_t n = strlen(str); sb_need(s, n); if (!s->err) { memcpy(s->buf + s->len, str, n); s->len += n; } }
static void sb_printf(SB *s, const char *fmt, ...)
{
	if (s->err) return;
	va_list ap, cp;
	va_start(ap, fmt); va_copy(cp, ap);
	int need = vsnprintf(NULL, 0, fmt, cp); va_end(cp);
	if (need < 0) { s->err = 1; va_end(ap); return; }
	sb_need(s, (size_t)need);
	if (!s->err) { vsnprintf(s->buf + s->len, s->cap - s->len, fmt, ap); s->len += (size_t)need; }
	va_end(ap);
}
/* JSON string literal with escaping; NULL -> null. */
static void sb_jstr(SB *s, const char *v)
{
	if (!v) { sb_puts(s, "null"); return; }
	sb_putc(s, '"');
	for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
		switch (*p) {
		case '"':  sb_puts(s, "\\\""); break;
		case '\\': sb_puts(s, "\\\\"); break;
		case '\n': sb_puts(s, "\\n"); break;
		case '\r': sb_puts(s, "\\r"); break;
		case '\t': sb_puts(s, "\\t"); break;
		default:
			if (*p < 0x20) sb_printf(s, "\\u%04x", *p);
			else           sb_putc(s, (char)*p);
		}
	}
	sb_putc(s, '"');
}
/* ---- query-param URL-decode (mutations carry label/notes text) ----------- */
static int hexv(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}
/* application/x-www-form-urlencoded decode of one value into out (NUL-term). */
static void url_decode(const char *s, size_t n, char *out, size_t cap)
{
	size_t o = 0;
	for (size_t i = 0; i < n && o + 1 < cap; i++) {
		char c = s[i];
		if (c == '+') { out[o++] = ' '; }
		else if (c == '%' && i + 2 < n) {
			int hi = hexv(s[i + 1]), lo = hexv(s[i + 2]);
			if (hi >= 0 && lo >= 0) { out[o++] = (char)(hi * 16 + lo); i += 2; }
			else out[o++] = c;
		} else out[o++] = c;
	}
	out[o] = 0;
}
/* Decode query param `key` into out; returns 1 if present, 0 if absent. */
static int qparam_dec(const char *qstr, const char *key, char *out, size_t cap)
{
	size_t vl = 0;
	const char *v = qs_get(qstr, key, &vl);
	if (!v) { if (cap) out[0] = 0; return 0; }
	url_decode(v, vl, out, cap);
	return 1;
}

/* column emitters (value or JSON null), reading by sqlite column type. */
static void sb_col_text(SB *s, sqlite3_stmt *st, int col)
{
	if (sqlite3_column_type(st, col) == SQLITE_NULL) sb_puts(s, "null");
	else sb_jstr(s, (const char *)sqlite3_column_text(st, col));
}
static void sb_col_int(SB *s, sqlite3_stmt *st, int col)
{
	if (sqlite3_column_type(st, col) == SQLITE_NULL) sb_puts(s, "null");
	else sb_printf(s, "%lld", (long long)sqlite3_column_int64(st, col));
}

static void sb_finish(SB *s, ApiClient *cli)
{
	if (s->err || !s->buf) api_send(cli->fd, 500, "text/plain", "telemetry: out of memory\n", -1);
	else api_send(cli->fd, 200, "application/json", s->buf, (int)s->len);
	free(s->buf);
}

/* ---- per-session record row (collected for the series emit) --------------- */
typedef struct {
	double ts_ms;
	double rssi_comb;   int h_rssi;
	double uplink_rssi; int h_up;
	double rssi_spread; int h_spread;
	double snr_avg;     int h_snr;
	double per;         int h_per;
	long   mcs;         int h_mcs;
	long   pkt_lost;    int h_lost;
	long   fec_rec;     int h_fec;
	int    mcs_pkts[8];  /* per-rung max pkts from ant[] */
} SRow;

/* max pkts per MCS rung across ant[] of one raw_json datagram (diversity
 * antennas repeat the same packet set -> MAX, not sum), mirroring webapp's
 * mcs_dist. */
static void mcs_dist_from_raw(const char *raw, int len, int out[8])
{
	for (int i = 0; i < 8; i++) out[i] = 0;
	if (!raw || len <= 0) return;
	JTok t[768];
	int n = json_parse(raw, len, t, 768);
	if (n < 1 || t[0].type != JT_OBJ) return;
	int ant = jfind(raw, t, n, 0, "ant");
	if (ant < 0 || t[ant].type != JT_ARR) return;
	int kids = t[ant].size, ci = ant + 1;
	for (int k = 0; k < kids; k++) {
		if (t[ci].type == JT_OBJ) {
			long m, p = 0;
			int mv = jfind(raw, t, n, ci, "mcs");
			int pv = jfind(raw, t, n, ci, "pkts");
			if (mv >= 0 && jint(raw, &t[mv], &m) == 0 && m >= 0 && m < 8) {
				if (pv >= 0) jint(raw, &t[pv], &p);
				if ((int)p > out[m]) out[m] = (int)p;
			}
		}
		ci = jskip(t, n, ci);
	}
}

/* ---- read endpoints ------------------------------------------------------- */
static void tele_sessions(ApiClient *cli)
{
	sqlite3 *db = NULL;
	if (wfb_logger_open(&db) != 0) { api_send(cli->fd, 200, "application/json", "[]", -1); return; }
	sqlite3_stmt *st = NULL;
	const char *sql =
		"SELECT s.id, s.source, s.started_at, s.ended_at, s.location, s.scenario,"
		" s.notes, COUNT(r.id), MIN(r.ts_ms), MAX(r.ts_ms)"
		" FROM sessions s LEFT JOIN records r ON r.session_id = s.id"
		" GROUP BY s.id ORDER BY s.id DESC";
	SB sb = {0};
	sb_putc(&sb, '[');
	if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
		int first = 1;
		while (sqlite3_step(st) == SQLITE_ROW) {
			if (!first) sb_putc(&sb, ',');
			first = 0;
			sb_printf(&sb, "{\"id\":%lld,\"source\":", (long long)sqlite3_column_int64(st, 0));
			sb_col_text(&sb, st, 1);
			sb_puts(&sb, ",\"started_at\":"); sb_col_text(&sb, st, 2);
			sb_puts(&sb, ",\"ended_at\":");   sb_col_text(&sb, st, 3);
			sb_puts(&sb, ",\"location\":");   sb_col_text(&sb, st, 4);
			sb_puts(&sb, ",\"scenario\":");   sb_col_text(&sb, st, 5);
			sb_puts(&sb, ",\"notes\":");      sb_col_text(&sb, st, 6);
			sb_puts(&sb, ",\"n_records\":");  sb_col_int(&sb, st, 7);
			sb_puts(&sb, ",\"first_ts\":");   sb_col_int(&sb, st, 8);
			sb_puts(&sb, ",\"last_ts\":");    sb_col_int(&sb, st, 9);
			sb_putc(&sb, '}');
		}
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
	sb_putc(&sb, ']');
	sb_finish(&sb, cli);
}

static void tele_session_one(ApiClient *cli, long sid)
{
	sqlite3 *db = NULL;
	if (wfb_logger_open(&db) != 0) { api_send(cli->fd, 404, "application/json", "{\"error\":\"no db\"}", -1); return; }
	sqlite3_stmt *st = NULL;
	int found = 0;
	SB sb = {0};
	if (sqlite3_prepare_v2(db,
	        "SELECT id, source, started_at, ended_at, location, antenna_cfg, tx_power,"
	        " channel, scenario, weather, notes FROM sessions WHERE id=?", -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(st, 1, sid);
		if (sqlite3_step(st) == SQLITE_ROW) {
			found = 1;
			sb_printf(&sb, "{\"id\":%lld,\"source\":", (long long)sqlite3_column_int64(st, 0));
			sb_col_text(&sb, st, 1);
			sb_puts(&sb, ",\"started_at\":");  sb_col_text(&sb, st, 2);
			sb_puts(&sb, ",\"ended_at\":");    sb_col_text(&sb, st, 3);
			sb_puts(&sb, ",\"location\":");    sb_col_text(&sb, st, 4);
			sb_puts(&sb, ",\"antenna_cfg\":"); sb_col_text(&sb, st, 5);
			sb_puts(&sb, ",\"tx_power\":");    sb_col_text(&sb, st, 6);
			sb_puts(&sb, ",\"channel\":");     sb_col_int(&sb, st, 7);
			sb_puts(&sb, ",\"scenario\":");    sb_col_text(&sb, st, 8);
			sb_puts(&sb, ",\"weather\":");     sb_col_text(&sb, st, 9);
			sb_puts(&sb, ",\"notes\":");       sb_col_text(&sb, st, 10);
			sb_putc(&sb, '}');
		}
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
	if (!found) { free(sb.buf); api_send(cli->fd, 404, "application/json", "{\"error\":\"no session\"}", -1); return; }
	sb_finish(&sb, cli);
}

static void tele_labels(ApiClient *cli, long sid)
{
	sqlite3 *db = NULL;
	if (wfb_logger_open(&db) != 0) { api_send(cli->fd, 200, "application/json", "[]", -1); return; }
	/* t0 = first record ts_ms (the x=0 reference the chart plots against) */
	long long t0 = 0;
	sqlite3_stmt *q = NULL;
	if (sqlite3_prepare_v2(db, "SELECT MIN(ts_ms) FROM records WHERE session_id=?", -1, &q, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(q, 1, sid);
		if (sqlite3_step(q) == SQLITE_ROW && sqlite3_column_type(q, 0) != SQLITE_NULL)
			t0 = sqlite3_column_int64(q, 0);
	}
	sqlite3_finalize(q);
	sqlite3_stmt *st = NULL;
	SB sb = {0};
	sb_putc(&sb, '[');
	if (sqlite3_prepare_v2(db,
	        "SELECT id, t0_ms, t1_ms, kind, value, author FROM labels"
	        " WHERE session_id=? ORDER BY t0_ms, id", -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(st, 1, sid);
		int first = 1;
		while (sqlite3_step(st) == SQLITE_ROW) {
			if (!first) sb_putc(&sb, ',');
			first = 0;
			sb_printf(&sb, "{\"id\":%lld,\"t0_s\":%.3f,\"t1_s\":%.3f,\"kind\":",
			          (long long)sqlite3_column_int64(st, 0),
			          (sqlite3_column_int64(st, 1) - t0) / 1000.0,
			          (sqlite3_column_int64(st, 2) - t0) / 1000.0);
			sb_col_text(&sb, st, 3);
			sb_puts(&sb, ",\"value\":");  sb_col_text(&sb, st, 4);
			sb_puts(&sb, ",\"author\":"); sb_col_text(&sb, st, 5);
			sb_putc(&sb, '}');
		}
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
	sb_putc(&sb, ']');
	sb_finish(&sb, cli);
}

static void tele_series(ApiClient *cli, long sid)
{
	sqlite3 *db = NULL;
	if (wfb_logger_open(&db) != 0) { api_send(cli->fd, 404, "application/json", "{\"error\":\"no db\"}", -1); return; }

	/* collect records (denormalised cols + per-row mcs_dist from raw_json) */
	SRow *rows = NULL;
	size_t nrows = 0, cap = 0;
	long long t0 = 0; int have_t0 = 0;
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(db,
	        "SELECT ts_ms, rssi_comb, uplink_rssi, rssi_spread, snr_avg, per, mcs,"
	        " pkt_lost, fec_rec, raw_json FROM records WHERE session_id=? ORDER BY ts_ms, id",
	        -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(st, 1, sid);
		while (sqlite3_step(st) == SQLITE_ROW) {
			if (nrows >= cap) {
				cap = cap ? cap * 2 : 1024;
				SRow *nr = realloc(rows, cap * sizeof(*rows));
				if (!nr) break;
				rows = nr;
			}
			SRow *r = &rows[nrows];
			memset(r, 0, sizeof(*r));
			r->ts_ms = (double)sqlite3_column_int64(st, 0);
			if (!have_t0) { t0 = sqlite3_column_int64(st, 0); have_t0 = 1; }
			#define COLD(idx, field, has) do { if (sqlite3_column_type(st, idx) != SQLITE_NULL) { r->field = sqlite3_column_double(st, idx); r->has = 1; } } while (0)
			#define COLI(idx, field, has) do { if (sqlite3_column_type(st, idx) != SQLITE_NULL) { r->field = sqlite3_column_int64(st, idx); r->has = 1; } } while (0)
			COLD(1, rssi_comb, h_rssi);
			COLD(2, uplink_rssi, h_up);
			COLD(3, rssi_spread, h_spread);
			COLD(4, snr_avg, h_snr);
			COLD(5, per, h_per);
			COLI(6, mcs, h_mcs);
			COLI(7, pkt_lost, h_lost);
			COLI(8, fec_rec, h_fec);
			#undef COLD
			#undef COLI
			const char *raw = (const char *)sqlite3_column_text(st, 9);
			int rlen = sqlite3_column_bytes(st, 9);
			mcs_dist_from_raw(raw, rlen, r->mcs_pkts);
			nrows++;
		}
	}
	sqlite3_finalize(st);

	/* labels -> overlays (x-seconds vs t0) */
	SB sb = {0};
	sb_puts(&sb, "{\"series\":{\"t\":[");
	for (size_t i = 0; i < nrows; i++) sb_printf(&sb, "%s%.3f", i ? "," : "", (rows[i].ts_ms - t0) / 1000.0);
	sb_putc(&sb, ']');

	#define EMIT_DBL(name, field, has) do { \
		sb_puts(&sb, ",\"" name "\":["); \
		for (size_t i = 0; i < nrows; i++) { if (i) sb_putc(&sb, ','); \
			if (rows[i].has) sb_printf(&sb, "%.6g", rows[i].field); else sb_puts(&sb, "null"); } \
		sb_putc(&sb, ']'); } while (0)
	#define EMIT_INT(name, field, has) do { \
		sb_puts(&sb, ",\"" name "\":["); \
		for (size_t i = 0; i < nrows; i++) { if (i) sb_putc(&sb, ','); \
			if (rows[i].has) sb_printf(&sb, "%ld", rows[i].field); else sb_puts(&sb, "null"); } \
		sb_putc(&sb, ']'); } while (0)
	EMIT_DBL("rssi_comb",   rssi_comb,   h_rssi);
	EMIT_DBL("uplink_rssi", uplink_rssi, h_up);
	EMIT_DBL("rssi_spread", rssi_spread, h_spread);
	EMIT_DBL("snr_avg",     snr_avg,     h_snr);
	EMIT_DBL("per",         per,         h_per);
	EMIT_INT("mcs",         mcs,         h_mcs);
	EMIT_INT("pkt_lost",    pkt_lost,    h_lost);
	EMIT_INT("fec_rec",     fec_rec,     h_fec);
	#undef EMIT_DBL
	#undef EMIT_INT
	/* no ML on the live path — tier1_state is all null */
	sb_puts(&sb, ",\"tier1_state\":[");
	for (size_t i = 0; i < nrows; i++) sb_puts(&sb, i ? ",null" : "null");
	sb_puts(&sb, "]}");

	/* mcs_dist: { "0":[...], ..., "7":[...] } */
	sb_puts(&sb, ",\"mcs_dist\":{");
	for (int m = 0; m < 8; m++) {
		sb_printf(&sb, "%s\"%d\":[", m ? "," : "", m);
		for (size_t i = 0; i < nrows; i++) sb_printf(&sb, "%s%d", i ? "," : "", rows[i].mcs_pkts[m]);
		sb_putc(&sb, ']');
	}
	sb_putc(&sb, '}');

	/* overlays from labels */
	sb_puts(&sb, ",\"overlays\":[");
	sqlite3_stmt *lb = NULL;
	if (sqlite3_prepare_v2(db,
	        "SELECT t0_ms, t1_ms, kind, value, author FROM labels WHERE session_id=? ORDER BY t0_ms",
	        -1, &lb, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(lb, 1, sid);
		int first = 1;
		while (sqlite3_step(lb) == SQLITE_ROW) {
			if (!first) sb_putc(&sb, ',');
			first = 0;
			sb_printf(&sb, "{\"x0\":%.3f,\"x1\":%.3f,\"kind\":",
			          (sqlite3_column_int64(lb, 0) - t0) / 1000.0,
			          (sqlite3_column_int64(lb, 1) - t0) / 1000.0);
			sb_col_text(&sb, lb, 2);
			sb_puts(&sb, ",\"value\":");  sb_col_text(&sb, lb, 3);
			sb_puts(&sb, ",\"author\":"); sb_col_text(&sb, lb, 4);
			sb_putc(&sb, '}');
		}
	}
	sqlite3_finalize(lb);
	sqlite3_close(db);
	sb_putc(&sb, ']');

	sb_printf(&sb, ",\"is_vehicle\":false,\"vehicle_extra\":null,\"model_ver\":null,\"n\":%zu}", nrows);
	free(rows);
	sb_finish(&sb, cli);
}

static void tele_capture_status(ApiClient *cli)
{
	WfbLogStatus s;
	wfb_logger_status(&s);
	char body[256];
	int n = snprintf(body, sizeof(body),
		"{\"running\":%s,\"bind_error\":%s,\"db_error\":%s,\"session_id\":%ld,\"records\":%ld,"
		"\"bad\":%ld,\"age\":%.1f,\"max_duration\":%d,\"listen\":%d}",
		s.running ? "true" : "false", s.bind_error ? "true" : "false",
		s.db_error ? "true" : "false",
		s.session_id, s.records, s.bad, s.age_s, s.max_duration, s.listen_port);
	api_send(cli->fd, 200, "application/json", body, n);
}

/* ---- write endpoints (GET + ?action=, matching the GET-only API) ---------- */
static long tele_first_ts(sqlite3 *db, long sid)
{
	long t0 = 0;
	sqlite3_stmt *q = NULL;
	if (sqlite3_prepare_v2(db, "SELECT MIN(ts_ms) FROM records WHERE session_id=?", -1, &q, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(q, 1, sid);
		if (sqlite3_step(q) == SQLITE_ROW && sqlite3_column_type(q, 0) != SQLITE_NULL)
			t0 = (long)sqlite3_column_int64(q, 0);
	}
	sqlite3_finalize(q);
	return t0;
}

static void tele_label_add(ApiClient *cli, long sid, const char *qstr)
{
	char t0s[32] = "", t1s[32] = "", kind[24] = "event", value[256] = "", author[64] = "human:web";
	qparam_dec(qstr, "t0", t0s, sizeof(t0s));
	qparam_dec(qstr, "t1", t1s, sizeof(t1s));
	qparam_dec(qstr, "kind", kind, sizeof(kind));
	qparam_dec(qstr, "value", value, sizeof(value));
	qparam_dec(qstr, "author", author, sizeof(author));
	if (!kind[0]) snprintf(kind, sizeof(kind), "event");
	if (!author[0]) snprintf(author, sizeof(author), "human:web");

	sqlite3 *db = NULL;
	if (wfb_logger_open(&db) != 0) { api_send(cli->fd, 500, "application/json", "{\"ok\":false}", -1); return; }
	long t0 = tele_first_ts(db, sid);
	long long t0_ms = (long long)(strtod(t0s, NULL) * 1000.0 + 0.5) + t0;
	long long t1_ms = (long long)(strtod(t1s, NULL) * 1000.0 + 0.5) + t0;
	if (t1_ms < t0_ms) { long long tmp = t0_ms; t0_ms = t1_ms; t1_ms = tmp; }

	char now[40]; { time_t tt = time(NULL); struct tm tm; gmtime_r(&tt, &tm); strftime(now, sizeof(now), "%Y-%m-%dT%H:%M:%S+00:00", &tm); }
	sqlite3_stmt *st = NULL;
	long long lid = -1;
	if (sqlite3_prepare_v2(db,
	        "INSERT INTO labels (session_id,t0_ms,t1_ms,kind,value,author,created_at)"
	        " VALUES (?,?,?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(st, 1, sid);
		sqlite3_bind_int64(st, 2, t0_ms);
		sqlite3_bind_int64(st, 3, t1_ms);
		sqlite3_bind_text(st, 4, kind, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 5, value, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 6, author, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 7, now, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(st) == SQLITE_DONE) lid = sqlite3_last_insert_rowid(db);
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
	char body[64];
	if (lid >= 0) { int n = snprintf(body, sizeof(body), "{\"id\":%lld}", lid); api_send(cli->fd, 200, "application/json", body, n); }
	else api_send(cli->fd, 500, "application/json", "{\"ok\":false,\"error\":\"insert failed\"}", -1);
}

static void tele_label_del(ApiClient *cli, const char *qstr)
{
	int lid = 0;
	if (qs_get_int(qstr, "label_id", &lid) != 0 || lid <= 0) {
		api_send(cli->fd, 400, "application/json", "{\"ok\":false,\"error\":\"missing label_id\"}", -1); return;
	}
	sqlite3 *db = NULL;
	if (wfb_logger_open(&db) != 0) { api_send(cli->fd, 500, "application/json", "{\"ok\":false}", -1); return; }
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(db, "DELETE FROM labels WHERE id=?", -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(st, 1, lid);
		sqlite3_step(st);
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
	api_send(cli->fd, 200, "application/json", "{\"ok\":true}", -1);
}

static void tele_meta(ApiClient *cli, long sid, const char *qstr)
{
	/* the 7 META_FIELDS; channel is INTEGER, the rest TEXT. Present-and-empty
	 * clears to NULL (matches webapp). */
	static const char *F[] = { "location", "antenna_cfg", "tx_power", "channel",
	                           "scenario", "weather", "notes" };
	char vals[7][256];
	int present[7], np = 0;
	for (int i = 0; i < 7; i++) {
		present[i] = qparam_dec(qstr, F[i], vals[i], sizeof(vals[i]));
		if (present[i]) np++;
	}
	if (!np) { api_send(cli->fd, 200, "application/json", "{\"ok\":true,\"changed\":0}", -1); return; }

	char sql[512];
	int p = snprintf(sql, sizeof(sql), "UPDATE sessions SET ");
	int first = 1;
	for (int i = 0; i < 7; i++) {
		if (!present[i]) continue;
		p += snprintf(sql + p, sizeof(sql) - p, "%s%s=?", first ? "" : ", ", F[i]);
		first = 0;
	}
	snprintf(sql + p, sizeof(sql) - p, " WHERE id=?");

	sqlite3 *db = NULL;
	if (wfb_logger_open(&db) != 0) { api_send(cli->fd, 500, "application/json", "{\"ok\":false}", -1); return; }
	sqlite3_stmt *st = NULL;
	int ok = 0;
	if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
		int b = 1;
		for (int i = 0; i < 7; i++) {
			if (!present[i]) continue;
			if (!vals[i][0]) sqlite3_bind_null(st, b);
			else if (i == 3) sqlite3_bind_int(st, b, atoi(vals[i]));   /* channel */
			else sqlite3_bind_text(st, b, vals[i], -1, SQLITE_TRANSIENT);
			b++;
		}
		sqlite3_bind_int64(st, b, sid);
		ok = (sqlite3_step(st) == SQLITE_DONE);
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
	api_send(cli->fd, ok ? 200 : 500, "application/json",
	         ok ? "{\"ok\":true}" : "{\"ok\":false}", -1);
}

static void tele_session_delete(ApiClient *cli, long sid)
{
	sqlite3 *db = NULL;
	if (wfb_logger_open(&db) != 0) { api_send(cli->fd, 500, "application/json", "{\"ok\":false}", -1); return; }

	/* refuse the ACTIVE capture: an open live-* session the logger is still
	 * writing — deleting it would fail every subsequent insert's FK. */
	sqlite3_stmt *st = NULL;
	int active = 0, found = 0;
	if (sqlite3_prepare_v2(db, "SELECT source, ended_at FROM sessions WHERE id=?", -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(st, 1, sid);
		if (sqlite3_step(st) == SQLITE_ROW) {
			found = 1;
			const char *src = (const char *)sqlite3_column_text(st, 0);
			int ended_null = (sqlite3_column_type(st, 1) == SQLITE_NULL);
			if (src && !strncmp(src, "live", 4) && ended_null) active = 1;
		}
	}
	sqlite3_finalize(st);
	if (!found) { sqlite3_close(db); api_send(cli->fd, 404, "application/json", "{\"ok\":false,\"error\":\"no session\"}", -1); return; }
	if (active) {
		sqlite3_close(db);
		api_send(cli->fd, 409, "application/json",
		         "{\"ok\":false,\"error\":\"refusing to delete the active capture — roll a new session first\"}", -1);
		return;
	}

	char *err = NULL;
	int ok = 1;
	sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
	static const char *DELS[] = {
		"DELETE FROM predictions WHERE record_id IN (SELECT id FROM records WHERE session_id=%ld)",
		"DELETE FROM records WHERE session_id=%ld",
		"DELETE FROM labels WHERE session_id=%ld",
		"DELETE FROM sessions WHERE id=%ld",
	};
	for (size_t i = 0; i < sizeof(DELS) / sizeof(DELS[0]) && ok; i++) {
		char q[160]; snprintf(q, sizeof(q), DELS[i], sid);
		if (sqlite3_exec(db, q, NULL, NULL, &err) != SQLITE_OK) { ok = 0; sqlite3_free(err); err = NULL; }
	}
	sqlite3_exec(db, ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
	sqlite3_close(db);
	char body[96];
	if (ok) { int n = snprintf(body, sizeof(body), "{\"ok\":true,\"deleted\":%ld}", sid); api_send(cli->fd, 200, "application/json", body, n); }
	else api_send(cli->fd, 500, "application/json", "{\"ok\":false,\"error\":\"delete failed\"}", -1);
}

static void tele_capture_roll(ApiClient *cli, const char *qstr)
{
	int dur = -1;
	(void)qs_get_int(qstr, "duration", &dur);   /* optional; <0 keeps current */
	wfb_logger_roll(dur);
	api_send(cli->fd, 200, "application/json", "{\"ok\":true}", -1);
}

/* ---- dispatcher ----------------------------------------------------------- */
int tele_route(ApiClient *cli, const char *path, const char *qstr)
{
	/* pages */
	if (!strcmp(path, "/telemetry") || !strcmp(path, "/telemetry/")) {
		api_send_blob(cli->fd, "text/html; charset=utf-8", tele_index_html, tele_index_html_len);
		return 1;
	}
	if (!strcmp(path, "/telemetry/session")) {
		api_send_blob(cli->fd, "text/html; charset=utf-8", tele_session_html, tele_session_html_len);
		return 1;
	}
	if (!strncmp(path, "/telemetry/static/", 18)) {
		const char *a = path + 18;
		if      (!strcmp(a, "app.js"))           api_send_blob(cli->fd, "application/javascript", tele_app_js, tele_app_js_len);
		else if (!strcmp(a, "app.css"))          api_send_blob(cli->fd, "text/css", tele_app_css, tele_app_css_len);
		else if (!strcmp(a, "uPlot.iife.min.js")) api_send_blob(cli->fd, "application/javascript", tele_uplot_js, tele_uplot_js_len);
		else if (!strcmp(a, "uPlot.min.css"))    api_send_blob(cli->fd, "text/css", tele_uplot_css, tele_uplot_css_len);
		else api_send(cli->fd, 404, "text/plain", "no such asset\n", -1);
		return 1;
	}

	/* JSON API */
	if (!strcmp(path, "/api/v1/telemetry/sessions")) { tele_sessions(cli); return 1; }

	if (!strcmp(path, "/api/v1/telemetry/capture")) {
		size_t al = 0; const char *act = qs_get(qstr, "action", &al);
		if (act && al == 4 && !strncmp(act, "roll", 4)) tele_capture_roll(cli, qstr);
		else tele_capture_status(cli);
		return 1;
	}

	if (!strcmp(path, "/api/v1/telemetry/session")) {
		int sid = 0;
		if (qs_get_int(qstr, "id", &sid) != 0 || sid <= 0) {
			api_send(cli->fd, 400, "application/json", "{\"error\":\"missing id\"}", -1); return 1;
		}
		size_t al = 0; const char *act = qs_get(qstr, "action", &al);
		if (act && al == 6 && !strncmp(act, "delete", 6)) tele_session_delete(cli, sid);
		else tele_session_one(cli, sid);
		return 1;
	}
	if (!strcmp(path, "/api/v1/telemetry/series")) {
		int sid = 0;
		if (qs_get_int(qstr, "id", &sid) != 0 || sid <= 0) {
			api_send(cli->fd, 400, "application/json", "{\"error\":\"missing id\"}", -1); return 1;
		}
		tele_series(cli, sid);
		return 1;
	}
	if (!strcmp(path, "/api/v1/telemetry/meta")) {
		int sid = 0;
		if (qs_get_int(qstr, "id", &sid) != 0 || sid <= 0) {
			api_send(cli->fd, 400, "application/json", "{\"ok\":false,\"error\":\"missing id\"}", -1); return 1;
		}
		tele_meta(cli, sid, qstr);
		return 1;
	}
	if (!strcmp(path, "/api/v1/telemetry/labels")) {
		size_t al = 0; const char *act = qs_get(qstr, "action", &al);
		if (act && al == 3 && !strncmp(act, "add", 3)) {
			int sid = 0;
			if (qs_get_int(qstr, "id", &sid) != 0 || sid <= 0) {
				api_send(cli->fd, 400, "application/json", "{\"ok\":false,\"error\":\"missing id\"}", -1); return 1;
			}
			tele_label_add(cli, sid, qstr);
		} else if (act && al == 3 && !strncmp(act, "del", 3)) {
			tele_label_del(cli, qstr);
		} else {
			int sid = 0;
			if (qs_get_int(qstr, "id", &sid) != 0 || sid <= 0) {
				api_send(cli->fd, 400, "application/json", "{\"error\":\"missing id\"}", -1); return 1;
			}
			tele_labels(cli, sid);
		}
		return 1;
	}

	return 0;
}
