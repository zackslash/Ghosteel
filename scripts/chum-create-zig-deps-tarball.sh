#!/bin/bash
# Downloads the Zig compiler and all Zig package dependencies needed for
# offline OBS builds. Produces two files to upload to OBS as Source1/Source2.
#
# Uses zig build --fetch=all to resolve the full dependency tree from
# ghostty's build.zig.zon and store canonical tarballs in an isolated
# cache directory. Run this whenever the ghostty submodule is updated.
#
# Usage: ./scripts/chum-create-zig-deps-tarball.sh [output-dir]
# Default: rpm/ (spec expects Source1/Source2 there)
#
# Outputs:
#   zig-x86_64-linux-<version>.tar.xz  (Zig compiler)
#   zig-deps-cache.tar.gz             (all Zig package deps as tarballs)

set -euo pipefail

ZIG_VERSION="0.16.0"
ZIG_URL="https://ziglang.org/download/${ZIG_VERSION}/zig-x86_64-linux-${ZIG_VERSION}.tar.xz"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GHOSTTY_DIR="${REPO_ROOT}/ghostty"
OUTPUT_DIR="${1:-${REPO_ROOT}/rpm}"
mkdir -p "$OUTPUT_DIR"
WORK_DIR=$(mktemp -d)

ZIG_OUTPUT="${OUTPUT_DIR}/zig-x86_64-linux-${ZIG_VERSION}.tar.xz"
DEPS_OUTPUT="${OUTPUT_DIR}/zig-deps-cache.tar.gz"

trap 'rm -rf "$WORK_DIR"' EXIT

if [ ! -f "${GHOSTTY_DIR}/build.zig.zon" ]; then
    echo "ERROR: ${GHOSTTY_DIR}/build.zig.zon not found" >&2
    echo "Make sure the ghostty submodule is initialized" >&2
    exit 1
fi

# ── Download Zig compiler ────────────────────────────────────────────
echo "=== Downloading Zig ${ZIG_VERSION} ==="
if [ -f "$ZIG_OUTPUT" ]; then
    echo "  Already exists: $ZIG_OUTPUT (skipping)"
else
    echo "  Downloading: $ZIG_URL"
    curl -fsSL -o "${ZIG_OUTPUT}.partial" "$ZIG_URL"
    mv "${ZIG_OUTPUT}.partial" "$ZIG_OUTPUT"
    echo "  Saved: $ZIG_OUTPUT ($(du -h "$ZIG_OUTPUT" | awk '{print $1}'))"
fi

# ── Fetch all Zig package dependencies ───────────────────────────────
# Uses --fetch=all to resolve the full dependency tree (including lazy
# and transitive deps) and --global-cache-dir to isolate the cache so
# only ghostty's current deps are included (no old versions or pollution
# from other projects).
echo ""
echo "=== Fetching Zig package dependencies ==="
tar -xJf "$ZIG_OUTPUT" -C "$WORK_DIR"
ZIG_BIN="${WORK_DIR}/zig-x86_64-linux-${ZIG_VERSION}/zig"

# Delete zig-pkg/ so all packages are fetched fresh into the isolated cache.
# Zig 0.16.0 stores extracted deps in zig-pkg/ (project-local); if it already
# exists, no network fetch occurs and no tarballs are written to p/.
rm -rf "${GHOSTTY_DIR}/zig-pkg"

cd "$GHOSTTY_DIR"
"$ZIG_BIN" build --fetch=all --global-cache-dir "$WORK_DIR" 2>&1 || true
cd "$REPO_ROOT"

if [ ! -d "${WORK_DIR}/p" ] || [ -z "$(ls -A "${WORK_DIR}/p" 2>/dev/null)" ]; then
    echo "ERROR: No packages fetched to ${WORK_DIR}/p" >&2
    exit 1
fi

PKG_COUNT=$(find "${WORK_DIR}/p" -maxdepth 1 -type f -name '*.tar.gz' | wc -l)
echo "  Fetched $PKG_COUNT packages"

# ── Create deps tarball ──────────────────────────────────────────────
echo ""
echo "Creating deps tarball: $DEPS_OUTPUT"
tar -czf "$DEPS_OUTPUT" -C "${WORK_DIR}" p

DEPS_SIZE=$(du -h "$DEPS_OUTPUT" | awk '{print $1}')
ZIG_SIZE=$(du -h "$ZIG_OUTPUT" | awk '{print $1}')
echo ""
echo "=== Done ==="
echo "  Zig compiler: $ZIG_OUTPUT ($ZIG_SIZE)   → upload as OBS Source1"
echo "  Zig deps:     $DEPS_OUTPUT ($DEPS_SIZE) → upload as OBS Source2"
echo "  Packages:     $PKG_COUNT"
