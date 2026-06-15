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
             gs_supervisor_http.c
HDRS       = gs_supervisor.h
WEBUI_SRC  = webui/gs.html
WEBUI_HDR  = webui/gs_assets.h
OUT_HOST   = $(BUILD_DIR)/gs_supervisor.host
OUT_CROSS ?= $(BUILD_DIR)/gs_supervisor

# `shared/` carries WCMD wire-format and other cross-side headers.
SHARED_INC = -I../shared

CFLAGS_COMMON  = -std=gnu99 -O2 -Wall -Wextra -Wno-missing-field-initializers
LDFLAGS_COMMON =

CC       ?= cc
CROSS_CC ?=

DEPLOY_HOST ?= 192.168.2.20
DEPLOY_DIR  ?= /tmp

.PHONY: all host cross clean deploy webui mega

all: host

host: $(OUT_HOST)

webui: $(WEBUI_HDR)

# xxd -i embeds the file as `unsigned char gs_webui_html[]` with a matching
# `_len`. Promote both to const so the C side can put them in .rodata.
# The C source still does `#if __has_include("gs_webui_assets.h")`; we keep
# that filename for compatibility but generate it under webui/ now.
$(WEBUI_HDR): $(WEBUI_SRC)
	@echo "[gs_supervisor] embedding $(WEBUI_SRC) -> $@"
	@xxd -i -n gs_webui_html $< > $@
	@sed -i 's/^unsigned char gs_webui_html/const unsigned char gs_webui_html/' $@
	@sed -i 's/^unsigned int gs_webui_html_len/const unsigned int gs_webui_html_len/' $@

$(BUILD_DIR):
	@mkdir -p $@

$(OUT_HOST): $(SRCS) $(HDRS) $(WEBUI_HDR) | $(BUILD_DIR)
	@echo "[gs_supervisor] host -> $@"
	$(CC) $(CFLAGS_COMMON) $(SHARED_INC) -I. -Iwebui -g -o $@ $(SRCS) $(LDFLAGS_COMMON)

cross: $(OUT_CROSS)

$(OUT_CROSS): $(SRCS) $(HDRS) $(WEBUI_HDR) | $(BUILD_DIR)
	@if [ -z "$(CROSS_CC)" ]; then \
		echo "set CROSS_CC=<path-to-target-gcc>"; exit 2; \
	fi
	@echo "[gs_supervisor] cross ($(CROSS_CC)) -> $@"
	$(CROSS_CC) $(CFLAGS_COMMON) $(SHARED_INC) -I. -Iwebui -o $@ $(SRCS) $(LDFLAGS_COMMON)

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

# C daemon objects (supervisor) — built as C with the multicall guard.
MEGA_GS_OBJ = $(MEGA_OBJ)/gs_supervisor.o $(MEGA_OBJ)/gs_supervisor_csa.o \
              $(MEGA_OBJ)/gs_supervisor_scan.o $(MEGA_OBJ)/gs_supervisor_http.o
# Dispatcher + applet table (C++).
MEGA_DISP_OBJ = $(MEGA_OBJ)/wfb_multicall.o $(MEGA_OBJ)/gs_applets.o
# wfb-ng tool objects. The four with a main() are renamed to *_main via
# -Dmain=; the rest carry no main and are compiled plain.
MEGA_WFBNG_OBJ = $(MEGA_OBJ)/rx.o $(MEGA_OBJ)/tx.o $(MEGA_OBJ)/tx_cmd.o \
                 $(MEGA_OBJ)/keygen.o $(MEGA_OBJ)/peek.o $(MEGA_OBJ)/zfex.o \
                 $(MEGA_OBJ)/wifibroadcast.o $(MEGA_OBJ)/radiotap.o \
                 $(MEGA_OBJ)/venc_ring.o

mega: $(MEGA_OUT)

$(MEGA_OBJ):
	@mkdir -p $@

# --- supervisor (C, multicall) ---
$(MEGA_OBJ)/%.o: %.c $(HDRS) $(WEBUI_HDR) | $(MEGA_OBJ)
	$(CC) $(CFLAGS_COMMON) $(MEGA_DEFS) $(MEGA_CFLAGS) $(SHARED_INC) -I. -Iwebui -c -o $@ $<

