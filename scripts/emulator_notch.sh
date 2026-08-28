#!/bin/bash
set -euo pipefail

# Sailfish OS Emulator Notch Simulator
#
# Simulates a notched/cutout display (e.g. Jolla Phone 2026 punch-hole) by
# writing the Silica cutout dconf key the device adaptation would ship:
#   /desktop/sailfish/silica/cutouts = [[x, 0, w, h]]
# With the key set, Screen.hasCutouts goes true and Screen.topCutout exposes
# the rect, which is what ghosteel's automatic notch inset reads. Emptying
# the list simulates an adaptation that reports no cutout: the inset falls
# back to zero, same as Silica's own StatusArea behaves on such a device.
#
# The write must run on defaultuser's session bus (as root, dconf fails
# with "Cannot autolaunch D-Bus without X11 $DISPLAY").
#
# Usage:
#   ./emulator_notch.sh on [w] [h]   # centered punch-hole (default 60x60,
#                                    #   centered for the 720-wide default
#                                    #   profile; on wider profiles pass the
#                                    #   full x y w h form). Takes 0 or 2
#                                    # args here, not 1.
#   ./emulator_notch.sh on x y w h   # full geometry (y must be 0 for the
#                                    #   key to count as a top cutout)
#   ./emulator_notch.sh off          # empty the list (auto-detect dead)
#   ./emulator_notch.sh reset        # discard user value; vendor default
#                                    #   (the default profile ships one)
#   ./emulator_notch.sh status       # show the current cutout config
#
# Notes:
#   - Lipstick reacts to the key live (the clock drops below the cutout);
#     apps see Screen.cutoutsChanged, and ghosteel's Auto mode re-reads
#     topPadding without a restart.
#   - Screenshot pixels do not show a hole; verify via layout changes
#     (empty band at the top, "stty size" row count dropping).

SSH_KEY="$HOME/SailfishOS/vmshare/ssh/private_keys/sdk"
SSH_PORT=2223
SSH_USER="defaultuser"
SSH_HOST="localhost"
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -p "$SSH_PORT" -i "$SSH_KEY")

DCONF_KEY="/desktop/sailfish/silica/cutouts"
USER_BUS="DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/100000/dbus/user_bus_socket"

sshroot()  { ssh "${SSH_OPTS[@]}" root@"$SSH_HOST" "$@"; }

write_cutouts() {
    local value="$1"
    sshroot "su -l $SSH_USER -c '$USER_BUS dconf write $DCONF_KEY \"$value\"'"
}

read_cutouts() {
    sshroot "su -l $SSH_USER -c '$USER_BUS dconf read $DCONF_KEY'" 2>/dev/null || true
}

# --- Pre-flight checks ---

if [[ ! -f "$SSH_KEY" ]]; then
    echo "ERROR: SSH key not found: $SSH_KEY" >&2
    exit 1
fi

if ! sshroot "true" 2>/dev/null; then
    echo "ERROR: Cannot reach emulator at $SSH_HOST:$SSH_PORT" >&2
    echo "Is the Sailfish OS emulator running?" >&2
    exit 1
fi

case "${1:-}" in
    on)
        shift
        # Silica coordinate space, not fb0 (which reports half-resolution on
        # the default profile). 720-wide default profile: centered. JP2
        # profile (1080-wide): pass explicit x, e.g. "on 510 0 60 60".
        x=330; y=0; w=60; h=60
        case $# in
            0) ;;
            2) w="$1"; h="$2"; x=$(( (720 - w) / 2 )) ;;
            4) x="$1"; y="$2"; w="$3"; h="$4" ;;
            *)
                echo "ERROR: usage: $0 on [w h] | on x y w h" >&2
                exit 1
                ;;
        esac
        if [[ "$y" -ne 0 ]]; then
            echo "ERROR: y must be 0 — Silica only treats y==0 rects as the top cutout." >&2
            exit 1
        fi
        if [[ ! "$x$w$h" =~ ^[0-9]+$ ]]; then
            echo "ERROR: x, w, h must be non-negative integers." >&2
            exit 1
        fi
        write_cutouts "[[$x, $y, $w, $h]]"
        echo "Cutout set: x=$x y=$y w=$w h=$h (Screen.topCutout now reports it)."
        echo "ghosteel Auto mode should shift the grid down on its next layout pass."
        ;;

    off)
        # An explicit empty list, not dconf reset: reset falls back to the
        # vendor key (the default emulator profile ships its own cutout),
        # and dconf needs the @aai type tag to infer an empty array.
        sshroot "su -l $SSH_USER -c '$USER_BUS dconf write $DCONF_KEY \"@aai []\"'"
        echo "Cutout list emptied. Screen.hasCutouts is now false — Auto detects"
        echo "nothing (the JP2-today case); Manual inset still works."
        ;;

    reset)
        sshroot "su -l $SSH_USER -c '$USER_BUS dconf reset $DCONF_KEY'"
        echo "User value discarded — the vendor/adaptation default (if any) applies."
        ;;

    status)
        val="$(read_cutouts)"
        if [[ -n "$val" ]]; then
            echo "cutouts: $val"
        else
            echo "cutouts: (no user value — vendor default, if any, applies)"
        fi
        ;;

    *)
        echo "Usage: $0 {on [w h] | on x y w h | off | reset | status}" >&2
        echo "  on [w h]      simulate a centered punch-hole (default 60x60)"
        echo "  on x y w h    full geometry (y must be 0)"
        echo "  off           empty the cutout list (auto-detect dead, Manual mode path)"
        echo "  reset         discard user value; vendor/adaptation default applies"
        echo "  status        show the current cutout config"
        exit 1
        ;;
esac
