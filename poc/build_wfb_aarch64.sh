#!/bin/bash
#
# Cross-build wfb-ng (patched: SHM input + cooperative RX diversity +
# plaintext mode) for aarch64 ground stations — primary target is the
# Radxa Zero 3E running the sbc-groundstations Buildroot image.
#
# Reuses the same patched wfb-ng tree that build_wfb_tx.sh prepares
# (poc/build/wfb-ng/) and links dynamically against Buildroot's
# libpcap / libsodium / libevent staging libraries.
#
# Usage:
#   ./build_wfb_aarch64.sh            # build all
#   ./build_wfb_aarch64.sh --clean    # remove build/aarch64/
#   ./build_wfb_aarch64.sh --deploy   # build + scp to DEPLOY_HOST
#
# Output (all in build/aarch64/):
#   wfb_tx       - patched wfb_tx with -H (SHM), -x, -Y flags
#   wfb_rx       - patched wfb_rx with cooperative RX diversity +
#                  plaintext mode + JSON stats with diversity / adapters
#   wfb_tx_cmd   - runtime control client (set_fec [-T fec_timeout_ms],
#                  set_radio, get_fec, get_radio)
#   wfb_keygen   - key generator
#
# Requirements:
#   - sbc-groundstations checked out at ../../sbc-groundstations/ with the
#     waybeam_radxa3e_defconfig already built once (provides toolchain +
#     staging sysroot with libpcap, libsodium, libevent)
#   - Patched wfb-ng tree prepared by build_wfb_tx.sh (run that first or
#     set BOOTSTRAP_FROM=fresh below)
#
# Env:
#   DEPLOY_HOST=192.168.2.20    target Radxa3 IP (default)
#   DEPLOY_DIR=/usr/bin         destination dir
#   SBC_DEFCONFIG=waybeam_radxa3e_defconfig
#   STATIC=1                    link statically against libsodium (default 0)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WFB_ROOT="$(dirname "$SCRIPT_DIR")"
COORD_ROOT="$(dirname "$WFB_ROOT")"
BUILD_DIR="$SCRIPT_DIR/build"
OUT_DIR="$BUILD_DIR/aarch64"

DEPLOY_HOST="${DEPLOY_HOST:-192.168.2.20}"
DEPLOY_DIR="${DEPLOY_DIR:-/usr/bin}"
SBC_DEFCONFIG="${SBC_DEFCONFIG:-waybeam_radxa3e_defconfig}"

# ── Toolchain (Buildroot host SDK from sbc-groundstations) ──────────

SBC_OUT="$COORD_ROOT/sbc-groundstations/output/$SBC_DEFCONFIG"
HOST_DIR="$SBC_OUT/host"
SYSROOT="$HOST_DIR/aarch64-buildroot-linux-gnu/sysroot"

CROSS_PREFIX="aarch64-none-linux-gnu"
CROSS_CC="$HOST_DIR/bin/${CROSS_PREFIX}-gcc"
CROSS_CXX="$HOST_DIR/bin/${CROSS_PREFIX}-g++"
CROSS_AR="$HOST_DIR/bin/${CROSS_PREFIX}-ar"
CROSS_RANLIB="$HOST_DIR/bin/${CROSS_PREFIX}-ranlib"
CROSS_STRIP="$HOST_DIR/bin/${CROSS_PREFIX}-strip"

if [ ! -x "$CROSS_CC" ]; then
    echo "ERROR: aarch64 toolchain not found at $HOST_DIR/bin/$CROSS_PREFIX-gcc"
    echo ""
    echo "Build the sbc-groundstations image once first:"
    echo "  cd $COORD_ROOT/sbc-groundstations"
    echo "  DEFCONFIG=$SBC_DEFCONFIG ./build.sh"
    echo ""
    exit 1
fi

if [ ! -d "$SYSROOT" ]; then
    echo "ERROR: sysroot missing at $SYSROOT"
    exit 1
fi

