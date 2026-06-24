#!/bin/bash
set -euo pipefail

# Sailfish OS Emulator Desktop Shortcut Test
# Installs desktop launcher files for -e/--exec and -s/--session CLI flags,
# restarts lipstick so the launcher picks them up, and ensures ghosteel is running.
#
# Usage: ./emulator_desktop_test.sh [--clean]

SSH_KEY="$HOME/SailfishOS/vmshare/ssh/private_keys/sdk"
SSH_PORT=2223
SSH_USER="defaultuser"
SSH_HOST="localhost"
SSH_TARGET="$SSH_USER@$SSH_HOST"

SSH_OPTS=(-o StrictHostKeyChecking=no -o ConnectTimeout=5 -p "$SSH_PORT" -i "$SSH_KEY")

DESKTOP_DIR=".local/share/applications"
SOCKET_PATH="/run/user/100000/ghosteel-singleton"

# --- Pre-flight checks ---

if [[ ! -f "$SSH_KEY" ]]; then
    echo "ERROR: SSH key not found: $SSH_KEY" >&2
    exit 1
fi

echo "[0/4] Checking emulator connectivity..."
if ! ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "true" 2>/dev/null; then
    echo "ERROR: Cannot reach emulator at $SSH_HOST:$SSH_PORT" >&2
    echo "Is the Sailfish OS emulator running?" >&2
    exit 1
fi

# --- Clean mode: remove test desktop files and exit ---

if [[ "${1:-}" == "--clean" ]]; then
    echo "[1/2] Removing test desktop files..."
    ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "rm -f ~/$DESKTOP_DIR/ghosteel-top.desktop ~/$DESKTOP_DIR/ghosteel-top-cpu.desktop ~/$DESKTOP_DIR/ghosteel-top-slow.desktop ~/$DESKTOP_DIR/ghosteel-lazygit.desktop ~/$DESKTOP_DIR/ghosteel-htop.desktop ~/$DESKTOP_DIR/ghosteel-sysmon.desktop ~/$DESKTOP_DIR/ghosteel-fail.desktop /tmp/fail-after-3.sh"

    echo "[2/2] Restarting lipstick..."
    ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "systemctl --user restart lipstick.service"

    echo "Done. Test desktop files removed."
    exit 0
fi

# --- Install desktop files ---

# Helper to create a desktop file on the emulator
create_desktop() {
    local file="$1" exec="$2" name="$3" comment="$4"
    ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "cat > ~/$DESKTOP_DIR/$file" <<EOF
[Desktop Entry]
Type=Application
X-Nemo-Application-Type=silica-qt5
X-Nemo-Single-Instance=no
Icon=ghosteel
Exec=$exec
Name=$name
Comment=$comment

[X-Sailjail]
OrganizationName=com.zackslash
ApplicationName=ghosteel
Permissions=UserDirs;Secrets;
Sandboxing=Disabled
EOF
}

echo "[1/4] Creating desktop files on emulator..."
ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "mkdir -p ~/$DESKTOP_DIR"

# Create a script that sleeps then exits with error (for testing auto-remove + session list)
ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "printf '#!/bin/sh\nsleep 3\nexit 1\n' > /tmp/fail-after-3.sh && chmod +x /tmp/fail-after-3.sh"

create_desktop "ghosteel-top.desktop"      "ghosteel -e top"             "Top (Ghosteel)"      "Run top in Ghosteel terminal"
create_desktop "ghosteel-top-cpu.desktop"  "ghosteel -e top -b -n 5"     "Top Batch (Ghosteel)" "Top in batch mode, 5 iterations"
create_desktop "ghosteel-top-slow.desktop" "ghosteel -e top -d 5"        "Top Slow (Ghosteel)" "Top with 5-second refresh"
create_desktop "ghosteel-lazygit.desktop"  "ghosteel -e lazygit"         "Lazygit (Ghosteel)"  "Run lazygit in Ghosteel terminal"
create_desktop "ghosteel-htop.desktop"     "ghosteel -s htop -e htop"    "Htop (Ghosteel)"     "Named htop session in Ghosteel"
create_desktop "ghosteel-sysmon.desktop"   "ghosteel -s sysmon -e top"   "Sysmon (Ghosteel)"   "Named sysmon session in Ghosteel"
create_desktop "ghosteel-fail.desktop"     "ghosteel -e /tmp/fail-after-3.sh" "Fail Test (Ghosteel)" "Runs 3s then exits with error"

echo "[2/4] Verifying desktop files..."
ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "ls -la ~/$DESKTOP_DIR/ghosteel-*.desktop"

echo "[3/4] Ensuring ghosteel is running..."
if ! ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "test -S $SOCKET_PATH" 2>/dev/null; then
    echo "  Ghosteel not running — starting..."
    ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "ghosteel > /tmp/ghosteel.log 2>&1 &"
    sleep 2
else
    echo "  Ghosteel already running."
fi

echo "[4/4] Restarting lipstick to pick up new desktop files..."
ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "systemctl --user restart lipstick.service"
sleep 1

echo ""
echo "Done. Seven desktop shortcuts installed:"
echo "  Top (Ghosteel)        — anonymous exec: ghosteel -e top"
echo "  Top Batch (Ghosteel)  — anonymous exec: ghosteel -e top -b -n 5"
echo "  Top Slow (Ghosteel)   — anonymous exec: ghosteel -e top -d 5"
echo "  Lazygit (Ghosteel)    — anonymous exec: ghosteel -e lazygit"
echo "  Htop (Ghosteel)       — named session:  ghosteel -s htop -e htop"
echo "  Sysmon (Ghosteel)     — named session:  ghosteel -s sysmon -e top"
echo "  Fail Test (Ghosteel)  — anonymous exec: /tmp/fail-after-3.sh (3s then exit 1)"
echo ""
echo "Test scenarios:"
echo "  1. Cold start: kill ghosteel, tap any icon — should launch and run command"
echo "  2. Warm start: with ghosteel running, tap 'Top' — should create new session"
echo "  3. Reuse: tap 'Top' again — should switch to existing top session"
echo "  4. Named reuse: tap 'Sysmon' — creates named session, tap again — reuses it"
echo "  5. Same binary, different args: tap 'Top' then 'Top Batch' — two SEPARATE sessions"
echo "  6. Same binary, different args: tap 'Top Batch' then 'Top Slow' — two SEPARATE sessions"
echo "  7. Args reuse: tap 'Top Batch' twice — second tap reuses existing Top Batch session"
echo "  8. Independence: tap 'Top' then 'Sysmon' — two separate sessions with same command"
echo "  9. Auto-remove + session list: tap 'Fail Test' — runs 3s, shows error, then session list"
echo ""
echo "Clean up with: $0 --clean"
