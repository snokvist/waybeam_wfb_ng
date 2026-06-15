# Data-session store + web UI — architecture

> **Phase 5 update (2026-06-15):** the **live** capture + dashboard are now
> in-process C inside `wfb-gs` — the udp:6700 → SQLite logger is
> `ground/wfb_logger.c` (a background thread of `gs_supervisor`) and the
> dashboard is `ground/gs_supervisor_telemetry.c`, served at `/telemetry` on the
> supervisor's HTTP port. The schema + `derive_columns` were ported bit-for-bit,
> so this `wfb.sqlite` is unchanged. The Python capture/UI
> (`wfb_capture.py`/`wfb_ingest.py`/`webui/`/`webui_session.sh`/`capture_session.sh`)
> is retired to `archive/python-telemetry/`. `wfb_store.py` and the offline
> ML/import tools below remain for workstation use. Import old logs without
> Python via `wfb-gs telemetry-import <file.jsonl>`. Deferred to the archived
> Python (offline): the uplink↔downlink overlay, ML `tier1_state` bands, and the
> SCP vehicle-fetch import.

A single store for **all wfb telemetry data sessions** (live and historical),
with a small web UI to browse/replay them, label metadata and events by hand,
and **see the ML labels** (Tier-1 state, MCS recommendation, Tier-2 notes)
overlaid on the raw stream. This is the corpus the MCS strategy
(`MCS_STRATEGY.md`) is trained from and the active-learning surface that grows
it.

## Goals

1. **One home for every session** — capture once, never lose it; live and
   historical use the same schema and views.
