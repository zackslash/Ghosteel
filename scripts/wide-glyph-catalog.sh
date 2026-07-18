#!/bin/sh
# Wide-glyph rendering catalog for Ghosteel.
#
# PURPOSE
#   Validate wide-glyph rendering fix (#76). Output must be rendered THROUGH
#   Ghosteel — run it inside a Ghosteel session
#   (see scripts/emulator_wide_glyph_test.sh) so the bytes pass through the
#   VT engine and the glyph width calculation.
#
# WHAT IT TESTS
#   Ghosteel must assign the correct cell width to every codepoint:
#     - Emoji and CJK ideographs: 2 cells wide
#     - Fullwidth Latin: 2 cells wide
#     - Ambiguous box-drawing: 1 cell wide (NOT widened)
#     - Narrow Latin: 1 cell wide (NOT widened)
#     - ZWJ sequences: rendered as ONE composite glyph (single width)
#
#   After any wide glyph, subsequent narrow glyphs must align on cell
#   boundaries with no extra gaps or misalignment.
#
#   Horizontal squashing / extra gaps after wide glyphs  = FAIL (pre-fix)
#   ZWJ family showing lone base codepoint               = FAIL (pre-fix)
#   Box-drawing widened to 2 cells                        = FAIL (regression)

hr() { printf '%s\n' "------------------------------------------------------------"; }

printf '\n'
hr
printf '%s\n' "GHOSTEEL WIDE-GLYPH RENDERING CATALOG"
hr

# --- Section A: Single-codepoint wide glyphs (emoji) -------------------------
printf '\n%s\n' "Section A — Single-codepoint wide emoji (2 cells each)"
hr
printf '%s\n' "😀 (U+1F600 — Grinning Face)"
printf '%s\n' "😎 (U+1F60E — Smiling Face with Sunglasses)"

# --- Section B: CJK ideographs (2 cells each) --------------------------------
printf '\n%s\n' "Section B — CJK ideographs (2 cells each)"
hr
printf '%s\n' "国 (U+56FD — country)"
printf '%s\n' "人 (U+4EBA — person)"

# --- Section C: Fullwidth Latin (2 cells each) --------------------------------
printf '\n%s\n' "Section C — Fullwidth Latin (2 cells each)"
hr
printf '%s\n' "Ａ (U+FF21) Ｂ (U+FF22) Ｃ (U+FF23)"

# --- Section D: Box-drawing — Ambiguous, must stay 1 cell ---------------------
printf '\n%s\n' "Section D — Box-drawing: Ambiguous width, must stay 1 cell"
hr
printf '%s\n' "─ (U+2500) │ (U+2502) ┌ (U+250C) ┐ (U+2510)"

# --- Section E: Narrow Latin — regression guard, must NOT widen ---------------
printf '\n%s\n' "Section E — Narrow Latin: must stay 1 cell"
hr
printf '%s\n' "abc ABC"

# --- Section F: ZWJ family sequence (multi-codepoint, 1 composite glyph) -----
printf '\n%s\n' "Section F — ZWJ family: one composite glyph"
hr
printf '%s\n' "👨‍👩‍👧 (man ZWJ woman ZWJ girl — one family glyph)"

# --- Section G: Mixed row — alignment after wide glyphs ----------------------
printf '\n%s\n' "Section G — Mixed row: alignment check after wide glyphs"
hr
printf '%s\n' "A😀B国C"

# --- Section H: All together — interleaved wide and narrow --------------------
printf '\n%s\n' "Section H — Interleaved wide/narrow on one line"
hr
printf '%s\n' "X😀Y国ZＡＢＣ─│┌┐abcABC"

printf '\n'
hr
printf '%s\n' "END OF CATALOG"
printf '%s\n' "Verify: Wide glyphs fill 2 cells; narrow/ambiguous fill 1 cell."
printf '%s\n' "        ZWJ family renders as one composite glyph."
printf '%s\n' "        Mixed row A😀B国C aligns correctly (no gaps)."
hr
printf '\n'

# Keep the session open so the rendered glyphs stay visible. An -e command
# that exits causes Ghosteel to auto-close the session, so without this the
# catalog flashes and disappears. Block here until the user closes the
# session from the UI or sends EOF (Ctrl+D).
printf '%s\n' "[ Session kept open for inspection. Close it from the Ghosteel UI,"
printf '%s\n' "  or press Ctrl+D, when done. ]"
read -r _ 2>/dev/null || true
