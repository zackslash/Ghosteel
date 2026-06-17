#!/bin/bash
# Kitty Graphics Protocol test script for Ghosteel
# Downloads a test PNG from the project repo and displays it.
#
# Usage: Run inside Ghosteel terminal:
#   bash tests/test_kitty_graphics.sh
#
# Requirements: curl, base64

set -e

TEST_URL="https://raw.githubusercontent.com/zackslash/Ghosteel/main/icons/full.png"
TEST_IMG="/tmp/ghosteel_kitty_test.png"

echo "=== Kitty Graphics Protocol Test ==="
echo ""

# Download test image
echo "1. Downloading test image..."
curl -sLo "$TEST_IMG" "$TEST_URL"
SIZE=$(wc -c < "$TEST_IMG")
echo "   Downloaded: $TEST_IMG ($SIZE bytes)"

# Verify it's a PNG
TYPE=$(file -b "$TEST_IMG" 2>/dev/null || echo "unknown")
echo "   Type: $TYPE"
echo ""

# Encode path to base64
B64=$(echo -n "$TEST_IMG" | base64)

# Display at various sizes
echo "2. Displaying at 10x5 cells..."
printf '\x1b_Ga=T,f=100,t=f,c=10,r=5;%s\x1b\\' "$B64"
echo ""
echo ""

echo "3. Displaying at 30x15 cells..."
printf '\x1b_Ga=T,f=100,t=f,c=30,r=15;%s\x1b\\' "$B64"
echo ""
echo ""

echo "4. Displaying at native size..."
printf '\x1b_Ga=T,f=100,t=f;%s\x1b\\' "$B64"
echo ""
echo ""

echo "=== Done ==="
echo "If images appeared above, Kitty Graphics is working."
