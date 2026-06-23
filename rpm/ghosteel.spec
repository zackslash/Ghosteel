Name:       ghosteel

Summary:    Ghosteel terminal emulator for Sailfish OS
Version:    0.0.0
Release:    1
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

# Apply Ghosteel patches to Ghostty submodule (patch, not git apply — no .git in OBS tarball)
for p in patches/*.patch; do
    [ -f "$p" ] && patch -d ghostty -p1 < "$p"
done

# Extract Zig compiler
tar -xJf %{_sourcedir}/zig-x86_64-linux-0.15.2.tar.xz -C %{_builddir}

# Set up Zig package cache with uucode dependency
UUCODE_HASH="uucode-0.2.0-ZZjBPqZVVABQepOqZHR7vV_NcaN-wats0IB6o-Exj6m9"
ZIG_CACHE="%{_builddir}/zig-cache"
mkdir -p "${ZIG_CACHE}/p/${UUCODE_HASH}"
tar -xzf "%{_sourcedir}/${UUCODE_HASH}.tar.gz" --strip-components=1 -C "${ZIG_CACHE}/p/${UUCODE_HASH}"

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

# Build libghostty-vt from source (skipped if CI already provided prebuilt lib)
if [ ! -f lib/${LIB_ARCH}/libghostty-vt.a ]; then
    ZIG="%{_builddir}/zig-x86_64-linux-0.15.2/zig"
    cd ghostty
    "${ZIG}" build -Demit-lib-vt -Dtarget="${ZIG_TARGET}" -Doptimize=ReleaseSafe --system "%{_builddir}/zig-cache/p" 2>&1 || exit 1
    mkdir -p %{_builddir}/%{name}-%{version}/lib/${LIB_ARCH}
    cp zig-out/lib/libghostty-vt.a %{_builddir}/%{name}-%{version}/lib/${LIB_ARCH}/
    cd %{_builddir}/%{name}-%{version}
fi

# Build Qt application
%qmake5

%make_build


%install
%qmake5_install


desktop-file-install --delete-original         --dir %{buildroot}%{_datadir}/applications                %{buildroot}%{_datadir}/applications/*.desktop

%files
%license LICENSE
%{_bindir}/%{name}
%{_datadir}/%{name}
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png
%{_datadir}/dbus-1/services/com.zackslash.ghosteel.service

