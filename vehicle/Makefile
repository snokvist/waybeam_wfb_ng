# Build link_controller — the vehicle-side wfb adaptive FEC + WCMD/CSA daemon.
#
# Run from waybeam_wfb_ng/vehicle/:
#   make             # cross-build for SigmaStar Infinity6E (armv7l), incl. WebUI
#   make webui       # regenerate the embedded WebUI header
#   make host        # native host build (debug, x86)
#   make deploy      # scp the cross binary to DEPLOY_HOST
#   make clean
#
# Env:
#   DEPLOY_HOST=192.168.1.13   target armv7l vehicle IP (default)
#   DEPLOY_DIR=/tmp            destination dir

TOOLCHAIN   ?= ../toolchain/toolchain.sigmastar-infinity6e
CROSS_CC    ?= $(TOOLCHAIN)/bin/arm-openipc-linux-gnueabihf-gcc
CROSS_CXX   ?= $(TOOLCHAIN)/bin/arm-openipc-linux-gnueabihf-g++
CROSS_STRIP ?= $(TOOLCHAIN)/bin/arm-openipc-linux-gnueabihf-strip

BUILD_DIR   ?= build
OUT          = $(BUILD_DIR)/link_controller
OUT_HOST     = $(BUILD_DIR)/link_controller.host
SRC          = link_controller.c csa/csa.c
CSA_HDR      = csa/csa.h
WEBUI_SRC    = webui/index.html
WEBUI_HDR    = webui/index_assets.h

# `shared/` carries WCMD wire-format and other cross-side headers.
SHARED_INC   = -I../shared

CFLAGS_COMMON = -std=gnu99 -O2 -Wall -Wextra -Wno-missing-field-initializers
LDFLAGS_COMMON = -lm

DEPLOY_HOST ?= 192.168.1.13
DEPLOY_DIR  ?= /tmp

.PHONY: all webui clean deploy host help test mega

all: $(OUT)

webui: $(WEBUI_HDR)

# xxd -i embeds the file as `unsigned char webui_index_html[]` with a
# matching `_len`. We rename to `webui_html[]` / `webui_html_len` so the C
# code doesn't depend on the input filename.
$(WEBUI_HDR): $(WEBUI_SRC)
	@echo "[link] embedding $(WEBUI_SRC) -> $@"
	@xxd -i -n webui_html $< > $@
	@# xxd uses "unsigned int <name>_len = N;" — promote to const so the
	@# C side can declare it `const unsigned int`.
	@sed -i 's/^unsigned char webui_html/const unsigned char webui_html/' $@
	@sed -i 's/^unsigned int webui_html_len/const unsigned int webui_html_len/' $@

$(BUILD_DIR):
	@mkdir -p $@

$(OUT): $(SRC) $(CSA_HDR) $(WEBUI_HDR) | $(BUILD_DIR)
	$(CROSS_CC) $(CFLAGS_COMMON) $(SHARED_INC) -I. -Icsa -Iwebui -o $@ $(SRC) $(LDFLAGS_COMMON)
	$(CROSS_STRIP) $@
	@echo "[link] built $@"
	@ls -la $@

host: $(OUT_HOST)

$(OUT_HOST): $(SRC) $(CSA_HDR) $(WEBUI_HDR) | $(BUILD_DIR)
	cc $(CFLAGS_COMMON) $(SHARED_INC) -I. -Icsa -Iwebui -g -o $@ $(SRC) $(LDFLAGS_COMMON)
	@echo "[link] host build: $@"

# Host unit test for the MCS control law. test_selector_law.c #includes
# link_controller.c with -DLC_TEST_NO_MAIN (so its main() is excluded) and
# drives selector_update() directly; csa/csa.c is linked for the file-scope
# CSA refs the daemon carries.
TEST_SELECTOR = $(BUILD_DIR)/test_selector_law
test: $(TEST_SELECTOR)
	@$(TEST_SELECTOR)

$(TEST_SELECTOR): ../tests/test_selector_law.c $(SRC) $(CSA_HDR) $(WEBUI_HDR) | $(BUILD_DIR)
	cc $(CFLAGS_COMMON) -Wno-unused-function -DLC_TEST_NO_MAIN $(SHARED_INC) \
		-I. -Icsa -Iwebui -g \
		-o $@ ../tests/test_selector_law.c csa/csa.c $(LDFLAGS_COMMON)

