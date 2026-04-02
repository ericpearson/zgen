#!/bin/bash
# Regression tests for known-fixed rendering issues.
# Requires ROMs in build/ and save states in ~/.genesis/saves/.
# Run from the project root: bash tests/test_regressions.sh

set -e
cd "$(dirname "$0")/.."

PASS=0
FAIL=0
SKIP=0

run_test() {
    local name="$1"
    shift
    if "$@"; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name"
        FAIL=$((FAIL + 1))
    fi
}

skip_test() {
    echo "  SKIP: $1 (ROM or save state missing)"
    SKIP=$((SKIP + 1))
}

# --- Panorama Cotton HUD ---
# The bottom HUD (EXP/SPEED/COTTON) must be visible at frame 32000.
# Generate start-mash input if needed.
test_cotton_hud() {
    local rom="build/Panorama Cotton (Japan).md"
    [ -f "$rom" ] || return 2

    python3 -c "
for f in range(100, 80000, 10):
    print(f'{f}:S')
    print(f'{f+3}:')
" > /tmp/test_start_mash.txt

    ./build/cpz_trace "$rom" none 32000 32000 /tmp/test_cotton_hud.ppm \
        --input-file /tmp/test_start_mash.txt 2>/dev/null

    python3 -c "
data = open('/tmp/test_cotton_hud.ppm','rb').read().split(b'\n',3)[3]
w = 320
# HUD area: lines 210-220, should have non-black colored pixels (HUD content)
hud_colors = set()
for line in range(210, 221):
    for x in range(40, 280):
        off = (line * w + x) * 3
        c = (data[off], data[off+1], data[off+2])
        if c != (0,0,0):
            hud_colors.add(c)
# The HUD has multiple colors (text, bars, portrait). Require at least 5.
if len(hud_colors) >= 5:
    exit(0)
else:
    print(f'Only {len(hud_colors)} non-black colors in HUD area (need >= 5)')
    exit(1)
"
}

# --- Top Gear 2 road flickering ---
# No black bands in the road area across 120 frames from save state.
test_topgear2_road() {
    local rom="build/Top Gear 2 (USA).md"
    local ss="$HOME/.genesis/saves/Top Gear 2 (USA).ss0"
    [ -f "$rom" ] && [ -f "$ss" ] || return 2

    python3 -c "
import subprocess, sys
bad = 0
for f in range(1, 121):
    subprocess.run([
        './build/cpz_trace', '$rom', '$ss',
        str(f), str(f), f'/tmp/test_tg2_{f}.ppm'
    ], capture_output=True, timeout=30)
    data = open(f'/tmp/test_tg2_{f}.ppm','rb').read().split(b'\n',3)[3]
    w = 320
    for line in range(100, 140):
        black = sum(1 for x in range(40,280)
                    if data[(line*w+x)*3:(line*w+x)*3+3] == b'\x00\x00\x00')
        if black > 50:
            bad += 1
            break
if bad > 0:
    print(f'{bad}/120 frames have black road bands')
    sys.exit(1)
sys.exit(0)
"
}

# --- Run tests ---
echo "=== Rendering regression tests ==="

if [ -f "build/Panorama Cotton (Japan).md" ]; then
    run_test "Panorama Cotton flying level HUD" test_cotton_hud
else
    skip_test "Panorama Cotton flying level HUD"
fi

if [ -f "build/Top Gear 2 (USA).md" ] && [ -f "$HOME/.genesis/saves/Top Gear 2 (USA).ss0" ]; then
    run_test "Top Gear 2 road (120 frames)" test_topgear2_road
else
    skip_test "Top Gear 2 road (120 frames)"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ]
