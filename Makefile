# Top-level umbrella for waybeam_wfb_ng builds.
#
# Each component carries its own Makefile.  This file just composes the
# common targets so a contributor can do `make all` from the repo root
# without remembering which directory each binary lives in.

.PHONY: all ground vehicle probes wfb-ng test test-archive clean help \
        ground-host vehicle-host

# Default = build everything that's small and host-native (so a fresh
# checkout proves the C compiles before any cross toolchain is needed).
all: ground-host vehicle-host probes

# ── Ground side ─────────────────────────────────────────────────────────
# `make ground` cross-compiles for the configured CROSS_CC; falls back
# to host build if CROSS_CC isn't set (matches ground/Makefile defaults).
ground:
	$(MAKE) -C ground

ground-host:
	$(MAKE) -C ground host

# ── Vehicle side ────────────────────────────────────────────────────────
# `make vehicle` cross-builds for SigmaStar Infinity6E (armv7l) using the
# toolchain symlink at the repo root (../toolchain/...).
vehicle:
	$(MAKE) -C vehicle

vehicle-host:
	$(MAKE) -C vehicle host

# ── Dev probes ──────────────────────────────────────────────────────────
probes:
	$(MAKE) -C probes

# ── wfb-ng fork ─────────────────────────────────────────────────────────
# Run-script-driven; `make wfb-ng` invokes the armv7 build by default.
# For aarch64 / openwrt run wfb-ng/build-aarch64.sh / build-openwrt.sh
# directly.
wfb-ng:
	cd wfb-ng && ./build-armv7.sh

# ── Tests ───────────────────────────────────────────────────────────────
# Default test target is the wire-format conformance suite (fast).
# Prefer the repo's .venv interpreter so contributors don't have to
# remember to `source .venv/bin/activate` first.
PYTEST_PY = $(shell test -x .venv/bin/python && echo .venv/bin/python || echo python)

test:
	$(PYTEST_PY) -m pytest tests/protocols -v

# Archived controller-logic suite (legacy, ~328 tests).
test-archive:
	PYTHONPATH=archive/python $(PYTEST_PY) -m pytest archive/python/tests -v

# ── Cleanup ─────────────────────────────────────────────────────────────
clean:
	$(MAKE) -C ground clean
	$(MAKE) -C vehicle clean
	$(MAKE) -C probes clean

help:
	@echo "Common targets:"
	@echo "  make all              host-build ground + vehicle + probes (default)"
	@echo "  make ground           cross-build gs_supervisor"
	@echo "  make ground-host      host-build gs_supervisor"
	@echo "  make vehicle          cross-build link_controller for armv7l"
	@echo "  make vehicle-host     host-build link_controller"
	@echo "  make probes           build host-native dev probes (rtp_timing_probe)"
	@echo "  make wfb-ng           build the patched wfb-ng fork (armv7)"
	@echo "  make test             pytest tests/protocols (wire-format conformance)"
	@echo "  make test-archive     pytest archive/python/tests (legacy)"
	@echo "  make clean            wipe per-component build/ trees"
