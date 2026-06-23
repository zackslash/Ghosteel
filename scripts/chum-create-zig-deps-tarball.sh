#!/bin/bash
# Downloads the Zig compiler and all Zig package dependencies needed for
# offline OBS builds. Produces two files to upload to OBS as Source1/Source2.
#
# Uses Ghostty's Flatpak zig-packages.json as the authoritative dep list.
# Run this whenever the ghostty submodule is updated (new release, version bump, etc.)
#
# Usage: ./scripts/chum-create-zig-deps-tarball.sh [output-dir]
# Default output: repo root
#
# Outputs:
#   zig-x86_64-linux-<version>.tar.xz  (Zig compiler binary — upload as Source1)
#   zig-deps-cache.tar.gz               (all Zig package deps — upload as Source2)

set -euo pipefail

ZIG_VERSION="0.15.2"
ZIG_URL="https://ziglang.org/download/${ZIG_VERSION}/zig-x86_64-linux-${ZIG_VERSION}.tar.xz"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GHOSTTY_DIR="${REPO_ROOT}/ghostty"
PACKAGES_JSON="${GHOSTTY_DIR}/flatpak/zig-packages.json"
OUTPUT_DIR="${1:-${REPO_ROOT}}"
WORK_DIR=$(mktemp -d)

ZIG_OUTPUT="${OUTPUT_DIR}/zig-x86_64-linux-${ZIG_VERSION}.tar.xz"
DEPS_OUTPUT="${OUTPUT_DIR}/zig-deps-cache.tar.gz"

trap 'rm -rf "$WORK_DIR"' EXIT

if [ ! -f "$PACKAGES_JSON" ]; then
    echo "ERROR: $PACKAGES_JSON not found" >&2
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

# ── Download Zig package dependencies ────────────────────────────────
echo ""
echo "=== Downloading Zig package dependencies ==="
echo "Reading packages from $PACKAGES_JSON..."
mkdir -p "${WORK_DIR}/p"

TOTAL=$(jq length "$PACKAGES_JSON")
COUNT=0
SKIPPED=0

for i in $(seq 0 $((TOTAL - 1))); do
    TYPE=$(jq -r ".[$i].type" "$PACKAGES_JSON")
    DEST=$(jq -r ".[$i].dest" "$PACKAGES_JSON")
    # Strip "vendor/" prefix — we build the p/ directory directly
    DEST="${DEST#vendor/}"

    COUNT=$((COUNT + 1))

    if [ "$TYPE" = "git" ]; then
        URL=$(jq -r ".[$i].url" "$PACKAGES_JSON")
        COMMIT=$(jq -r ".[$i].commit" "$PACKAGES_JSON")
        echo "[$COUNT/$TOTAL] git clone $URL @ $COMMIT -> $DEST"
        git clone --quiet "$URL" "${WORK_DIR}/${DEST}"
        git -C "${WORK_DIR}/${DEST}" checkout --quiet "$COMMIT"
        rm -rf "${WORK_DIR}/${DEST}/.git"
    elif [ "$TYPE" = "archive" ]; then
        URL=$(jq -r ".[$i].url" "$PACKAGES_JSON")
        SHA256=$(jq -r ".[$i].sha256" "$PACKAGES_JSON")
        FILENAME=$(basename "$URL")
        echo "[$COUNT/$TOTAL] $FILENAME -> $DEST"

        # Download
        curl -fsSL -o "${WORK_DIR}/${FILENAME}" "$URL"

        # Verify checksum
        ACTUAL_SHA256=$(sha256sum "${WORK_DIR}/${FILENAME}" | awk '{print $1}')
        if [ "$ACTUAL_SHA256" != "$SHA256" ]; then
            echo "  ERROR: SHA256 mismatch for $FILENAME" >&2
            echo "  Expected: $SHA256" >&2
            echo "  Actual:   $ACTUAL_SHA256" >&2
            exit 1
        fi

        # Extract — detect compression
        mkdir -p "${WORK_DIR}/${DEST}"
        case "$FILENAME" in
            *.tar.gz|*.tgz)  tar -xzf "${WORK_DIR}/${FILENAME}" --strip-components=1 -C "${WORK_DIR}/${DEST}" ;;
            *.tar.xz)        tar -xJf "${WORK_DIR}/${FILENAME}" --strip-components=1 -C "${WORK_DIR}/${DEST}" ;;
            *.tar.zst)       tar --zstd -xf "${WORK_DIR}/${FILENAME}" --strip-components=1 -C "${WORK_DIR}/${DEST}" ;;
            *.tar)           tar -xf "${WORK_DIR}/${FILENAME}" --strip-components=1 -C "${WORK_DIR}/${DEST}" ;;
            *.zip)           unzip -q -o "${WORK_DIR}/${FILENAME}" -d "${WORK_DIR}/${DEST}" ;;
            *)               echo "  WARNING: Unknown archive format: $FILENAME" >&2; SKIPPED=$((SKIPPED + 1)) ;;
        esac

        # Clean up downloaded file
        rm -f "${WORK_DIR}/${FILENAME}"
    else
        echo "[$COUNT/$TOTAL] Unknown type: $TYPE, skipping"
        SKIPPED=$((SKIPPED + 1))
    fi
done

echo ""
echo "Creating deps tarball: $DEPS_OUTPUT"
tar -czf "$DEPS_OUTPUT" -C "${WORK_DIR}" p

DEPS_SIZE=$(du -h "$DEPS_OUTPUT" | awk '{print $1}')
ZIG_SIZE=$(du -h "$ZIG_OUTPUT" | awk '{print $1}')
echo ""
echo "=== Done ==="
echo "  Zig compiler: $ZIG_OUTPUT ($ZIG_SIZE)   → upload as OBS Source1"
echo "  Zig deps:     $DEPS_OUTPUT ($DEPS_SIZE) → upload as OBS Source2"
echo "  Packages:     $((COUNT - SKIPPED)) included, $SKIPPED skipped"
