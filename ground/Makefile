# Build gs_supervisor — the ground-side wfb supervisor.
#
# Run from waybeam_wfb_ng/ground/:
#   make                    # host build (debug, x86)
#   make cross CROSS_CC=... # cross build for the target
#   make deploy             # scp cross binary to DEPLOY_HOST
#   make clean
#
# Cross build examples:
#   make cross \
#        CROSS_CC=../toolchain/aarch64/bin/aarch64-openwrt-linux-gcc \
#        OUT_CROSS=build/gs_supervisor.aarch64
#   make cross \
#        CROSS_CC=../toolchain/openwrt-mips/bin/mips-openwrt-linux-gcc \
#        OUT_CROSS=build/gs_supervisor.mipsel

BUILD_DIR ?= build
SRCS       = gs_supervisor.c \
             gs_supervisor_csa.c \
             gs_supervisor_scan.c \
             gs_supervisor_http.c \
             gs_supervisor_telemetry.c \
             wfb_logger.c
HDRS       = gs_supervisor.h wfb_logger.h
WEBUI_SRC  = webui/gs.html
WEBUI_HDR  = webui/gs_assets.h
# Telemetry dashboard assets (Phase 5): embedded the same way as gs.html.
TELE_DIR   = webui/telemetry
TELE_HDR   = webui/telemetry_assets.h
TELE_SRCS  = $(TELE_DIR)/index.html $(TELE_DIR)/session.html $(TELE_DIR)/app.js \
             $(TELE_DIR)/app.css $(TELE_DIR)/uPlot.iife.min.js $(TELE_DIR)/uPlot.min.css
OUT_HOST   = $(BUILD_DIR)/gs_supervisor.host
OUT_CROSS ?= $(BUILD_DIR)/gs_supervisor

# `shared/` carries WCMD wire-format and other cross-side headers.
SHARED_INC = -I../shared

# Vendored SQLite amalgamation (Phase 5 telemetry logger). Compiled ONCE as a
# standalone object (9 MB source; never in SRCS) and linked into both the
# standalone and mega builds. NOT sodium-gated — the logger ships in every
# ground build. THREADSAFE=2 = multi-thread (logger thread writes; HTTP handlers
# read/write on their own connections).
SQLITE_DIR       = vendor/sqlite
SQLITE_INC       = -I$(SQLITE_DIR)
SQLITE_DEFS      = -DSQLITE_THREADSAFE=2 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0 \
                   -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_DEPRECATED
SQLITE_OBJ_HOST  = $(BUILD_DIR)/sqlite3.host.o
SQLITE_OBJ_CROSS = $(BUILD_DIR)/sqlite3.cross.o
# logger needs pthreads (capture thread) + libm (sqlite).
GS_LDLIBS        = -lpthread -lm

CFLAGS_COMMON  = -std=gnu99 -O2 -Wall -Wextra -Wno-missing-field-initializers
LDFLAGS_COMMON =

CC       ?= cc
CROSS_CC ?=

DEPLOY_HOST ?= 192.168.2.20
DEPLOY_DIR  ?= /tmp

.PHONY: all host cross clean deploy webui mega

all: host

host: $(OUT_HOST)

webui: $(WEBUI_HDR) $(TELE_HDR)

# xxd -i embeds the file as `unsigned char gs_webui_html[]` with a matching
# `_len`. Promote both to const so the C side can put them in .rodata.
# The C source still does `#if __has_include("gs_webui_assets.h")`; we keep
# that filename for compatibility but generate it under webui/ now.
$(WEBUI_HDR): $(WEBUI_SRC)
	@echo "[gs_supervisor] embedding $(WEBUI_SRC) -> $@"
	@xxd -i -n gs_webui_html $< > $@
	@sed -i 's/^unsigned char gs_webui_html/const unsigned char gs_webui_html/' $@
	@sed -i 's/^unsigned int gs_webui_html_len/const unsigned int gs_webui_html_len/' $@

# Telemetry dashboard: embed each asset (html/js/css + uPlot vendor) as a
# distinct const symbol, all in one header. `static const` so the consumer
# (gs_supervisor_telemetry.c) is the sole owner — no link conflicts.
$(TELE_HDR): $(TELE_SRCS)
	@echo "[gs_supervisor] embedding telemetry assets -> $@"
	@{ \
	  xxd -i -n tele_index_html   $(TELE_DIR)/index.html; \
	  xxd -i -n tele_session_html $(TELE_DIR)/session.html; \
	  xxd -i -n tele_app_js       $(TELE_DIR)/app.js; \
	  xxd -i -n tele_app_css      $(TELE_DIR)/app.css; \
	  xxd -i -n tele_uplot_js     $(TELE_DIR)/uPlot.iife.min.js; \
	  xxd -i -n tele_uplot_css    $(TELE_DIR)/uPlot.min.css; \
	} | sed -e 's/^unsigned char/static const unsigned char/' \
	        -e 's/^unsigned int/static const unsigned int/' > $@

