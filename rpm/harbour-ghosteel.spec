Name:       harbour-ghosteel

Summary:    Ghosteel terminal emulator for Sailfish OS (sandboxed store build)
Version:    0.0.0
Release:    1
%define debug_package %{nil}
%define zig_version 0.17.0
License:    MIT
URL:        https://github.com/zackslash/Ghosteel
Source0:    %{name}-%{version}.tar.bz2
Source1:    zig-x86_64-linux-%{zig_version}.tar.xz
Source2:    zig-deps-cache.tar.gz
Source3:    store-skeleton.profile
Source4:    busybox-GPLv2
Source5:    busybox-1.38.0.tar.bz2
Requires:   sailfishsilica-qt5 >= 0.10.9
Requires:   nemo-qml-plugin-notifications-qt5
Requires:   qt5-qtgraphicaleffects
Requires:   sailfishsecretsdaemon
Requires:   sailfishsecretsdaemon-cryptoplugins-default
Requires:   sailfishsecretsdaemon-secretsplugins-default
BuildRequires:  pkgconfig(sailfishapp) >= 1.0.2
# sailfishapp_i18n chains lupdate/lrelease with || :, so a missing tool
# would silently ship an RPM with no .qm files; require the provider.
BuildRequires:  qt5-qttools
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
BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  xz

ExclusiveArch:  %arm aarch64 %ix86

%description
Ghosteel brings a modern terminal engine to SailfishOS, powered by the same
engine as Ghostty with accurate rendering for TUI applications.

This is the sandboxed Jolla Store build. It runs entirely inside the
Sailfish OS sandbox with its own persistent home and a bundled busybox
toolset: no system apps are visible to it and there is no root access or
package manager. The full version (system shell, installed apps, desktop
shortcuts) is available from Chum or OpenRepos.

%prep
%setup -q -n %{name}-%{version}

# Extract Zig compiler (OBS only)
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

# Extract the bundled busybox source (store userland; OBS runs the
# preparation step, in-place SDK builds skip it)
if [ -f "%{SOURCE5}" ]; then
    tar -xjf "%{SOURCE5}" -C %{_builddir}
fi

%build

# Detect target architecture for Zig cross-compilation
%ifarch aarch64
    ZIG_TARGET="aarch64-linux-gnu.2.28"
    ZIG_MUSL_TARGET="aarch64-linux-musl"
    LIB_ARCH="aarch64"
%endif
%ifarch %arm
    ZIG_TARGET="arm-linux-gnueabihf.2.28"
    ZIG_MUSL_TARGET="arm-linux-musleabihf"
    LIB_ARCH="armv7hl"
%endif
%ifarch %ix86
    ZIG_TARGET="x86-linux-gnu.2.28"
    ZIG_MUSL_TARGET="x86-linux-musl"
    LIB_ARCH="i486"
%endif

# Fail early if architecture is unsupported
if [ -z "${ZIG_TARGET}" ]; then
    echo "ERROR: Unsupported architecture %{_arch}" >&2
    exit 1
fi

# IDE flow bootstrap (see rpm/ghosteel.spec for the rationale)
if [ ! -f lib/${LIB_ARCH}/libghostty-vt.a ] && [ -L .sfdk/src ]; then
    SRC="$(readlink -f .sfdk/src)"
    if [ -d "$SRC" ]; then
        echo "IDE build: linking source tree from $SRC"
        for item in ghosteel.pro src qml shaders translations \
                    ghostty lib icons rpm \
                    ghosteel.desktop harbour-ghosteel.desktop LICENSE; do
            if [ -e "$SRC/$item" ]; then
                rm -rf "./$item" 2>/dev/null
                ln -s "$SRC/$item" .
            fi
        done
    fi
fi

