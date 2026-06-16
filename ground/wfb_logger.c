/* wfb_logger.c — in-process udp:6700 -> SQLite telemetry logger. See wfb_logger.h.
 *
 * Port of telemetry/wfb_capture.py (Capture loop) + telemetry/wfb_store.py
 * (schema, derive_columns, create/insert/close). Behaviour kept identical:
 * a session opens on the first datagram, commits every 20 records or 2 s, rolls
 * a fresh session at max_duration, and closes cleanly (stamp ended_at + WAL
 * checkpoint) on stop. raw_json is stored verbatim; the hot columns are a
 * denormalised cache derived per record exactly as wfb_store.derive_columns. */
#include "wfb_logger.h"
#include "sqlite3.h"
#include "wfb_json.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* gs_supervisor logging hooks (defined in gs_supervisor.c). */
void logf_(const char *level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define LOGI(...) logf_("info", __VA_ARGS__)
#define LOGW(...) logf_("warn", __VA_ARGS__)
#define LOGE(...) logf_("err",  __VA_ARGS__)

#define WFB_LOG_MAX_TOKS 768          /* a 5-antenna rx_ant datagram is ~180 toks */
#define WFB_LOG_COMMIT_EVERY 20
#define WFB_LOG_COMMIT_SECS  2.0

/* ---- embedded schema (telemetry/schema.sql, kept in sync) ----------------- */
static const char *SCHEMA_SQL =
	"PRAGMA journal_mode = WAL;\n"
	"PRAGMA foreign_keys = ON;\n"
	"CREATE TABLE IF NOT EXISTS sessions ("
	"  id INTEGER PRIMARY KEY, started_at TEXT NOT NULL, ended_at TEXT,"
	"  source TEXT, location TEXT, antenna_cfg TEXT, tx_power TEXT,"
	"  channel INTEGER, scenario TEXT, weather TEXT, notes TEXT);\n"
	"CREATE TABLE IF NOT EXISTS records ("
	"  id INTEGER PRIMARY KEY, session_id INTEGER NOT NULL REFERENCES sessions(id),"
	"  ts_ms INTEGER NOT NULL, seq INTEGER, mcs INTEGER, rssi_comb REAL,"
	"  rssi_spread REAL, snr_avg REAL, pkt_all INTEGER, pkt_uniq INTEGER,"
	"  pkt_lost INTEGER, fec_rec INTEGER, dec_err INTEGER, per REAL,"
	"  uplink_rssi REAL, uplink_pkt INTEGER, uplink_lost INTEGER, raw_json TEXT NOT NULL);\n"
	"CREATE INDEX IF NOT EXISTS idx_records_session_ts ON records(session_id, ts_ms);\n"
	"CREATE TABLE IF NOT EXISTS predictions ("
	"  id INTEGER PRIMARY KEY, record_id INTEGER NOT NULL REFERENCES records(id),"
	"  model_ver TEXT NOT NULL, tier1_state INTEGER, scores TEXT, mcs_reco INTEGER,"
	"  tier2_note TEXT);\n"
	"CREATE INDEX IF NOT EXISTS idx_pred_record ON predictions(record_id);\n"
	"CREATE TABLE IF NOT EXISTS labels ("
	"  id INTEGER PRIMARY KEY, session_id INTEGER NOT NULL REFERENCES sessions(id),"
	"  t0_ms INTEGER NOT NULL, t1_ms INTEGER NOT NULL, kind TEXT NOT NULL,"
	"  value TEXT NOT NULL, author TEXT NOT NULL, confidence REAL, created_at TEXT NOT NULL);\n"
	"CREATE INDEX IF NOT EXISTS idx_labels_session ON labels(session_id, t0_ms);\n";

/* uplink_* columns were added after the store first shipped; CREATE TABLE
 * IF NOT EXISTS can't add them to a pre-existing table, so apply additively. */
static const char *ADDED_COLUMNS[][2] = {
	{ "uplink_rssi", "REAL" },
	{ "uplink_pkt",  "INTEGER" },
	{ "uplink_lost", "INTEGER" },
};

/* ---- module state --------------------------------------------------------- */
static struct {
	WfbLogConfig    cfg;
	pthread_t       thread;
	int             started;
	pthread_mutex_t lock;       /* guards the fields below + roll/stop signals */
	int             stop;
	int             roll;
	int             pending_duration;  /* INT_MIN-style sentinel: -2 = none */
	/* status snapshot */
	int    running;
	int    bind_error;
	int    db_error;            /* 1 = sqlite open/init failed; capture disabled */
	long   session_id;
	double session_start;       /* monotonic seconds */
	long   records;
	long   bad;
	int    max_duration;        /* live (roll may change it) */
} L = { .pending_duration = -2, .session_id = -1, .lock = PTHREAD_MUTEX_INITIALIZER };

/* ---- small helpers -------------------------------------------------------- */
static double mono_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void iso_now(char *out, size_t cap)
{
	time_t t = time(NULL);
	struct tm tm;
	gmtime_r(&t, &tm);
	strftime(out, cap, "%Y-%m-%dT%H:%M:%S+00:00", &tm);
}

/* strtod over a JT_PRIM token (jint is integer-only; rssi/snr avg are numeric
 * and may be float). Returns 0 + *out on success, -1 otherwise. */
static int jdbl(const char *js, const JTok *t, double *out)
{
	if (!t || t->type != JT_PRIM) return -1;
	char buf[48];
	int slen = t->end - t->start;
	if (slen <= 0 || slen >= (int)sizeof(buf)) return -1;
	memcpy(buf, js + t->start, (size_t)slen);
	buf[slen] = 0;
	char *end = NULL;
	double v = strtod(buf, &end);
	if (!end || *end) return -1;   /* rejects "true"/"null"/garbage */
	*out = v;
	return 0;
}

/* ---- denormalised column derivation (mirror of wfb_store.derive_columns) --- */
typedef struct {
	long long ts_ms;
	long seq;      int has_seq;
	int  mcs;      int has_mcs;
	double rssi_comb;   int has_rssi_comb;
	double rssi_spread; int has_rssi_spread;
	double snr_avg;     int has_snr_avg;
	long pkt_all;  int has_pkt_all;
	long pkt_uniq; int has_pkt_uniq;
	long pkt_lost; int has_pkt_lost;
	long fec_rec;  int has_fec_rec;
	long dec_err;  int has_dec_err;
	double per;    int has_per;
} Derived;

/* object child "sec.key" as int / double, with presence. */
static int sub_int(const char *js, JTok *t, int n, int obj, const char *key, long *out)
{
	int v = jfind(js, t, n, obj, key);
	return (v >= 0 && jint(js, &t[v], out) == 0) ? 0 : -1;
}
static int sub_avg(const char *js, JTok *t, int n, int obj, const char *key, double *out)
{
	/* key -> nested object with an "avg" numeric child */
	int s = jfind(js, t, n, obj, key);
	if (s < 0 || t[s].type != JT_OBJ) return -1;
	int v = jfind(js, t, n, s, "avg");
	return (v >= 0 && jdbl(js, &t[v], out) == 0) ? 0 : -1;
}

static void derive(const char *js, JTok *t, int n, Derived *d)
{
	memset(d, 0, sizeof(*d));

	long lv;
	if (sub_int(js, t, n, 0, "ts_ms", &lv) == 0) d->ts_ms = lv;
	if (sub_int(js, t, n, 0, "seq", &lv) == 0) { d->seq = lv; d->has_seq = 1; }

	/* ant[]: rssi.avg, snr.avg, mcs across chains */
	double rssi_min = 0, rssi_max = 0; int rssi_cnt = 0;
	double snr_sum = 0; int snr_cnt = 0;
	int mcs_vals[16], mcs_cnt = 0;
	int ant = jfind(js, t, n, 0, "ant");
	if (ant >= 0 && t[ant].type == JT_ARR) {
		int kids = t[ant].size;
		int ci = ant + 1;
		for (int k = 0; k < kids; k++) {
			if (t[ci].type == JT_OBJ) {
				double r, s;
				if (sub_avg(js, t, n, ci, "rssi", &r) == 0) {
					if (rssi_cnt == 0) { rssi_min = rssi_max = r; }
					else { if (r < rssi_min) rssi_min = r; if (r > rssi_max) rssi_max = r; }
					rssi_cnt++;
				}
				if (sub_avg(js, t, n, ci, "snr", &s) == 0) { snr_sum += s; snr_cnt++; }
				long m;
				if (sub_int(js, t, n, ci, "mcs", &m) == 0 && mcs_cnt < (int)(sizeof(mcs_vals)/sizeof(mcs_vals[0])))
					mcs_vals[mcs_cnt++] = (int)m;
			}
			ci = jskip(t, n, ci);
		}
	}
	if (rssi_cnt > 0) {
		d->rssi_comb = rssi_max; d->has_rssi_comb = 1;
		d->rssi_spread = rssi_max - rssi_min; d->has_rssi_spread = 1;
	}
	if (snr_cnt > 0) { d->snr_avg = snr_sum / snr_cnt; d->has_snr_avg = 1; }
	if (mcs_cnt > 0) {
		/* mode (per-antenna mcs is uniform in practice; defensive) */
		int best = mcs_vals[0], best_n = 0;
		for (int i = 0; i < mcs_cnt; i++) {
			int c = 0;
			for (int j = 0; j < mcs_cnt; j++) if (mcs_vals[j] == mcs_vals[i]) c++;
			if (c > best_n) { best_n = c; best = mcs_vals[i]; }
		}
		d->mcs = best; d->has_mcs = 1;
	}

	/* pkt block */
	int pkt = jfind(js, t, n, 0, "pkt");
	if (pkt >= 0 && t[pkt].type == JT_OBJ) {
		if (sub_int(js, t, n, pkt, "all", &lv) == 0) { d->pkt_all = lv; d->has_pkt_all = 1; }
		if (sub_int(js, t, n, pkt, "uniq", &lv) == 0) { d->pkt_uniq = lv; d->has_pkt_uniq = 1; }
		if (sub_int(js, t, n, pkt, "lost", &lv) == 0) { d->pkt_lost = lv; d->has_pkt_lost = 1; }
		if (sub_int(js, t, n, pkt, "fec_recovered", &lv) == 0) { d->fec_rec = lv; d->has_fec_rec = 1; }
		if (sub_int(js, t, n, pkt, "dec_err", &lv) == 0) { d->dec_err = lv; d->has_dec_err = 1; }
	}
	if (d->has_pkt_uniq && d->has_pkt_lost) {
		long denom = d->pkt_uniq + d->pkt_lost;
		d->per = denom > 0 ? (double)d->pkt_lost / (double)denom : 0.0;
		d->has_per = 1;
	}
	/* uplink_rx is present only on imported vehicle sessions; live GS rows
	 * carry no such block, so uplink_* stay NULL (no derivation here). */
}

/* ---- SQLite store --------------------------------------------------------- */
static int conn_tune(sqlite3 *db)
{
	char *err = NULL;
	if (sqlite3_exec(db, "PRAGMA busy_timeout=3000; PRAGMA journal_mode=WAL;",
	                 NULL, NULL, &err) != SQLITE_OK) {
		LOGW("telemetry: pragma: %s", err ? err : "?");
		sqlite3_free(err);
		return -1;
	}
	return 0;
}

static int store_init(sqlite3 *db)
{
	char *err = NULL;
	if (sqlite3_exec(db, SCHEMA_SQL, NULL, NULL, &err) != SQLITE_OK) {
		LOGE("telemetry: schema: %s", err ? err : "?");
		sqlite3_free(err);
		return -1;
	}
	/* additive columns (idempotent — ignore "duplicate column" errors) */
	for (size_t i = 0; i < sizeof(ADDED_COLUMNS) / sizeof(ADDED_COLUMNS[0]); i++) {
		char sql[128];
		snprintf(sql, sizeof(sql), "ALTER TABLE records ADD COLUMN %s %s",
		         ADDED_COLUMNS[i][0], ADDED_COLUMNS[i][1]);
		sqlite3_exec(db, sql, NULL, NULL, NULL);   /* dup-column error is fine */
	}
	return 0;
}

static long store_create_session(sqlite3 *db, const WfbLogConfig *cfg)
{
	char ts[40];
	iso_now(ts, sizeof(ts));
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(db,
	        "INSERT INTO sessions (started_at, source, channel, tx_power, antenna_cfg)"
	        " VALUES (?,?,?,?,?)", -1, &st, NULL) != SQLITE_OK)
		return -1;
	sqlite3_bind_text(st, 1, ts, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, cfg->source[0] ? cfg->source : "live-gs", -1, SQLITE_TRANSIENT);
	if (cfg->channel > 0) sqlite3_bind_int(st, 3, cfg->channel); else sqlite3_bind_null(st, 3);
	if (cfg->tx_power[0]) sqlite3_bind_text(st, 4, cfg->tx_power, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 4);
	if (cfg->antenna_cfg[0]) sqlite3_bind_text(st, 5, cfg->antenna_cfg, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 5);
	int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE) return -1;
	return (long)sqlite3_last_insert_rowid(db);
}

