#!/bin/bash
# Downloads the Zig compiler and all Zig package dependencies needed for
# offline OBS builds. Produces two files to upload to OBS as Source1/Source2.
#
# Uses zig build to fetch deps from ghostty's build.zig.zon (which references
# stable mirror URLs). Run this whenever the ghostty submodule is updated.
#
# Usage: ./scripts/chum-create-zig-deps-tarball.sh [output-dir]
# Default: rpm/ (spec expects Source1/Source2 there)
#
# Outputs:
#   zig-x86_64-linux-<version>.tar.xz  (Zig compiler)
#   zig-deps-cache.tar.gz             (all Zig package deps)

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

# ── Fetch Zig package dependencies via zig build ─────────────────────
# Uses build.zig.zon as the source of truth (stable mirror URLs), not the
# flatpak zig-packages.json which can reference stale GitHub commits.
echo ""
echo "=== Fetching Zig package dependencies via zig build ==="
tar -xJf "$ZIG_OUTPUT" -C "$WORK_DIR"
ZIG_BIN="${WORK_DIR}/zig-x86_64-linux-${ZIG_VERSION}/zig"

# Run zig build to ensure all deps are fetched. Zig 0.16.0 stores packages
# in the default global cache (~/.cache/zig/p/); --global-cache-dir only
# redirects the compiler cache (z/), not the package cache (p/).
cd "$GHOSTTY_DIR"
"$ZIG_BIN" build -Demit-lib-vt -Dtarget=x86-linux-gnu.2.28 -Doptimize=ReleaseSafe 2>&1 || \
    echo "  (build errors expected — deps are cached regardless)"
cd "$REPO_ROOT"

# Copy deps from the default cache to the work directory.
ZIG_CACHE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/zig"
if [ ! -d "${ZIG_CACHE_DIR}/p" ] || [ -z "$(ls -A "${ZIG_CACHE_DIR}/p" 2>/dev/null)" ]; then
    echo "ERROR: No package cache found at ${ZIG_CACHE_DIR}/p" >&2
    echo "Run ./scripts/build-libs.sh first to populate the cache" >&2
    exit 1
fi
mkdir -p "${WORK_DIR}/p"
cp -r "${ZIG_CACHE_DIR}/p/"* "${WORK_DIR}/p/"

PKG_COUNT=$(find "${WORK_DIR}/p" -maxdepth 1 -mindepth 1 -type d | wc -l)
echo "  Copied $PKG_COUNT packages to cache"

# ── Strip bloat never needed for a `zig build --system` release build ───
# Keeps the cache under OBS's RPM build disk limit.
#
# Some deps (e.g. libxev) openDir() these dirs at configure time, so
# stripping a referenced name crashes `zig build` with FileNotFound.
# Skip names referenced as string literals in the package's build.zig.
echo ""
echo "=== Stripping test/doc/example bloat ==="
STRIP_NAMES=(
    test tests Test
    doc docs
    example examples samples
    fuzz fuzzing
    reference
    ci .ci .github
    gnulib-tests gnulib-local
    # gettext: only gettext-runtime/intl/ is used
    gettext-tools libtextstyle
    # libxml2: XML conformance test output
    result xstc
)
BEFORE_SIZE=$(du -sh "${WORK_DIR}/p" | awk '{print $1}')
for pkg in "${WORK_DIR}/p"/*/; do
    [ -d "$pkg" ] || continue
    for name in "${STRIP_NAMES[@]}"; do
        # bare "name" or path form "name/..." → dir is needed at configure
        # time; keep it to avoid an openDir FileNotFound crash.
        if [ -f "${pkg}build.zig" ] && grep -qF -e "\"$name\"" -e "\"$name/" "${pkg}build.zig"; then
            echo "  keeping $name/ in $(basename "$pkg") (referenced by build.zig)"
            continue
        fi
        find "$pkg" -type d -name "$name" -prune -exec rm -rf {} +
    done
done
AFTER_SIZE=$(du -sh "${WORK_DIR}/p" | awk '{print $1}')
echo "  Cache size: ${AFTER_SIZE} (was ${BEFORE_SIZE})"

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
