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

.PHONY: all webui clean deploy host help test

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

clean:
	rm -f $(OUT) $(OUT_HOST) $(WEBUI_HDR)

deploy: $(OUT)
	scp -O $(OUT) root@$(DEPLOY_HOST):$(DEPLOY_DIR)/
	@echo "[link] deployed to root@$(DEPLOY_HOST):$(DEPLOY_DIR)/link_controller"

help:
	@echo "make             # cross-build for armv7l (incl. WebUI)"
	@echo "make webui       # regenerate embedded WebUI header"
	@echo "make host        # native host build"
	@echo "make deploy      # scp to root@\$$DEPLOY_HOST"
	@echo "make clean"