static void store_insert(sqlite3_stmt *ins, long session_id, const Derived *d,
                         const char *raw, int raw_len)
{
	sqlite3_reset(ins);
	sqlite3_clear_bindings(ins);
	int c = 1;
	sqlite3_bind_int64(ins, c++, session_id);
	sqlite3_bind_int64(ins, c++, d->ts_ms);                                       /* ts_ms (NOT NULL; 0 ok) */
	if (d->has_seq)         sqlite3_bind_int64(ins, c, d->seq);          else sqlite3_bind_null(ins, c); c++;
	if (d->has_mcs)         sqlite3_bind_int(ins, c, d->mcs);            else sqlite3_bind_null(ins, c); c++;
	if (d->has_rssi_comb)   sqlite3_bind_double(ins, c, d->rssi_comb);   else sqlite3_bind_null(ins, c); c++;
	if (d->has_rssi_spread) sqlite3_bind_double(ins, c, d->rssi_spread); else sqlite3_bind_null(ins, c); c++;
	if (d->has_snr_avg)     sqlite3_bind_double(ins, c, d->snr_avg);     else sqlite3_bind_null(ins, c); c++;
	if (d->has_pkt_all)     sqlite3_bind_int64(ins, c, d->pkt_all);      else sqlite3_bind_null(ins, c); c++;
	if (d->has_pkt_uniq)    sqlite3_bind_int64(ins, c, d->pkt_uniq);     else sqlite3_bind_null(ins, c); c++;
	if (d->has_pkt_lost)    sqlite3_bind_int64(ins, c, d->pkt_lost);     else sqlite3_bind_null(ins, c); c++;
	if (d->has_fec_rec)     sqlite3_bind_int64(ins, c, d->fec_rec);      else sqlite3_bind_null(ins, c); c++;
	if (d->has_dec_err)     sqlite3_bind_int64(ins, c, d->dec_err);      else sqlite3_bind_null(ins, c); c++;
	if (d->has_per)         sqlite3_bind_double(ins, c, d->per);         else sqlite3_bind_null(ins, c); c++;
	sqlite3_bind_null(ins, c++);   /* uplink_rssi */
	sqlite3_bind_null(ins, c++);   /* uplink_pkt  */
	sqlite3_bind_null(ins, c++);   /* uplink_lost */
	sqlite3_bind_text(ins, c++, raw, raw_len, SQLITE_TRANSIENT);  /* raw_json verbatim */
	if (sqlite3_step(ins) != SQLITE_DONE)
		LOGW("telemetry: insert: %s", sqlite3_errmsg(sqlite3_db_handle(ins)));
}

