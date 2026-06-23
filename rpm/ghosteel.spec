Name:       ghosteel

Summary:    Ghosteel terminal emulator for Sailfish OS
Version:    0.0.0
Release:    1
%define debug_package %{nil}
License:    MIT
URL:        https://github.com/zackslash/Ghosteel
Source0:    %{name}-%{version}.tar.bz2
Source1:    https://ziglang.org/download/0.15.2/zig-x86_64-linux-0.15.2.tar.xz
Source2:    https://deps.files.ghostty.org/uucode-0.2.0-ZZjBPqZVVABQepOqZHR7vV_NcaN-wats0IB6o-Exj6m9.tar.gz
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
Ghosteel terminal emulator for Sailfish OS, powered by libghostty.


%prep
%setup -q -n %{name}-%{version}

# Apply patches to ghostty submodule
# OBS: ghostty/ populated by tar_scm with submodules=enable
# SDK: ghostty/ populated by "method: tar" + git submodule update --init
if [ -d ghostty/src ]; then
    for p in patches/*.patch; do
        [ -f "$p" ] && patch -d ghostty -p1 < "$p"
    done
fi

# Extract Zig compiler (OBS only — Source1 is fetched by OBS before build)
if [ -f "%{_sourcedir}/zig-x86_64-linux-0.15.2.tar.xz" ]; then
    tar -xJf "%{_sourcedir}/zig-x86_64-linux-0.15.2.tar.xz" -C %{_builddir}
fi

# Set up Zig package cache (OBS only — Source2 is fetched by OBS before build)
UUCODE_HASH="uucode-0.2.0-ZZjBPqZVVABQepOqZHR7vV_NcaN-wats0IB6o-Exj6m9"
if [ -f "%{_sourcedir}/${UUCODE_HASH}.tar.gz" ]; then
    ZIG_CACHE="%{_builddir}/zig-cache"
    mkdir -p "${ZIG_CACHE}/p/${UUCODE_HASH}"
    tar -xzf "%{_sourcedir}/${UUCODE_HASH}.tar.gz" \
        --strip-components=1 -C "${ZIG_CACHE}/p/${UUCODE_HASH}"
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
# only the build section (skipping the prep step entirely).  With method: tar the source
# tarball is created but never extracted, so the build directory is empty.
#
# .sfdk/src is a symlink that sfdk creates pointing to the original
# source directory on the host machine.  We symlink everything we need
# from there so that qmake and the rest of the build can proceed.
#
# When sfdk build (full pipeline) runs, the prep step extracts the tarball and
# ghosteel.pro already exists → this block is skipped.
# ─────────────────────────────────────────────────────────────────────
if [ ! -f ghosteel.pro ] && [ -L .sfdk/src ]; then
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
    #   SDK: must be installed on host (zig 0.15.2)
    ZIG=""
    if [ -f "%{_builddir}/zig-x86_64-linux-0.15.2/zig" ]; then
        ZIG="%{_builddir}/zig-x86_64-linux-0.15.2/zig"
    elif command -v zig >/dev/null 2>&1; then
        ZIG="zig"
    fi

    if [ -z "$ZIG" ]; then
        echo "ERROR: lib/${LIB_ARCH}/libghostty-vt.a not found and no Zig compiler available." >&2
        echo "" >&2
        echo "Install Zig 0.15.2 from https://ziglang.org/" >&2
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

    # Offline build if OBS pre-fetched uucode into cache
    UUCODE_HASH="uucode-0.2.0-ZZjBPqZVVABQepOqZHR7vV_NcaN-wats0IB6o-Exj6m9"
    SYSTEM_FLAG=""
    if [ -d "%{_builddir}/zig-cache/p/${UUCODE_HASH}" ]; then
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
