#!/bin/bash
# Build libghostty-vt static libraries for all Sailfish OS architectures.
# Requires Zig 0.17.0 (see README for installation).
#
# Usage: ./scripts/build-libs.sh [aarch64|armv7hl|i486|all]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
GHOSTTY_DIR="$PROJECT_ROOT/ghostty"
LIB_DIR="$PROJECT_ROOT/lib"

ZIG_VERSION_REQUIRED="0.17.0"
ZIG="${ZIG:-zig}"

check_zig() {
    if ! command -v "$ZIG" &>/dev/null; then
        echo "Error: zig not found. Install Zig $ZIG_VERSION_REQUIRED from https://ziglang.org/"
        exit 1
    fi
    local version
    version=$("$ZIG" version)
    if [[ "$version" != "$ZIG_VERSION_REQUIRED" ]]; then
        echo "Error: Zig $ZIG_VERSION_REQUIRED required, found $version"
        echo "Install with: curl -L https://ziglang.org/download/$ZIG_VERSION_REQUIRED/zig-x86_64-linux-$ZIG_VERSION_REQUIRED.tar.xz | tar -xJ -C ~/.local/"
        exit 1
    fi
}

build_arch() {
    local arch="$1"
    local target="$2"
    local out_dir="$LIB_DIR/$arch"

    echo "Building libghostty-vt for $arch ($target)..."
    mkdir -p "$out_dir"

    # Report the version the pinned headers carry, regardless of git state
    # (CI shallow clones can't see the vt- tag). Same source as rpm/ghosteel.spec.
    local lib_version
    lib_version=$(sed -n 's/^#define GHOSTTY_VT_VERSION "\(.*\)"/\1/p' "$GHOSTTY_DIR/include/ghostty/vt/version.h")
    if [ -z "$lib_version" ]; then
        echo "Error: GHOSTTY_VT_VERSION not found in ghostty headers" >&2
        exit 1
    fi

    cd "$GHOSTTY_DIR"
    "$ZIG" build -Demit-lib-vt -Dtarget="$target" -Doptimize=ReleaseSafe \
        -Dlib-version-string="$lib_version" 2>&1

    cp "$GHOSTTY_DIR/zig-out/lib/libghostty-vt.a" "$out_dir/"
    echo "  -> $out_dir/libghostty-vt.a"
}

# Build the static musl busybox for the store (Jolla Harbour) variant.
# The sandbox blacklists the system busybox, so the store build ships its
# own; static musl avoids disallowed shared-library dependencies and runs
# on any SFOS release. Built here because the sfdk build engine is 32-bit
# and cannot run the x86_64 Zig toolchain (same reason as libghostty-vt).
build_busybox() {
    local arch="$1"
    local musl_target="$2"
    local out_dir="$LIB_DIR/$arch"
    local bb_dir="$PROJECT_ROOT/.build/busybox-1.38.0"

    echo "Building busybox for $arch ($musl_target)..."
    mkdir -p "$out_dir"

    if [ ! -d "$bb_dir" ]; then
        mkdir -p "$PROJECT_ROOT/.build"
        tar -xjf "$PROJECT_ROOT/rpm/busybox-1.38.0.tar.bz2" -C "$PROJECT_ROOT/.build"
    fi

    # Fresh config per arch (defconfig + static) in a copy of the tree:
    # the build dir is shared across arches. SKIP_STRIP: busybox would run
    # the host strip, which cannot parse cross-arch binaries; tc is off
    # because musl's kernel headers lack the CBQ defines it needs.
    local work="$PROJECT_ROOT/.build/busybox-$arch"
    rm -rf "$work"
    cp -a "$bb_dir" "$work"
    cd "$work"
    make defconfig
    sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
    sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' .config
    make -j"$(nproc)" SKIP_STRIP=y CC="$ZIG cc -target $musl_target" busybox
    if command -v llvm-strip >/dev/null 2>&1; then
        llvm-strip --strip-all busybox
    fi

    cp "$work/busybox" "$out_dir/busybox"
    echo "  -> $out_dir/busybox"
}

main() {
    # Shared zig-out path is not covered by zig's cache locking.
    exec 9>"$PROJECT_ROOT/.build-libs.lock"
    flock 9

    local arch="${1:-all}"

    check_zig

    echo "=== Building libghostty-vt for Sailfish OS ==="
    echo "Zig: $("$ZIG" version)"
    echo ""

    case "$arch" in
        aarch64)
            build_arch "aarch64" "aarch64-linux-gnu.2.28"
            build_busybox "aarch64" "aarch64-linux-musl"
            ;;
        armv7hl)
            build_arch "armv7hl" "arm-linux-gnueabihf.2.28"
            build_busybox "armv7hl" "arm-linux-musleabihf"
            ;;
        i486)
            build_arch "i486" "x86-linux-gnu.2.28"
            build_busybox "i486" "x86-linux-musl"
            ;;
        all)
            build_arch "aarch64" "aarch64-linux-gnu.2.28"
            build_busybox "aarch64" "aarch64-linux-musl"
            build_arch "armv7hl" "arm-linux-gnueabihf.2.28"
            build_busybox "armv7hl" "arm-linux-musleabihf"
            build_arch "i486" "x86-linux-gnu.2.28"
            build_busybox "i486" "x86-linux-musl"
            ;;
        *)
            echo "Unknown architecture: $arch"
            echo "Usage: $0 [aarch64|armv7hl|i486|all]"
            exit 1
            ;;
    esac

    echo ""
    echo "=== Build complete ==="
    ls -lh "$LIB_DIR"/*/
}

main "$@"
