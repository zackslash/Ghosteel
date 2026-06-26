#!/bin/bash
# Updates ghostty-version file with the current ghostty submodule commit.
# Run this after bumping the ghostty submodule, before tagging a release.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"

if [ ! -d "$REPO_DIR/ghostty/.git" ] && [ ! -f "$REPO_DIR/ghostty/.git" ]; then
    echo "ERROR: ghostty submodule not initialized" >&2
    echo "Run: git submodule update --init" >&2
    exit 1
fi

SHA=$(git -C "$REPO_DIR" ls-tree HEAD ghostty | awk '{print $3}' | cut -c1-9)
if [ -z "$SHA" ]; then
    echo "ERROR: could not determine ghostty commit" >&2
    exit 1
fi

echo "$SHA" > "$REPO_DIR/rpm/ghostty-version"
echo "Updated rpm/ghostty-version: $SHA"
