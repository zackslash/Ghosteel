Name:       ghosteel

Summary:    Ghosteel terminal emulator for Sailfish OS
Version:    0.0.0
Release:    1
%define debug_package %{nil}
%define zig_version 0.15.2
License:    MIT
URL:        https://github.com/zackslash/Ghosteel
Source0:    %{name}-%{version}.tar.bz2
Source1:    zig-x86_64-linux-%{zig_version}.tar.xz
Source2:    zig-deps-cache.tar.gz
Requires:   sailfishsilica-qt5 >= 0.10.9
Requires:   libGLESv2
Requires:   libEGL
Requires:   nemo-qml-plugin-notifications-qt5
Requires:   sailfishsecretsdaemon
Requires:   sailfishsecretsdaemon-cryptoplugins-default
Requires:   sailfishsecretsdaemon-secretsplugins-default
BuildRequires:  pkgconfig(sailfishapp) >= 1.0.2
BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5DBus)
BuildRequires:  pkgconfig(Qt5Qml)
BuildRequires:  pkgconfig(Qt5Quick)
BuildRequires:  pkgconfig(Qt5OpenGL)
BuildRequires:  pkgconfig(glesv2)
BuildRequires:  pkgconfig(egl)
BuildRequires:  pkgconfig(freetype2)
BuildRequires:  pkgconfig(harfbuzz)
BuildRequires:  pkgconfig(sailfishsecrets)
BuildRequires:  pkgconfig(sailfishcrypto)
BuildRequires:  desktop-file-utils
BuildRequires:  xz
BuildRequires:  patch

ExclusiveArch:  %arm aarch64 %ix86

%description
Ghosteel brings a modern terminal engine to SailfishOS. Most mobile terminals
use legacy VT parsers that struggle with complex TUI apps. Ghosteel uses the
same engine that powers Ghostty, giving you accurate rendering for tmux,
neovim, htop, and other TUI applications.

- Ghostty VT engine with full escape sequence support, 24-bit truecolor
- GPU rendering via OpenGL ES 2.0/3.0 with cursor trail shaders
- Multi-session support with named sessions, switching, and persistence
- Launch TUI apps from desktop shortcuts (ghosteel -e htop)
- Touch text selection with Sailfish-style magnifier and double/triple tap
- Pinch-to-zoom for font size adjustment, per-session
- Configurable extra keys bar with sticky Ctrl/Alt, arrow keys, F1-F12
- Automatic URL detection and hyperlinks, tap to open in browser
- Inline images via Kitty Graphics Protocol with PNG decoding
- Encrypted scrollback, backed by Sailfish Secrets
- Dark and Light color schemes with adjustable opacity

%if 0%{?_chum}
Title: Ghosteel Terminal
Type: desktop-application
Categories:
 - Terminal
 - System
PackageIcon: https://raw.githubusercontent.com/zackslash/Ghosteel/main/icons/full.png
Screenshots:
 - https://raw.githubusercontent.com/zackslash/Ghosteel/main/screenshots/screenshot-neofetch-device.png
 - https://raw.githubusercontent.com/zackslash/Ghosteel/main/screenshots/screenshot-btop1-device.png
 - https://raw.githubusercontent.com/zackslash/Ghosteel/main/screenshots/screenshot-sessions-device.png
 - https://raw.githubusercontent.com/zackslash/Ghosteel/main/screenshots/screenshot-lazygit-device-fullscreen.png
 - https://raw.githubusercontent.com/zackslash/Ghosteel/main/screenshots/screenshot-search-device.png
Custom:
  Repo: https://github.com/zackslash/Ghosteel
  DescriptionMD: https://raw.githubusercontent.com/zackslash/Ghosteel/main/description.chum.md
%endif

%prep
%setup -q -n %{name}-%{version}