# ── Mega binary (multi-call) ─────────────────────────────────────────
# Single `wfb-air` bundling link_controller + wfb_tx/wfb_rx/wfb_tx_cmd/
# wfb_keygen as applets (busybox-style). See docs/design/mega-binary.md.
#
# Inputs you must supply (the wfb-ng tools are a vendored upstream fork,
# fetched+patched by wfb-ng/build-armv7.sh, and need libsodium + libpcap):
#   WFBNG_SRC    patched wfb-ng src/ dir (e.g. ../wfb-ng/build/wfb-ng/src)
#   CROSS_CC / CROSS_CXX   target C / C++ compilers (armv7l by default)
#   MEGA_CFLAGS  extra compile flags (e.g. -I<sodium>/include, -I<venc>/include)
#   MEGA_LDFLAGS link flags/libs (default: -lpcap -lsodium -lm)
#
# Defaults below point at the cross libsodium/libpcap that wfb-ng/build-armv7.sh
# installs, so `make mega` works out of the box after that script has run once.
# Override MEGA_CFLAGS/MEGA_LDFLAGS for a different sysroot (e.g. host-arch).
WFBNG_SRC    ?= ../wfb-ng/build/wfb-ng/src
SODIUM_DIR   ?= ../wfb-ng/build/sodium-install
PCAP_DIR     ?= ../wfb-ng/build/pcap-install
MEGA_OUT     ?= $(BUILD_DIR)/wfb-air
MEGA_OBJ      = $(BUILD_DIR)/mega
MEGA_DEFS     = -DWFB_MULTICALL -DWFB_WITH_WFBNG
MEGA_CFLAGS  ?= -I$(SODIUM_DIR)/include -I$(PCAP_DIR)/include
MEGA_LDFLAGS ?= -L$(SODIUM_DIR)/lib -L$(PCAP_DIR)/lib -lpcap -lsodium -lm
MULTICALL    = ../multicall
MEGA_WFB_VERSION ?= shm-patched-mega
MEGA_WFB_DEFS     = -DWFB_VERSION=\"$(MEGA_WFB_VERSION)\"

# Cross compilers drive the mega rules in wfb-ng/mega.mk.
MEGA_CC  := $(CROSS_CC)
MEGA_CXX := $(CROSS_CXX)
# link_controller + CSA (C, multicall) | dispatcher + air applet table (C++).
MEGA_LC_OBJ   = $(MEGA_OBJ)/link_controller.o $(MEGA_OBJ)/csa.o
MEGA_DISP_OBJ = $(MEGA_OBJ)/wfb_multicall.o $(MEGA_OBJ)/air_applets.o

# Shared wfb-ng tool + dispatcher object rules (defines MEGA_WFBNG_OBJ).
include ../wfb-ng/mega.mk

mega: $(MEGA_OUT)

# --- link_controller + csa (C, multicall) ---
$(MEGA_OBJ)/link_controller.o: link_controller.c $(CSA_HDR) $(WEBUI_HDR) | $(MEGA_OBJ)
	$(CROSS_CC) $(CFLAGS_COMMON) $(MEGA_DEFS) $(MEGA_CFLAGS) $(SHARED_INC) -I. -Icsa -Iwebui -c -o $@ $<
$(MEGA_OBJ)/csa.o: csa/csa.c $(CSA_HDR) | $(MEGA_OBJ)
	$(CROSS_CC) $(CFLAGS_COMMON) $(MEGA_DEFS) $(MEGA_CFLAGS) $(SHARED_INC) -I. -Icsa -c -o $@ $<

# --- air applet table (C++) ---
$(MEGA_OBJ)/air_applets.o: air_applets.cpp $(MULTICALL)/wfb_multicall.h | $(MEGA_OBJ)
	$(CROSS_CXX) -std=gnu++11 -O2 -Wall $(MEGA_DEFS) $(MEGA_CFLAGS) -I$(MULTICALL) -c -o $@ $<

$(MEGA_OUT): $(MEGA_LC_OBJ) $(MEGA_DISP_OBJ) $(MEGA_WFBNG_OBJ)
	@echo "[wfb-air] linking mega binary -> $@"
	$(CROSS_CXX) $(MEGA_CFLAGS) -o $@ $^ $(MEGA_LDFLAGS)
	$(CROSS_STRIP) $@
	@echo "[wfb-air] done: applets = link tx rx tx_cmd keygen"

clean:
	rm -f $(OUT) $(OUT_HOST) $(WEBUI_HDR)
	rm -rf $(MEGA_OBJ) $(MEGA_OUT)

deploy: $(OUT)
	scp -O $(OUT) root@$(DEPLOY_HOST):$(DEPLOY_DIR)/
	@echo "[link] deployed to root@$(DEPLOY_HOST):$(DEPLOY_DIR)/link_controller"

help:
	@echo "make             # cross-build for armv7l (incl. WebUI)"
	@echo "make webui       # regenerate embedded WebUI header"
	@echo "make host        # native host build"
	@echo "make mega        # single wfb-air multi-call binary (needs WFBNG_SRC)"
	@echo "make deploy      # scp to root@\$$DEPLOY_HOST"
	@echo "make clean"
