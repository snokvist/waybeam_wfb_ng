# Cross-compile csa_agent and udp_ts_recv for the bench targets.
#
# Defaults assume the host has an armhf cross-gcc on PATH and an OpenWrt
# ath79 staging_dir reachable via OPENWRT_STAGING. Override on the command
# line if your toolchains live elsewhere:
#
#   make ARM_GCC=arm-linux-gnueabihf-gcc \
#        MIPS_GCC=mips-openwrt-linux-musl-gcc \
#        OPENWRT_STAGING=/path/to/openwrt/staging_dir/toolchain-mips_24kc_*
#
# Targets:
#   make armhf      - csa_agent.armhf, udp_ts_recv.armhf  (vehicle, sigmastar)
#   make mips24kc   - csa_agent.mips24kc                   (cpe510, ath79)
#   make all        - both
#   make clean

ARM_GCC ?= arm-linux-gnueabihf-gcc
MIPS_GCC ?= mips-openwrt-linux-musl-gcc

CFLAGS_COMMON := -static -Os -Wall -Wextra

.PHONY: all armhf mips24kc clean
all: armhf mips24kc

armhf: csa_agent.armhf udp_ts_recv.armhf
mips24kc: csa_agent.mips24kc

csa_agent.armhf: csa_agent.c
	$(ARM_GCC) $(CFLAGS_COMMON) -o $@ $<

udp_ts_recv.armhf: udp_ts_recv.c
	$(ARM_GCC) $(CFLAGS_COMMON) -o $@ $<

csa_agent.mips24kc: csa_agent.c
	$(MIPS_GCC) $(CFLAGS_COMMON) -o $@ $<

clean:
	rm -f csa_agent.armhf csa_agent.mips24kc udp_ts_recv.armhf
