#!/bin/sh
# Kitty graphics render catalog for Ghosteel.
#
# Emits kitty APC image sequences (f=100 PNG when the app icon is
# installed, f=32 direct RGBA otherwise) at several z-layers so the
# render pipeline (src/glrenderer_kitty.cpp, the two-pass cell draw)
# can be validated visually.
#
# Sections:
#   A  default placement (z=0; same above-text bucket as z=1 in
#      ghostty — kept as boundary documentation)
#   B  below-text (z=-1): text stays readable ON TOP, image above the
#      background, not dimmed by the background-opacity veil
#   C  above-text (z=1): image fully covers the text beneath it
#   D  f=32 direct RGBA checker (exercises the non-PNG upload path)
#
# Every image is scaled into the same 24x12-cell rect anchored at the
# cursor, and the cursor is returned to column 0 of the text row before
# emitting, so the image genuinely overlaps the X row (that overlap is
# what sections A/B/C test).
#
# Written for busybox ash (no fold/bc; expr substr + printf).
# Blocks at the end (reads stdin) so the session stays open.

set -eu

# Session title so the catalog session is easy to find in the list.
printf '\033]2;kitty graphics catalog\007'

# --- pick a payload: installed Ghosteel app icon, else f=32 checker -------
ICON=""
for size in 86x86 108x108 128x128 64x64 172x172 256x256; do
    p="/usr/share/icons/hicolor/$size/apps/ghosteel.png"
    if [ -f "$p" ]; then
        ICON="$p"
        break
    fi
done

# --- f=32 direct RGBA checker: 4x2 px, blue/yellow ------------------------
# blue = 0x20,0x60,0xC0 / yellow = 0xF0,0xC0,0x20, alpha 0xFF.
# Row 1: B Y B Y; row 2: Y B Y B. Octal escapes in the printf format.
rgba_b64=$(printf '\040\140\300\377\360\300\040\377\040\140\300\377\360\300\040\377\360\300\040\377\040\140\300\377\360\300\040\377\040\140\300\377' \
    | base64 | tr -d '\n')

payload_b64=$rgba_b64
payload_ctl="a=T,f=32,s=4,v=2,q=1"
payload_kind="f=32 checker (no icon found)"
if [ -n "$ICON" ]; then
    b=$(base64 "$ICON" 2>/dev/null | tr -d '\n') || b=""
    if [ -n "$b" ]; then
        payload_b64=$b
        payload_ctl="a=T,f=100,q=1"
        payload_kind="installed icon ($ICON)"
    fi
fi

# Destination rect for sections A-C: 24 cells wide, 12 rows tall.
RECT="c=24,r=12"

# emit_chunked <control-args> <base64>
# Chunks the payload into <=4096-char pieces: first chunk carries the
# control args plus m=1, continuations carry only m=.
emit_chunked() {
    _args=$1
    _b64=$2
    _len=${#_b64}
    _off=1
    while [ "$_off" -le "$_len" ]; do
        _chunk=$(expr substr "$_b64" "$_off" 4096)
        if [ $((_off + 4096)) -gt "$_len" ]; then
            _m=0
        else
            _m=1
        fi
        if [ "$_off" -eq 1 ]; then
            printf '\033_G%s,m=%s;%s\033\\' "$_args" "$_m" "$_chunk"
        else
            printf '\033_Gm=%s;%s\033\\' "$_m" "$_chunk"
        fi
        _off=$((_off + 4096))
    done
}

# section <label> <z-suffix> : prints the header, an X row, holds the
# cursor on that row, and places the image over it, then moves output
# below the 12-row rect. ${_z:+,$_z} appends ",z=..." only when _z is
# non-empty — a trailing comma would corrupt the control string and
# silently break multi-chunk transmission.
section() {
    _label=$1
    _z=$2
    echo "--- $_label ---"
    printf '%s' "$TEXTROW"
    printf '\r'
    emit_chunked "$payload_ctl,$RECT${_z:+,$_z}" "$payload_b64"
    printf '\n\n\n\n\n\n\n\n\n\n\n\n\n\n'
}

# Kept under the narrowest portrait width so it never wraps.
TEXTROW="XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"

echo "=== Ghosteel kitty graphics catalog ==="
echo "Payload: $payload_kind (scaled into 24x12 cells)"
echo ""

section "[A] Default placement (z=0; same bucket as C in ghostty)" ""
echo "Expect: image visible; X's under it hidden."
echo ""

section "[B] Below-text (z=-1)" "z=-1"
echo "Expect: X's readable ON TOP of the image; image above the background."
echo "If the image looks veiled/dimmed by the background color, or is"
echo "invisible with backgroundOpacity=1.0, the pass split is broken."
echo ""

section "[C] Above-text (z=1)" "z=1"
echo "Expect: image fully covers the X's."
echo ""

echo "--- [D] f=32 direct RGBA checker (4x2 px shown as 16x8 cells) ---"
echo "Expect: crisp blue/yellow checker, exact colors, no black or garbage."
emit_chunked "a=T,f=32,s=4,v=2,c=16,r=8,q=1" "$rgba_b64"
printf '\n\n\n\n\n\n\n\n\n\n\n\n'

echo "--- catalog done; session stays open. Cleanup inside a session:"
echo "    printf '\\033_Ga=d,d=A\\033\\\\'   (delete all kitty images)"
echo ""
cat > /dev/null