$(BUILD_DIR):
	@mkdir -p $@

# SQLite amalgamation -> object (vendored; -w because it is not our code).
$(SQLITE_OBJ_HOST): $(SQLITE_DIR)/sqlite3.c | $(BUILD_DIR)
	@echo "[gs_supervisor] sqlite3 (host) -> $@"
	$(CC) -O2 -w $(SQLITE_DEFS) -c -o $@ $<

$(OUT_HOST): $(SRCS) $(HDRS) $(WEBUI_HDR) $(TELE_HDR) $(SQLITE_OBJ_HOST) | $(BUILD_DIR)
	@echo "[gs_supervisor] host -> $@"
	$(CC) $(CFLAGS_COMMON) $(SHARED_INC) $(SQLITE_INC) -I. -Iwebui -g -o $@ $(SRCS) $(SQLITE_OBJ_HOST) $(LDFLAGS_COMMON) $(GS_LDLIBS)

cross: $(OUT_CROSS)

$(OUT_CROSS): $(SRCS) $(HDRS) $(WEBUI_HDR) $(TELE_HDR) | $(BUILD_DIR)
	@if [ -z "$(CROSS_CC)" ]; then \
		echo "set CROSS_CC=<path-to-target-gcc>"; exit 2; \
	fi
	@echo "[gs_supervisor] sqlite3 (cross) -> $(SQLITE_OBJ_CROSS)"
	@test -f $(SQLITE_OBJ_CROSS) || $(CROSS_CC) -O2 -w $(SQLITE_DEFS) -c -o $(SQLITE_OBJ_CROSS) $(SQLITE_DIR)/sqlite3.c
	@echo "[gs_supervisor] cross ($(CROSS_CC)) -> $@"
	$(CROSS_CC) $(CFLAGS_COMMON) $(SHARED_INC) $(SQLITE_INC) -I. -Iwebui -o $@ $(SRCS) $(SQLITE_OBJ_CROSS) $(LDFLAGS_COMMON) $(GS_LDLIBS)

deploy: $(OUT_CROSS)
	scp $(OUT_CROSS) root@$(DEPLOY_HOST):$(DEPLOY_DIR)/

# ── Mega binary (multi-call) ─────────────────────────────────────────
# Single `wfb-gs` bundling supervisor + wfb_rx/wfb_tx/wfb_tx_cmd/wfb_keygen
# as applets (busybox-style). See docs/design/mega-binary.md.
#
# Inputs you must supply (the wfb-ng tools are a vendored upstream fork,
# fetched+patched by wfb-ng/build-*.sh, and need libsodium + libpcap):
#   WFBNG_SRC    patched wfb-ng src/ dir (e.g. ../wfb-ng/build/wfb-ng/src)
#   CC / CXX     target C / C++ compilers (host cc/g++ by default)
#   MEGA_CFLAGS  extra compile flags (e.g. --sysroot=..., -I<sodium>/include)
#   MEGA_LDFLAGS link flags/libs (default: -lpcap -lsodium)
#
# Example (aarch64 ground, after running wfb-ng/build-aarch64.sh once):
#   make mega \
#     WFBNG_SRC=../wfb-ng/build/wfb-ng/src \
#     CC=aarch64-none-linux-gnu-gcc CXX=aarch64-none-linux-gnu-g++ \
#     MEGA_CFLAGS="--sysroot=$$SYSROOT -I$$SODIUM/include" \
#     MEGA_LDFLAGS="--sysroot=$$SYSROOT -lpcap -lsodium -lrt -lpthread"
CXX          ?= g++
# C++ cross compiler for the mega build. Derived from CROSS_CC if not given
# (the gcc->g++ swap covers the usual toolchain names); override if it doesn't fit.
CROSS_CXX    ?= $(CROSS_CC:gcc=g++)
WFBNG_SRC    ?= ../wfb-ng/build/wfb-ng/src
MEGA_OUT     ?= $(BUILD_DIR)/wfb-gs
MEGA_OBJ      = $(BUILD_DIR)/mega
MEGA_DEFS     = -DWFB_MULTICALL -DWFB_WITH_WFBNG
MEGA_CFLAGS  ?=
MEGA_LDFLAGS ?= -lpcap -lsodium
MULTICALL    = ../multicall
# Defines the vendored wfb-ng sources expect. Mirror the standalone build's
# WFB_VERSION; pass arch-specific -DZFEX_* SIMD tuning via MEGA_CFLAGS if
# wanted (zfex.c falls back to a portable path when they are absent).
MEGA_WFB_VERSION ?= shm-patched-mega
MEGA_WFB_DEFS     = -DWFB_VERSION=\"$(MEGA_WFB_VERSION)\"