if [ ! -f "$SYSROOT/usr/include/pcap.h" ] || [ ! -f "$SYSROOT/usr/include/sodium.h" ]; then
    echo "ERROR: sysroot at $SYSROOT lacks pcap.h or sodium.h"
    echo "(check that libpcap + libsodium were enabled in the defconfig)"
    exit 1
fi

if [ "${1:-}" = "--clean" ]; then
    echo "Cleaning $OUT_DIR ..."
    rm -rf "$OUT_DIR"
    echo "Done."
    exit 0
fi

DO_DEPLOY=0
if [ "${1:-}" = "--deploy" ]; then
    DO_DEPLOY=1
fi

mkdir -p "$OUT_DIR"

# ── Patched wfb-ng tree (prepared by build_wfb_tx.sh) ────────────────

WFB_DIR="$BUILD_DIR/wfb-ng"

if [ ! -d "$WFB_DIR" ] || [ ! -f "$WFB_DIR/src/tx.cpp" ]; then
    echo "=== Patched wfb-ng tree missing; running build_wfb_tx.sh to prepare it ==="
    "$SCRIPT_DIR/build_wfb_tx.sh"
fi

# Sanity check: the patch must have been applied (rx.cpp diversity hooks
# only exist in the patched tree).
if ! grep -q "diversity" "$WFB_DIR/src/rx.cpp"; then
    echo "ERROR: $WFB_DIR/src/rx.cpp does not look patched (no 'diversity' keyword)."
    echo "Run: $SCRIPT_DIR/build_wfb_tx.sh --clean && $SCRIPT_DIR/build_wfb_tx.sh"
    exit 1
fi

# ── Build flags ──────────────────────────────────────────────────────

# Size-minimization flags (mirrors poc/build_wfb_openwrt.sh CPE510 build).
# Dynamic link against Buildroot's libpcap / libsodium / libstdc++ /
# libgcc_s (all present on the Radxa3 rootfs); -Os + -flto + gc-sections
# trim ~75-80% off vs the unoptimised -O2 build.
SIZE_CFLAGS="-Os -flto -ffunction-sections -fdata-sections -fno-stack-protector"
SIZE_CFLAGS="$SIZE_CFLAGS -fmerge-all-constants -fno-asynchronous-unwind-tables"
SIZE_LDFLAGS="-flto -Wl,--gc-sections -Wl,-s -Wl,--build-id=none -Wl,--as-needed"

CFLAGS_COMMON="$SIZE_CFLAGS -Wall -fno-strict-aliasing"
CFLAGS_COMMON="$CFLAGS_COMMON --sysroot=$SYSROOT"
CFLAGS_COMMON="$CFLAGS_COMMON -I$WFB_ROOT/include"
CFLAGS_COMMON="$CFLAGS_COMMON -DZFEX_UNROLL_ADDMUL_SIMD=8"
CFLAGS_COMMON="$CFLAGS_COMMON -DZFEX_INLINE_ADDMUL"
CFLAGS_COMMON="$CFLAGS_COMMON -DZFEX_INLINE_ADDMUL_SIMD"
CFLAGS_COMMON="$CFLAGS_COMMON -DWFB_VERSION=\"shm-patched-aarch64\""

LDFLAGS_COMMON="--sysroot=$SYSROOT $SIZE_LDFLAGS -lrt -lpthread"

# wfb_tx never calls libpcap; the stub from build_wfb_tx.sh keeps the
# include lightweight even when the real pcap.h is in the sysroot.
TX_CFLAGS="$CFLAGS_COMMON -I$WFB_DIR/src/stub"
TX_LDFLAGS="$LDFLAGS_COMMON -lsodium"

# wfb_rx wants the real libpcap (sysroot has it).
RX_CFLAGS="$CFLAGS_COMMON"
RX_LDFLAGS="$LDFLAGS_COMMON -lpcap -lsodium"

