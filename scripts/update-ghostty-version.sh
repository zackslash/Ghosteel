#!/bin/bash
# Updates ghostty.version with the current ghostty submodule commit.
# Run this after bumping the ghostty submodule, before tagging a release.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"

if [ ! -d "$REPO_DIR/ghostty/.git" ] && [ ! -f "$REPO_DIR/ghostty/.git" ]; then
    echo "ERROR: ghostty submodule not initialized" >&2
    echo "Run: git submodule update --init" >&2
    exit 1
fi

SHA=$(git -C "$REPO_DIR/ghostty" rev-parse --short=9 HEAD | cut -c1-9)

# CI derives GHOSTTY_VERSION from the committed gitlink; warn when it
# disagrees with the checkout so the two can't drift silently.
STAGED=$(git -C "$REPO_DIR" ls-files -s ghostty | awk '{print $2}')
CHECKED_OUT=$(git -C "$REPO_DIR/ghostty" rev-parse HEAD)
if [ -n "$STAGED" ] && [ "$STAGED" != "$CHECKED_OUT" ]; then
    echo "WARN: staged submodule pin ($STAGED) != checked-out HEAD ($CHECKED_OUT)" >&2
    echo "      commit the bump or CI and ghostty.version disagree" >&2
fi

echo "$SHA" > "$REPO_DIR/ghostty.version"
echo "Updated ghostty.version: $SHA"
