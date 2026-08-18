#!/bin/bash
set -euo pipefail

# Sailfish OS Emulator Kitty Graphics Render Test
#
# Pushes scripts/kitty-image-catalog.sh to the emulator and renders it
# through Ghosteel so you can visually validate the kitty image pipeline
# (src/glrenderer_kitty.cpp upload/cache paths and the two-pass cell draw
# that puts below-text images above the background, under the text).
#
# Usage: ./emulator_kitty_image_test.sh           # push catalog + launch
#        ./emulator_kitty_image_test.sh --clean   # remove from emulator

SSH_KEY="$HOME/SailfishOS/vmshare/ssh/private_keys/sdk"
SSH_PORT=2223
SSH_USER="defaultuser"
SSH_HOST="localhost"
SSH_TARGET="$SSH_USER@$SSH_HOST"

SSH_OPTS=(-o StrictHostKeyChecking=no -o ConnectTimeout=5 -p "$SSH_PORT" -i "$SSH_KEY")
SCP_OPTS=(-o StrictHostKeyChecking=no -o ConnectTimeout=5 -P "$SSH_PORT" -i "$SSH_KEY")

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CATALOG="$SCRIPT_DIR/kitty-image-catalog.sh"
REMOTE_CATALOG="/tmp/ghosteel-kitty-image-catalog.sh"
SOCKET_PATH="/run/user/100000/ghosteel-singleton"

# --- Pre-flight checks ---

if [[ ! -f "$CATALOG" ]]; then
    echo "ERROR: Catalog not found: $CATALOG" >&2
    exit 1
fi

if [[ ! -f "$SSH_KEY" ]]; then
    echo "ERROR: SSH key not found: $SSH_KEY" >&2
    exit 1
fi

echo "[0/3] Checking emulator connectivity..."
if ! ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "true" 2>/dev/null; then
    echo "ERROR: Cannot reach emulator at $SSH_HOST:$SSH_PORT" >&2
    echo "Is the Sailfish OS emulator running?" >&2
    exit 1
fi

# --- Clean mode ---

if [[ "${1:-}" == "--clean" ]]; then
    echo "[1/1] Removing catalog from emulator..."
    ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "rm -f $REMOTE_CATALOG"
    echo "Done. Catalog removed."
    exit 0
fi

# --- Main sequence ---

echo "[1/3] Uploading catalog..."
scp "${SCP_OPTS[@]}" "$CATALOG" "$SSH_TARGET:$REMOTE_CATALOG"
ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "chmod +x $REMOTE_CATALOG"

echo "[2/3] Ensuring Ghosteel is running..."
if ! ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "test -S $SOCKET_PATH" 2>/dev/null; then
    echo "  Ghosteel not running — cold-starting with the catalog..."
    ssh "${SSH_OPTS[@]}" "$SSH_TARGET" \
        "ghosteel -e $REMOTE_CATALOG > /tmp/ghosteel-kitty.log 2>&1 &"
    sleep 2
else
    echo "  Ghosteel already running — opening catalog in a new session..."
    ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "ghosteel -e $REMOTE_CATALOG" \
        > /tmp/ghosteel-kitty.log 2>&1 || true
fi

echo "[3/3] Done. The catalog should now be visible in the emulator."
echo ""
echo "============================================================"
echo "VISUAL RUBRIC — what to check in the Ghosteel window"
echo "============================================================"
echo "  [A] Default   z=0 (same above-text bucket as C in ghostty; kept"
echo "                as boundary documentation). Image visible, X's under"
echo "                it hidden."
echo "  [B] z=-1      THE key check (two-pass cell draw):"
echo "                X's readable ON TOP of the image, image above the"
echo "                background. FAIL if the image is veiled/dimmed by"
echo "                the background color or invisible at opacity 1.0."
echo "  [C] z=1       Icon fully covers the X's."
echo "  [D] f=32      Crisp blue/yellow checker, exact colors."
echo "                Black or garbage rects = upload-path failure."
echo ""
echo "PASS = B shows text over a fully visible icon; C covers text;"
echo "       D is exact colors."
echo "FAIL = B's icon dimmed/hidden under the background, or D shows"
echo "       black/garbage rectangles."
echo ""
echo "For the strictest B check, set backgroundOpacity=1.0 in settings"
echo "first: the below-text icon must remain visible even at full opacity."
echo ""
echo "The catalog blocks (reads stdin) so the session stays open until you"
echo "close it from the UI or press Ctrl+D."
echo ""
echo "Re-run inside any Ghosteel session:  $REMOTE_CATALOG"
echo "Clean up:                            $0 --clean"
