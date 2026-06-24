#!/bin/bash
# Generates the OBS _service file for Chum submission.
# Points tar_git at a specific tag or commit in the Ghosteel repo.
#
# Usage: ./scripts/chum-generate-service.sh [tag-or-commit]
# Default: HEAD
#
# Output: _service (in repo root, gitignored)

set -euo pipefail

REPO_URL="https://github.com/zackslash/Ghosteel.git"
OUTPUT="_service"
REVISION="${1:-HEAD}"

if [ "$REVISION" = "HEAD" ]; then
    REVISION=$(git rev-parse HEAD)
fi

# Validate it looks like a commit hash or tag
if ! git rev-parse --verify "$REVISION" >/dev/null 2>&1; then
    echo "ERROR: '$REVISION' is not a valid git ref" >&2
    exit 1
fi

# Resolve to full commit hash if it's a tag/short ref
FULL_HASH=$(git rev-parse "$REVISION")

cat > "$OUTPUT" <<EOF
<services>
  <service name="tar_git">
    <param name="url">${REPO_URL}</param>
    <param name="revision">${FULL_HASH}</param>
  </service>
</services>
EOF

echo "Wrote ${OUTPUT} (revision: ${FULL_HASH:0:12})"