if [ "${STATIC:-0}" = "1" ]; then
    # Buildroot only ships libsodium.la (shared); libpcap.a IS shipped.
    # Static-everything is not viable without rebuilding libsodium with
    # --enable-static. Document the partial story and bail.
    echo "ERROR: STATIC=1 requires a static libsodium build."
    echo "Either rebuild libsodium with --enable-static into the Buildroot"
    echo "sysroot, or run with STATIC=0 (default; dynamic link)."
    exit 1
fi

# ── Build ────────────────────────────────────────────────────────────

cd "$WFB_DIR"

# Pre-clean .o files from previous arch builds (the cross builds in
# build_wfb_tx.sh leave armv7l objects under the same src/ paths).
rm -f src/*.o

echo "=== Building wfb_tx (aarch64) ==="
$CROSS_CXX $TX_CFLAGS -std=gnu++11 -c -o src/tx.o            src/tx.cpp
$CROSS_CC  $TX_CFLAGS -std=gnu99   -c -o src/zfex.o          src/zfex.c
$CROSS_CXX $TX_CFLAGS -std=gnu++11 -c -o src/wifibroadcast.o src/wifibroadcast.cpp
$CROSS_CC  $TX_CFLAGS -std=gnu99   -c -o src/venc_ring.o     src/venc_ring.c
$CROSS_CXX -o "$OUT_DIR/wfb_tx" \
    src/tx.o src/zfex.o src/wifibroadcast.o src/venc_ring.o \
    $TX_LDFLAGS
$CROSS_STRIP "$OUT_DIR/wfb_tx"

echo "=== Building wfb_rx (aarch64) ==="
$CROSS_CXX $RX_CFLAGS -std=gnu++11 -c -o src/rx.o          src/rx.cpp
$CROSS_CC  $RX_CFLAGS -std=gnu99   -c -o src/radiotap.o    src/radiotap.c
$CROSS_CXX -o "$OUT_DIR/wfb_rx" \
    src/rx.o src/zfex.o src/wifibroadcast.o src/radiotap.o \
    $RX_LDFLAGS
$CROSS_STRIP "$OUT_DIR/wfb_rx"

echo "=== Building wfb_tx_cmd (aarch64) ==="
$CROSS_CC $TX_CFLAGS -std=gnu99 -c -o src/tx_cmd.o src/tx_cmd.c
$CROSS_CC -o "$OUT_DIR/wfb_tx_cmd" src/tx_cmd.o $TX_LDFLAGS
$CROSS_STRIP "$OUT_DIR/wfb_tx_cmd"

echo "=== Building wfb_keygen (aarch64) ==="
$CROSS_CC $TX_CFLAGS -std=gnu99 -c -o src/keygen.o src/keygen.c
$CROSS_CC -o "$OUT_DIR/wfb_keygen" src/keygen.o $TX_LDFLAGS
$CROSS_STRIP "$OUT_DIR/wfb_keygen"

# ── Summary ──────────────────────────────────────────────────────────

echo ""
echo "=== Build complete ==="
ls -lh "$OUT_DIR"/{wfb_tx,wfb_rx,wfb_tx_cmd,wfb_keygen}
echo ""
echo "Architecture check:"
file "$OUT_DIR"/wfb_rx 2>/dev/null || \
    "$HOST_DIR/bin/${CROSS_PREFIX}-readelf" -h "$OUT_DIR/wfb_rx" | grep -E "Class|Machine"

# ── Deploy ───────────────────────────────────────────────────────────

if [ "$DO_DEPLOY" = "1" ]; then
    echo ""
    echo "=== Deploying to root@${DEPLOY_HOST}:${DEPLOY_DIR} ==="
    scp -O \
        "$OUT_DIR/wfb_tx" \
        "$OUT_DIR/wfb_rx" \
        "$OUT_DIR/wfb_tx_cmd" \
        "$OUT_DIR/wfb_keygen" \
        "root@${DEPLOY_HOST}:${DEPLOY_DIR}/"
    echo "Deployed."
else
    echo "To deploy:  ./build_wfb_aarch64.sh --deploy"
    echo "  or:       DEPLOY_HOST=192.168.2.20 ./build_wfb_aarch64.sh --deploy"
fi