static void store_close_session(sqlite3 *db, long session_id)
{
	char ts[40];
	iso_now(ts, sizeof(ts));
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(db, "UPDATE sessions SET ended_at=? WHERE id=?", -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_text(st, 1, ts, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(st, 2, session_id);
		sqlite3_step(st);
		sqlite3_finalize(st);
	}
	/* bound the -wal file now that the session is sealed */
	sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
}

/* ---- public: db connection for HTTP handlers ------------------------------ */
int wfb_logger_open(sqlite3 **out)
{
	*out = NULL;
	if (!L.cfg.db[0]) return -1;
	sqlite3 *db = NULL;
	if (sqlite3_open_v2(L.cfg.db, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
		if (db) sqlite3_close(db);
		return -1;
	}
	conn_tune(db);
	*out = db;
	return 0;
}

const char *wfb_logger_db_path(void) { return L.cfg.db; }

/* ---- the capture loop ----------------------------------------------------- */
static void status_set_session(long id, double start, long records, long bad)
{
	pthread_mutex_lock(&L.lock);
	L.session_id = id;
	L.session_start = start;
	L.records = records;
	L.bad = bad;
	pthread_mutex_unlock(&L.lock);
}

static void *capture_run(void *arg)
{
	(void)arg;
	const WfbLogConfig *cfg = &L.cfg;

	sqlite3 *db = NULL;
	if (sqlite3_open_v2(cfg->db, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
		LOGE("telemetry: open %s: %s", cfg->db, db ? sqlite3_errmsg(db) : "?");
		if (db) sqlite3_close(db);
		pthread_mutex_lock(&L.lock); L.running = 0; L.db_error = 1; pthread_mutex_unlock(&L.lock);
		return NULL;
	}
	conn_tune(db);
	if (store_init(db) < 0) { sqlite3_close(db); return NULL; }

	sqlite3_stmt *ins = NULL;
	if (sqlite3_prepare_v2(db,
	        "INSERT INTO records (session_id, ts_ms, seq, mcs, rssi_comb, rssi_spread,"
	        " snr_avg, pkt_all, pkt_uniq, pkt_lost, fec_rec, dec_err, per,"
	        " uplink_rssi, uplink_pkt, uplink_lost, raw_json)"
	        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &ins, NULL) != SQLITE_OK) {
		LOGE("telemetry: prepare insert: %s", sqlite3_errmsg(db));
		sqlite3_close(db);
		return NULL;
	}

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) { LOGE("telemetry: socket: %s", strerror(errno)); sqlite3_finalize(ins); sqlite3_close(db); return NULL; }
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)cfg->listen_port);
	addr.sin_addr.s_addr = cfg->bind[0] ? inet_addr(cfg->bind) : htonl(INADDR_LOOPBACK);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		LOGW("telemetry: bind %s:%d failed: %s — capture disabled (UI read-only)",
		     cfg->bind, cfg->listen_port, strerror(errno));
		pthread_mutex_lock(&L.lock); L.bind_error = 1; L.running = 0; pthread_mutex_unlock(&L.lock);
		close(fd); sqlite3_finalize(ins); sqlite3_close(db);
		return NULL;
	}
	/* 1 s recv timeout keeps the loop responsive to stop/roll + the
	 * max-duration deadline regardless of datagram rate (mirror settimeout(1.0)). */
	struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	pthread_mutex_lock(&L.lock); L.running = 1; pthread_mutex_unlock(&L.lock);
	LOGI("telemetry: listening udp %s:%d -> %s (source=%s, max_duration=%ds)",
	     cfg->bind, cfg->listen_port, cfg->db, cfg->source[0] ? cfg->source : "live-gs", cfg->max_duration);

	long session_id = -1;
	long records = 0, bad = 0;
	double session_start = 0;
	int in_txn = 0, pend = 0;
	double last_commit = mono_s();
	char buf[65536];
	JTok toks[WFB_LOG_MAX_TOKS];

	#define BEGIN_TXN() do { if (!in_txn) { sqlite3_exec(db, "BEGIN", NULL, NULL, NULL); in_txn = 1; } } while (0)
	#define COMMIT_TXN() do { if (in_txn) { sqlite3_exec(db, "COMMIT", NULL, NULL, NULL); in_txn = 0; } last_commit = mono_s(); } while (0)
	#define CLOSE_SESSION() do { \
		if (session_id >= 0) { COMMIT_TXN(); store_close_session(db, session_id); \
			LOGI("telemetry: closed session %ld: %ld records (%ld bad)", session_id, records, bad); \
			session_id = -1; status_set_session(-1, 0, records, bad); } \
	} while (0)

	for (;;) {
		/* stop / roll signals */
		pthread_mutex_lock(&L.lock);
		int want_stop = L.stop;
		int want_roll = L.roll;
		int pend_dur = L.pending_duration;
		if (want_roll) { L.roll = 0; L.pending_duration = -2; }
		pthread_mutex_unlock(&L.lock);
		if (want_stop) break;

		ssize_t got = recvfrom(fd, buf, sizeof(buf) - 1, 0, NULL, NULL);
		if (got > 0) {
			buf[got] = 0;
			int n = json_parse(buf, (int)got, toks, WFB_LOG_MAX_TOKS);
			if (n < 1 || toks[0].type != JT_OBJ) {
				bad++;
			} else {
				if (session_id < 0) {
					BEGIN_TXN();
					session_id = store_create_session(db, cfg);
					if (session_id < 0) { COMMIT_TXN(); LOGE("telemetry: create_session failed"); }
					else {
						session_start = mono_s();
						records = 0; bad = 0;
						LOGI("telemetry: opened session %ld", session_id);
						status_set_session(session_id, session_start, 0, 0);
					}
				}
				if (session_id >= 0) {
					BEGIN_TXN();
					Derived d;
					derive(buf, toks, n, &d);
					store_insert(ins, session_id, &d, buf, (int)got);
					records++; pend++;
				}
			}
			status_set_session(session_id, session_start, records, bad);
		}

		double now = mono_s();

		if (want_roll) {
			if (pend_dur >= 0) { pthread_mutex_lock(&L.lock); L.max_duration = pend_dur; pthread_mutex_unlock(&L.lock); }
			if (session_id >= 0) LOGI("telemetry: session %ld rolled on request", session_id);
			CLOSE_SESSION();
		}

		int max_dur;
		pthread_mutex_lock(&L.lock); max_dur = L.max_duration; pthread_mutex_unlock(&L.lock);
		if (session_id >= 0 && max_dur > 0 && (now - session_start) >= max_dur) {
			LOGI("telemetry: session %ld hit max-duration %ds — rolled", session_id, max_dur);
			CLOSE_SESSION();
		}

		if (pend >= WFB_LOG_COMMIT_EVERY || (now - last_commit) >= WFB_LOG_COMMIT_SECS) {
			if (pend) { COMMIT_TXN(); pend = 0; } else { last_commit = now; }
		}
	}

	/* clean shutdown: final commit + seal the open session */
	if (pend) COMMIT_TXN();
	CLOSE_SESSION();
	pthread_mutex_lock(&L.lock); L.running = 0; pthread_mutex_unlock(&L.lock);
	close(fd);
	sqlite3_finalize(ins);
	sqlite3_close(db);
	LOGI("telemetry: capture stopped");
	return NULL;
}

