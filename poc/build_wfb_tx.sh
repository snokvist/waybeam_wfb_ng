#!/bin/bash
#
# Build patched wfb_tx (SHM ring input) + SHM diagnostic tools.
#
# Cross-compiles everything with the star6e toolchain for SigmaStar
# Infinity6E (armv7l). Clones wfb-ng, cross-compiles libsodium,
# applies the SHM input patch, and builds all binaries.
#
# Usage:
#   ./build_wfb_tx.sh            # build all
#   ./build_wfb_tx.sh --clean    # remove build directory
#   ./build_wfb_tx.sh --deploy   # build + scp to device
#
# Output (all in build/):
#   wfb_tx             - patched wfb_tx with -H (SHM), -x, -Y flags
#                        (cross, dynamic — libsodium/libstdc++ from device rootfs)
#   wfb_tx_cmd         - runtime control client (set_fec [-T fec_timeout_ms],
#                        set_radio, get_fec, get_radio) (cross, dynamic)
#   wfb_keygen         - key generator (cross, dynamic)
#   shm_ring_stats     - ring status checker (cross, dynamic)
#   shm_consumer_test  - ring throughput tester (cross, dynamic)
#   wfb_rx_native      - x86_64 native build for the ground-station laptop;
#                        the cross pcap stub only covers wfb_tx's inject path
#   wfb_tx_native      - x86_64 native build of wfb_tx for benchtop / lab use
#                        (same source set as the cross build, dynamic libsodium)
#
# Requirements:
#   - star6e toolchain at ../toolchain/toolchain.sigmastar-infinity6e/
#   - Host x86_64: g++, gcc, libsodium-dev, libpcap-dev (for wfb_rx_native)
#   - git, curl, autotools (first run only, for libsodium cross-build)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENC_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$SCRIPT_DIR/build"
# Pin to 1.0.18: matches OpenIPC's libsodium SONAME (libsodium.so.23) so the
# dynamic-linked wfb_tx finds it on-device. libsodium 1.0.19+ bumps to
# libsodium.so.26 and breaks runtime. Bump only when OpenIPC bumps.
LIBSODIUM_VER="1.0.18"
# libpcap version: must produce SONAME libpcap.so.1 to match OpenIPC's
# /usr/lib/libpcap.so.1.10.5 SONAME libpcap.so.1.  1.10.x is fine; bump
# only when OpenIPC bumps.
LIBPCAP_VER="1.10.5"
DEPLOY_HOST="${DEPLOY_HOST:-192.168.1.10}"
DEPLOY_DIR="${DEPLOY_DIR:-/usr/bin}"

# Toolchain (at repo root level)
TOOLCHAIN_DIR="$VENC_ROOT/toolchain/toolchain.sigmastar-infinity6e"
CROSS_PREFIX="arm-openipc-linux-gnueabihf"
CROSS_CC="$TOOLCHAIN_DIR/bin/${CROSS_PREFIX}-gcc"
CROSS_CXX="$TOOLCHAIN_DIR/bin/${CROSS_PREFIX}-g++"
CROSS_AR="$TOOLCHAIN_DIR/bin/${CROSS_PREFIX}-ar"
CROSS_RANLIB="$TOOLCHAIN_DIR/bin/${CROSS_PREFIX}-ranlib"
CROSS_STRIP="$TOOLCHAIN_DIR/bin/${CROSS_PREFIX}-strip"

# Verify toolchain exists
if [ ! -x "$CROSS_CC" ]; then
    echo "ERROR: Toolchain not found at $TOOLCHAIN_DIR"
    echo "Expected: $CROSS_CC"
    exit 1
fi

if [ "${1:-}" = "--clean" ]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
    echo "Done."
    exit 0
fi

DO_DEPLOY=0
if [ "${1:-}" = "--deploy" ]; then
    DO_DEPLOY=1
fi

mkdir -p "$BUILD_DIR"

# ── Step 1: Download and cross-compile libsodium ─────────────────────

