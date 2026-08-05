#!/bin/bash
set -euo pipefail

# Sailfish OS Emulator OSC 52 Clipboard Test
#
# Verifies the OSC 52 clipboard round-trip through ghosteel: a program writes
# a payload to the clipboard via OSC 52, reads it back via OSC 52, and checks
# they match. Exercises the path where OSC 52 routes through
# QGuiApplication::clipboard() (the same clipboard copy/paste uses).
#
# The read half only completes silently when clipboard read access is "Allow",
# so this script sets that policy temporarily and restores the previous value.
#
# Usage: ./emulator_clipboard_test.sh           # run the test
#        ./emulator_clipboard_test.sh --clean   # remove test files

SSH_KEY="$HOME/SailfishOS/vmshare/ssh/private_keys/sdk"
SSH_PORT=2223
SSH_USER="defaultuser"
SSH_HOST="localhost"
SSH_TARGET="$SSH_USER@$SSH_HOST"
SSH_OPTS=(-o StrictHostKeyChecking=no -o ConnectTimeout=5 -p "$SSH_PORT" -i "$SSH_KEY")

CONF_PATH=".config/com.zackslash/ghosteel.conf"
RESULT_PATH="/tmp/osc52_result.txt"
SELFTEST_PATH="/tmp/osc52_selftest.sh"
GHOSTEEL_LOG="/tmp/ghosteel-osc52.log"

# --- Pre-flight checks ---

if [[ ! -f "$SSH_KEY" ]]; then
    echo "ERROR: SSH key not found: $SSH_KEY" >&2
    exit 1
fi

echo "[0/5] Checking emulator connectivity..."
if ! ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "true" 2>/dev/null; then
    echo "ERROR: Cannot reach emulator at $SSH_HOST:$SSH_PORT" >&2
    echo "Is the Sailfish OS emulator running?" >&2
    exit 1
fi

# --- Settings helpers (clipboardReadPolicy lives under the [terminal] INI group) ---

get_policy() {
    ssh "${SSH_OPTS[@]}" "$SSH_TARGET" \
        "grep -m1 '^clipboardReadPolicy=' ~/$CONF_PATH 2>/dev/null | cut -d= -f2 || true"
}

# Set clipboardReadPolicy=<value>. The key name is unique, so a global replace
# is safe. Handles key-present (common) and key/group-absent fallbacks.
set_policy() {
    local want="$1"
    ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "
        conf=~/$CONF_PATH
        mkdir -p \"\$(dirname \"\$conf\")\"
        if grep -q '^clipboardReadPolicy=' \"\$conf\"; then
            sed -i 's/^clipboardReadPolicy=.*/clipboardReadPolicy=$want/' \"\$conf\"
        elif grep -q '^\\[terminal\\]' \"\$conf\"; then
            sed -i '/^\\[terminal\\]/a clipboardReadPolicy=$want' \"\$conf\"
        else
            printf '[terminal]\\nclipboardReadPolicy=$want\\n' >> \"\$conf\"
        fi
    "
}

restore_policy() {
    local prev="$1"
    if [[ -z "$prev" ]]; then
        # Key was absent originally; remove the line we added.
        ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "sed -i '/^clipboardReadPolicy=/d' ~/$CONF_PATH"
    else
        set_policy "$prev"
    fi
}

# --- Clean mode ---

if [[ "${1:-}" == "--clean" ]]; then
    echo "[clean] Removing test files..."
    ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "rm -f $SELFTEST_PATH $RESULT_PATH $GHOSTEEL_LOG"
    echo "Done. (clipboardReadPolicy left as-is; adjust in Settings if needed.)"
    exit 0
fi

# --- 1. Install the in-session self-test ---

echo "[1/5] Uploading OSC 52 self-test..."
ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "cat > $SELFTEST_PATH" <<'EOF'
#!/bin/bash
# OSC 52 round-trip self-test. Runs INSIDE a ghosteel session.
# Writes a payload to the clipboard via OSC 52, reads it back, compares.
RESULT=/tmp/osc52_result.txt
PAYLOAD="GHOSTEEL_OSC52_OK"
rm -f "$RESULT"
sleep 2   # let the session and QML signal chain initialize

# Write the payload to the system clipboard via OSC 52.
printf '\033]52;c;%s\007' "$(printf '%s' "$PAYLOAD" | base64)"
sleep 0.5  # let ghosteel set the clipboard

# Read the clipboard back via OSC 52; ghosteel replies on stdin.
printf '\033]52;c?\007'
raw=""
IFS= read -r -t 3 -d "$(printf '\007')" raw 2>/dev/null || true
payload="${raw##*;}"          # strip the "ESC]52;c;" prefix, leave base64
decoded="$(printf '%s' "$payload" | base64 -d 2>/dev/null || true)"

if [[ "$decoded" == "$PAYLOAD" ]]; then
    echo "PASS: OSC 52 round-trip OK ('$decoded')" > "$RESULT"
else
    echo "FAIL: expected '$PAYLOAD', got '$decoded' (raw='$raw')" > "$RESULT"
fi
EOF
ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "chmod +x $SELFTEST_PATH"

# --- 2. Set clipboard read policy to Allow for an automated run ---

PREV_POLICY="$(get_policy)"
echo "[2/5] Setting clipboard read policy to Allow (was: ${PREV_POLICY:-absent})..."
set_policy 1

# --- 3. Cold-start ghosteel running the self-test (so Allow is in effect) ---

echo "[3/5] Launching ghosteel with the self-test..."
ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "rm -f $RESULT_PATH"
ssh "${SSH_OPTS[@]}" "$SSH_TARGET" \
    "pkill -x ghosteel 2>/dev/null || true; sleep 1; ghosteel -e $SELFTEST_PATH > $GHOSTEEL_LOG 2>&1 &"

# --- 4. Poll for the result ---

echo "[4/5] Waiting for round-trip result..."
for _ in $(seq 1 20); do
    if ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "test -f $RESULT_PATH" 2>/dev/null; then
        break
    fi
    sleep 1
done

# --- 5. Report ---

echo "[5/5] Result:"
if ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "test -f $RESULT_PATH" 2>/dev/null; then
    ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "cat $RESULT_PATH"
    rc=0
else
    echo "FAIL: no result within timeout (see $GHOSTEEL_LOG on the device)"
    rc=1
fi

# --- Restore the original policy ---

restore_policy "$PREV_POLICY"
echo "(clipboard read policy restored to: ${PREV_POLICY:-absent})"

echo ""
echo "Manual scenarios for the warning flow (set in Settings > Clipboard read access):"
echo "  Allow (default for this test): round-trip passes silently"
echo "  Ask:  a program reading the clipboard shows a confirmation dialog"
echo "  Deny: the read returns nothing; round-trip would FAIL, as expected"
echo ""
echo "Clean up with: $0 --clean"
exit "${rc:-0}"
