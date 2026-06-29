<p align="center">
  <img src="icons/full.png" alt="Ghosteel" width="128">
</p>

<h1 align="center">Ghosteel</h1>

Desktop-class terminal for SailfishOS, powered by <a href="https://github.com/ghostty-org/ghostty">Ghostty</a>'s VT engine. Truecolor, GPU-rendered, multi-session, TUI apps, encrypted scrollback.

Ghosteel brings a modern terminal engine to SailfishOS. Most mobile terminals use
legacy VT parsers with known limitations. Ghosteel uses the same engine that powers
[Ghostty](https://github.com/ghostty-org/ghostty), giving you accurate rendering for
`tmux`, `neovim`, `htop`, and other TUI applications.

<div align="center">

[![SailfishOS:Chum](https://img.shields.io/badge/SailfishOS-Chum-1CA198)](https://build.sailfishos.org/package/show/sailfishos:chum/ghosteel)

</div>

<div align="center">
  <table>
    <tr>
      <td align="center"><img src="screenshots/screenshot-neofetch-device.png" width="180"></td>
      <td align="center"><img src="screenshots/screenshot-btop1-device.png" width="180"></td>
      <td align="center"><img src="screenshots/screenshot-sessions-device.png" width="180"></td>
    </tr>
    <tr>
      <td align="center"><sub>Truecolor &amp; GPU Rendering</sub></td>
      <td align="center"><sub>TUI App Support</sub></td>
      <td align="center"><sub>Multi-session</sub></td>
    </tr>
  </table>
</div>

## Features

- **Ghostty VT engine**: full escape sequence support, 24-bit color, alternate screen buffer
- **GPU rendering**: OpenGL ES 2.0/3.0 renderer with cursor trails shader support
- **Multi-session**: create, name, switch, and persist sessions with per-session working directories
- **[Command Sessions](#desktop-file-launchers)**: launch commands in new sessions (`ghosteel -e htop`), switch to named sessions (`ghosteel -s editor`), create .desktop file launchers for TUI apps
- **Touch text selection**: long-press with Sailfish-style magnifier, velocity-aware hiding, double/triple tap
- **Pinch-to-zoom**: two-finger pinch to adjust font size with live overlay, per-session font size, optional (off by default)
- **Extra keys bar**: configurable sticky modifiers (Ctrl/Alt), arrow keys, F1-F12, PgUp/PgDn
- **Link detection**: OSC 8 hyperlinks and automatic URL detection, tap to open in browser
- **Inline images**: Kitty Graphics Protocol support, PNG decoding, configurable toggle
- **Encrypted scrollback**: AES-256 encryption via Sailfish Secrets, configurable retention (7–365 days or disabled)
- **2 color schemes**: Dark and Light, adjustable opacity
- **35+ translations**

## Install

**Recommended**: Install via [SailfishOS:Chum](https://build.sailfishos.org/package/show/sailfishos:chum/ghosteel):

```bash
devel-su pkcon install ghosteel
```

Also available from [OpenRepos](https://openrepos.net/content/zackslash/ghosteel-terminal) or [GitHub Releases](https://github.com/zackslash/Ghosteel/releases):

```bash
devel-su pkcon install-local ./ghosteel-<version>.rpm
```

### Desktop file launchers

Create `.desktop` files in `~/.local/share/applications/` to launch TUI apps directly into Ghosteel sessions:

```ini
[Desktop Entry]
Type=Application
X-Nemo-Application-Type=silica-qt5
X-Nemo-Single-Instance=no
Icon=ghosteel
Exec=ghosteel -e top
Name=Top

[X-Sailjail]
OrganizationName=com.zackslash
ApplicationName=ghosteel
Permissions=UserDirs;Secrets;
Sandboxing=Disabled
```

Use `-s <name>` to give the session a persistent name (survives app restart):

```ini
Exec=ghosteel -s sysmon -e top
```

Note: set `X-Nemo-Single-Instance` to `no`, otherwise the SailfishOS invoker swallows CLI args for already-running apps. If you edit a desktop file that's already on the homescreen, restart lipstick for the launcher to pick up changes:

```bash
systemctl --user restart lipstick.service
```

## Build

Requires Sailfish OS SDK and Zig 0.15.2 (Ghostty is incompatible with 0.16+).

```bash
# Install Zig 0.15.2
curl -L https://ziglang.org/download/0.15.2/zig-x86_64-linux-0.15.2.tar.xz | tar -xJ -C ~/.local/
ln -sf ~/.local/zig-x86_64-linux-0.15.2/zig ~/.local/bin/zig

# Build libghostty-vt for your target architecture
./scripts/build-libs.sh          # all architectures
./scripts/build-libs.sh aarch64  # single architecture

# Build RPM
mb2 build
```

## Architecture

Built with Qt/QML and Sailfish Silica. Terminal engine is Ghostty's `libghostty-vt` (Zig, static C library). Rendering via OpenGL ES. Supports Ghostty-compatible post-processing shaders on ES 3.0+. Native cover page, single-instance via QLocalSocket IPC. Built for aarch64, armv7hl, and i486.

## Development

### IDE Setup

Before first IDE build:

```bash
git submodule update --init
./scripts/build-libs.sh i486
```

The Ghostty submodule needs a patch for a Zig i386 C ABI bug. It's applied automatically by the spec during builds. For manual builds: `git -C ghostty apply patches/ghostty-i386-abi-fix.patch`

See [CONTRIBUTING.md](CONTRIBUTING.md) for git hooks and development guidelines.

## License

MIT
