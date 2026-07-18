#!/bin/sh
# Scrollback width restore catalog for Ghosteel.
#
# PURPOSE
#   Validate scrollback display width restore fix (#72). Output must be
#   rendered THROUGH Ghosteel — run it inside a Ghosteel session
#   (see scripts/emulator_scrollback_width_test.sh) so the bytes pass through
#   the VT engine and the scrollback buffer.
#
# WHAT IT TESTS
#   When Ghosteel restores scrollback at a different column count (e.g. after
#   resize or restart), lines that were wrapped at the old width must re-wrap
#   cleanly at cell boundaries. The bug caused mid-glyph breaks, extra blank
#   lines, or duplicated prompt lines after restore.
#
#   Mid-glyph breaks / extra blank lines          = FAIL (pre-fix)
#   Duplicated prompt (❯ appears twice)           = FAIL (pre-fix)
#   Accented characters corrupted on re-wrap      = FAIL (pre-fix)
#   Clean re-wrap with aligned box-drawing        = PASS

hr() { printf '%s\n' "------------------------------------------------------------"; }

printf '\n'
hr
printf '%s\n' "GHOSTEEL SCROLLBACK WIDTH RESTORE CATALOG"
hr

# --- Section A: Cyrillic at known widths -------------------------------------
printf '\n%s\n' "Section A — Cyrillic (36-cell rows)"
hr
printf '%s\n' "ПриветМирПриветМирПриветМирПриветМир"
printf '%s\n' "ПриветМирПриветМирПриветМирПриветМир"
printf '%s\n' "ПриветМирПриветМирПриветМирПриветМир"
printf '%s\n' "ПриветМирПриветМирПриветМирПриветМир"

# --- Section B: Latin-1 accented text ----------------------------------------
printf '\n%s\n' "Section B — Latin-1 accented text"
hr
printf '%s\n' "café résumé naïve café résumé naïve café résumé naïve"
printf '%s\n' "café résumé naïve café résumé naïve café résumé naïve"
printf '%s\n' "café résumé naïve café résumé naïve café résumé naïve"
printf '%s\n' "café résumé naïve café résumé naïve café résumé naïve"

# --- Section C: Box-drawing borders ------------------------------------------
printf '\n%s\n' "Section C — Box-drawing borders"
hr
printf '%s\n' "┌──────────────────────────────────────────┐"
printf '%s\n' "│ Column A     │ Column B     │ Column C   │"
printf '%s\n' "├──────────────┼──────────────┼────────────┤"
printf '%s\n' "│ ПриветМир    │ café résumé  │ A😀B国C    │"
printf '%s\n' "│ тест         │ naïve        │ ABC        │"
printf '%s\n' "└──────────────┴──────────────┴────────────┘"

# --- Section D: Multibyte prompt line (#4 trigger) ---------------------------
printf '\n%s\n' "Section D — Multibyte prompt (❯ U+276F)"
hr
printf '%s' "❯ "

# --- Separator and instruction -----------------------------------------------
printf '\n\n'
hr
printf '%s\n' "=== Scrollback populated. Now follow the rubric in the driver output. ==="
hr
printf '\n'

# Keep the session open so the scrollback stays visible. An -e command that
# exits causes Ghosteel to auto-close the session, so without this the
# catalog flashes and disappears. Block here until the user closes the
# session from the UI or sends EOF (Ctrl+D).
printf '%s\n' "[ Session kept open for inspection. Close it from the Ghosteel UI,"
printf '%s\n' "  or press Ctrl+D, when done. ]"
read -r _ 2>/dev/null || true
