#!/bin/bash
# Creates a tarball of all Zig package dependencies needed for offline OBS builds.
# Uses Ghostty's Flatpak zig-packages.json as the authoritative package list.
#
# Run this whenever the ghostty submodule is updated (new release, version bump, etc.)
# then upload the resulting tarball to OBS as Source2.
#
# Usage: ./scripts/create-zig-deps-tarball.sh [output.tar.gz]
# Default output: zig-deps-cache.tar.gz (in repo root)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GHOSTTY_DIR="${REPO_ROOT}/ghostty"
PACKAGES_JSON="${GHOSTTY_DIR}/flatpak/zig-packages.json"
OUTPUT="${1:-${REPO_ROOT}/zig-deps-cache.tar.gz}"
WORK_DIR=$(mktemp -d)

trap 'rm -rf "$WORK_DIR"' EXIT

if [ ! -f "$PACKAGES_JSON" ]; then
    echo "ERROR: $PACKAGES_JSON not found" >&2
    echo "Make sure the ghostty submodule is initialized" >&2
    exit 1
fi

echo "Reading packages from $PACKAGES_JSON..."
mkdir -p "${WORK_DIR}/p"

# Parse JSON and download each package
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
echo "Creating tarball: $OUTPUT"
tar -czf "$OUTPUT" -C "${WORK_DIR}" p

SIZE=$(du -h "$OUTPUT" | awk '{print $1}')
echo ""
echo "Done! $OUTPUT ($SIZE)"
echo "Packages: $((COUNT - SKIPPED)) included, $SKIPPED skipped"
echo ""
echo "Upload this to OBS as Source2, replacing the previous zig-deps-cache.tar.gz."