/* ---- public API ----------------------------------------------------------- */
void wfb_logger_defaults(WfbLogConfig *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	/* Opt-in: a config with no `telemetry` block (and no wfb-link log.enabled
	 * override) must NOT auto-start the udp:6700 capture or create wfb.sqlite in
	 * the supervisor's CWD — that would persist logs on read-only/overlay deploys
	 * (e.g. rk3566_passive.json, which carries no telemetry block). Enable
	 * explicitly via telemetry.enabled or wfb-link.json log.enabled. */
	cfg->enabled = false;
	snprintf(cfg->db, sizeof(cfg->db), "wfb.sqlite");
	snprintf(cfg->bind, sizeof(cfg->bind), "127.0.0.1");
	cfg->listen_port = 6700;
	cfg->max_duration = 1200;
	snprintf(cfg->source, sizeof(cfg->source), "live-gs");
}

int wfb_logger_start(const WfbLogConfig *cfg)
{
	if (cfg) L.cfg = *cfg;
	if (!L.cfg.db[0]) wfb_logger_defaults(&L.cfg);   /* never started w/ empty path */
	if (!L.cfg.enabled) {
		LOGI("telemetry: logger disabled (telemetry.enabled=false / log.enabled=false)");
		return 0;
	}
	L.max_duration = L.cfg.max_duration;
	L.stop = 0; L.roll = 0; L.pending_duration = -2; L.session_id = -1;
	if (pthread_create(&L.thread, NULL, capture_run, NULL) != 0) {
		LOGE("telemetry: pthread_create: %s", strerror(errno));
		return -1;
	}
	L.started = 1;
	return 0;
}

