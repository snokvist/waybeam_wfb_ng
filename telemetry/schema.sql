-- wfb data-session store schema (see DATASTORE.md).
-- Applied by wfb_store.init_db(); all statements are idempotent.
-- raw_json is the source of truth; the denormalised columns on `records` are a
-- rebuildable cache for fast plotting/filtering.

PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

-- one row per capture session (a range walk, a bench run, a live flight)
CREATE TABLE IF NOT EXISTS sessions (
    id          INTEGER PRIMARY KEY,
    started_at  TEXT NOT NULL,          -- ISO8601
    ended_at    TEXT,
    source      TEXT,                   -- 'live-tee' | 'import:<file>' | 'bench'
    -- free-form metadata for training stratification / filtering:
    location    TEXT,
    antenna_cfg TEXT,
    tx_power    TEXT,
    channel     INTEGER,
    scenario    TEXT,                   -- 'range-walk' | 'attenuation' | 'flight' ...
    weather     TEXT,
    notes       TEXT
);

-- one row per wfb_rx -Y datagram. raw_json kept verbatim so we can re-flatten
-- with a future wfb_schema without re-capturing; hot columns denormalised out.
CREATE TABLE IF NOT EXISTS records (
    id          INTEGER PRIMARY KEY,
    session_id  INTEGER NOT NULL REFERENCES sessions(id),
    ts_ms       INTEGER NOT NULL,
    seq         INTEGER,
    mcs         INTEGER,
    rssi_comb   REAL,                   -- combined/max across chains
    rssi_spread REAL,                   -- max-min of per-chain rssi.avg
    snr_avg     REAL,                   -- train-time only; may be NULL live
    pkt_all     INTEGER,
    pkt_uniq    INTEGER,
    pkt_lost    INTEGER,
    fec_rec     INTEGER,
    dec_err     INTEGER,
    per         REAL,                   -- derived packet error rate
    -- Vehicle-side UPLINK reception (GS->vehicle), from the vehicle's own
    -- wfb_rx -Y surfaced via link_controller /status "uplink_rx". Genuinely
    -- independent antenna data from rssi_comb (which, on an imported vehicle
    -- session, is the GS-relayed DOWNLINK score). NULL on GS-side (live-*)
    -- rows and any pre-uplink legacy row. Additive migration in wfb_store.
    uplink_rssi REAL,                   -- smoothed RSSI the vehicle hears the GS at
    uplink_pkt  INTEGER,                -- uplink pkts received in the sample
    uplink_lost INTEGER,                -- uplink pkts lost in the sample
    raw_json    TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_records_session_ts ON records(session_id, ts_ms);

-- ML predictions, kept separate so re-scoring a session with a new model
-- version doesn't touch the raw records.
CREATE TABLE IF NOT EXISTS predictions (
    id          INTEGER PRIMARY KEY,
    record_id   INTEGER NOT NULL REFERENCES records(id),
    model_ver   TEXT NOT NULL,
    tier1_state INTEGER,               -- 0 ok / 1 degraded / 2 critical
    scores      TEXT,                  -- JSON class probabilities
    mcs_reco    INTEGER,               -- recommended MCS, if the model emits one
    tier2_note  TEXT                   -- Gemma explanation, when escalated
);
CREATE INDEX IF NOT EXISTS idx_pred_record ON predictions(record_id);

-- labels from humans, rules, or outcome-mining; time-range scoped so a span can
-- be labeled without per-record clicks. author distinguishes ground truth from
-- model output for training.
CREATE TABLE IF NOT EXISTS labels (
    id          INTEGER PRIMARY KEY,
    session_id  INTEGER NOT NULL REFERENCES sessions(id),
    t0_ms       INTEGER NOT NULL,      -- range [t0,t1]; t0==t1 for a point event
    t1_ms       INTEGER NOT NULL,
    kind        TEXT NOT NULL,         -- 'state' | 'event' | 'cliff' | 'action'
    value       TEXT NOT NULL,
    author      TEXT NOT NULL,         -- 'human:<who>' | 'rule' | 'outcome' | 'model:<ver>'
    confidence  REAL,
    created_at  TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_labels_session ON labels(session_id, t0_ms);