SODIUM_DIR="$BUILD_DIR/libsodium-${LIBSODIUM_VER}"
SODIUM_PREFIX="$BUILD_DIR/sodium-install"

if [ ! -f "$SODIUM_PREFIX/lib/libsodium.so" ]; then
    echo "=== Building libsodium ${LIBSODIUM_VER} ==="

    if [ ! -d "$SODIUM_DIR" ]; then
        cd "$BUILD_DIR"
        TARBALL="libsodium-${LIBSODIUM_VER}.tar.gz"
        if [ ! -f "$TARBALL" ]; then
            echo "Downloading libsodium..."
            # download.libsodium.org keeps only the latest few versions; pull
            # older pinned releases from GitHub instead.
            curl -fL -o "$TARBALL" \
                "https://github.com/jedisct1/libsodium/releases/download/${LIBSODIUM_VER}-RELEASE/${TARBALL}" \
                || curl -fL -o "$TARBALL" \
                    "https://download.libsodium.org/libsodium/releases/${TARBALL}"
        fi
        tar xzf "$TARBALL"
    fi

    cd "$SODIUM_DIR"
    # Build shared libsodium (and keep static for the native wfb_rx build).
    # The .so here only satisfies the cross linker — at runtime the device's
    # /usr/lib/libsodium.so.23 (from OpenIPC's wfb-bins-only deps) is used.
    ./configure \
        --host=arm-linux-gnueabihf \
        --prefix="$SODIUM_PREFIX" \
        --enable-shared \
        --enable-static \
        --disable-asm \
        CC="$CROSS_CC" \
        AR="$CROSS_AR" \
        RANLIB="$CROSS_RANLIB" \
        CFLAGS="-Os -fPIC"

    make -j"$(nproc)"
    make install
    echo "libsodium installed to $SODIUM_PREFIX"
else
    echo "=== libsodium already built ==="
fi

# ── Step 1b: Cross-compile libpcap (for the cross wfb_rx build) ──────
#
# libpcap-dev is not in the SigmaStar toolchain sysroot.  We cross-build
# libpcap as a static archive and link wfb_rx against it; on-device the
# binary uses /usr/lib/libpcap.so.1 from the OpenIPC rootfs at runtime
# (the static archive is just a shim providing headers + stubs that the
# dynamic loader resolves against the rootfs SONAME).
#
# Actually the cleaner approach: build libpcap shared so the binary
# dynamically resolves against the on-device libpcap.so.1.  The toolchain
# doesn't ship libpcap headers, so we install a fresh build into our own
# prefix and link against -lpcap from there (header path) but expect the
# device's libpcap.so.1 at runtime — which works because the SONAMEs match.

PCAP_DIR="$BUILD_DIR/libpcap-${LIBPCAP_VER}"
PCAP_PREFIX="$BUILD_DIR/pcap-install"

if [ ! -f "$PCAP_PREFIX/lib/libpcap.so" ]; then
    echo "=== Building libpcap ${LIBPCAP_VER} (cross) ==="
    if [ ! -d "$PCAP_DIR" ]; then
        cd "$BUILD_DIR"
        TARBALL="libpcap-${LIBPCAP_VER}.tar.gz"
        if [ ! -f "$TARBALL" ]; then
            echo "Downloading libpcap..."
            curl -fsSL -o "$TARBALL" \
                "https://www.tcpdump.org/release/${TARBALL}"
        fi
        tar xzf "$TARBALL"
    fi
    cd "$PCAP_DIR"
    # libpcap auto-detects build artifacts.  Disable optional features so
    # we don't drag in dbus/usb/bluetooth headers we don't have for the
    # SigmaStar sysroot.
    CC="$CROSS_CC" AR="$CROSS_AR" RANLIB="$CROSS_RANLIB" STRIP="$CROSS_STRIP" \
        ./configure --host="$CROSS_PREFIX" \
                    --prefix="$PCAP_PREFIX" \
                    --disable-shared \
                    --enable-static \
                    --without-libnl \
                    --disable-bluetooth \
                    --disable-dbus \
                    --disable-rdma \
                    --disable-usb \
                    --disable-canusb \
                    --with-pcap=linux \
                    --enable-remote=no \
                    >/dev/null
    make -s -j$(nproc)
    make -s install
    cd "$SCRIPT_DIR"
    echo "libpcap installed to $PCAP_PREFIX"
