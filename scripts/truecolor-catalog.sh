#!/bin/sh
# Truecolor render catalog for Ghosteel.
#
# Emits direct-color SGR sequences (38;2 / 48;2) so the render pipeline
# (ghostty core SGR parsing + glrenderer exact-RGB cell fills) can be
# validated visually, and reports the session environment so the
# COLORTERM=truecolor advertisement (src/ptymanager.cpp) is verified in
# the same run.
#
# Sections:
#   A  environment report: COLORTERM must read "truecolor" once a build
#      with the env fix is deployed; the SGR tests below are meaningful
#      even on older builds (the terminal has always rendered 24-bit)
#   B  smooth 24-bit gradient strip over a 256-color control strip that
#      SHOULD look banded; if the truecolor strip resembles the control,
#      something is quantizing
#   C  exact-color swatches plus a 24-bit foreground ramp
#   D  256-color palette vs direct color; direct values are deliberately
#      off palette entries, so a snapped render makes the pair identical
#
# Kept under the narrowest portrait width (40 cells) so nothing wraps.
# Written for busybox ash (no bc; printf + $(( )) arithmetic).
# Blocks at the end (reads stdin) so the session stays open.

set -eu

# Session title so the catalog session is easy to find in the list.
printf '\033]2;truecolor catalog\007'

# House catalog convention: stay under the narrowest portrait width.
cells=40

echo "=== [A] Session environment ==="
if [ -z "${GHOSTTY_RESOURCES_DIR:-}" ]; then
    echo "NOTE: not spawned by Ghosteel (no GHOSTTY_RESOURCES_DIR);"
    echo "      [A] reports this context's env, not Ghosteel's."
fi
printf 'COLORTERM=%s  TERM=%s\n' "${COLORTERM:-<unset>}" "${TERM:-<unset>}"
if [ "${COLORTERM:-}" = "truecolor" ]; then
    echo "env: OK - apps (fish <4.1, neovim) will auto-enable 24-bit color"
else
    echo "env: FAIL for this branch - COLORTERM must read 'truecolor'"
    echo "     (raw SGR rendering below still works on older builds)"
fi
echo ""

echo "=== [B] 24-bit gradient (blue -> red) with 256-color control ==="
echo "Expect: truecolor rows perfectly smooth; the control strip below"
echo "SHOULD look banded. Truecolor rows resembling the control = FAIL."
row=0
while [ $row -lt 2 ]; do
    i=0
    while [ $i -lt $cells ]; do
        r=$(( i * 255 / (cells - 1) ))
        printf '\033[48;2;%d;0;%dm ' "$r" "$(( 255 - r ))"
        i=$(( i + 1 ))
    done
    printf '\033[0m\n'
    row=$(( row + 1 ))
done
i=0
while [ $i -lt $cells ]; do
    r=$(( i * 5 / (cells - 1) ))
    printf '\033[48;5;%dm ' "$(( 16 + 36 * r + (5 - r) ))"
    i=$(( i + 1 ))
done
printf '\033[0m  <- 256-color control (banded is correct here)\n'
echo ""

echo "=== [C] Exact-color swatches + foreground ramp ==="
echo "Expect: each block matches its label exactly (pure, unshifted);"
echo "fg ramp as smooth as [B]."
printf '  \033[48;2;255;0;0m  \033[0m 255;0;0 red\n'
printf '  \033[48;2;0;255;0m  \033[0m 0;255;0 green\n'
printf '  \033[48;2;0;0;255m  \033[0m 0;0;255 blue\n'
printf '  \033[48;2;255;255;0m  \033[0m 255;255;0 yellow\n'
printf '  \033[48;2;255;0;255m  \033[0m 255;0;255 magenta\n'
printf '  \033[48;2;0;255;255m  \033[0m 0;255;255 cyan\n'
i=0
while [ $i -lt $cells ]; do
    r=$(( i * 255 / (cells - 1) ))
    printf '\033[38;2;%d;0;%dm#' "$r" "$(( 255 - r ))"
    i=$(( i + 1 ))
done
printf '\033[0m fg ramp, same blue-red sweep as [B], expect smooth\n'
echo ""

echo "=== [D] 256-color palette vs direct color (off-palette values) ==="
echo "Expect: each direct block matches its hue label and is visibly"
echo "distinct from the palette neighbor; identical pair = snapped = FAIL."
printf '  palette \033[48;5;196m  \033[0m  direct \033[48;2;250;40;40m  \033[0m red\n'
printf '  palette \033[48;5;46m  \033[0m  direct \033[48;2;40;250;40m  \033[0m green\n'
printf '  palette \033[48;5;21m  \033[0m  direct \033[48;2;40;40;250m  \033[0m blue\n'
printf '  palette \033[48;5;208m  \033[0m  direct \033[48;2;255;100;0m  \033[0m orange\n'
echo ""

echo "=== catalog done; session stays open. ==="
echo "PASS = [A] COLORTERM=truecolor, [B] truecolor rows smooth and"
echo "       visibly smoother than the control, [C] exact swatches and"
echo "       smooth fg ramp, [D] direct blocks distinct and correctly hued."
echo "FAIL = [A] any other COLORTERM, banded truecolor rows, shifted or"
echo "       swapped swatch colors, or a direct block identical to its"
echo "       palette neighbor."
echo "Press Ctrl+D (or close the session) to finish."
cat > /dev/null
