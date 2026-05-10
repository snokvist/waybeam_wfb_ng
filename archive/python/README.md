# archive/python/ — superseded Python implementation

These modules were the original waybeam_wfb_ng implementation: an
adaptive FEC controller and an MCS selector written in Python, with a
386-test pytest suite, an installable CLI (`python -m fec_controller`,
`python -m mcs_selector`), and a complete simulation harness.

They were **superseded on 2026-05-10** by `vehicle/link_controller.c`
which subsumes both subsystems into a single C daemon along with the
WCMD command proxy and CSA orchestration.  The C version is what runs
on the vehicle today (`/usr/bin/link_controller`); these Python modules
are not deployed.

## What's here

```
fec_controller/          # 17 modules, 3218 LOC
  protocol.py            # rtp_sidecar.h wire format (Python parser/builder)
  config.py              # ControllerConfig — all FEC tunables
  controller.py          # FECController — EWMA + gating + size-to-k/n
  headroom.py            # learned I/P size variance
  link_budget.py         # safe bitrate from MCS/BW/GI table
  service.py             # async UDP sidecar consumer
  wfb_control.py         # wfb_tx CMD_SET_FEC sender
  cli.py / __main__.py   # `python -m fec_controller {run,simulate,table}`
  simulation.py          # synthetic stream generator
  payload_sizer.py       # variable RTP payload sizing
  ...

mcs_selector/            # 10 modules, 1349 LOC
  selector.py            # MCS choice from rx_ant scoring
  scorer.py              # signal-quality cost function
  ranges.py              # PHY rate tables per BW/GI
  service.py             # async UDP rx_ant consumer
  protocol.py            # rx_ant + wfb_cmd protocol
  ...
```

## Why archive instead of delete

1. **Reference for future ports.**  The C implementation in
   `link_controller.c` reuses the algorithms (EWMA, hysteresis,
   asymmetric gating, headroom learner, MCS scoring) ported from
   here.  Anyone debugging the C version will want to compare against
   the canonical Python that was test-validated to a much higher level
   of coverage.
2. **The protocol.py modules were the test-validated source of truth
   for the wire formats.**  `tests/protocols/` (kept at the top level
   of the repo) imports and exercises these via vendored copies, so
   any future change to `shared/wcmd_proto.h` or `shared/rtp_sidecar.h`
   gets caught against a working Python parser.
3. **Simulation harness.**  `fec_controller/simulation.py` and
   `payload_benchmark.py` are runnable today (`pip install -e .` from
   `archive/python/`) and produce useful reference tables.  Useful for
   exploring policy changes before re-implementing in C.

## Running the archived tests

The full Python test suite still passes:

```bash
cd archive/python/
pip install -r requirements.txt   # if added
python -m pytest -v               # legacy controller-logic tests
```

The wire-format tests that should keep running against `shared/*.h`
were carved out into `tests/protocols/` — see that directory's
README.

## Don't deploy this

The vehicles run `link_controller.c`.  Don't `pip install` and run
this on the vehicle expecting it to coexist — both tools talk to the
same wfb_tx control socket and same venc HTTP API and would step on
each other.
