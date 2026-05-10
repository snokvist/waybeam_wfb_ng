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

.PHONY: all host cross clean deploy webui

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

clean:
	rm -f $(OUT_HOST) $(OUT_CROSS) $(WEBUI_HDR)
