Ghosteel brings a modern terminal engine to SailfishOS. Most mobile terminals
use legacy VT parsers that struggle with complex TUI apps. Ghosteel uses the
same engine that powers [Ghostty](https://github.com/ghostty-org/ghostty),
giving you accurate rendering for tmux, neovim, htop, and other TUI
applications.

## Features

- **Ghostty VT engine** — full escape sequence support, 24-bit truecolor
- **GPU rendering** — OpenGL ES 2.0/3.0 with cursor trail shaders
- **Multi-session** — named sessions, switching, and persistence
- **Command sessions** — launch TUI apps from desktop shortcuts (`ghosteel -e htop`)
- **Touch text selection** — Sailfish-style magnifier, double/triple tap
- **Pinch-to-zoom** — per-session font size adjustment
- **Extra keys bar** — sticky Ctrl/Alt, arrow keys, F1–F12
- **URL detection** — automatic hyperlinks, tap to open in the browser
- **Inline images** — Kitty Graphics Protocol with PNG decoding
- **Encrypted scrollback** — Encryption backed by Sailfish Secrets
- **Color schemes** — Dark and Light with adjustable opacity

---

Source code: [github.com/zackslash/Ghosteel](https://github.com/zackslash/Ghosteel) · License: MIT
