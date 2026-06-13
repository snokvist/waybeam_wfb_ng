#!/bin/bash
#
# Build wfb-ng for OpenWRT MIPS24KC big-endian (TP-Link CPE510 et al).
#
# Targets:
#   wfb_tx       - patched (SHM + -Y UDP stats), static, BE MIPS24KC
#   wfb_rx       - patched (-Y UDP stats), static, real libpcap from OpenWRT staging
#   wfb_tx_cmd   - runtime control client, static
#   wfb_keygen   - key generator, static
#
# Assumes you have an OpenWRT build tree with libsodium and libpcap already
# compiled into the target staging directory (default: <coord>/openwrt).
#
# Usage:
#   ./build_wfb_openwrt.sh             # build all
#   ./build_wfb_openwrt.sh --clean     # wipe build-openwrt/
#   ./build_wfb_openwrt.sh --deploy    # build + scp to $DEPLOY_HOST
#
# Environment:
#   OPENWRT_ROOT  - path to OpenWRT build tree (default: <coord>/openwrt)
#   DEPLOY_HOST   - device IP for --deploy (default: 192.168.2.2)
#   DEPLOY_DIR    - install path on device (default: /usr/bin)
#
# Why this script (vs. build_wfb_tx.sh): different toolchain (musl vs glibc),
# different ABI (mips_24kc BE vs armv7l LE), and OpenWRT already provides
# libsodium + libpcap so we skip the libsodium tarball cross-build.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WFB_NG_ROOT="$(dirname "$SCRIPT_DIR")"
COORD_ROOT="$(dirname "$WFB_NG_ROOT")"
BUILD_DIR="$SCRIPT_DIR/build-openwrt"

DEPLOY_HOST="${DEPLOY_HOST:-192.168.2.2}"
DEPLOY_DIR="${DEPLOY_DIR:-/usr/bin}"

OPENWRT_ROOT="${OPENWRT_ROOT:-$COORD_ROOT/openwrt}"
TOOLCHAIN_DIR="$OPENWRT_ROOT/staging_dir/toolchain-mips_24kc_gcc-14.3.0_musl"
TARGET_DIR="$OPENWRT_ROOT/staging_dir/target-mips_24kc_musl"
CROSS_PREFIX="mips-openwrt-linux-musl"

# OpenWRT's gcc wrapper wants STAGING_DIR pointing at the toolchain root so it
# can locate sysroot headers/libs. Without this we get spammy warnings and
# header-search bugs.
export STAGING_DIR="$TOOLCHAIN_DIR"

CROSS_CC="$TOOLCHAIN_DIR/bin/${CROSS_PREFIX}-gcc"
CROSS_CXX="$TOOLCHAIN_DIR/bin/${CROSS_PREFIX}-g++"
CROSS_STRIP="$TOOLCHAIN_DIR/bin/${CROSS_PREFIX}-strip"

# Sanity checks
if [ ! -x "$CROSS_CC" ]; then
    echo "ERROR: OpenWRT toolchain not found at $TOOLCHAIN_DIR" >&2
    echo "Set OPENWRT_ROOT or build OpenWRT first." >&2
    exit 1
fi
for lib in libsodium.a libpcap.a; do
    if [ ! -f "$TARGET_DIR/usr/lib/$lib" ]; then
        echo "ERROR: missing $lib in $TARGET_DIR/usr/lib/" >&2
        echo "In OpenWRT: make package/libs/libsodium/compile package/network/utils/libpcap/compile V=s" >&2
        exit 1
    fi
done

if [ "${1:-}" = "--clean" ]; then
    echo "Cleaning $BUILD_DIR"
    rm -rf "$BUILD_DIR"
    exit 0
fi

DO_DEPLOY=0
[ "${1:-}" = "--deploy" ] && DO_DEPLOY=1

mkdir -p "$BUILD_DIR"

# Reuse the wfb-ng tree from build_wfb_tx.sh if present (already patched);
# fall back to a fresh clone in build-openwrt/.
if [ -d "$SCRIPT_DIR/build/wfb-ng" ]; then
    WFB_DIR="$SCRIPT_DIR/build/wfb-ng"
    echo "=== Reusing wfb-ng tree from $WFB_DIR ==="
else
    WFB_DIR="$BUILD_DIR/wfb-ng"
    # Pin to the same known-good commit as build-armv7.sh so both patches apply.
    WFB_NG_SHA="af6ba85eface27279709477077d3362c69bb2576"
    if [ ! -d "$WFB_DIR" ]; then
        echo "=== Cloning wfb-ng into $WFB_DIR (pinned $WFB_NG_SHA) ==="
        git init -q "$WFB_DIR"
        git -C "$WFB_DIR" remote add origin https://github.com/svpcom/wfb-ng.git
        git -C "$WFB_DIR" fetch --depth 1 origin "$WFB_NG_SHA"
        git -C "$WFB_DIR" checkout -q FETCH_HEAD
    fi
