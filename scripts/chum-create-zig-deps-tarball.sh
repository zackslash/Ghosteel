#!/bin/bash
# Downloads the Zig compiler and all Zig package dependencies needed for
# offline OBS builds. Produces two files to upload to OBS as Source1/Source2.
#
# Uses zig build --fetch=all to resolve the full dependency tree, then
# extracts and strips each package for smaller cache size. The extracted
# dirs are shipped (not tarballs) and used with --system mode, which
# trusts pre-extracted directories without hash verification.
#
# Usage: ./scripts/chum-create-zig-deps-tarball.sh [output-dir]
# Default: rpm/ (spec expects Source1/Source2 there)
#
# Outputs:
#   zig-x86_64-linux-<version>.tar.xz  (Zig compiler)
#   zig-deps-cache.tar.gz             (stripped Zig package deps as dirs)

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
echo ""
echo "=== Fetching Zig package dependencies ==="
tar -xJf "$ZIG_OUTPUT" -C "$WORK_DIR"
ZIG_BIN="${WORK_DIR}/zig-x86_64-linux-${ZIG_VERSION}/zig"

# Delete zig-pkg/ so all packages are fetched fresh into the isolated cache.
rm -rf "${GHOSTTY_DIR}/zig-pkg"

cd "$GHOSTTY_DIR"
# --fetch=all resolves and downloads the full dep tree but exits non-zero
# if downstream build steps fail; deps are cached regardless.
"$ZIG_BIN" build --fetch=all --global-cache-dir "$WORK_DIR" 2>&1 || true
cd "$REPO_ROOT"

if [ ! -d "${WORK_DIR}/p" ] || [ -z "$(ls -A "${WORK_DIR}/p" 2>/dev/null)" ]; then
    echo "ERROR: No packages fetched to ${WORK_DIR}/p" >&2
    exit 1
fi

TARBALL_COUNT=$(find "${WORK_DIR}/p" -maxdepth 1 -type f -name '*.tar.gz' | wc -l)
echo "  Fetched $TARBALL_COUNT packages"

# ── Extract and strip each package ───────────────────────────────────
# Extract tarballs to directories, strip test/doc/example bloat, and ship
# the dirs (not tarballs). --system mode trusts dirs without hash
# verification, allowing us to strip without breaking Zig's content hashes.
echo ""
echo "=== Extracting and stripping packages ==="
STRIP_NAMES=(
    test tests Test
    doc docs
    example examples samples
    fuzz fuzzing
    reference
    ci .ci .github
    perf kokoro ndk_test gtests
    gnulib-tests gnulib-local
    gettext-tools libtextstyle
    result xstc
)

mkdir -p "${WORK_DIR}/pkg"
EXTRACTED=0
for pkg_tarball in "${WORK_DIR}/p"/*.tar.gz; do
    [ -f "$pkg_tarball" ] || continue
    hash=$(basename "$pkg_tarball" .tar.gz)
    pkg_dir="${WORK_DIR}/pkg/${hash}"
    mkdir -p "$pkg_dir"
    tar -xzf "$pkg_tarball" --strip-components=1 -C "$pkg_dir" 2>/dev/null

    # Strip bloat never needed for a release build.
    for name in "${STRIP_NAMES[@]}"; do
        # Keep dirs referenced as string literals in build.zig
        # (some packages openDir() them at configure time).
        if [ -f "${pkg_dir}/build.zig" ] && grep -qF -e "\"$name\"" -e "\"$name/" "${pkg_dir}/build.zig"; then
            continue
        fi
        find "$pkg_dir" -type d -name "$name" -prune -exec rm -rf {} +
    done

    EXTRACTED=$((EXTRACTED + 1))
done

PKG_SIZE=$(du -sh "${WORK_DIR}/pkg" | awk '{print $1}')
echo "  Extracted and stripped $EXTRACTED packages (${PKG_SIZE})"

# ── Create deps tarball ──────────────────────────────────────────────
echo ""
echo "Creating deps tarball: $DEPS_OUTPUT"
tar -czf "$DEPS_OUTPUT" -C "${WORK_DIR}/pkg" .

DEPS_SIZE=$(du -h "$DEPS_OUTPUT" | awk '{print $1}')
ZIG_SIZE=$(du -h "$ZIG_OUTPUT" | awk '{print $1}')
echo ""
echo "=== Done ==="
echo "  Zig compiler: $ZIG_OUTPUT ($ZIG_SIZE)   → upload as OBS Source1"
echo "  Zig deps:     $DEPS_OUTPUT ($DEPS_SIZE) → upload as OBS Source2"
echo "  Packages:     $EXTRACTED"
