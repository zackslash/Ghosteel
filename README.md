<p align="center">
  <img src="icons/full.png" alt="Ghosteel" width="128">
</p>

# Ghosteel

Terminal emulator for SailfishOS powered by [Ghostty](https://github.com/ghostty-org/ghostty)'s libghostty-vt engine. Truecolor, multi-session, TUI apps.

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

C++ host app with QML/Silica UI. Terminal engine is Ghostty's `libghostty-vt` (Zig, built as a static C library). Rendering via QPainter. Built for aarch64, armv7hl, and i486.

## Development

The Ghostty submodule needs a patch applied before building locally. The patch works around a Zig i386 C ABI bug that corrupts struct-by-value parameters. It's applied automatically in CI, but for local builds:

```bash
git -C ghostty apply patches/ghostty-i386-abi-fix.patch
```

The patch changes `ghostty_terminal_new` to accept options by pointer instead of by value. Can be dropped once Ghostty upgrades to Zig >= 0.16.0. See `patches/ghostty-i386-abi-fix.patch` for details.

## License

MIT