fi

# Apply shm-input.patch (which also adds -Y UDP stats push). Patch is
# idempotent: skip if already applied.
if git -C "$WFB_DIR" apply --reverse --check "$SCRIPT_DIR/shm-input.patch" 2>/dev/null; then
    echo "=== shm-input.patch already applied ==="
elif git -C "$WFB_DIR" apply --check "$SCRIPT_DIR/shm-input.patch" 2>/dev/null; then
    echo "=== Applying shm-input.patch ==="
    git -C "$WFB_DIR" apply "$SCRIPT_DIR/shm-input.patch"
else
    echo "ERROR: wfb-ng tree at $WFB_DIR is dirty and rejects the patch." >&2
    echo "Run with --clean first if this is the openwrt tree, or clean build/wfb-ng." >&2
    exit 1
fi

# peek.patch (NAL-aware link protection) on top of shm-input.patch. Idempotent.
if git -C "$WFB_DIR" apply --reverse --check "$SCRIPT_DIR/peek.patch" 2>/dev/null; then
    echo "=== peek.patch already applied ==="
elif git -C "$WFB_DIR" apply --check "$SCRIPT_DIR/peek.patch" 2>/dev/null; then
    echo "=== Applying peek.patch ==="
    git -C "$WFB_DIR" apply "$SCRIPT_DIR/peek.patch"
else
    echo "ERROR: peek.patch does not apply on top of shm-input.patch." >&2
    exit 1
fi

# venc_ring.{c,h} are needed at compile time even though CPE510 won't run the
# SHM path. Copying them keeps the source set identical to the armv7l build.
cp "$WFB_NG_ROOT/include/venc_ring.h" "$WFB_DIR/src/venc_ring.h"
cp "$WFB_NG_ROOT/src/venc_ring.c"     "$WFB_DIR/src/venc_ring.c"

# Common flags
INC="-I$TARGET_DIR/usr/include -I$WFB_NG_ROOT/include"
LIB="-L$TARGET_DIR/usr/lib"
ZFEX="-DZFEX_UNROLL_ADDMUL_SIMD=8 -DZFEX_INLINE_ADDMUL -DZFEX_INLINE_ADDMUL_SIMD"

# Size-minimization flags. Cumulative effect on wfb_rx + wfb_tx is large
# (~80% reduction) thanks to:
#   1) dynamic linking against the device's libsodium/libpcap/libstdc++/
#      libatomic (already present on the OpenWRT rootfs — confirmed on
#      stock build for CPE510).
#   2) -flto across translation units → tighter dead-code elimination.
#   3) -Wl,--gc-sections + -ffunction-sections → drop unreferenced sections.
#   4) -fno-stack-protector / -fmerge-all-constants / -fno-asynchronous-
#      unwind-tables → small per-fn savings that compound over the binary.
#   5) -Wl,-s strips at link time; -Wl,--build-id=none drops the GNU note.
#
# If you need a fully self-contained binary (no on-device deps), add
# -static back in CFLAGS_BASE/LDFLAGS_BASE — sizes go from ~100 KB back to
# ~500 KB but everything bakes in.
SIZE_CFLAGS="-Os -flto -ffunction-sections -fdata-sections -fno-stack-protector"
SIZE_CFLAGS="$SIZE_CFLAGS -fmerge-all-constants -fno-asynchronous-unwind-tables"
SIZE_LDFLAGS="-flto -Wl,--gc-sections -Wl,-s -Wl,--build-id=none -Wl,--as-needed"

CFLAGS_BASE="$SIZE_CFLAGS -Wall -fno-strict-aliasing $INC $ZFEX -DWFB_VERSION=\"shm-patched-mips24kc\""
LDFLAGS_BASE="$LIB $SIZE_LDFLAGS"
# MIPS32 has no native 64-bit atomic ops; std::atomic<uint64_t> in tx.cpp
# needs libatomic. With dynamic linking we use libatomic.so from the
# device (/lib/libatomic.so.1). C-only links (tx_cmd, keygen) don't need
# this.
CXX_STATIC_EXTRA="-latomic"

cd "$WFB_DIR"

OBJDIR="$BUILD_DIR/obj"
mkdir -p "$OBJDIR"