# Build libghostty-vt from source only if no prebuilt lib exists
if [ ! -f lib/${LIB_ARCH}/libghostty-vt.a ]; then

    ZIG=""
    if [ -f "%{_builddir}/zig-x86_64-linux-%{zig_version}/zig" ]; then
        ZIG="%{_builddir}/zig-x86_64-linux-%{zig_version}/zig"
    elif command -v zig >/dev/null 2>&1; then
        ZIG="zig"
    fi

    if [ -z "$ZIG" ]; then
        echo "ERROR: lib/${LIB_ARCH}/libghostty-vt.a not found and no Zig compiler available." >&2
        exit 1
    fi

    if [ ! -d ghostty/src ]; then
        echo "ERROR: ghostty/ submodule not initialized." >&2
        exit 1
    fi

    # --system mode trusts pre-extracted dirs without hash verification,
    # allowing the deps cache to be stripped for smaller size.
    CACHE_FLAG=""
    if ls %{_builddir}/zig-cache/*/ >/dev/null 2>&1; then
        CACHE_FLAG="--system %{_builddir}/zig-cache"
    fi

    cd ghostty
    LIB_VERSION=$(sed -n 's/^#define GHOSTTY_VT_VERSION "\(.*\)"/\1/p' include/ghostty/vt/version.h)
    if [ -z "$LIB_VERSION" ]; then
        echo "ERROR: GHOSTTY_VT_VERSION not found in ghostty headers" >&2
        exit 1
    fi
    "$ZIG" build -Demit-lib-vt \
        -Dtarget="${ZIG_TARGET}" \
        -Doptimize=ReleaseSafe \
        -Dlib-version-string="${LIB_VERSION}" \
        ${CACHE_FLAG} 2>&1 || exit 1
    mkdir -p %{_builddir}/%{name}-%{version}/lib/${LIB_ARCH}
    cp zig-out/lib/libghostty-vt.a %{_builddir}/%{name}-%{version}/lib/${LIB_ARCH}/
    cd %{_builddir}/%{name}-%{version}
fi

# The Zig toolchain, deps cache and Zig caches are dead weight once
# libghostty-vt.a is in lib/; remove them so OBS packaging has room.
if [ -d "%{_builddir}/zig-x86_64-linux-%{zig_version}" ] && [ ! -L ghostty ]; then
    rm -rf "%{_builddir}/zig-x86_64-linux-%{zig_version}" \
           "%{_builddir}/zig-cache" \
           ghostty/.zig-cache ghostty/zig-out
    [ -n "$HOME" ] && rm -rf "$HOME/.cache/zig"
fi

# Inject version info (no .git/ in OBS tarballs, so git describe fails)
sed -i "s/isEmpty(GIT_VERSION): GIT_VERSION = \"[^\"]*\"/isEmpty(GIT_VERSION): GIT_VERSION = \"%{version}\"/" ghosteel.pro

# Bundled busybox userland. Static on purpose: the sandbox blacklists the
# system busybox and masks the system PATH dirs entirely, so the terminal
# ships its own toolset; static musl avoids pulling in disallowed
# shared-library dependencies and works on any SFOS release. Prefer the
# prebuilt binary from scripts/build-libs.sh (the sfdk build engine is
# 32-bit and cannot run the x86_64 Zig toolchain); fall back to building
# with Zig here (CI docker and OBS hosts are x86_64). Extractions are
# idempotent because in-place SDK builds skip the preparation step.
BUSYBOX_BIN=""
if [ -f "lib/${LIB_ARCH}/busybox" ]; then
    BUSYBOX_BIN="lib/${LIB_ARCH}/busybox"
fi
if [ ! -d "%{_builddir}/busybox-1.38.0" ]; then
    BB_TARBALL=""
    if [ -f "%{SOURCE5}" ]; then
        BB_TARBALL="%{SOURCE5}"
    elif [ -f rpm/busybox-1.38.0.tar.bz2 ]; then
        BB_TARBALL="rpm/busybox-1.38.0.tar.bz2"
    elif [ -L .sfdk/src ] && [ -f "$(readlink -f .sfdk/src)/rpm/busybox-1.38.0.tar.bz2" ]; then
        BB_TARBALL="$(readlink -f .sfdk/src)/rpm/busybox-1.38.0.tar.bz2"
    fi
    if [ -n "$BB_TARBALL" ] && [ -z "$BUSYBOX_BIN" ]; then
        tar -xjf "$BB_TARBALL" -C %{_builddir}
    fi
fi
if [ -z "$BUSYBOX_BIN" ]; then
    if [ ! -d "%{_builddir}/busybox-1.38.0" ]; then
        echo "ERROR: busybox-1.38.0 source tarball not found" >&2
        exit 1
    fi
    ZIG_BB=""
    if [ -f "%{_builddir}/zig-x86_64-linux-%{zig_version}/zig" ]; then
        ZIG_BB="%{_builddir}/zig-x86_64-linux-%{zig_version}/zig"
    elif command -v zig >/dev/null 2>&1; then
        ZIG_BB="zig"
    fi
    if [ -z "$ZIG_BB" ]; then
        # Zig not extracted (prep skipped or cleaned up): extract the
        # vendored toolchain tarball (CI docker builds)
        ZIG_TARBALL=""
        if [ -f "%{SOURCE1}" ]; then
            ZIG_TARBALL="%{SOURCE1}"
        elif [ -f "rpm/zig-x86_64-linux-%{zig_version}.tar.xz" ]; then
            ZIG_TARBALL="rpm/zig-x86_64-linux-%{zig_version}.tar.xz"
        elif [ -L .sfdk/src ] && [ -f "$(readlink -f .sfdk/src)/rpm/zig-x86_64-linux-%{zig_version}.tar.xz" ]; then
            ZIG_TARBALL="$(readlink -f .sfdk/src)/rpm/zig-x86_64-linux-%{zig_version}.tar.xz"
        fi
        if [ -n "$ZIG_TARBALL" ]; then
            tar -xJf "$ZIG_TARBALL" -C %{_builddir}
            ZIG_BB="%{_builddir}/zig-x86_64-linux-%{zig_version}/zig"
        fi
    fi
    if [ -z "$ZIG_BB" ]; then
        echo "ERROR: no Zig compiler for the busybox build" >&2
        exit 1
    fi
    cd "%{_builddir}/busybox-1.38.0"
    make defconfig
    sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
    sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' .config
    make %{?_smp_mflags} SKIP_STRIP=y CC="$ZIG_BB cc -target ${ZIG_MUSL_TARGET}" busybox
    if command -v llvm-strip >/dev/null 2>&1; then
        llvm-strip --strip-all busybox
    fi
    cd %{_builddir}/%{name}-%{version}
    BUSYBOX_BIN="%{_builddir}/busybox-1.38.0/busybox"
fi

# Build Qt application; store_build selects the sandboxed variant
%qmake5 CONFIG+=store_build

%make_build


%install
# Recompute arch mapping (shell state does not survive between scriptlets)
%ifarch aarch64
    LIB_ARCH="aarch64"
%endif
%ifarch %arm
    LIB_ARCH="armv7hl"
%endif
%ifarch %ix86
    LIB_ARCH="i486"
%endif

%qmake5_install

# Bundled busybox userland: the app prepends this directory to the child
# PATH and execs the shell by absolute path. Shell state does not persist
# between scriptlets, so re-resolve the binary here (prebuilt first, then
# the in-build copy).
LIBEXEC=%{buildroot}%{_datadir}/%{name}/libexec
mkdir -p "$LIBEXEC"
BB_INSTALL="lib/${LIB_ARCH}/busybox"
[ -f "$BB_INSTALL" ] || BB_INSTALL="%{_builddir}/busybox-1.38.0/busybox"
cp "$BB_INSTALL" "$LIBEXEC/busybox"
for applet in sh ash ls cat cp mv rm mkdir rmdir ln touch chmod \
              grep egrep fgrep sed awk ps top head tail wc du dd stat env \
              which uname id whoami sleep date clear free uptime kill \
              pidof find xargs tar ping vi mktemp realpath readlink; do
    ln -sf busybox "$LIBEXEC/$applet"
done

# First-launch home skeleton copied into the persistent sandbox home
mkdir -p %{buildroot}%{_datadir}/%{name}/skeleton
if [ -f rpm/store-skeleton/.profile ]; then
    install -m 644 rpm/store-skeleton/.profile \
        %{buildroot}%{_datadir}/%{name}/skeleton/.profile
else
    install -m 644 "%{SOURCE3}" \
        %{buildroot}%{_datadir}/%{name}/skeleton/.profile
fi

# Busybox is GPLv2; ship its license alongside the app's
mkdir -p %{buildroot}%{_datadir}/%{name}/licenses
if [ -f rpm/busybox-GPLv2 ]; then
    install -m 644 rpm/busybox-GPLv2 \
        %{buildroot}%{_datadir}/%{name}/licenses/busybox-GPLv2
else
    install -m 644 "%{SOURCE4}" \
        %{buildroot}%{_datadir}/%{name}/licenses/busybox-GPLv2
fi

desktop-file-install --delete-original \
    --dir %{buildroot}%{_datadir}/applications \
    %{buildroot}%{_datadir}/applications/*.desktop

# The versioned %license dir is not an allowed Harbour path; ship the
# license as a regular file inside the app's share tree instead.
rm -rf %{buildroot}%{_datadir}/licenses
install -m 644 LICENSE %{buildroot}%{_datadir}/%{name}/LICENSE

# Strip the main binary (debug_package is disabled, so no brp strip runs).
%ifarch aarch64
    CROSS_STRIP="aarch64-meego-linux-gnu-strip"
%endif
%ifarch %arm
    CROSS_STRIP="armv7hl-meego-linux-gnueabi-strip"
%endif
%ifarch %ix86
    CROSS_STRIP="i486-meego-linux-gnu-strip"
%endif
if command -v llvm-strip >/dev/null 2>&1; then
    llvm-strip --strip-all %{buildroot}%{_bindir}/%{name}
elif command -v "$CROSS_STRIP" >/dev/null 2>&1; then
    "$CROSS_STRIP" --strip-all %{buildroot}%{_bindir}/%{name}
fi

%files
%{_bindir}/%{name}
%{_datadir}/%{name}
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png
