# archive/python-telemetry — retired Python telemetry capture + dashboard

These files were the **runtime** telemetry path until Phase 5 of the autonomous
mega-binary work folded them into `wfb-gs` as C
(`ground/wfb_logger.c` + `ground/gs_supervisor_telemetry.c`). They are kept here
for reference only and are **no longer launched** by `gs_supervisor`.

| File | Replaced by |
|---|---|
| `wfb_capture.py` (udp:6700 → sqlite Capture loop) | the in-process logger thread in `ground/wfb_logger.c` |
| `wfb_ingest.py` (standalone ingester CLI) | the logger thread (starts with the supervisor) |
| `webui/` (Flask app + uPlot dashboard) | `ground/gs_supervisor_telemetry.c` + `ground/webui/telemetry/` (served at `/telemetry`) |
| `webui_session.sh` (system.up launcher) | removed from `system.up`/`down`; logger is in-process |
| `capture_session.sh` (external ingester launcher) | the logger thread |

The schema and `derive_columns` denormalisation were ported **bit-for-bit**, so
existing `telemetry/wfb.sqlite` files are still read and written by the C logger
without migration.

## Still live in `telemetry/` (offline tools, NOT runtime)

`wfb_store.py` (schema/query lib + JSONL import CLI), `import_vehicle_session.{py,sh}`
(vehicle-uplink overlay import — deferred, offline), and the ML/analysis scripts
(`wfb_active.py`, `train_tier1.py`, `wfb_link_score.py`, …) remain in `telemetry/`.
They are run by hand on a workstation, never by the ground station at runtime.

## Importing an old JSONL capture without Python

The C binary now carries a one-shot importer:

```
wfb-gs telemetry-import <file.jsonl> [--db PATH] [--source TAG]
```

(`telemetry/wfb_store.py import …` still works too, for the offline workflow.)

## Note

These archived scripts assumed `wfb_store.py` was importable from one directory
up; since `wfb_store.py` stayed in `telemetry/`, the archived `wfb_capture.py` /
`webui/webapp.py` imports no longer resolve from here. That is intentional — they
are retired, not maintained.
