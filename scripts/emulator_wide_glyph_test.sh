#!/bin/bash
set -euo pipefail

# Sailfish OS Emulator Wide-Glyph Rendering Test
#
# Pushes scripts/wide-glyph-catalog.sh to the emulator and renders it through
# Ghosteel so you can visually validate wide-glyph rendering (#76).
# Tests that emoji, CJK, fullwidth, and ZWJ sequences occupy correct cell
# widths, and that ambiguous-width box-drawing and narrow Latin are NOT widened.
#
# Usage: ./emulator_wide_glyph_test.sh           # push catalog + launch in Ghosteel
#        ./emulator_wide_glyph_test.sh --clean    # remove catalog from emulator

SSH_KEY="$HOME/SailfishOS/vmshare/ssh/private_keys/sdk"
SSH_PORT=2223
SSH_USER="defaultuser"
SSH_HOST="localhost"
SSH_TARGET="$SSH_USER@$SSH_HOST"

SSH_OPTS=(-o StrictHostKeyChecking=no -o ConnectTimeout=5 -p "$SSH_PORT" -i "$SSH_KEY")
SCP_OPTS=(-o StrictHostKeyChecking=no -o ConnectTimeout=5 -P "$SSH_PORT" -i "$SSH_KEY")

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CATALOG="$SCRIPT_DIR/wide-glyph-catalog.sh"
REMOTE_CATALOG="/tmp/ghosteel-wide-glyph-catalog.sh"
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
    # Sets OSC 2 title so the session is easy to find in the session list.
    ssh "${SSH_OPTS[@]}" "$SSH_TARGET" \
        "ghosteel -e $REMOTE_CATALOG > /tmp/ghosteel-wide-glyph.log 2>&1 &"
    sleep 2
else
    echo "  Ghosteel already running — opening catalog in a new session..."
    ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "ghosteel -e $REMOTE_CATALOG" \
        > /tmp/ghosteel-wide-glyph.log 2>&1 || true
fi

echo "[3/3] Done. The wide-glyph catalog should now be visible in the emulator."
echo ""
echo "============================================================"
echo "=== VISUAL RUBRIC (wide-glyph rendering) ==="
echo "============================================================"
echo ""
echo "PASS if:"
echo "  - 😀 😎 fill 2 cells each (no horizontal squashing to ~50% width)"
echo "  - 国 人 each fill 2 cells"
echo "  - ＡＢＣ each fill 2 cells"
echo "  - ─ │ ┌ ┐ each fill 1 cell (Ambiguous, NOT widened)"
echo "  - abc ABC each fill 1 cell (Narrow, NOT widened)"
echo "  - 👨‍👩‍👧 renders as ONE composite family glyph (not a lone 👨)"
echo "  - In \"A😀B国C\": 😀 and 国 each occupy 2 cells;"
echo "    B and C follow immediately with no gap"
echo ""
echo "FAIL (pre-fix symptom) if:"
echo "  - Any wide glyph looks horizontally squeezed AND followed by an empty gap"
echo "  - The ZWJ family shows just a lone 👨 (base codepoint only)"
echo ""
echo "The catalog blocks (reads stdin) so the session stays open until you"
echo "close it from the UI or press Ctrl+D — otherwise the -e command exits"
echo "and Ghosteel auto-closes the session."
echo ""
echo "Re-run inside any Ghosteel session:  $REMOTE_CATALOG"
echo "Clean up:                            $0 --clean"