# ── Shared objects (build once, reuse for tx and rx) ─────────────────
echo "=== Building shared objects ==="
$CROSS_CC  $CFLAGS_BASE -std=gnu99   -c -o "$OBJDIR/zfex.o"          src/zfex.c
$CROSS_CXX $CFLAGS_BASE -std=gnu++11 -c -o "$OBJDIR/wifibroadcast.o" src/wifibroadcast.cpp
$CROSS_CC  $CFLAGS_BASE -std=gnu99   -c -o "$OBJDIR/radiotap.o"      src/radiotap.c

# ── wfb_tx ───────────────────────────────────────────────────────────
echo "=== Building wfb_tx ==="
$CROSS_CXX $CFLAGS_BASE -std=gnu++11 -c -o "$OBJDIR/tx.o"        src/tx.cpp
$CROSS_CXX $CFLAGS_BASE -std=gnu++11 -c -o "$OBJDIR/peek.o"      src/peek.cpp
$CROSS_CC  $CFLAGS_BASE -std=gnu99   -c -o "$OBJDIR/venc_ring.o" src/venc_ring.c
$CROSS_CXX -o "$BUILD_DIR/wfb_tx" \
    "$OBJDIR/tx.o" "$OBJDIR/peek.o" "$OBJDIR/zfex.o" "$OBJDIR/wifibroadcast.o" "$OBJDIR/venc_ring.o" \
    $LDFLAGS_BASE -lsodium -lrt $CXX_STATIC_EXTRA
$CROSS_STRIP "$BUILD_DIR/wfb_tx"

# ── wfb_rx ───────────────────────────────────────────────────────────
echo "=== Building wfb_rx ==="
$CROSS_CXX $CFLAGS_BASE -std=gnu++11 -c -o "$OBJDIR/rx.o" src/rx.cpp
$CROSS_CXX -o "$BUILD_DIR/wfb_rx" \
    "$OBJDIR/rx.o" "$OBJDIR/zfex.o" "$OBJDIR/wifibroadcast.o" "$OBJDIR/radiotap.o" \
    $LDFLAGS_BASE -lpcap -lsodium -lrt $CXX_STATIC_EXTRA
$CROSS_STRIP "$BUILD_DIR/wfb_rx"

# ── wfb_tx_cmd ───────────────────────────────────────────────────────
echo "=== Building wfb_tx_cmd ==="
$CROSS_CC $CFLAGS_BASE -std=gnu99 -c -o "$OBJDIR/tx_cmd.o" src/tx_cmd.c
$CROSS_CC -o "$BUILD_DIR/wfb_tx_cmd" "$OBJDIR/tx_cmd.o" $LDFLAGS_BASE
$CROSS_STRIP "$BUILD_DIR/wfb_tx_cmd"

# ── wfb_keygen ───────────────────────────────────────────────────────
echo "=== Building wfb_keygen ==="
$CROSS_CC $CFLAGS_BASE -std=gnu99 -c -o "$OBJDIR/keygen.o" src/keygen.c
$CROSS_CC -o "$BUILD_DIR/wfb_keygen" "$OBJDIR/keygen.o" $LDFLAGS_BASE -lsodium
$CROSS_STRIP "$BUILD_DIR/wfb_keygen"

echo
echo "=== Build complete ==="
ls -lh "$BUILD_DIR/wfb_tx" "$BUILD_DIR/wfb_rx" "$BUILD_DIR/wfb_tx_cmd" "$BUILD_DIR/wfb_keygen"
echo
file "$BUILD_DIR/wfb_tx" | head -1

if [ "$DO_DEPLOY" = "1" ]; then
    # wfb_keygen is a one-shot host tool — keys can be generated anywhere
    # and copied to the device, so we don't deploy it by default to save
    # space. Pass DEPLOY_KEYGEN=1 to include it.
    DEPLOY_FILES=("$BUILD_DIR/wfb_tx" "$BUILD_DIR/wfb_rx" "$BUILD_DIR/wfb_tx_cmd")
    if [ "${DEPLOY_KEYGEN:-0}" = "1" ]; then
        DEPLOY_FILES+=("$BUILD_DIR/wfb_keygen")
    fi
    echo
    echo "=== Deploying to root@${DEPLOY_HOST}:${DEPLOY_DIR} ==="
    scp -O "${DEPLOY_FILES[@]}" "root@${DEPLOY_HOST}:${DEPLOY_DIR}/"
    echo "Deployed (keygen excluded; set DEPLOY_KEYGEN=1 to include it)."
else
    echo
    echo "Manual deploy (keygen excluded by default):"
    echo "  scp -O $BUILD_DIR/{wfb_tx,wfb_rx,wfb_tx_cmd} root@$DEPLOY_HOST:$DEPLOY_DIR/"
    echo "Or:"
    echo "  $0 --deploy"
    echo "  DEPLOY_KEYGEN=1 $0 --deploy   # include wfb_keygen too"
fi