# --- dispatcher + applet table (C++) ---
$(MEGA_OBJ)/wfb_multicall.o: $(MULTICALL)/wfb_multicall.cpp $(MULTICALL)/wfb_multicall.h | $(MEGA_OBJ)
	$(CXX) -std=gnu++11 -O2 -Wall $(MEGA_DEFS) $(MEGA_CFLAGS) -I$(MULTICALL) -c -o $@ $<
$(MEGA_OBJ)/gs_applets.o: gs_applets.cpp $(MULTICALL)/wfb_multicall.h | $(MEGA_OBJ)
	$(CXX) -std=gnu++11 -O2 -Wall $(MEGA_DEFS) $(MEGA_CFLAGS) -I$(MULTICALL) -c -o $@ $<

# --- wfb-ng tools (main() renamed to applet entry via -Dmain=) ---
$(MEGA_OBJ)/rx.o:     $(WFBNG_SRC)/rx.cpp           | $(MEGA_OBJ)
	$(CXX) -std=gnu++11 -O2 -Wall -fno-strict-aliasing $(MEGA_WFB_DEFS) -Dmain=wfb_rx_main     $(MEGA_CFLAGS) -c -o $@ $<
$(MEGA_OBJ)/tx.o:     $(WFBNG_SRC)/tx.cpp           | $(MEGA_OBJ)
	$(CXX) -std=gnu++11 -O2 -Wall -fno-strict-aliasing $(MEGA_WFB_DEFS) -Dmain=wfb_tx_main     $(MEGA_CFLAGS) -c -o $@ $<
$(MEGA_OBJ)/tx_cmd.o: $(WFBNG_SRC)/tx_cmd.c         | $(MEGA_OBJ)
	$(CC)  -std=gnu99   -O2 -Wall -fno-strict-aliasing $(MEGA_WFB_DEFS) -Dmain=wfb_tx_cmd_main $(MEGA_CFLAGS) -c -o $@ $<
$(MEGA_OBJ)/keygen.o: $(WFBNG_SRC)/keygen.c         | $(MEGA_OBJ)
	$(CC)  -std=gnu99   -O2 -Wall -fno-strict-aliasing $(MEGA_WFB_DEFS) -Dmain=wfb_keygen_main $(MEGA_CFLAGS) -c -o $@ $<
$(MEGA_OBJ)/peek.o:           $(WFBNG_SRC)/peek.cpp          | $(MEGA_OBJ)
	$(CXX) -std=gnu++11 -O2 -Wall -fno-strict-aliasing $(MEGA_WFB_DEFS) $(MEGA_CFLAGS) -c -o $@ $<
$(MEGA_OBJ)/wifibroadcast.o:  $(WFBNG_SRC)/wifibroadcast.cpp | $(MEGA_OBJ)
	$(CXX) -std=gnu++11 -O2 -Wall -fno-strict-aliasing $(MEGA_WFB_DEFS) $(MEGA_CFLAGS) -c -o $@ $<
$(MEGA_OBJ)/zfex.o:           $(WFBNG_SRC)/zfex.c            | $(MEGA_OBJ)
	$(CC)  -std=gnu99   -O2 -Wall -fno-strict-aliasing $(MEGA_WFB_DEFS) $(MEGA_CFLAGS) -c -o $@ $<
$(MEGA_OBJ)/radiotap.o:       $(WFBNG_SRC)/radiotap.c        | $(MEGA_OBJ)
	$(CC)  -std=gnu99   -O2 -Wall -fno-strict-aliasing $(MEGA_WFB_DEFS) $(MEGA_CFLAGS) -c -o $@ $<
$(MEGA_OBJ)/venc_ring.o:      $(WFBNG_SRC)/venc_ring.c       | $(MEGA_OBJ)
	$(CC)  -std=gnu99   -O2 -Wall -fno-strict-aliasing $(MEGA_WFB_DEFS) $(MEGA_CFLAGS) -c -o $@ $<

$(MEGA_OUT): $(MEGA_GS_OBJ) $(MEGA_DISP_OBJ) $(MEGA_WFBNG_OBJ)
	@echo "[wfb-gs] linking mega binary -> $@"
	$(CXX) $(MEGA_CFLAGS) -o $@ $^ $(MEGA_LDFLAGS) $(LDFLAGS_COMMON)
	@echo "[wfb-gs] done: applets = supervisor rx tx tx_cmd keygen"

clean:
	rm -f $(OUT_HOST) $(OUT_CROSS) $(WEBUI_HDR)
	rm -rf $(MEGA_OBJ) $(MEGA_OUT)