else
    echo "=== libpcap already built ==="
fi

# ── Step 2: Clone wfb-ng ─────────────────────────────────────────────

WFB_DIR="$BUILD_DIR/wfb-ng"

if [ ! -d "$WFB_DIR" ]; then
    echo "=== Cloning wfb-ng ==="
    git clone --depth 1 https://github.com/svpcom/wfb-ng.git "$WFB_DIR"
else
    echo "=== wfb-ng already cloned ==="
fi

# ── Step 3: Copy venc_ring files + create pcap stub ──────────────────

echo "=== Copying venc_ring files ==="
cp "$VENC_ROOT/include/venc_ring.h" "$WFB_DIR/src/venc_ring.h"
cp "$VENC_ROOT/src/venc_ring.c" "$WFB_DIR/src/venc_ring.c"

# Stub pcap.h — wifibroadcast.hpp includes it but TX doesn't use pcap
mkdir -p "$WFB_DIR/src/stub"
cat > "$WFB_DIR/src/stub/pcap.h" << 'STUB'
/* Stub: pcap.h not needed for wfb_tx (only wfb_rx uses libpcap) */
#ifndef PCAP_H_STUB
#define PCAP_H_STUB
#endif
STUB

# ── Step 4: Apply SHM input patch ────────────────────────────────────

cd "$WFB_DIR"

if git apply --reverse --check "$SCRIPT_DIR/shm-input.patch" 2>/dev/null; then
    echo "=== Patch already applied ==="
elif git apply --check "$SCRIPT_DIR/shm-input.patch" 2>/dev/null; then
    echo "=== Applying SHM input patch ==="
    git apply "$SCRIPT_DIR/shm-input.patch"
    echo "Patch applied successfully."
else
    echo "ERROR: Existing wfb-ng tree is stale or dirty and does not match shm-input.patch."
    echo "Run ./build_wfb_tx.sh --clean and rebuild so src/tx.cpp is regenerated."
    exit 1
fi

# ── Step 5: Build wfb_tx + wfb_keygen ───────────────────────────────

echo "=== Building wfb_tx ==="

# Size-minimization flags (mirrors poc/build_wfb_openwrt.sh CPE510 build).
# Cumulative effect on wfb_tx + helpers is large (~75-80% reduction vs the
# old `-O2 -static` build) thanks to:
#   1) dynamic linking against the device's libsodium / libstdc++ / libgcc_s
#      (already on the OpenIPC rootfs via the wfb-bins-only package deps).
#   2) -Os + -flto across translation units → tighter dead-code elimination.
#   3) -Wl,--gc-sections + -ffunction-sections / -fdata-sections.
#   4) -fno-stack-protector / -fmerge-all-constants /
#      -fno-asynchronous-unwind-tables → small per-fn savings that compound.
#   5) -Wl,-s strips at link time; -Wl,--build-id=none drops the GNU note.
#
# If you need a fully self-contained binary (no on-device .so deps), add
# -static back into WFB_LDFLAGS — sizes go from ~120 KB back to ~500 KB but
# everything bakes in.
SIZE_CFLAGS="-Os -flto -ffunction-sections -fdata-sections -fno-stack-protector"
SIZE_CFLAGS="$SIZE_CFLAGS -fmerge-all-constants -fno-asynchronous-unwind-tables"
SIZE_LDFLAGS="-flto -Wl,--gc-sections -Wl,-s -Wl,--build-id=none -Wl,--as-needed"