2. **Human labeling** — session metadata (location, antenna config, TX power,
   channel, scenario, weather) and event/window labels (e.g. "should have
   stepped down here", "true cliff for MCS5"), feeding training.
3. **ML labels visible** — Tier-1 predictions and Tier-2 explanations shown
   inline against the raw record so a human can agree/correct (active learning).

## Stack — Flask + uPlot (locked); store/ingest stay stdlib

- **Web layer: Flask** (web UI + JSON API). Decided.
- **Charting: [uPlot](https://github.com/leeoniya/uPlot)**, vendored into the
  repo. Chosen because telemetry is large multi-series time-series (10 Hz ×
  minutes → tens of thousands of points; per-chain RSSI, PER, MCS, SNR): uPlot
  is purpose-built for that, ~40 KB, and stays smooth where Chart.js stalls past
  a few thousand points. Plotly is the richer-but-heavier fallback if we later
  need 3-D/interactive analysis views.
- **Store + ingest: stdlib `sqlite3`** — no deps. `wfb_store.py` (schema +
  importer + query helpers) and the live ingester are pure stdlib, so the data
  path carries no Flask/uPlot dependency and can run headless. Flask only serves
  what `wfb_store.py` already exposes.

A single SQLite file is trivially copied/backed up. Flask deps live in
`requirements-webui.txt` (training-box only; the SSC338Q never runs this).

## Deploy — one app (capture + DB + web UI)

The web UI captures the live stats_tap into SQLite **in-process** (a background
thread) and serves the UI from the same process, so a normal deploy is one
service. No separate ingester, no `sudo`, no shell glue. SIGTERM closes the
live session cleanly (stamps `ended_at`, commits the final window).

```bash
# one command: venv + Flask + a systemd unit (x86) or BusyBox init (aarch64)
telemetry/deploy/install.sh                  # UI :8080, capture udp:6700
#   -> http://<host>:8080

# or run it directly (capture is on by default):
pip install -r telemetry/requirements-webui.txt
python3 telemetry/webui/webapp.py --db telemetry/wfb.sqlite --port 8080 --listen 6700
```

The app **owns udp:6700**. If you keep gs_supervisor's `system.up` running
`capture_session.sh start`, the two fight for the port — drop that hook for the
one-app deploy, or run the app `--no-capture` to keep the legacy split below.

## Quick start (legacy split / imports)

```bash
# 1. import an existing capture (creates the DB on first run)
python3 telemetry/wfb_store.py import telemetry/real_wfb.jsonl --scenario range-walk

# 2. (live, split) standalone ingester — same capture core as the one-app path,
#    for deployments that want capture decoupled from the UI process
python3 telemetry/wfb_ingest.py --listen 6700 --scenario flight

# 3. browse UI-only against a DB someone else is writing (WAL read)
pip install -r telemetry/requirements-webui.txt
python3 telemetry/webui/webapp.py --db telemetry/wfb.sqlite --port 8080 --no-capture
#   -> http://127.0.0.1:8080
```

## SQLite schema

```sql
-- one row per capture session (a range walk, a bench run, a live flight)
CREATE TABLE sessions (
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

-- one row per wfb_rx -Y datagram; raw_json kept verbatim so we can re-flatten
-- with a future wfb_schema without re-capturing. Hot columns are denormalised
-- out for fast plotting/filtering.
CREATE TABLE records (
    id          INTEGER PRIMARY KEY,
    session_id  INTEGER NOT NULL REFERENCES sessions(id),
    ts_ms       INTEGER NOT NULL,
    seq         INTEGER,
    mcs         INTEGER,
    rssi_comb   REAL,                   -- combined/max across chains
    rssi_spread REAL,
    snr_avg     REAL,                   -- train-time only; may be NULL live
    pkt_all     INTEGER,
    pkt_uniq    INTEGER,
    pkt_lost    INTEGER,
    fec_rec     INTEGER,
    dec_err     INTEGER,
    per         REAL,                   -- derived packet error rate
    uplink_rssi REAL,                   -- vehicle's OWN reception of the GS
    uplink_pkt  INTEGER,                --   (GS->vehicle uplink), from the
    uplink_lost INTEGER,                --   vehicle wfb_rx -Y via uplink_rx
    raw_json    TEXT NOT NULL
);
CREATE INDEX idx_records_session_ts ON records(session_id, ts_ms);

-- DOWNLINK vs UPLINK on a vehicle session. For an imported vehicle session,
-- rssi_comb is the GS-relayed DOWNLINK score (how the GS hears the vehicle —
-- the rx_ant the vehicle adapts its FEC/MCS on, which arrives via the uplink
-- tunnel). The uplink_* columns are the genuinely INDEPENDENT measurement:
-- how the vehicle hears the GS, from the vehicle's own wfb_rx -Y, surfaced by
-- link_controller under /status "uplink_rx" (S99wfb wires wfb_rx -Y →
-- --uplink-stats 5811). Plotting both shows the link asymmetry (commonly tens
-- of dB) and proves WCMD/CSA actionable data reaches the vehicle. NULL on
-- GS-side (live-*) rows and on legacy sessions captured before uplink_rx.
-- The columns are added additively by wfb_store.init_db on existing stores.

-- ML predictions, kept separate so re-scoring a session with a new model
-- version doesn't touch the raw records.
CREATE TABLE predictions (
    id          INTEGER PRIMARY KEY,
    record_id   INTEGER NOT NULL REFERENCES records(id),
    model_ver   TEXT NOT NULL,
    tier1_state INTEGER,               -- 0 ok / 1 degraded / 2 critical
    scores      TEXT,                  -- JSON class probabilities
    mcs_reco    INTEGER,               -- recommended MCS, if the model emits one
    tier2_note  TEXT                   -- Gemma explanation, when escalated
);
CREATE INDEX idx_pred_record ON predictions(record_id);

-- labels from humans, rules, or outcome-mining; time-range scoped so a span can
-- be labeled without per-record clicks. author distinguishes ground truth from
-- model output for training.
CREATE TABLE labels (
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
CREATE INDEX idx_labels_session ON labels(session_id, t0_ms);
```

## Ingestion

```
                       ┌──────────────────────────────────────────┐
   wfb_rx -Y ──► tee ──┤ --forward 127.0.0.1:6600  (upstream, untouched)
  (gs_supervisor)      │ --tap     127.0.0.1:6700  ──► ingester ──► sqlite (live session)
                       └──────────────────────────────────────────┘

   historical: real_wfb.jsonl / sample_wfb.jsonl ──► importer ──► sqlite (one session/file)
```

- **Live:** reuse `wfb_stats_tee.py` — point a `--tap` at the ingester; it opens
  a new `sessions` row on first datagram, denormalises hot columns, stores
  `raw_json`. The tee already forwards upstream first, so capture adds no
  back-channel latency.
- **Historical:** an importer reads existing JSONL into a session row (source
  `import:<file>`), so the bench/synthetic captures we already have land in the
  same store.

## Web UI views

| view | what it shows |
|------|---------------|
| **Session list** | all sessions with metadata, duration, record count, label coverage; filter by scenario/channel/etc. |
| **Live** | auto-refresh (poll or SSE): current records, Tier-1 state, MCS, RSSI/PER timeline as they arrive |
| **Session detail** | timeseries (RSSI per chain + combined, PER, MCS, SNR if present) with **ML labels overlaid** (Tier-1 ok/degraded/critical bands, MCS reco) and **human labels** overlaid distinctly |
| **Label tools** | select a time range on the chart → assign a label (scenario/event/cliff/action) + notes; edit session metadata |

## ML labels + active-learning loop

The detail view renders `predictions` against the raw record. A human reviewing
a disagreement can drop a `labels` row (author `human:*`) to correct it; that
becomes ground truth for the next retrain.

```
 capture ─► ingest ─► Tier-1 score (predictions) ─► UI overlay
                                                       │
                              human agrees/corrects ───┘ (labels: author=human)
                                                       │
                       train_tier1.py reads labels + records ─► new model_ver
                                                       │
                              re-score sessions ───────┘  (predictions, new ver)
```

`train_tier1.py` gains a path to read records+labels directly from the DB (or
export a labeled JSONL) instead of only flat files. Tier-2 Gemma can run
**offline** over flagged spans to pre-fill `tier2_note` and mine candidate
labels for human review.

## Conventions

- The SQLite file (`telemetry/wfb.sqlite`) **is git-ignored** — it is a runtime
  artifact that live captures (`wfb_ingest.py`) and imports continuously churn,
  so it doesn't belong in version control. Its WAL/SHM/journal sidecars are
  ignored too. A fresh clone has no DB; the first `wfb_store.py` / ingest call
  creates it from `schema.sql`. For a browsable/labelable demo out of the box,
  regenerate the tagged `synthetic-demo` session with
  `python3 telemetry/gen_sample.py` (source `bench-synthetic`, `rule-demo`
  Tier-1 predictions). Back up / share a real DB by copying the single file.
- Store + ingest are stdlib `sqlite3` only. The web layer is Flask + vendored
  uPlot (`requirements-webui.txt`); no further runtime deps without a decision.
- `raw_json` is the source of truth; denormalised columns are a cache and can be
  rebuilt from it if `wfb_schema` evolves.

## Phased build (proposed)

1. **Schema + importer** — DONE. `schema.sql` + `wfb_store.py` (stdlib): JSONL
   importer, per-record denormalisation (`derive_columns`: best-chain rssi_comb,
   spread, snr droppable→NULL, derived PER), `rederive`, and query helpers
   (`list_sessions`/`get_session`/`get_records`). CLI: `init|import|list|show|
   rederive`. DB git-ignored.
2. **Live ingester** — DONE. `wfb_ingest.py` (stdlib): UDP listener on a tee
   `--tap`, opens a session on first datagram, batched commits, closes on
   Ctrl-C/idle. WAL lets the UI read while it writes.
3. **Read-only web UI** — DONE. `webui/webapp.py` (Flask) + vendored uPlot:
   session list, per-session synchronised time-series (RSSI/SNR, PER/lost,
   MCS/Tier-1), with ML labels (`predictions.tier1_state`) and human `labels`
   spans shaded over the stream. Endpoints: `/`, `/session/<id>`,
   `/api/sessions`, `/api/session/<id>/series`.
4. **Labeling tools** — DONE. Label-mode toggle (drag a chart span = select
   instead of zoom) → label form (kind/value/author) → `POST
   /api/session/<id>/labels`; a labels manager (list + delete via `DELETE
   /api/label/<id>`); session-metadata editor (`POST /api/session/<id>/meta`).
   New labels redraw as overlay bands immediately.
5. **Active learning** — DONE. `wfb_active.py`: `train` builds a labeled dataset
   from the store (rule-bootstrap base, human `state` labels override) and trains
   via `train_tier1.train_and_export`; `score` writes a model version's
   predictions over chosen sessions (latest version wins in the UI; re-score
   replaces); `gemma` runs Tier-2 over each contiguous degraded/critical span and
   writes `tier2_note` (`--suggest-labels` mines candidate `model:`-authored
   labels for human review). The cycle: label in UI → `train` → `score` →
   `gemma`/review → relabel.