void wfb_logger_stop(void)
{
	if (!L.started) return;
	pthread_mutex_lock(&L.lock); L.stop = 1; pthread_mutex_unlock(&L.lock);
	pthread_join(L.thread, NULL);
	L.started = 0;
}

void wfb_logger_roll(int duration)
{
	pthread_mutex_lock(&L.lock);
	L.roll = 1;
	L.pending_duration = (duration >= 0) ? duration : -2;
	pthread_mutex_unlock(&L.lock);
}

void wfb_logger_status(WfbLogStatus *out)
{
	pthread_mutex_lock(&L.lock);
	out->running     = L.running;
	out->bind_error  = L.bind_error;
	out->db_error    = L.db_error;
	out->session_id  = L.session_id;
	out->records     = L.records;
	out->bad         = L.bad;
	out->age_s       = (L.session_id >= 0) ? (mono_s() - L.session_start) : 0.0;
	out->max_duration = L.max_duration;
	out->listen_port = L.cfg.listen_port;
	pthread_mutex_unlock(&L.lock);
}

/* ---- one-shot JSONL import (offline; replaces telemetry/wfb_store.py import) - */
long wfb_logger_import_jsonl(const char *db_path, const char *jsonl_path, const char *source)
{
	char src[32];   /* matches WfbLogConfig.source — create_session copies into it */
	if (source && source[0]) snprintf(src, sizeof(src), "%s", source);
	else {
		const char *base = strrchr(jsonl_path, '/');
		base = base ? base + 1 : jsonl_path;
		snprintf(src, sizeof(src), "import:%s", base);
	}

	FILE *fp = fopen(jsonl_path, "r");
	if (!fp) { fprintf(stderr, "telemetry-import: open %s: %s\n", jsonl_path, strerror(errno)); return -1; }

	sqlite3 *db = NULL;
	if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
		fprintf(stderr, "telemetry-import: open db %s: %s\n", db_path, db ? sqlite3_errmsg(db) : "?");
		if (db) sqlite3_close(db);
		fclose(fp);
		return -1;
	}
	conn_tune(db);
	if (store_init(db) < 0) { sqlite3_close(db); fclose(fp); return -1; }

	sqlite3_stmt *ins = NULL;
	if (sqlite3_prepare_v2(db,
	        "INSERT INTO records (session_id, ts_ms, seq, mcs, rssi_comb, rssi_spread,"
	        " snr_avg, pkt_all, pkt_uniq, pkt_lost, fec_rec, dec_err, per,"
	        " uplink_rssi, uplink_pkt, uplink_lost, raw_json)"
	        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &ins, NULL) != SQLITE_OK) {
		fprintf(stderr, "telemetry-import: prepare: %s\n", sqlite3_errmsg(db));
		sqlite3_close(db); fclose(fp); return -1;
	}

	WfbLogConfig cfg;
	memset(&cfg, 0, sizeof(cfg));
	snprintf(cfg.source, sizeof(cfg.source), "%s", src);

	sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
	long sid = store_create_session(db, &cfg);
	if (sid < 0) { sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL); sqlite3_finalize(ins); sqlite3_close(db); fclose(fp); return -1; }

	char  *line = NULL;
	size_t lcap = 0;
	ssize_t llen;
	long n = 0, bad = 0;
	JTok toks[WFB_LOG_MAX_TOKS];
	while ((llen = getline(&line, &lcap, fp)) != -1) {
		while (llen > 0 && (line[llen - 1] == '\n' || line[llen - 1] == '\r')) line[--llen] = 0;
		if (llen == 0) continue;
		int nt = json_parse(line, (int)llen, toks, WFB_LOG_MAX_TOKS);
		if (nt < 1 || toks[0].type != JT_OBJ) { bad++; continue; }
		Derived d;
		derive(line, toks, nt, &d);
		store_insert(ins, sid, &d, line, (int)llen);
		n++;
	}
	free(line);
	sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
	store_close_session(db, sid);
	sqlite3_finalize(ins);
	sqlite3_close(db);
	fclose(fp);
	fprintf(stderr, "telemetry-import: %ld records (%ld bad) from %s -> session %ld in %s\n",
	        n, bad, jsonl_path, sid, db_path);
	return n;
}

int wfb_telemetry_import_main(int argc, char **argv)
{
	const char *jsonl = NULL, *db = "wfb.sqlite", *source = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--db") && i + 1 < argc) db = argv[++i];
		else if (!strcmp(argv[i], "--source") && i + 1 < argc) source = argv[++i];
		else if (argv[i][0] == '-') { fprintf(stderr, "telemetry-import: unknown option %s\n", argv[i]); return 2; }
		else jsonl = argv[i];
	}
	if (!jsonl) {
		fprintf(stderr, "usage: telemetry-import <file.jsonl> [--db PATH] [--source TAG]\n");
		return 2;
	}
	return wfb_logger_import_jsonl(db, jsonl, source) < 0 ? 1 : 0;
}
