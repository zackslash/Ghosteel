<p align="center">
  <img src="icons/full.png" alt="Ghosteel" width="128">
</p>

# Ghosteel

Desktop-class terminal for SailfishOS, powered by [Ghostty](https://github.com/ghostty-org/ghostty)'s VT engine. Truecolor, GPU-rendered, multi-session, TUI apps, encrypted scrollback.

Ghosteel brings a modern terminal engine to SailfishOS. Most mobile terminals use
legacy VT parsers with known limitations. Ghosteel uses the same engine that powers
[Ghostty](https://github.com/ghostty-org/ghostty), giving you accurate rendering for
`tmux`, `neovim`, `htop`, and other TUI applications.

## Features

- **Ghostty VT engine**: full escape sequence support, 24-bit color, alternate screen buffer
- **GPU rendering**: OpenGL ES 2.0/3.0 renderer with cursor trails shader support
- **Multi-session**: create, name, switch, and persist sessions with per-session working directories
- **Encrypted scrollback**: AES-256 encryption via Sailfish Secrets, configurable retention (7 to 365 days)
- **Link detection**: OSC 8 hyperlinks and automatic URL detection, tap to open in browser
- **Touch text selection**: long-press with Sailfish-style magnifier, velocity-aware hiding, double/triple tap
- **Extra keys bar**: configurable sticky modifiers (Ctrl/Alt), arrow keys, F1-F12, PgUp/PgDn
- **CJK-aware**: proper wide-character handling in selection, search, and scrollback
- **2 color schemes**: Dark and Light, adjustable opacity
- **35+ translations**

## Install

Download the `.rpm` for your architecture from [Releases](https://github.com/zackslash/sfos-ghostty/releases):

```bash
devel-su pkcon install-local ./ghosteel-<version>.rpm
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

Built with Qt/QML and Sailfish Silica. Terminal engine is Ghostty's `libghostty-vt` (Zig, static C library). Rendering via OpenGL ES. Supports Ghostty-compatible post-processing shaders on ES 3.0+. Native cover page, single-instance via D-Bus. Built for aarch64, armv7hl, and i486.

## Development

The Ghostty submodule needs a patch applied before building locally. The patch works around a Zig i386 C ABI bug that corrupts struct-by-value parameters. It's applied automatically in CI, but for local builds:

```bash
git -C ghostty apply patches/ghostty-i386-abi-fix.patch
```

The patch changes `ghostty_terminal_new` to accept options by pointer instead of by value. Can be dropped once Ghostty upgrades to Zig >= 0.16.0. See `patches/ghostty-i386-abi-fix.patch` for details.

## License

MIT
