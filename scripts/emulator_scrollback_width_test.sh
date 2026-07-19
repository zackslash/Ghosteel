#!/bin/bash
set -euo pipefail

# Sailfish OS Emulator Scrollback Width Restore Test
#
# Pushes scripts/scrollback-width-catalog.sh to the emulator and renders it
# through Ghosteel so you can visually validate scrollback display width
# restore (#72). The bug only manifests on RESTORE/RESTART — live echo looks
# fine. This is a "recipe" script: it sets up the scenario, then the rubric
# documents the manual restart steps.
#
# Usage: ./emulator_scrollback_width_test.sh           # push catalog + launch
#        ./emulator_scrollback_width_test.sh --clean    # remove catalog

SSH_KEY="$HOME/SailfishOS/vmshare/ssh/private_keys/sdk"
SSH_PORT=2223
SSH_USER="defaultuser"
SSH_HOST="localhost"
SSH_TARGET="$SSH_USER@$SSH_HOST"

SSH_OPTS=(-o StrictHostKeyChecking=no -o ConnectTimeout=5 -p "$SSH_PORT" -i "$SSH_KEY")
SCP_OPTS=(-o StrictHostKeyChecking=no -o ConnectTimeout=5 -P "$SSH_PORT" -i "$SSH_KEY")

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CATALOG="$SCRIPT_DIR/scrollback-width-catalog.sh"
REMOTE_CATALOG="/tmp/ghosteel-scrollback-width-catalog.sh"
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
        "ghosteel -e $REMOTE_CATALOG > /tmp/ghosteel-scrollback-width.log 2>&1 &"
    sleep 2
else
    echo "  Ghosteel already running — opening catalog in a new session..."
    ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "ghosteel -e $REMOTE_CATALOG" \
        > /tmp/ghosteel-scrollback-width.log 2>&1 || true
fi

echo "[3/3] Done. The scrollback-width catalog should now be visible in the emulator."
echo ""
echo "============================================================"
echo "=== VISUAL RUBRIC (scrollback width restore) ==="
echo "============================================================"
echo ""
echo "THIS BUG ONLY MANIFESTS ON RESTORE — live echo will look fine."
echo ""
echo "Step 1: The payload above has filled the scrollback with non-ASCII text"
echo "        at known widths. Wait for it to finish."
echo ""
echo "Step 2: Restart Ghosteel, OR close+reopen the session. This triggers the"
echo "        scrollback export+restore path where #3 and #4 live."
echo "        (Resizing the window does NOT trigger this path — it uses ghostty's"
echo "        grid reflow, a different code path that won't surface either bug.)"
echo ""
echo "Step 3: After restore, verify:"
echo "  PASS if:"
echo "    - Cyrillic rows re-wrap on cell boundaries (no mid-glyph breaks)"
echo "    - Latin-1 rows (café/résumé/naïve) keep their accents intact, no corruption"
echo "    - Box-drawing borders remain aligned"
echo ""
echo "  FAIL (pre-fix symptom) if:"
echo "    - Rows wrap mid-glyph or have extra/missing blank lines"
echo ""
echo "  #4 (duplicate multibyte prompt on restore) — CANNOT trigger via this harness:"
echo "      The -e launch path runs the catalog as the session command with no shell,"
echo "      so ❯ in the payload is output text, not a shell prompt — bug #4's"
echo "      prompt-strip logic never sees it. To actually test #4, run the recipe"
echo "      manually inside a real shell:"
echo "        1. Open a shell session in Ghosteel (NOT via this script's -e mode)."
echo "        2. Set a multibyte prompt in the shell, e.g.:  PS1='❯ '"
echo "           (or use fish/elvish, which default to a ❯ prompt)."
echo "        3. Run the catalog inside that shell to fill scrollback:"
echo "             /tmp/ghosteel-scrollback-width-catalog.sh"
echo "        4. Restart Ghosteel (or close+reopen the session)."
echo "        5. FAIL (pre-fix) if ❯ appears TWICE — the stale exported prompt"
echo "                followed by the fresh re-emitted one."
echo "           PASS (post-fix) if ❯ appears ONCE."
echo ""
echo "The catalog blocks (reads stdin) so the session stays open until you"
echo "close it from the UI or press Ctrl+D — otherwise the -e command exits"
echo "and Ghosteel auto-closes the session."
echo ""
echo "Re-run inside any Ghosteel session:  $REMOTE_CATALOG"
echo "Clean up:                            $0 --clean"