# Apply patches to ghostty submodule
# OBS: ghostty/ populated by tar_scm with submodules=enable
# SDK: ghostty/ populated by "method: tar" + git submodule update --init
if [ -d ghostty/src ]; then
    for p in patches/*.patch; do
        [ -f "$p" ] && patch --forward -d ghostty -p1 < "$p"
    done
fi

# Extract Zig compiler (OBS only — Source1 is fetched by OBS before build)
if [ -f "%{_sourcedir}/zig-x86_64-linux-%{zig_version}.tar.xz" ]; then
    tar -xJf "%{_sourcedir}/zig-x86_64-linux-%{zig_version}.tar.xz" -C %{_builddir}
fi

# Set up Zig package cache with all dependencies (OBS only)
# Source2 is a pre-built cache containing all Zig deps for offline builds
if [ -f "%{_sourcedir}/zig-deps-cache.tar.gz" ]; then
    ZIG_CACHE="%{_builddir}/zig-cache"
    mkdir -p "${ZIG_CACHE}"
    tar -xzf "%{_sourcedir}/zig-deps-cache.tar.gz" -C "${ZIG_CACHE}"
fi

%build

# Detect target architecture for Zig cross-compilation
%ifarch aarch64
    ZIG_TARGET="aarch64-linux-gnu.2.28"
    LIB_ARCH="aarch64"
%endif
%ifarch %arm
    ZIG_TARGET="arm-linux-gnueabihf.2.28"
    LIB_ARCH="armv7hl"
%endif
%ifarch %ix86
    ZIG_TARGET="x86-linux-gnu.2.28"
    LIB_ARCH="i486"
%endif

# Fail early if architecture is unsupported
if [ -z "${ZIG_TARGET}" ]; then
    echo "ERROR: Unsupported architecture %{_arch}" >&2
    exit 1
fi

# ── IDE flow bootstrap ──────────────────────────────────────────────
# The Sailfish SDK IDE's "Build" button runs sfdk qmake, which executes
# only the build section (skipping the prep step entirely).  With method: tar
# the source tarball is created but never extracted, so the build directory
# is empty.
#
# .sfdk/src is a symlink that sfdk creates pointing to the original
# source directory on the host machine.  We symlink everything we need
# from there so that qmake and the rest of the build can proceed.
#
# When sfdk build (full pipeline) runs, the prep step extracts the tarball
# and the library already exists → this block is skipped.
#
# Check for the actual library file (not just the directory) because a
# previous IDE build may have left lib/ as a stale symlink while the
# library itself went missing.
# ─────────────────────────────────────────────────────────────────────
if [ ! -f lib/${LIB_ARCH}/libghostty-vt.a ] && [ -L .sfdk/src ]; then
    SRC="$(readlink -f .sfdk/src)"
    if [ -d "$SRC" ]; then
        echo "IDE build: linking source tree from $SRC"
        for item in ghosteel.pro src qml shaders translations \
                    ghostty lib patches dbus-1 icons \
                    ghosteel.desktop LICENSE; do
            if [ -e "$SRC/$item" ]; then
                rm -rf "./$item" 2>/dev/null
                ln -s "$SRC/$item" .
            fi
        done
        # Apply patches to ghostty (normally done in the prep step)
        if [ -d ghostty/src ]; then
            for p in patches/*.patch; do
                [ -f "$p" ] || continue
                if patch --forward --dry-run -d ghostty -p1 < "$p" >/dev/null 2>&1; then
                    patch --forward -d ghostty -p1 < "$p"
                fi
            done
        fi
    fi
fi

# Build libghostty-vt from source only if no prebuilt lib exists
if [ ! -f lib/${LIB_ARCH}/libghostty-vt.a ]; then

    # Find Zig compiler
    #   OBS: extracted from Source1 in the prep step
    #   SDK: must be installed on host (zig %{zig_version})
    ZIG=""
    if [ -f "%{_builddir}/zig-x86_64-linux-%{zig_version}/zig" ]; then
        ZIG="%{_builddir}/zig-x86_64-linux-%{zig_version}/zig"
    elif command -v zig >/dev/null 2>&1; then
        ZIG="zig"
    fi

    if [ -z "$ZIG" ]; then
        echo "ERROR: lib/${LIB_ARCH}/libghostty-vt.a not found and no Zig compiler available." >&2
        echo "" >&2
        echo "Install Zig %{zig_version} from https://ziglang.org/" >&2
        echo "Then either:" >&2
        echo "  - Run ./scripts/build-libs.sh (one-time, builds all arches)" >&2
        echo "  - Or ensure ghostty submodule is initialized (builds per sfdk target)" >&2
        exit 1
    fi

    if [ ! -d ghostty/src ]; then
        echo "ERROR: ghostty/ submodule not initialized." >&2
        echo "" >&2
        echo "Run: git submodule update --init" >&2
        exit 1
    fi

    # Use --system to disable network and point Zig at pre-fetched deps
    # Only set when the full deps cache exists (OBS builds)
    SYSTEM_FLAG=""
    if [ -d "%{_builddir}/zig-cache/p" ]; then
        SYSTEM_FLAG="--system %{_builddir}/zig-cache/p"
    fi

    cd ghostty
    "$ZIG" build -Demit-lib-vt \
        -Dtarget="${ZIG_TARGET}" \
        -Doptimize=ReleaseSafe \
        ${SYSTEM_FLAG} 2>&1 || exit 1
    mkdir -p %{_builddir}/%{name}-%{version}/lib/${LIB_ARCH}
    cp zig-out/lib/libghostty-vt.a %{_builddir}/%{name}-%{version}/lib/${LIB_ARCH}/
    cd %{_builddir}/%{name}-%{version}
fi

# Inject version info (no .git/ in OBS tarballs, so git describe fails)
# GIT_VERSION: use RPM version set by tar_git from git tag
sed -i "s/isEmpty(GIT_VERSION): GIT_VERSION = \"[^\"]*\"/isEmpty(GIT_VERSION): GIT_VERSION = \"%{version}\"/" ghosteel.pro
# GHOSTTY_VERSION: read from rpm/ghostty-version (updated by scripts/update-ghostty-version.sh)
GHOSTTY_SHA=$(cat rpm/ghostty-version 2>/dev/null || true)
if [ -n "$GHOSTTY_SHA" ]; then
    sed -i "s|^GHOSTTY_VERSION = .*|GHOSTTY_VERSION = \"$GHOSTTY_SHA\"|" ghosteel.pro
fi

# Build Qt application
%qmake5

%make_build


%install
%qmake5_install


desktop-file-install --delete-original \
    --dir %{buildroot}%{_datadir}/applications \
    %{buildroot}%{_datadir}/applications/*.desktop

%files
%license LICENSE
%{_bindir}/%{name}
%{_datadir}/%{name}
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png
%{_datadir}/dbus-1/services/com.zackslash.ghosteel.service
