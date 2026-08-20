#!/bin/bash
set -euo pipefail

# Sailfish OS Emulator Orientation Driver
#
# The emulator has no usable orientation sensors (sensorfwd runs but every
# orientation plugin is unavailable) and its mce exposes no orientation D-Bus
# methods, so rotation cannot be simulated the way a real device rotates. The
# Lipstick compositor DOES honor app-requested orientation, so this script
# drives rotation through the real Silica path: it appends a small polling
# timer to the DEPLOYED TerminalPage.qml that watches /tmp/qa_orient on the
# guest and locks allowedOrientations to the requested mode. Page.orientation
# then transitions exactly as in production, firing the app's orientation
# policy (onOrientationChanged, activation hooks).
#
# Usage:
#   ./emulator_orientation.sh install      # patch deployed TerminalPage.qml (backup kept on guest)
#   ./emulator_orientation.sh P|L|PI|LI|A  # switch orientation (P=portrait, L=landscape,
#                                          #  PI/LI inverted, A=Orientation.All/auto)
#   ./emulator_orientation.sh status       # show driver state + current mode
#   ./emulator_orientation.sh --clean      # restore original QML and restart ghosteel

SSH_KEY="$HOME/SailfishOS/vmshare/ssh/private_keys/sdk"
SSH_PORT=2223
SSH_USER="defaultuser"
SSH_HOST="localhost"
SSH_TARGET="$SSH_USER@$SSH_HOST"
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -p "$SSH_PORT" -i "$SSH_KEY")

QML_PATH="/usr/share/ghosteel/qml/pages/TerminalPage.qml"
BACKUP_PATH="/tmp/TerminalPage.qml.qa-orig"
MODE_FILE="/tmp/qa_orient"
MARKER="QA-DRIVER emulator_orientation.sh"

sshroot() { ssh "${SSH_OPTS[@]}" root@"$SSH_HOST" "$@"; }
sshuser() { ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "$@"; }

WORK=""; PATCHED=""; DRIVER_BLOCK=""
trap 'rm -f "$WORK" "$PATCHED" "$DRIVER_BLOCK"' EXIT

restart_app() {
    sshuser "pkill -x ghosteel 2>/dev/null || true; sleep 1; nohup ghosteel > /tmp/ghosteel-orient.log 2>&1 &"
}

# --- Pre-flight checks ---

if [[ ! -f "$SSH_KEY" ]]; then
    echo "ERROR: SSH key not found: $SSH_KEY" >&2
    exit 1
fi

echo "[0/3] Checking emulator connectivity..."
if ! sshuser "true" 2>/dev/null; then
    echo "ERROR: Cannot reach emulator at $SSH_HOST:$SSH_PORT" >&2
    echo "Is the Sailfish OS emulator running?" >&2
    exit 1
fi

# --- Modes ---

if [[ "${1:-}" == "--clean" ]]; then
    echo "[1/2] Restoring original TerminalPage.qml..."
    restored=no
    if sshroot "test -f $BACKUP_PATH" 2>/dev/null; then
        sshroot "cp $BACKUP_PATH $QML_PATH && rm -f $BACKUP_PATH $MODE_FILE"
        restored=yes
    else
        sshroot "rm -f $MODE_FILE"
        echo "  No backup found; the deployed QML was NOT restored." >&2
        echo "  Driver may still be installed; reinstall the app package to reset it." >&2
    fi

    echo "[2/2] Restarting ghosteel..."
    restart_app
    if [[ "$restored" == yes ]]; then
        echo "Done. Deployed QML restored, driver state cleared."
        exit 0
    fi
    exit 1
fi

if [[ "${1:-}" == "status" ]]; then
    if sshroot "grep -q '$MARKER' $QML_PATH" 2>/dev/null; then
        echo "driver: installed"
    else
        echo "driver: not installed (run: $0 install)"
    fi
    echo "mode:   $(sshuser "cat $MODE_FILE 2>/dev/null" || echo "(none)")"
    sshuser "pgrep -x ghosteel >/dev/null" 2>/dev/null && echo "app:    running" || echo "app:    not running"
    exit 0
fi

if [[ "${1:-}" == "install" ]]; then
    echo "[1/3] Fetching deployed TerminalPage.qml..."
    WORK=$(mktemp /tmp/ghosteel-orient-work.XXXXXX.qml)
    sshroot "cat $QML_PATH" > "$WORK"

    if grep -q "$MARKER" "$WORK"; then
        echo "  Driver already installed; nothing to do."
        exit 0
    fi

    # --- Driver block appended inside the Page object (before its closing brace).
    # Pure QML has no file I/O; a synchronous XMLHttpRequest on a file:// URL is
    # the standard workaround and reads the mode file with no caching.
    DRIVER_BLOCK=$(mktemp /tmp/ghosteel-orient-block.XXXXXX)
    cat > "$DRIVER_BLOCK" <<'EOF'

    // QA-DRIVER emulator_orientation.sh (remove with --clean)
    property string _qaOrientMode: ""
    Timer {
        interval: 250
        running: true
        repeat: true
        onTriggered: {
            var xhr = new XMLHttpRequest()
            xhr.open("GET", "file:///tmp/qa_orient", false)
            try { xhr.send() } catch (e) { return }
            if (xhr.readyState !== 4) return
            var mode = String(xhr.responseText || "").trim()
            if (mode === "" || mode === page._qaOrientMode) return
            var table = { "P": Orientation.Portrait, "L": Orientation.Landscape,
                          "PI": Orientation.PortraitInverted, "LI": Orientation.LandscapeInverted,
                          "A": Orientation.All }
            if (!(mode in table)) return
            page._qaOrientMode = mode
            page.allowedOrientations = table[mode]
        }
    }
EOF

    PATCHED=$(mktemp /tmp/ghosteel-orient-patched.XXXXXX.qml)
    # Trim trailing blank lines, verify the last live line closes the Page,
    # then splice the driver in before that brace.
    awk -v block_file="$DRIVER_BLOCK" '
        { lines[NR] = $0 }
        END {
            last = NR
            while (last > 0 && lines[last] ~ /^[[:space:]]*$/) last--
            if (lines[last] != "}") {
                printf "unexpected file tail (line %d): %s\n", last, lines[last] > "/dev/stderr"
                exit 1
            }
            for (i = 1; i < last; i++) print lines[i]
            while ((getline line < block_file) > 0) print line
            print "}"
        }
    ' "$WORK" > "$PATCHED"

    echo "[2/3] Backing up original and installing driver..."
    sshroot "cp $QML_PATH $BACKUP_PATH"
    sshroot "cat > $QML_PATH" < "$PATCHED"
    sshuser "rm -f $MODE_FILE"

    echo "[3/3] Restarting ghosteel..."
    restart_app
    sleep 2
    echo "Done. Switch with: $0 P|L|PI|LI|A  (restore with: $0 --clean)"
    exit 0
fi

case "${1:-}" in
    P|L|PI|LI|A)
        ;;
    *)
        echo "Usage: $0 install|P|L|PI|LI|A|status|--clean" >&2
        exit 1
        ;;
esac

MODE="$1"
if ! sshroot "grep -q '$MARKER' $QML_PATH" 2>/dev/null; then
    echo "ERROR: driver not installed (run: $0 install)" >&2
    exit 1
fi

sshuser "printf '%s' '$MODE' > $MODE_FILE"
echo "orientation -> $MODE (picked up within 250ms; no restart needed)"