WFB_CFLAGS="$SIZE_CFLAGS -Wall -fno-strict-aliasing -I$SODIUM_PREFIX/include -I$VENC_ROOT/include -I$WFB_DIR/src/stub"
WFB_CFLAGS="$WFB_CFLAGS -DZFEX_UNROLL_ADDMUL_SIMD=8 -DZFEX_INLINE_ADDMUL -DZFEX_INLINE_ADDMUL_SIMD"
# WFB_VERSION must reach the compiler as `"shm-patched"` (a string).  Single
# quotes around the value would be interpreted by the C preprocessor as a
# multi-character literal and gcc warns about it; escape the inner double
# quotes only and let bash word-splitting deliver the token verbatim.
WFB_CFLAGS="$WFB_CFLAGS -DWFB_VERSION=\"shm-patched\""
WFB_LDFLAGS="-L$SODIUM_PREFIX/lib $SIZE_LDFLAGS -lrt -lsodium"

cd "$WFB_DIR"

# Drop stale .o files from prior arch/flavour builds (e.g. an aarch64 run via
# build_wfb_aarch64.sh, or an older `-O2 -static` run). LTO objects from a
# different toolchain confuse the linker.
rm -f src/*.o

echo "  Compiling tx.cpp..."
$CROSS_CXX $WFB_CFLAGS -std=gnu++11 -c -o src/tx.o src/tx.cpp

echo "  Compiling zfex.c..."
$CROSS_CC $WFB_CFLAGS -std=gnu99 -c -o src/zfex.o src/zfex.c

echo "  Compiling wifibroadcast.cpp..."
$CROSS_CXX $WFB_CFLAGS -std=gnu++11 -c -o src/wifibroadcast.o src/wifibroadcast.cpp

echo "  Compiling venc_ring.c..."
$CROSS_CC $WFB_CFLAGS -std=gnu99 -c -o src/venc_ring.o src/venc_ring.c

echo "  Linking wfb_tx..."
$CROSS_CXX -o wfb_tx src/tx.o src/zfex.o src/wifibroadcast.o src/venc_ring.o $WFB_LDFLAGS

echo "  Stripping..."
$CROSS_STRIP wfb_tx
cp wfb_tx "$BUILD_DIR/wfb_tx"

echo "  Building wfb_keygen..."
$CROSS_CC $WFB_CFLAGS -std=gnu99 -c -o src/keygen.o src/keygen.c
$CROSS_CC -o wfb_keygen src/keygen.o $WFB_LDFLAGS
$CROSS_STRIP wfb_keygen
cp wfb_keygen "$BUILD_DIR/wfb_keygen"

echo "  Building wfb_tx_cmd..."
$CROSS_CC $WFB_CFLAGS -std=gnu99 -c -o src/tx_cmd.o src/tx_cmd.c
$CROSS_CC -o wfb_tx_cmd src/tx_cmd.o $WFB_LDFLAGS
$CROSS_STRIP wfb_tx_cmd
cp wfb_tx_cmd "$BUILD_DIR/wfb_tx_cmd"

# ── Cross wfb_rx (armhf) ────────────────────────────────────────────
# Uses the locally cross-built libpcap (static linkage of pcap is fine —
# we still rely on libsodium/libstdc++ from the device rootfs).
echo "  Building wfb_rx (cross armhf)..."
RX_CFLAGS="$SIZE_CFLAGS -Wall -fno-strict-aliasing"
RX_CFLAGS="$RX_CFLAGS -I$SODIUM_PREFIX/include -I$VENC_ROOT/include"
RX_CFLAGS="$RX_CFLAGS -I$PCAP_PREFIX/include"
RX_CFLAGS="$RX_CFLAGS -DZFEX_UNROLL_ADDMUL_SIMD=8 -DZFEX_INLINE_ADDMUL -DZFEX_INLINE_ADDMUL_SIMD"
RX_CFLAGS="$RX_CFLAGS -DWFB_VERSION=\"shm-patched-cross\""
RX_LDFLAGS="-L$SODIUM_PREFIX/lib -L$PCAP_PREFIX/lib $SIZE_LDFLAGS -lrt -lsodium -lpcap"

$CROSS_CXX $RX_CFLAGS -std=gnu++11 -c -o src/rx.o src/rx.cpp
$CROSS_CC  $RX_CFLAGS -std=gnu99 -c -o src/radiotap.o src/radiotap.c
$CROSS_CXX -o wfb_rx \
    src/rx.o src/zfex.o src/wifibroadcast.o src/radiotap.o \
    $RX_LDFLAGS
$CROSS_STRIP wfb_rx
cp wfb_rx "$BUILD_DIR/wfb_rx"

# ── Native wfb_rx (x86_64) for the ground station laptop ────────────
# wfb_rx needs real libpcap (not the stub used by the cross build of
# wfb_tx) so we compile it natively with the host toolchain and system
# libpcap/libsodium.  Drop it onto the ground-station laptop via:
#   cp build/wfb_rx_native /home/snokvist/dev/wfb-ng/wfb_rx
# (or wherever you run wfb_rx from).
echo "  Building wfb_rx (native x86_64)..."
NATIVE_BUILD="$BUILD_DIR/native"
mkdir -p "$NATIVE_BUILD"
NATIVE_CFLAGS="-Wall -O2 -fno-strict-aliasing -I$VENC_ROOT/include"
NATIVE_CFLAGS="$NATIVE_CFLAGS -DZFEX_UNROLL_ADDMUL_SIMD=8 -DZFEX_INLINE_ADDMUL -DZFEX_INLINE_ADDMUL_SIMD"
NATIVE_CFLAGS="$NATIVE_CFLAGS -DWFB_VERSION=\"shm-patched-native\""
g++ $NATIVE_CFLAGS -std=gnu++11 -c -o "$NATIVE_BUILD/rx_native.o" src/rx.cpp
gcc $NATIVE_CFLAGS -std=gnu99 -c -o "$NATIVE_BUILD/zfex_native.o" src/zfex.c
g++ $NATIVE_CFLAGS -std=gnu++11 -c -o "$NATIVE_BUILD/wfb_native.o" src/wifibroadcast.cpp
gcc $NATIVE_CFLAGS -std=gnu99 -c -o "$NATIVE_BUILD/radiotap_native.o" src/radiotap.c
g++ -o "$BUILD_DIR/wfb_rx_native" \
    "$NATIVE_BUILD/rx_native.o" \
    "$NATIVE_BUILD/zfex_native.o" \
    "$NATIVE_BUILD/wfb_native.o" \
    "$NATIVE_BUILD/radiotap_native.o" \
    -lrt -lsodium -lpcap
strip "$BUILD_DIR/wfb_rx_native"

# ── Native wfb_tx (x86_64) for benchtop / lab use ───────────────────
# Mirrors the cross build but links dynamically against system
# libsodium and uses the same stub pcap.h (wfb_tx never calls libpcap;
# the include is only there because wifibroadcast.hpp pulls it in).
# Useful for testing TX behaviour against a host wfb_rx_native loop or
# for driving an x86 ground station as a transmitter without flashing
# a SigmaStar device.
echo "  Building wfb_tx (native x86_64)..."
NATIVE_TX_CFLAGS="$NATIVE_CFLAGS -I$WFB_DIR/src/stub"
g++ $NATIVE_TX_CFLAGS -std=gnu++11 -c -o "$NATIVE_BUILD/tx_native.o" src/tx.cpp
gcc $NATIVE_TX_CFLAGS -std=gnu99 -c -o "$NATIVE_BUILD/venc_ring_native.o" src/venc_ring.c
g++ -o "$BUILD_DIR/wfb_tx_native" \
    "$NATIVE_BUILD/tx_native.o" \
    "$NATIVE_BUILD/zfex_native.o" \
    "$NATIVE_BUILD/wfb_native.o" \
    "$NATIVE_BUILD/venc_ring_native.o" \
    -lrt -lsodium
strip "$BUILD_DIR/wfb_tx_native"

# ── Step 6: Build SHM diagnostic tools ───────────────────────────────

echo "=== Building SHM tools ==="

TOOL_CFLAGS="-Os -Wall -I$VENC_ROOT/include"
TOOL_LDFLAGS="-lrt -lpthread"

echo "  Building shm_ring_stats..."
$CROSS_CC $TOOL_CFLAGS -o "$BUILD_DIR/shm_ring_stats" \
    "$VENC_ROOT/tools/shm_ring_stats.c" \
    "$VENC_ROOT/src/venc_ring.c" \
    $TOOL_LDFLAGS
$CROSS_STRIP "$BUILD_DIR/shm_ring_stats"

echo "  Building shm_consumer_test..."
$CROSS_CC $TOOL_CFLAGS -o "$BUILD_DIR/shm_consumer_test" \
    "$VENC_ROOT/tools/shm_consumer_test.c" \
    "$VENC_ROOT/src/venc_ring.c" \
    $TOOL_LDFLAGS
$CROSS_STRIP "$BUILD_DIR/shm_consumer_test"

# ── Summary ──────────────────────────────────────────────────────────

echo ""
echo "=== Build complete ==="
echo ""
ls -lh "$BUILD_DIR/wfb_tx" "$BUILD_DIR/wfb_rx" \
       "$BUILD_DIR/wfb_keygen" "$BUILD_DIR/wfb_tx_cmd" \
       "$BUILD_DIR/wfb_rx_native" "$BUILD_DIR/wfb_tx_native" \
       "$BUILD_DIR/shm_ring_stats" "$BUILD_DIR/shm_consumer_test"
echo ""

# ── Deploy ───────────────────────────────────────────────────────────

if [ "$DO_DEPLOY" = "1" ]; then
    echo "=== Deploying to root@${DEPLOY_HOST}:${DEPLOY_DIR} ==="
    scp -O \
        "$BUILD_DIR/wfb_tx" \
        "$BUILD_DIR/wfb_rx" \
        "$BUILD_DIR/wfb_keygen" \
        "$BUILD_DIR/wfb_tx_cmd" \
        "$BUILD_DIR/shm_ring_stats" \
        "$BUILD_DIR/shm_consumer_test" \
        "root@${DEPLOY_HOST}:${DEPLOY_DIR}/"
    echo "Deployed successfully."
    echo ""
    echo "On device:"
    echo "  # Quick test — verify binaries run:"
    echo "  wfb_tx --help"
    echo "  shm_ring_stats --help"
    echo ""
    echo "  # Full test — start venc with SHM, then consumer:"
    echo "  venc -c /etc/venc.json &    # config: server = shm://venc_wfb"
    echo "  sleep 1"
    echo "  shm_ring_stats venc_wfb     # check ring created"
    echo "  shm_consumer_test venc_wfb 5 # read 5 seconds of packets"
    echo "  wfb_tx -H venc_wfb -K /etc/drone.key -k 8 -n 12 -p 0 wlan0"
else
    echo "To deploy:  ./build_wfb_tx.sh --deploy"
    echo "  or:       DEPLOY_HOST=192.168.1.10 ./build_wfb_tx.sh --deploy"
    echo ""
    echo "Manual deploy (vehicle):"
    echo "  scp -O $BUILD_DIR/{wfb_tx,wfb_tx_cmd,wfb_keygen,shm_ring_stats,shm_consumer_test} root@$DEPLOY_HOST:$DEPLOY_DIR/"
    echo ""
    echo "Manual deploy (ground-station x86_64 native wfb_rx):"
    echo "  cp $BUILD_DIR/wfb_rx_native /wherever/you/run/wfb_rx"
    echo ""
    echo "Manual deploy (x86_64 native wfb_tx for benchtop / lab use):"
    echo "  cp $BUILD_DIR/wfb_tx_native /wherever/you/run/wfb_tx"
fi
