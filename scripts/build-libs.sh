#!/bin/bash
# Build libghostty-vt static libraries for all Sailfish OS architectures.
# Requires Zig 0.16.0 (see README for installation).
#
# Usage: ./scripts/build-libs.sh [aarch64|armv7hl|i486|all]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
GHOSTTY_DIR="$PROJECT_ROOT/ghostty"
LIB_DIR="$PROJECT_ROOT/lib"

ZIG_VERSION_REQUIRED="0.16.0"
ZIG="${ZIG:-zig}"

# Check Zig version
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

    cd "$GHOSTTY_DIR"
    "$ZIG" build -Demit-lib-vt -Dtarget="$target" -Doptimize=ReleaseSafe 2>&1

    cp "$GHOSTTY_DIR/zig-out/lib/libghostty-vt.a" "$out_dir/"
    echo "  -> $out_dir/libghostty-vt.a"
}

main() {
    local arch="${1:-all}"

    check_zig

    echo "=== Building libghostty-vt for Sailfish OS ==="
    echo "Zig: $("$ZIG" version)"
    echo ""

    case "$arch" in
        aarch64)
            build_arch "aarch64" "aarch64-linux-gnu.2.28"
            ;;
        armv7hl)
            build_arch "armv7hl" "arm-linux-gnueabihf.2.28"
            ;;
        i486)
            build_arch "i486" "x86-linux-gnu.2.28"
            ;;
        all)
            build_arch "aarch64" "aarch64-linux-gnu.2.28"
            build_arch "armv7hl" "arm-linux-gnueabihf.2.28"
            build_arch "i486" "x86-linux-gnu.2.28"
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
