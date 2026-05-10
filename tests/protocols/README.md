# tests/protocols/ — wire-format conformance tests

Active regression net for the wire formats both daemons depend on:

- `shared/wcmd_proto.h` — WCMD command channel (GS → vehicle)
- `shared/rtp_sidecar.h` — venc per-frame metadata (vehicle internal)
- wfb-ng rx_ant JSON schema — wfb_rx `-Y` UDP push
- wfb-ng wfb_cmd binary protocol — `wfb_tx -C` control socket

These tests validate that any change to those wire formats round-trips
through a Python parser/builder cleanly — i.e. they catch packed-struct
sizing mistakes, byte-order regressions, and field-rename drift before
the C code ships them onto a live link.

## What's here

```
tests/protocols/
├── _proto/                # vendored Python parsers (frozen copies)
│   ├── fec_protocol.py    # rtp_sidecar.h equivalent
│   └── mcs_protocol.py    # rx_ant + wfb_cmd equivalent
├── test_fec_protocol.py   # 30 tests (was tests/test_protocol.py)
└── test_mcs_protocol.py   # 28 tests (was tests/test_mcs_protocol.py)
```

The `_proto/` modules are **frozen copies** of
`archive/python/{fec_controller,mcs_selector}/protocol.py` taken on
2026-05-10.  We don't import them from `archive/` because we want
these tests to be self-contained and survive any future cleanup of
the archive.

## Running

```bash
source .venv/bin/activate
pip install pytest          # only dependency
python -m pytest tests/protocols -v
```

## Updating after a wire-format change

If `shared/wcmd_proto.h` or `shared/rtp_sidecar.h` change:

1. Adjust `tests/protocols/_proto/{fec,mcs}_protocol.py` to match.
2. Add a test for the new field/value/struct in the corresponding
   `test_*_protocol.py`.
3. Verify the C code in `vehicle/link_controller.c` and
   `ground/gs_supervisor.c` produces bytes that round-trip through
   the updated Python parser.

## What this is NOT

These tests do **not** exercise the C daemons themselves — they only
cover the Python parser of the same wire format.  Black-box tests
that drive a live `gs_supervisor` and verify `link_controller`
behaviour live elsewhere (or, today, are run manually via the
verification matrix in the gs_supervisor PR).
