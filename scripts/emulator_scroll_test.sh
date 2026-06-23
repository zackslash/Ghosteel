#!/bin/bash
set -euo pipefail

# Sailfish OS Emulator Two-Finger Scroll Test
# Usage: ./emulator_scroll_test.sh [start_x] [start_y] [end_x] [end_y] [steps] [delay_ms]
# Default: drag from center (270,480) to bottom (270,900) over 60 steps

SSH_KEY="$HOME/SailfishOS/vmshare/ssh/private_keys/sdk"
SSH_PORT=2223
SSH_USER="defaultuser"
SSH_HOST="localhost"
SSH_TARGET="$SSH_USER@$SSH_HOST"

SSH_OPTS=(-o StrictHostKeyChecking=no -o ConnectTimeout=5 -p "$SSH_PORT" -i "$SSH_KEY")
SCP_OPTS=(-o StrictHostKeyChecking=no -o ConnectTimeout=5 -P "$SSH_PORT" -i "$SSH_KEY")

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SOURCE_FILE="$SCRIPT_DIR/mt_drag.c"

START_X=${1:-270}
START_Y=${2:-480}
END_X=${3:-270}
END_Y=${4:-900}
STEPS=${5:-60}
DELAY=${6:-16}

# --- Pre-flight checks ---

if [[ ! -f "$SOURCE_FILE" ]]; then
    echo "ERROR: Source file not found: $SOURCE_FILE" >&2
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

# --- Main sequence ---

echo "[1/3] Uploading source..."
if ! scp "${SCP_OPTS[@]}" "$SOURCE_FILE" "$SSH_TARGET:/tmp/mt_drag.c"; then
    echo "ERROR: Upload failed" >&2
    exit 1
fi

echo "[2/3] Compiling on emulator..."
if ! ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "gcc -Wall -o /tmp/mt_drag /tmp/mt_drag.c" 2>&1; then
    echo "ERROR: Compilation failed (see above)" >&2
    exit 1
fi

echo "[3/3] Running: ($START_X,$START_Y) -> ($END_X,$END_Y) ${STEPS} steps @ ${DELAY}ms..."
ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "sudo /tmp/mt_drag $START_X $START_Y $END_X $END_Y $STEPS $DELAY"

echo "Done."
