#!/bin/sh
# Glyph atlas render catalog for Ghosteel.
#
# PURPOSE
#   Validate the GlyphAtlas upload path in src/glyphatlas.cpp (rasterizeGlyph).
#   Output must be rendered THROUGH Ghosteel — run it inside a Ghosteel session
#   (see scripts/emulator_glyph_test.sh) so the bytes pass through the VT
#   engine and the refactored glyph atlas.
#
# WHAT IT TESTS
#   GlyphAtlas keeps a reusable CPU scratch QImage (m_glyphScratch) sized to the
#   WIDEST glyph it has ever rasterized. After any wide glyph (CJK/emoji), every
#   subsequent NARROWER glyph is uploaded to the GPU via a tightly-packed buffer
#   (the m_uploadBuf re-stride loop), because GLES2 has no GL_UNPACK_ROW_LENGTH
#   and glTexSubImage2D reads glyphWidth*4 bytes per row.
#
#   If that re-stride ever regresses, narrow glyphs following a wide one render
#   as horizontal bands with transparent gaps (or as fragments of the prior
#   glyph). This catalog engineers that exact sequence:
#
#       Section A (ASCII baseline)  ->  Section C (wide CJK)  ->  Section D (ASCII again)
#
#   Section D MUST look identical to Section A. If D is garbled, the atlas
#   upload stride is broken. Section E hammers the narrow/wide alternation on
#   a single line — the most demanding case.
#
#   Garbled bands / transparent vertical gaps        = FAIL (stride regression)
#   Tofu boxes for emoji (Section G)                 = OK (font lacks the glyph)
#   Bold/underline not styled (Section F)            = separate concern, not this test

ESC=$(printf '\033')
BOLD="${ESC}[1m"
UNDERLINE="${ESC}[4m"
RESET="${ESC}[0m"

hr() { printf '%s\n' "------------------------------------------------------------"; }

printf '\n'
hr
printf '%s\n' "GHOSTEEL GLYPH ATLAS RENDER CATALOG"
hr

# --- Section A: ASCII baseline (REFERENCE — Section D must match this) -------
printf '\n%s\n' "Section A — ASCII baseline (REFERENCE)"
hr
printf '%s\n' "The quick brown fox jumps over the lazy dog. 0123456789"
printf '%s\n' "ABCDEFGHIJ KLMNOPQRST UVWXYZ abcdefghij klmnopqrst uvwxyz"
printf '%s\n' "Punctuation: (paren) [bracket] {brace} <angle> semi; colon: comma, dot."
printf '%s\n' "Symbols: plus+ minus- star* slash/ equal= percent% hash# at& pipe| qmark!"

# --- Section B: accented / extended Latin ------------------------------------
printf '\n%s\n' "Section B — Accented and extended Latin"
hr
printf '%s\n' "Accented: cafe resume naive facade uber nino Espana Zurich Angstrom OEuvre AEon"
printf '%s\n' "Diacritics: a a^ a~ a: a( e e^ e: i i^ i: o o^ o~ o: u u^ u: y: c~ n~ ?i i!"

# --- Section C: wide CJK (grows the scratch to its high-water mark) ----------
printf '\n%s\n' "Section C — Wide CJK (grows scratch; sets high-water mark)"
hr
printf '%s\n' "Chinese:   你好世界，中文测试，字体渲染验证。"
printf '%s\n' "Japanese:  こんにちは日本語、カタカナ、ひらがな。"
printf '%s\n' "Korean:    안녕하세요 한글 테스트입니다。"
printf '%s\n' "Fullwidth: ｆｕｌｌｗｉｄｔｈ ＃＠＋＝！？"

# --- Section D: ASCII again (STRIDE-BUG TRIGGER — must equal Section A) -------
printf '\n%s\n' "Section D — ASCII after wide (COMPARE TO SECTION A)"
hr
printf '%s\n' "The quick brown fox jumps over the lazy dog. 0123456789"
printf '%s\n' "ABCDEFGHIJ KLMNOPQRST UVWXYZ abcdefghij klmnopqrst uvwxyz"
printf '%s\n' "Punctuation: (paren) [bracket] {brace} <angle> semi; colon: comma, dot."
printf '%s\n' "Symbols: plus+ minus- star* slash/ equal= percent% hash# at& pipe| qmark!"

# --- Section E: narrow/wide interleaved on one line (hardest case) -----------
printf '\n%s\n' "Section E — Narrow/wide interleaved (alternates every cell)"
hr
printf '%s\n' "A中B文C字D日E本F語G韓H文I測J試K字L體M"
printf '%s\n' "mix混合 mixontop文字混在 ABC中DEF文GHI字JKLabc"
printf '%s\n' "1一2二3三4四5五6六7七8八9九0零end"

# --- Section F: bold / underline (font-weight variants in the atlas) ---------
printf '\n%s\n' "Section F — Bold / underline weight variants"
hr
printf '%s\n' "${BOLD}Bold ASCII: The quick brown fox 0123456789${RESET}"
printf '%s\n' "${UNDERLINE}Underline ASCII: abcdefghij UVWXYZ${RESET}"
printf '%s\n' "${BOLD}太字：日本語テスト Bold CJK${RESET}"
printf '%s\n' "${BOLD}${UNDERLINE}Bold+Underline mixed A中B文C字${RESET}"

# --- Section G: emoji (informational; tofu is OK, banding is not) ------------
printf '\n%s\n' "Section G — Emoji (tofu OK; horizontal bands = FAIL)"
hr
printf '%s\n' "Emoji: 😀 🚀 🎉 🐧 ✅ ❤️ ⭐ → ← ↑ ↓"
printf '\n'
hr
printf '%s\n' "END OF CATALOG"
printf '%s\n' "Verify: Section D looks identical to Section A."
printf '%s\n' "         Section E lines are clean (no banding between glyphs)."
hr
printf '\n'

# Keep the session open so the rendered glyphs stay visible. An -e command
# that exits causes Ghosteel to auto-close the session, so without this the
# catalog flashes and disappears. Block here until the user closes the
# session from the UI or sends EOF (Ctrl+D).
printf '%s\n' "[ Session kept open for inspection. Close it from the Ghosteel UI,"
printf '%s\n' "  or press Ctrl+D, when done. ]"
read -r _ 2>/dev/null || true
