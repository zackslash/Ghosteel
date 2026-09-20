#!/bin/bash
set -euo pipefail

# Sailfish OS Emulator Truecolor Render Test
#
# Pushes scripts/truecolor-catalog.sh to the emulator and renders it
# through Ghosteel so you can visually validate both halves of the
# truecolor support: the COLORTERM=truecolor advertisement set for every
# spawned session (src/ptymanager.cpp) and the 24-bit SGR rendering
# itself (ghostty core parsing + glrenderer exact-RGB cell fills).
#
# Usage: ./emulator_truecolor_test.sh           # push catalog + launch
#        ./emulator_truecolor_test.sh --clean   # remove from emulator

SSH_KEY="$HOME/SailfishOS/vmshare/ssh/private_keys/sdk"
SSH_PORT=2223
SSH_USER="defaultuser"
SSH_HOST="localhost"
SSH_TARGET="$SSH_USER@$SSH_HOST"

SSH_OPTS=(-o StrictHostKeyChecking=no -o ConnectTimeout=5 -p "$SSH_PORT" -i "$SSH_KEY")
SCP_OPTS=(-o StrictHostKeyChecking=no -o ConnectTimeout=5 -P "$SSH_PORT" -i "$SSH_KEY")

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CATALOG="$SCRIPT_DIR/truecolor-catalog.sh"
REMOTE_CATALOG="/tmp/ghosteel-truecolor-catalog.sh"
GUEST_LOG="/tmp/ghosteel-truecolor.log"
HOST_LOG="/tmp/ghosteel-truecolor.log"
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
    echo "[1/1] Removing catalog and logs from emulator..."
    ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "rm -f $REMOTE_CATALOG $GUEST_LOG"
    rm -f "$HOST_LOG"
    echo "Done. Catalog and logs removed."
    exit 0
fi

# --- Main sequence ---

echo "[1/3] Uploading catalog..."
scp "${SCP_OPTS[@]}" "$CATALOG" "$SSH_TARGET:$REMOTE_CATALOG"
ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "chmod +x $REMOTE_CATALOG"

echo "[2/3] Ensuring Ghosteel is running..."
if ! ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "test -S $SOCKET_PATH" 2>/dev/null; then
    echo "  Ghosteel not running - cold-starting with the catalog..."
    ssh "${SSH_OPTS[@]}" "$SSH_TARGET" \
        "ghosteel -e $REMOTE_CATALOG > $GUEST_LOG 2>&1 &"
    sleep 2
else
    echo "  Ghosteel already running - opening catalog in a new session..."
    if ! ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "ghosteel -e $REMOTE_CATALOG" \
            2>&1 | tee "$HOST_LOG"; then
        echo "ERROR: ghosteel -e failed (stale singleton socket?)." >&2
        echo "--- log tail ---" >&2
        tail -20 "$HOST_LOG" >&2 || true
        exit 1
    fi
fi

echo "[3/3] Done. The catalog should now be visible in the emulator."
echo ""
echo "============================================================"
echo "VISUAL RUBRIC - what to check in the Ghosteel window"
echo "============================================================"
echo "  [A] Environment   COLORTERM must print 'truecolor'. An <unset>"
echo "                    (or other non-truecolor) value means the"
echo "                    deployed build predates the env fix (deploy the"
echo "                    new build first). Over plain ssh the catalog"
echo "                    warns: it reports that context's env instead."
echo "  [B] Gradient      Truecolor rows perfectly smooth; the labeled"
echo "                    256-color control strip SHOULD look banded."
echo "                    Truecolor rows resembling the control = FAIL."
echo "  [C] Swatches+ramp Each block exactly matches its RGB label; the"
echo "                    fg '#' ramp as smooth as [B]."
echo "  [D] Palette/direct  Direct blocks match their hue labels and are"
echo "                    visibly DISTINCT from the palette column (the"
echo "                    direct values are off-palette by design)."
echo ""
echo "PASS = A shows COLORTERM=truecolor, B smooth and clearly smoother"
echo "       than the control, C exact + smooth ramp, D direct blocks"
echo "       distinct and correctly hued."
echo "FAIL = [A] not COLORTERM=truecolor (stale build), banded truecolor"
echo "       rows, shifted/swapped swatch colors, or a direct block"
echo "       identical to its palette neighbor (snapped)."
echo ""
echo "The catalog blocks (reads stdin) so the session stays open until"
echo "you close it from the UI or press Ctrl+D."
echo ""
echo "Re-run inside any Ghosteel session:  $REMOTE_CATALOG"
echo "Clean up:                            $0 --clean"
