#!/bin/bash
set -euo pipefail

# Sailfish OS Emulator Language Driver
#
# The emulator image ships without sailfish-locale, so the Settings >
# Language & Region list is empty and no locales besides C/POSIX are
# generated: apps stay English regardless of LANG, and language QA is
# impossible. This script installs the missing package and switches the
# system language the same way the Settings page does (write
# /etc/locale.conf, then reboot), or launches ghosteel with the locale
# in its environment for a fast check without a reboot.
#
# Usage:
#   ./emulator_language.sh install          # install sailfish-locale (idempotent)
#   ./emulator_language.sh set <lang>       # switch system language + reboot
#                                           #   (e.g. set de, set nl_NL, set en_GB)
#   ./emulator_language.sh launch [lang]    # start ghosteel under a locale, no reboot
#   ./emulator_language.sh status           # show current language + generated locales
#
# Notes:
#   - <lang> maps to a locale code the way sailfish-locale names them:
#     "de" -> de_DE.utf8, "nl" -> nl_NL.utf8, "nl_BE" -> nl_BE.utf8.
#     Run "status" to list what is generated on the guest.
#   - "launch" sets LANG/LC_ALL for the app process only; system UI stays
#     in the current language. The app's .qm files load the same way in
#     both paths, so this is enough to verify translations.
#   - A reboot cycle takes a few minutes; wait for the emulator to come
#     back before continuing QA.

SSH_KEY="$HOME/SailfishOS/vmshare/ssh/private_keys/sdk"
SSH_PORT=2223
SSH_USER="defaultuser"
SSH_HOST="localhost"
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -p "$SSH_PORT" -i "$SSH_KEY")

LOCALE_CONF="/etc/locale.conf"

sshroot()  { ssh "${SSH_OPTS[@]}" root@"$SSH_HOST" "$@"; }
sshuser()  { ssh "${SSH_OPTS[@]}" "$SSH_USER@$SSH_HOST" "$@"; }

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

# Map a short language code to the generated locale name. sailfish-locale
# generates xx_XX.utf8 locales (de_DE.utf8, nl_NL.utf8, ...). A full code
# with a region is passed through.
locale_code() {
    local lang="$1"
    if [[ "$lang" == *_* ]]; then
        echo "${lang}.utf8"
    else
        local region
        region="$(echo "$lang" | tr '[:lower:]' '[:upper:]')"
        echo "${lang}_${region}.utf8"
    fi
}

case "${1:-}" in
    install)
        if sshroot "locale -a 2>/dev/null | grep -q '^de_DE\.utf8$'"; then
            echo "sailfish-locale already installed."
        else
            echo "Installing sailfish-locale (fills the Settings language list"
            echo "and generates the locale archive)..."
            sshroot "pkcon install -y sailfish-locale" >/dev/null
            if sshroot "locale -a 2>/dev/null | grep -q '^de_DE\.utf8$'"; then
                echo "Done. Available locales now include the sailfish-locale set."
            else
                echo "ERROR: package installed but de_DE.utf8 not generated." >&2
                exit 1
            fi
        fi
        ;;

    set)
        lang="${2:-}"
        if [[ -z "$lang" ]]; then
            echo "ERROR: usage: $0 set <lang>  (e.g. set de, set nl, set en_GB)" >&2
            exit 1
        fi
        loc="$(locale_code "$lang")"
        if ! sshroot "locale -a 2>/dev/null | grep -q '^${loc//./\\.}$'"; then
            echo "ERROR: locale $loc is not generated on the guest." >&2
            echo "Run '$0 status' to list available locales." >&2
            exit 1
        fi
        echo "Setting system language to $loc and rebooting (this is what the"
        echo "Settings page does: /etc/locale.conf + reboot)..."
        sshroot "printf 'LANG=%s\n' '$loc' > $LOCALE_CONF && sync"
        sshroot "systemctl reboot" >/dev/null 2>&1 || sshroot "reboot" >/dev/null 2>&1 || true
        echo "Rebooting. Wait for the emulator to come back before continuing."
        ;;

    launch)
        lang="${2:-}"
        if [[ -n "$lang" ]]; then
            loc="$(locale_code "$lang")"
        else
            loc="$(sshroot "cat $LOCALE_CONF 2>/dev/null" | sed -n 's/^LANG=//p')"
            loc="${loc:-en_US.utf8}"
        fi
        echo "Launching ghosteel under $loc (system language unchanged)..."
        sshroot "pkill -f '^/usr/bin/ghosteel' 2>/dev/null || true; sleep 1; \
su - $SSH_USER -c \"env XDG_RUNTIME_DIR=/run/user/100000 \
WAYLAND_DISPLAY=/run/display/wayland-0 LANG=$loc LC_ALL=$loc \
nohup /usr/bin/ghosteel >/tmp/ghosteel-lang.log 2>&1 &\"" >/dev/null 2>&1
        sleep 3
        if sshroot "pgrep -f '^/usr/bin/ghosteel' >/dev/null"; then
            echo "ghosteel running with LANG=$loc."
        else
            echo "ERROR: ghosteel failed to start; check /tmp/ghosteel-lang.log on the guest." >&2
            exit 1
        fi
        ;;

    status)
        echo "locale.conf: $(sshroot "cat $LOCALE_CONF 2>/dev/null" || echo '(missing)')"
        echo "sailfish-locale: $(sshroot "rpm -q sailfish-locale >/dev/null 2>&1 && echo installed || echo NOT installed")"
        echo "generated locales:"
        sshroot "locale -a 2>/dev/null | grep -vE '^(C|POSIX)$' | sed 's/^/  /'"
        ;;

    *)
        echo "Usage: $0 {install|set <lang>|launch [lang]|status}" >&2
        echo "  install         install sailfish-locale (fills the language list)"
        echo "  set <lang>      switch system language + reboot (e.g. set de)"
        echo "  launch [lang]   start ghosteel under a locale, no reboot"
        echo "  status          show current language + generated locales"
        exit 1
        ;;
esac
