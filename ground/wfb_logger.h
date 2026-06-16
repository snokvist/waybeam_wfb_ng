/* wfb_logger.h — in-process telemetry logger for gs_supervisor.
 *
 * Phase 5 of the autonomous mega-binary: the udp:6700 -> SQLite capture that
 * used to live in the Python telemetry stack (telemetry/wfb_capture.py +
 * wfb_store.py, launched via webui_session.sh) now runs as a background thread
 * inside wfb-gs. Schema, WAL, commit cadence, and session-rollover semantics are
 * preserved bit-for-bit (telemetry/schema.sql) so existing wfb.sqlite files and
 * queries survive.
 *
 * Threading model (faithful port of wfb_capture.Capture):
 *   - the logger thread owns the ONLY write connection used for the capture
 *     INSERT path — single writer, lock-free apart from the roll handoff;
 *   - other threads (HTTP request handlers) open their own short-lived
 *     connections via wfb_logger_open() and read/write labels+meta over WAL;
 *   - roll()/stop() from another thread only SET flags under the lock; the
 *     actual session close+reopen happens inside the capture loop.
 *
 * Not sodium-gated — links into BOTH the standalone and mega gs_supervisor
 * builds (unlike the key endpoints). Requires the vendored SQLite amalgamation
 * (ground/vendor/sqlite) and -lpthread.
 */
#ifndef WFB_LOGGER_H
#define WFB_LOGGER_H

#include <stdbool.h>

struct sqlite3;

/* Runtime config, filled from the gs_supervisor `telemetry` block (+ wfb-link
 * overlay log.enabled). Defaults reproduce the Python ingester's behaviour. */
typedef struct {
	bool enabled;          /* master gate (telemetry.enabled / log.enabled) */
	char db[512];          /* sqlite path (default "wfb.sqlite") */
	char bind[64];         /* udp bind addr (default "127.0.0.1") */
	int  listen_port;      /* udp port to ingest stats on (default 6700) */
	int  max_duration;     /* roll a fresh session at this age, s (0=unbounded) */
	char source[32];       /* sessions.source tag (default "live-gs") */
	/* objective metadata stamped on each opened session (optional) */
	int  channel;          /* 0 = unset */
	char tx_power[32];     /* "" = unset */
	char antenna_cfg[64];  /* "" = unset */
} WfbLogConfig;

/* Status snapshot — written by the capture thread, read by HTTP handlers
 * under the internal lock. */
typedef struct {
	int    running;
	int    bind_error;     /* 1 = udp bind failed; capture disabled, reads OK */
	int    db_error;       /* 1 = sqlite open/init failed; capture disabled, reads OK */
	long   session_id;     /* -1 when no session is open */
	long   records;
	long   bad;
	double age_s;          /* age of the current session */
	int    max_duration;
	int    listen_port;
} WfbLogStatus;

/* Fill `cfg` with the built-in defaults (enabled, 127.0.0.1:6700, wfb.sqlite,
 * 1200 s, source "live-gs", no metadata). */
void wfb_logger_defaults(WfbLogConfig *cfg);

/* Spawn the capture thread (no-op + returns 0 when cfg->enabled is false).
 * The config is copied; the caller's struct need not outlive the call.
 * Returns 0 on success (thread started or disabled), -1 on thread spawn error. */
int  wfb_logger_start(const WfbLogConfig *cfg);

/* Signal the capture loop to close its session and exit, then join. Safe to
 * call when not started. Clean (flag + join) — runs the final commit and
 * stamps ended_at; never a hard kill. */
void wfb_logger_stop(void);

/* Request a clean session roll: close the current session now, open a fresh one
 * on the next datagram. duration >= 0 sets a new max_duration; <0 keeps it.
 * Thread-safe; the loop does the actual DB work. */
void wfb_logger_roll(int duration);

/* Copy the current status snapshot. Safe before/after start. */
void wfb_logger_status(WfbLogStatus *out);

/* Configured db path (stable after start), for HTTP read/write connections.
 * Returns "" when the logger was never configured. */
const char *wfb_logger_db_path(void);

/* One-shot import of a JSONL capture (one wfb_rx -Y record per line) as a single
 * session, for migrating old logs without the retired Python tools. source tags
 * sessions.source (NULL -> "import:<basename>"). Returns #records, -1 on error. */
long wfb_logger_import_jsonl(const char *db_path, const char *jsonl_path, const char *source);

/* `telemetry-import <file.jsonl> [--db PATH]` applet entry (mega binary). */
int  wfb_telemetry_import_main(int argc, char **argv);

/* Open a fresh SQLite connection on the configured db (WAL, busy_timeout set),
 * for use by an HTTP request handler on its own thread. The schema is assumed
 * already initialised by the capture thread; on a logger-disabled build the
 * connection is still usable for reads of an existing file. Returns 0 + *out on
 * success, -1 on error (*out left NULL). Caller must sqlite3_close(*out). */
int  wfb_logger_open(struct sqlite3 **out);

#endif /* WFB_LOGGER_H */