# Mega build compilers: use the cross pair when CROSS_CC is set (matching the
# `make cross` convention), else host CC/CXX. Without this, `make cross
# CROSS_CC=aarch64-…` followed by `make mega` would silently link a host-arch
# wfb-gs. Drives both wfb-ng/mega.mk and the ground-local rules below.
ifeq ($(strip $(CROSS_CC)),)
MEGA_CC    := $(CC)
MEGA_CXX   := $(CXX)
MEGA_STRIP ?= strip
else
MEGA_CC    := $(CROSS_CC)
MEGA_CXX   := $(CROSS_CXX)
MEGA_STRIP ?= $(CROSS_CC:gcc=strip)
endif
# C daemon objects (supervisor) — built as C with the multicall guard.
# wfb_logger.o + sqlite3.o ship in the mega binary too (telemetry logger).
MEGA_GS_OBJ = $(MEGA_OBJ)/gs_supervisor.o $(MEGA_OBJ)/gs_supervisor_csa.o \
              $(MEGA_OBJ)/gs_supervisor_scan.o $(MEGA_OBJ)/gs_supervisor_http.o \
              $(MEGA_OBJ)/gs_supervisor_telemetry.o \
              $(MEGA_OBJ)/wfb_logger.o $(MEGA_OBJ)/sqlite3.o
# Dispatcher (wfb_multicall.o from mega.mk) + ground applet table.
MEGA_DISP_OBJ = $(MEGA_OBJ)/wfb_multicall.o $(MEGA_OBJ)/gs_applets.o

# Shared wfb-ng tool + dispatcher object rules (defines MEGA_WFBNG_OBJ).
include ../wfb-ng/mega.mk

mega: $(MEGA_OUT)

# --- supervisor (C, multicall) ---
# -I$(MULTICALL) lets the WFB_WITH_WFBNG key-management endpoint reach wfb_keyseed.h;
# -I$(SQLITE_DIR) lets wfb_logger.o find the vendored sqlite3.h.
$(MEGA_OBJ)/%.o: %.c $(HDRS) $(WEBUI_HDR) $(TELE_HDR) | $(MEGA_OBJ)
	$(MEGA_CC) $(CFLAGS_COMMON) $(MEGA_DEFS) $(MEGA_CFLAGS) $(SHARED_INC) $(SQLITE_INC) -I$(MULTICALL) -I. -Iwebui -c -o $@ $<

# --- vendored SQLite amalgamation (mega; host or cross per MEGA_CC) ---
$(MEGA_OBJ)/sqlite3.o: $(SQLITE_DIR)/sqlite3.c | $(MEGA_OBJ)
	@echo "[wfb-gs] sqlite3 -> $@"
	$(MEGA_CC) -O2 -w $(SQLITE_DEFS) $(MEGA_CFLAGS) -c -o $@ $<

# --- ground applet table (C++) ---
$(MEGA_OBJ)/gs_applets.o: gs_applets.cpp $(MULTICALL)/wfb_multicall.h $(MULTICALL)/wfb_applets_common.h $(MULTICALL)/wfb_keyseed.h $(MULTICALL)/wfb_configenv.h | $(MEGA_OBJ)
	$(MEGA_CXX) -std=gnu++11 -O2 -Wall $(MEGA_DEFS) $(MEGA_CFLAGS) -I$(MULTICALL) -c -o $@ $<

$(MEGA_OUT): $(MEGA_GS_OBJ) $(MEGA_DISP_OBJ) $(MEGA_WFBNG_OBJ)
	@echo "[wfb-gs] linking mega binary -> $@"
	$(MEGA_CXX) $(MEGA_CFLAGS) -o $@ $^ $(MEGA_LDFLAGS) $(LDFLAGS_COMMON) $(GS_LDLIBS)
	$(MEGA_STRIP) $@
	@echo "[wfb-gs] done (stripped): applets = supervisor rx tx tx_cmd keygen telemetry-import"

clean:
	rm -f $(OUT_HOST) $(OUT_CROSS) $(WEBUI_HDR) $(TELE_HDR) $(SQLITE_OBJ_HOST) $(SQLITE_OBJ_CROSS)
	rm -rf $(MEGA_OBJ) $(MEGA_OUT)
