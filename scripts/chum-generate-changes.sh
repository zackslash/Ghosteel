#!/bin/bash
# Generates rpm/ghosteel.changes from GitHub releases.
# Run before uploading to OBS for Chum submissions.
#
# Usage: ./scripts/chum-generate-changes.sh

set -euo pipefail

REPO="zackslash/Ghosteel"
OUTPUT="rpm/ghosteel.changes"

if ! command -v gh >/dev/null 2>&1; then
    echo "ERROR: gh (GitHub CLI) is required" >&2
    exit 1
fi

echo "Fetching releases from ${REPO}..."

# Get all releases, oldest first
TAGS=$(gh release list --repo "$REPO" --limit 100 --json tagName,publishedAt \
    | jq -r 'sort_by(.publishedAt) | .[] | .tagName')

{
for tag in $TAGS; do
    # Get release metadata
    INFO=$(gh release view "$tag" --repo "$REPO" --json body,publishedAt)
    DATE=$(echo "$INFO" | jq -r '.publishedAt' | xargs -I{} date -d {} "+%a %b %d %Y")
    BODY=$(echo "$INFO" | jq -r '.body')

    # Strip "v" prefix for version
    VERSION="${tag#v}"

    # Extract meaningful bullet points (lines starting with *)
    # Skip empty lines, "Full Changelog", "New Contributors", "What's Changed" headers
    CHANGES=$(echo "$BODY" | grep -E '^\* ' \
        | grep -v '^\*\*Full Changelog' \
        | grep -v '^\*\*New Contributors' \
        | sed 's/^\* /- /' \
        | sed 's/ by @[^ ]*//' \
        | sed 's/ in https:\/\/github.com\/[^ ]*//' \
        | head -20)

    # If no bullet points found, use a generic entry
    if [ -z "$CHANGES" ]; then
        CHANGES="- See https://github.com/${REPO}/releases/tag/${tag}"
    fi

    echo "* ${DATE} Luke Hines <luke@hines.im> - ${VERSION}"
    echo "${CHANGES}"
    echo ""
done
} > "$OUTPUT"

COUNT=$(grep -c '^\*' "$OUTPUT")
echo "Wrote ${OUTPUT} (${COUNT} releases)"
