#!/bin/bash
# Regression tests for known-fixed rendering issues.
# Requires ROMs in build/ and save states in ~/.genesis/saves/.
# Run from the project root: bash tests/test_regressions.sh

set -e
cd "$(dirname "$0")/.."

PASS=0
FAIL=0
SKIP=0

# Regression suite is thread-safe: each test writes to its own distinct set
# of /tmp/test_*.ppm / *.log / *.txt files, and tests only read ROMs / save
# states (never mutate them). Default runs tests in parallel; set
# REGRESSION_JOBS=1 (or REGRESSION_SERIAL=1) to fall back to the old serial
# path for debugging a specific test.
REGRESSION_JOBS="${REGRESSION_JOBS:-auto}"
if [ -n "${REGRESSION_SERIAL:-}" ]; then
    REGRESSION_JOBS=1
fi

# Registry populated by register_test; dispatched by run_registered_tests.
REG_NAMES=()
REG_FUNCS=()
REG_STATES=()   # "run" or "skip"

register_test() {
    # Usage: register_test "<name>" <func> <state>
    #   state = "run"  -> run the test function
    #   state = "skip" -> emit SKIP at dispatch time
    REG_NAMES+=("$1")
    REG_FUNCS+=("$2")
    REG_STATES+=("$3")
}

# Serial fallback (used when REGRESSION_JOBS=1): runs each registered test
# in the main shell, in registration order.
run_registered_tests_serial() {
    local i
    for i in "${!REG_NAMES[@]}"; do
        local name="${REG_NAMES[$i]}"
        local func="${REG_FUNCS[$i]}"
        local state="${REG_STATES[$i]}"
        if [ "$state" = "skip" ]; then
            echo "  SKIP: $name (ROM or save state missing)"
            SKIP=$((SKIP + 1))
        else
            if "$func"; then
                echo "  PASS: $name"
                PASS=$((PASS + 1))
            else
                echo "  FAIL: $name"
                FAIL=$((FAIL + 1))
            fi
        fi
    done
}

# Parallel dispatcher: spawns every runnable test in a subshell, waits for
# all, then emits their captured output + verdict in registration order so
# the summary matches the serial run visually. Safe because every test
# writes to distinct /tmp/test_* paths and only reads shared ROMs / saves.
run_registered_tests_parallel() {
    local log_dir
    log_dir="$(mktemp -d /tmp/regtest.XXXXXX)"
    local pids=()
    local i
    for i in "${!REG_NAMES[@]}"; do
        local state="${REG_STATES[$i]}"
        if [ "$state" = "skip" ]; then
            echo "SKIP" > "$log_dir/$i.status"
            continue
        fi
        local func="${REG_FUNCS[$i]}"
        local out="$log_dir/$i.out"
        local status_file="$log_dir/$i.status"
        (
            # Subshell: isolate set -e so a test failing doesn't abort siblings.
            set +e
            "$func" > "$out" 2>&1
            local rc=$?
            if [ $rc -eq 0 ]; then
                echo "PASS" > "$status_file"
            else
                echo "FAIL" > "$status_file"
            fi
        ) &
        pids+=("$!")
    done
    if [ "${#pids[@]}" -gt 0 ]; then
        wait "${pids[@]}" 2>/dev/null || true
    fi
    for i in "${!REG_NAMES[@]}"; do
        local name="${REG_NAMES[$i]}"
        local status_file="$log_dir/$i.status"
        local out="$log_dir/$i.out"
        local status="FAIL"
        if [ -f "$status_file" ]; then
            status=$(cat "$status_file")
        fi
        if [ -f "$out" ]; then
            cat "$out"
        fi
        case "$status" in
            PASS)
                echo "  PASS: $name"
                PASS=$((PASS + 1))
                ;;
            FAIL)
                echo "  FAIL: $name"
                FAIL=$((FAIL + 1))
                ;;
            SKIP)
                echo "  SKIP: $name (ROM or save state missing)"
                SKIP=$((SKIP + 1))
                ;;
        esac
    done
    rm -rf "$log_dir"
}

run_registered_tests() {
    case "$REGRESSION_JOBS" in
        1|serial)
            run_registered_tests_serial
            ;;
        *)
            run_registered_tests_parallel
            ;;
    esac
}

# Backwards-compat shims for any external caller reaching into this file.
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

write_cotton_gameplay_held_start_input() {
    # Cotton's menus poll Start in VBlank, so one-frame synthetic pulses are
    # not a cross-emulator oracle. Hold Start for 10 hardware frames every
    # 30 hardware frames; the reference raw-frame equivalent is 20 raw frames
    # every 60 raw frames because that frame counter advances twice per
    # hardware frame on this path.
    local path="$1"
    python3 -c "
import sys
with open(sys.argv[1], 'w') as f:
    for frame in range(50, 80000, 30):
        for hold in range(10):
            f.write(f'{frame + hold}:S\n')
" "$path"
}

write_cotton_gameplay_then_pause_input() {
    # Same cold-boot gameplay route as write_cotton_gameplay_held_start_input,
    # but stop menu-advance pulses once the first-stage gameplay scene is
    # reached and send one final Start burst to pause the level. This locks the
    # "paused gameplay HUD stays stable" case reported during the bottom-HUD
    # flash investigation.
    local path="$1"
    python3 -c "
import sys
with open(sys.argv[1], 'w') as f:
    for frame in range(50, 21900, 30):
        for hold in range(10):
            f.write(f'{frame + hold}:S\n')
    for hold in range(20):
        f.write(f'{21990 + hold}:S\n')
" "$path"
}

write_f1_race_start_input() {
    # F1 reaches the race screen from cold boot with repeated held Start
    # bursts. Keep this deterministic and save-state-free so the PAL viewport
    # regression covers the real boot path.
    local path="$1"
    python3 -c "
import sys
with open(sys.argv[1], 'w') as f:
    for frame in range(30, 12000, 45):
        for hold in range(8):
            f.write(f'{frame + hold}:S\n')
" "$path"
}

write_clue_menu_start_input() {
    # Clue reaches the attract-room scene by frame 1500. Press Start on the
    # next frame to enter the game-level selection menu from cold boot.
    local path="$1"
    printf '1501:S\n' > "$path"
}

# --- Panorama Cotton (single run, three checks) ---
# Run once for 22000 frames, dump frame at 22000.
# Then check HUD, sky, and floor rendering from that one frame.
#
# Input script design: held Start bursts, not one-frame pulses. The matching
# Reference oracle for this scene is raw frame 44000 with 20 raw-frame Start
# bursts every 60 raw frames. Ours frame 22000 and reference raw 44000 both
# land on the first-stage bike/HUD scene; the reference has full playfield
# content at rows 23..57.
test_cotton() {
    local rom="build/Panorama Cotton (Japan).md"
    [ -f "$rom" ] || return 2

    write_cotton_gameplay_held_start_input /tmp/test_start_mash.txt

    ./build/cpz_trace "$rom" none 22000 22000 /tmp/test_cotton.ppm \
        --input-file /tmp/test_start_mash.txt 2>/dev/null

    python3 -c "
import sys
data = open('/tmp/test_cotton.ppm','rb').read().split(b'\n',3)
w = int(data[1].split()[0])
px = data[3]

# Check 1: HUD area (lines 210-220) must have multiple non-black colors
hud_colors = set()
for y in range(210, 221):
    for x in range(40, 280):
        off = (y * w + x) * 3
        c = (px[off], px[off+1], px[off+2])
        if c != (0,0,0):
            hud_colors.add(c)
if len(hud_colors) < 5:
    print(f'HUD FAIL: only {len(hud_colors)} non-black colors (need >= 5)')
    sys.exit(1)

# Check 2: Sky area (lines 23-95) must not be mostly black.
# The sky should have a colored gradient, not a black void.
sky_bad = 0
for y in range(23, 96):
    black = sum(1 for x in range(w)
                if px[(y*w+x)*3:(y*w+x)*3+3] == b'\x00\x00\x00')
    if black > w * 0.5:
        sky_bad += 1
if sky_bad > 30:
    print(f'SKY FAIL: {sky_bad}/73 lines are >50%% black')
    sys.exit(1)

# Check 3: Floor area (lines 130-180) must not be mostly black.
floor_bad = 0
for y in range(130, 181):
    black = sum(1 for x in range(w)
                if px[(y*w+x)*3:(y*w+x)*3+3] == b'\x00\x00\x00')
    if black > w * 0.9:
        floor_bad += 1
if floor_bad > 10:
    print(f'FLOOR FAIL: {floor_bad}/51 lines are >90%% black')
    sys.exit(1)

print(f'HUD: {len(hud_colors)} colors, sky: {73-sky_bad}/73 ok, floor: {51-floor_bad}/51 ok')
sys.exit(0)
"
}

# --- Top Gear 2 road flickering ---
# Run once for 200 frames, check output log for full-line flickers in the
# active road/playfield area. Rows 208-223 are the game's bottom letterbox band
# in this save-state path; the road artifact this guards was visible inside
# active display, not in the letterbox.
test_topgear2_road() {
    local rom="build/Top Gear 2 (USA).md"
    local ss="$HOME/.genesis/saves/Top Gear 2 (USA).ss0"
    [ -f "$rom" ] && [ -f "$ss" ] || return 2

    ./build/cpz_trace "$rom" "$ss" 200 0 /dev/null > /tmp/test_tg2.log 2>&1

    python3 -c "
import re, sys
# Match: diffPrev=N (N>0) with diffBox=Y..Y x 0..255 (single full-width line in H32)
# The diffPrev= field distinguishes actual frame diffs from diffPrev2= echo lines.
pattern = re.compile(r'diffPrev=([1-9]\d*).*diffBox=(\d+)\.\.(\d+) x 0\.\.255 diffPixels=256')
bad = 0
for line in open('/tmp/test_tg2.log'):
    m = pattern.search(line)
    if m and m.group(2) == m.group(3) and 8 <= int(m.group(2)) <= 207:
        bad += 1
if bad > 0:
    print(f'{bad} single-line flickers detected')
    sys.exit(1)
print('0 active-road flickers in 200 frames')
sys.exit(0)
"
}

# --- Top Gear 2 race screen bottom red-line flash ---
# The latest local race save-state intermittently flashes a full-width red
# line in the bottom letterbox band during the first ~150 frames. This is the
# actual visible artifact that the older exact-frame hash test was trying to
# guard indirectly, but the hash baseline is save-state/local-version brittle.
test_topgear2_race_bottom_red_flash() {
    local rom="build/Top Gear 2 (USA).md"
    local ss="$HOME/.genesis/saves/Top Gear 2 (USA).ss0"
    [ -f "$rom" ] && [ -f "$ss" ] || return 2

    CPZ_DUMP_FRAMES="7:/tmp/test_tg2_red_007.ppm,15:/tmp/test_tg2_red_015.ppm,17:/tmp/test_tg2_red_017.ppm,19:/tmp/test_tg2_red_019.ppm,21:/tmp/test_tg2_red_021.ppm,73:/tmp/test_tg2_red_073.ppm,75:/tmp/test_tg2_red_075.ppm,77:/tmp/test_tg2_red_077.ppm,79:/tmp/test_tg2_red_079.ppm,139:/tmp/test_tg2_red_139.ppm,141:/tmp/test_tg2_red_141.ppm" \
        ./build/cpz_trace "$rom" "$ss" 141 0 /dev/null > /tmp/test_tg2_red.log 2>&1

    python3 - <<'PY'
import sys

frames = [7, 15, 17, 19, 21, 73, 75, 77, 79, 139, 141]
bad = []
for frame in frames:
    chunks = open(f'/tmp/test_tg2_red_{frame:03d}.ppm', 'rb').read().split(b'\n', 3)
    w, h = map(int, chunks[1].split())
    px = chunks[3]
    for y in range(208, min(224, h)):
        red = 0
        for x in range(w):
            off = (y * w + x) * 3
            r, g, b = px[off], px[off + 1], px[off + 2]
            if r > 200 and g < 40 and b < 40:
                red += 1
        if red == w:
            bad.append((frame, y))

if bad:
    preview = ', '.join(f'frame {frame} row {row}' for frame, row in bad[:12])
    print(f'TG2 race bottom red-line flash: {preview}')
    sys.exit(1)

print('TG2 race: no full-width red flash in bottom letterbox band')
PY
}

# --- OutRun title screen black row at frame 400 ---
# Cold boot frame 400 is on the title screen. Row 24 should be part of the
# blue title backdrop, not broken into black segments by a stale full-screen
# H40 vscroll sample from the previous line.
test_outrun_title_row_24() {
    local rom="build/OutRun (USA, Europe).md"
    [ -f "$rom" ] || return 2

    ./build/cpz_trace "$rom" none 400 400 /tmp/test_outrun_400.ppm 2>/dev/null

    python3 - <<'PY'
import sys

data = open('/tmp/test_outrun_400.ppm', 'rb').read().split(b'\n', 3)
w = int(data[1].split()[0])
px = data[3]

row = 24
n_black = 0
for x in range(w):
    off = (row * w + x) * 3
    if (px[off], px[off + 1], px[off + 2]) == (0, 0, 0):
        n_black += 1

if n_black != 0:
    print(f'OutRun frame 400 row 24 has {n_black}/{w} black pixels - expected 0')
    sys.exit(1)

print('OutRun frame 400 row 24 has 0 black pixels')
PY
}

# --- OutRun title screen sky stripes on tile-last-row ---
# Cold boot frame 400 title screen. The sky (rows 0..30 above the logo/clouds)
# must render as a uniform blue backdrop (single color per row). On a buggy
# build the last pixel row of each 8-row background tile (rows 7, 15, 31, 39,
# 47) picks up tile data from the wrong source — sandy / cloud colors
# (109,109,109) and (255,219,182) — bleeding horizontal stripes across the
# sky. The reference capture shows all sky rows uniform solid blue.
test_outrun_title_sky_stripes() {
    local rom="build/OutRun (USA, Europe).md"
    [ -f "$rom" ] || return 2

    ./build/cpz_trace "$rom" none 400 400 /tmp/test_outrun_sky.ppm 2>/dev/null

    python3 - <<'PY'
import sys

data = open('/tmp/test_outrun_sky.ppm', 'rb').read().split(b'\n', 3)
w = int(data[1].split()[0])
px = data[3]

# Sky rows that should be uniform solid blue on the cold-boot title screen.
# Rows 7, 15, 31, 39, 47 are tile-last-row positions above the logo/clouds
# area and are the ones that develop horizontal stripes when the renderer
# mis-fetches vscroll / nametable data at tile boundaries.
bad = []
for row in [7, 15, 31, 39, 47]:
    colors = set()
    for x in range(w):
        off = (row * w + x) * 3
        colors.add((px[off], px[off + 1], px[off + 2]))
    if len(colors) > 1:
        bad.append((row, len(colors)))

if bad:
    for row, n in bad:
        print(f'OutRun frame 400 row {row}: {n} unique colors - expected 1 (uniform blue sky)')
    sys.exit(1)

print('OutRun frame 400 sky rows 7, 15, 31, 39, 47 all uniform')
PY
}

# --- F1 PAL race viewport bottom border ---
# F1 (Europe) is a PAL 224-line game presented inside a 240-line PAL output
# viewport. During the race it disables display after line 200; without the
# PAL output offset, the final active racing line sits at the bottom of the
# 224-line dump/window and appears as a garbage line. The cold-boot reference
# for this input path dumps 256x240, with the last dense race line at output
# row 216 and rows 217..239 black.
test_f1_race_pal_bottom_border() {
    local rom="build/F1 (Europe).md"
    [ -f "$rom" ] || return 2

    write_f1_race_start_input /tmp/test_f1_start_input.txt

    CPZ_DUMP_FRAMES="1310:/tmp/test_f1_race_f1310.ppm,1312:/tmp/test_f1_race_f1312.ppm,1398:/tmp/test_f1_race_f1398.ppm,1400:/tmp/test_f1_race_f1400.ppm,1416:/tmp/test_f1_race_f1416.ppm,1418:/tmp/test_f1_race_f1418.ppm,1479:/tmp/test_f1_race_f1479.ppm,1480:/tmp/test_f1_race_f1480.ppm,1490:/tmp/test_f1_race_f1490.ppm,1492:/tmp/test_f1_race_f1492.ppm,1497:/tmp/test_f1_race_f1497.ppm,1499:/tmp/test_f1_race_f1499.ppm,1500:/tmp/test_f1_race_f1500.ppm" \
        ./build/cpz_trace "$rom" none 1500 0 /dev/null \
        --input-file /tmp/test_f1_start_input.txt >/tmp/test_f1_race_trace.log 2>/dev/null

    python3 - <<'PY'
import sys

def load_frame(frame):
    data = open(f'/tmp/test_f1_race_f{frame}.ppm', 'rb').read().split(b'\n', 3)
    w, h = map(int, data[1].split())
    return w, h, data[3]

def nonblack(px, w, row):
    return sum(
        1 for x in range(w)
        if px[(row * w + x) * 3:(row * w + x) * 3 + 3] != b'\x00\x00\x00'
    )

w, h, px = load_frame(1500)

if (w, h) != (256, 240):
    print(f'F1 race PAL viewport is {w}x{h}, expected 256x240')
    sys.exit(1)

row216 = nonblack(px, w, 216)
if row216 < 200:
    print(f'F1 race row 216 has {row216}/{w} non-black pixels, expected dense race content')
    sys.exit(1)

row216_colors = set()
row216_greenish = 0
for x in range(w):
    off = (216 * w + x) * 3
    r, g, b = px[off], px[off + 1], px[off + 2]
    row216_colors.add((r, g, b))
    if g > 180 and r < 80:
        row216_greenish += 1

if len(row216_colors) > 12 or row216_greenish > 0:
    print(
        f'F1 race row 216 still contains bottom garbage: '
        f'{len(row216_colors)} colors, {row216_greenish} bright-green pixels'
    )
    sys.exit(1)

sampled_frames = [1310, 1312, 1398, 1400, 1416, 1418, 1479, 1480, 1490, 1492, 1497, 1499, 1500]
bad_bottom = []
for frame in sampled_frames:
    fw, fh, fpx = load_frame(frame)
    if (fw, fh) != (256, 240):
        print(f'F1 race frame {frame} viewport is {fw}x{fh}, expected 256x240')
        sys.exit(1)
    bad_bottom.extend(
        (frame, y, nonblack(fpx, fw, y))
        for y in range(217, 240)
        if nonblack(fpx, fw, y) != 0
    )
if bad_bottom:
    preview = ', '.join(f'frame {frame} row {y}: {n}' for frame, y, n in bad_bottom[:8])
    print(f'F1 race bottom border not black after row 216: {preview}')
    sys.exit(1)

print(
    f'F1 race PAL viewport: {w}x{h}, row216={row216}, '
    f'row216_colors={len(row216_colors)}, rows 217..239 black across {len(sampled_frames)} sampled frames'
)
PY
}

# --- Panorama Cotton boot pink line ---
# Frame 343 from cold boot shows the "縦天然色カラー" boot logo. Row 120 of
# this frame should NOT be a solid horizontal pink line.
# Regression for a vsram[1] latch timing race observed on the boot logo.
test_cotton_boot_pink_line() {
    local rom="build/Panorama Cotton (Japan).md"
    [ -f "$rom" ] || return 2

    ./build/cpz_trace "$rom" none 343 343 /tmp/test_cot_boot.ppm 2>/dev/null

    python3 -c "
import sys
data = open('/tmp/test_cot_boot.ppm','rb').read().split(b'\n',3)
w = int(data[1].split()[0])
px = data[3]

# Row 120 should have 0 pink pixels (RGB 146,0,36) — pink filler tiles
# leak through when vscrollLatch[1] holds the stale 0x7A0 value.
n_pink = 0
for x in range(w):
    off = (120 * w + x) * 3
    if (px[off], px[off+1], px[off+2]) == (146, 0, 36):
        n_pink += 1

if n_pink > 0:
    print(f'Row 120 has {n_pink}/{w} pink pixels (RGB 146,0,36) - expected 0')
    sys.exit(1)
print(f'Row 120 has {n_pink}/{w} pink pixels (clean)')
sys.exit(0)
"
}

# --- Panorama Cotton logging must be non-invasive ---
# The Cotton sequence logger is a debugging aid. Enabling it must not change
# the emulated framebuffer for the same ROM, input script, and target frame.
test_cotton_seq_logging_non_invasive() {
    local rom="build/Panorama Cotton (Japan).md"
    [ -f "$rom" ] || return 2

    local input="/tmp/test_cotton_seq_input.txt"
    python3 -c "
for f in range(1, 400):
    print(f'{f}:S')
" > "$input"

    ./build/cpz_trace "$rom" none 343 343 /tmp/test_cot_seq_off.ppm --input-file "$input" \
        > /tmp/test_cot_seq_off.log 2>/dev/null
    GENESIS_LOG_COTTON_SEQ=1 ./build/cpz_trace "$rom" none 343 343 /tmp/test_cot_seq_on.ppm --input-file "$input" \
        > /tmp/test_cot_seq_on.log 2>/dev/null

    python3 - <<'PY'
import re
import sys

def extract_hash(path: str) -> str:
    for line in open(path, 'r', encoding='utf-8', errors='replace'):
        m = re.search(r'^frame=343 hash=([0-9a-f]+)', line)
        if m:
            return m.group(1)
    raise SystemExit(f'could not find frame 343 hash in {path}')

off_hash = extract_hash('/tmp/test_cot_seq_off.log')
on_hash = extract_hash('/tmp/test_cot_seq_on.log')
if off_hash != on_hash:
    print(f'GENESIS_LOG_COTTON_SEQ changed frame 343 hash: off={off_hash} on={on_hash}')
    sys.exit(1)
print(f'GENESIS_LOG_COTTON_SEQ preserved frame 343 hash: {off_hash}')
PY
}

# --- Panorama Cotton intro pink stripe sweep ---
# Sweep frames 1-1000 of cold-boot intro looking for the bug pattern:
# horizontal pink stripes (RGB 146,0,36) of width >=16 pixels on a single
# row. The bug pattern is "vsB latch race" — the H-int handler writes a
# new vsB value to swap plane regions but our cycle-0 latch captures the
# stale filler-region value, leaking pink filler tiles across the row.
#
# Cotton's character sprite contains a few pink pixels (146,0,36), but
# they're scattered across many rows of the sprite, never forming long
# horizontal runs. A run >=16 pixels in any single row indicates the
# vsB latch race bug.
test_cotton_intro_pink_stripes() {
    local rom="build/Panorama Cotton (Japan).md"
    [ -f "$rom" ] || return 2

    # Single run to frame 467, dumping 6 sample frames via CPZ_DUMP_FRAMES
    CPZ_DUMP_FRAMES="200:/tmp/test_intro_200.ppm,317:/tmp/test_intro_317.ppm,343:/tmp/test_intro_343.ppm,348:/tmp/test_intro_348.ppm,400:/tmp/test_intro_400.ppm,467:/tmp/test_intro_467.ppm" \
        ./build/cpz_trace "$rom" none 467 0 /dev/null 2>/dev/null

    local fail_count=0
    local fail_frames=""
    for f in 200 317 343 348 400 467; do
        python3 -c "
import sys
data = open('/tmp/test_intro_$f.ppm','rb').read().split(b'\n',3)
w = int(data[1].split()[0])
h = int(data[1].split()[1])
px = data[3]
# Find rows with a pink run >=16 pixels (the bug pattern)
PINK = (146, 0, 36)
worst_run = 0
worst_row = -1
for y in range(h):
    cur_run = 0
    max_run = 0
    for x in range(w):
        off = (y * w + x) * 3
        if (px[off], px[off+1], px[off+2]) == PINK:
            cur_run += 1
            if cur_run > max_run:
                max_run = cur_run
        else:
            cur_run = 0
    if max_run > worst_run:
        worst_run = max_run
        worst_row = y
sys.exit(1 if worst_run >= 16 else 0)
"
        if [ $? -ne 0 ]; then
            fail_count=$((fail_count + 1))
            fail_frames="$fail_frames $f"
        fi
    done
    if [ $fail_count -gt 0 ]; then
        echo "Found pink stripes (>=16 px) on frames:$fail_frames"
        return 1
    fi
    echo "0 frames with pink stripes (>=16 px) across sampled boot intro"
    return 0
}

# --- Panorama Cotton boot pink line, frame 348 ---
# Frame 348 from cold boot is the same boot logo screen as frame 343 with
# a slightly later vsram state. The slot-42 vsB-only latch with renderedPixels
# reset must scrub the leftmost pixels of row 120 (columns 0-2) too, not just
# pixels 27+. Without the reset, columns 0-2 still render with the stale
# cycle-0 vsB and leak 27 pink pixels at the left edge.
test_cotton_boot_pink_line_348() {
    local rom="build/Panorama Cotton (Japan).md"
    [ -f "$rom" ] || return 2

    ./build/cpz_trace "$rom" none 348 348 /tmp/test_cot_boot348.ppm 2>/dev/null

    python3 -c "
import sys
data = open('/tmp/test_cot_boot348.ppm','rb').read().split(b'\n',3)
w = int(data[1].split()[0])
h = int(data[1].split()[1])
px = data[3]

# Frame 348 must be entirely free of pink filler pixels (RGB 146,0,36)
# in the boot logo row range (115-125). A non-zero count indicates the
# vscrollLatch reset on dramatic vsB delta is broken.
n_pink = 0
for y in range(115, 126):
    for x in range(w):
        off = (y * w + x) * 3
        if (px[off], px[off+1], px[off+2]) == (146, 0, 36):
            n_pink += 1

if n_pink > 0:
    print(f'Boot rows 115-125 have {n_pink} pink pixels - expected 0')
    sys.exit(1)
print(f'Boot rows 115-125: 0 pink pixels (clean)')
sys.exit(0)
"
}

# --- Top Gear 2 race screen left edge black columns ---
# TG2's race screen H-int handler runs a "carry-over" VSRAM write at line
# N+1 cycles 33/73 (the previous line's H-int IRQ was delivered late and
# the FIFO drains in the next line). The cycle-0 vscrollLatch on those
# lines captured the wrong value, leaving column 0 (8 pixels at the left
# edge) rendered with the stale plane B vscroll. Specifically, save state
# rows 9, 116, 118, 207 were entirely black at columns 0-7 while the rest
# of the row had content.
test_topgear2_race_left_edge() {
    local rom="build/Top Gear 2 (USA).md"
    local ss="$HOME/.genesis/saves/Top Gear 2 (USA).ss0"
    [ -f "$rom" ] && [ -f "$ss" ] || return 2

    ./build/cpz_trace "$rom" "$ss" 5 5 /tmp/test_tg2_left.ppm 2>/dev/null

    python3 -c "
import sys
data = open('/tmp/test_tg2_left.ppm','rb').read().split(b'\n',3)
w = int(data[1].split()[0])
h = int(data[1].split()[1])
px = data[3]

# Find rows where columns 0-7 are entirely black but the row has content
# elsewhere. This is the leftover vsB latch race bug pattern.
bad_rows = []
for y in range(h):
    left_black = all((px[(y*w+x)*3], px[(y*w+x)*3+1], px[(y*w+x)*3+2]) == (0,0,0)
                     for x in range(8))
    if not left_black:
        continue
    right_nonblack = sum(1 for x in range(8, w)
                         if (px[(y*w+x)*3], px[(y*w+x)*3+1], px[(y*w+x)*3+2]) != (0,0,0))
    if right_nonblack > 100:
        bad_rows.append(y)

if bad_rows:
    print(f'TG2 race: rows with all-black col 0-7 + content elsewhere: {bad_rows}')
    sys.exit(1)
print('TG2 race: 0 left-edge black columns')
sys.exit(0)
"
}

# --- Panorama Cotton HUD/play-field boundary flicker ---
# Cotton's flying gameplay had a per-frame flicker at row 184 (boundary
# between the play field bottom and HUD top). The cycle-0 vscrollLatch on
# line 184 sometimes captured vsram[1]=0x0760 (line 183's H-int write,
# drain on time) and sometimes 0x0790 (line 182's value, when line 183's
# drain was delayed past line 184 cycle 0). Frame-to-frame timing variance
# alternated between the two states, producing a 1-pixel flickering bar.
#
# The fix is to drain pending VSRAM FIFO entries in beginScanlineCommon
# BEFORE the cycle-0 latch fires, ensuring vsram is up-to-date regardless
# of M68K timing variance.
#
# This test runs the cold-boot intro to gameplay and samples 5 consecutive
# frames at row 184. If row 184 differs across frames, that's the flicker.
test_cotton_hud_boundary_flicker() {
    local rom="build/Panorama Cotton (Japan).md"
    [ -f "$rom" ] || return 2

    python3 -c "
for f in range(100, 32100, 10):
    print(f'{f}:S')
    print(f'{f+3}:')
" > /tmp/test_cot_flick_mash.txt

    # Single run to frame 32000, dumping 5 consecutive frames via CPZ_DUMP_FRAMES
    CPZ_DUMP_FRAMES="31996:/tmp/test_cot_flick_31996.ppm,31997:/tmp/test_cot_flick_31997.ppm,31998:/tmp/test_cot_flick_31998.ppm,31999:/tmp/test_cot_flick_31999.ppm,32000:/tmp/test_cot_flick_32000.ppm" \
        ./build/cpz_trace "$rom" none 32000 0 /dev/null \
            --input-file /tmp/test_cot_flick_mash.txt 2>/dev/null

    python3 -c "
import sys
# Check the HUD boundary row for frame-to-frame flicker. Row 183 is playfield
# content and may legitimately animate after CPU timing corrections shift the
# sampled gameplay phase.
bad = []
for row in [184]:
    sigs = []
    for f in [31996, 31997, 31998, 31999, 32000]:
        data = open(f'/tmp/test_cot_flick_{f}.ppm','rb').read().split(b'\n',3)
        w = int(data[1].split()[0])
        px = data[3]
        sigs.append(bytes(px[row*w*3:(row+1)*w*3]))
    unique = len(set(sigs))
    if unique > 1:
        bad.append((row, unique))
if bad:
    for row, n in bad:
        print(f'Row {row} flicker: {n} unique signatures across 5 frames')
    sys.exit(1)
print('Row 184: stable across 5 frames')
sys.exit(0)
"
}

# --- Panorama Cotton bottom HUD full-band flash ---
# The first-stage bike/HUD gameplay scene can intermittently lose the entire
# bottom status HUD for a frame, replacing rows 184-223 with playfield
# background. This is distinct from the older row-184 boundary flicker: the
# whole status panel disappears on frames such as 22043/22067 while adjacent
# frames render the HUD normally.
test_cotton_bottom_hud_flash() {
    local rom="build/Panorama Cotton (Japan).md"
    [ -f "$rom" ] || return 2

    write_cotton_gameplay_held_start_input /tmp/test_cot_hud_flash_input.txt

    local dump_spec
    dump_spec="$(python3 - <<'PY'
frames = [22013] + list(range(22018, 22025)) + [22031, 22043, 22067]
print(','.join(f'{frame}:/tmp/test_cot_hud_flash_{frame}.ppm' for frame in frames))
PY
)"

    CPZ_DUMP_FRAMES="$dump_spec" \
        ./build/cpz_trace "$rom" none 22067 0 /dev/null \
            --input-file /tmp/test_cot_hud_flash_input.txt 2>/dev/null

    python3 - <<'PY'
import sys

bad = []
for f in [22013] + list(range(22018, 22025)) + [22031, 22043, 22067]:
    data = open(f'/tmp/test_cot_hud_flash_{f}.ppm','rb').read().split(b'\n',3)
    w = int(data[1].split()[0])
    px = data[3]

    # The bottom status panel uses black/dark tile pixels throughout rows
    # 184..223. The flash bug replaces this whole band with playfield
    # background, leaving zero dark pixels. Keep the threshold low enough to
    # allow normal HUD animation, but high enough to reject the missing-panel
    # state.
    dark = 0
    for y in range(184, 224):
        for x in range(w):
            off = (y * w + x) * 3
            if px[off] < 40 and px[off+1] < 40 and px[off+2] < 40:
                dark += 1
    if dark < 1000:
        bad.append((f, dark))

if bad:
    preview = ', '.join(f'frame {f}: {dark} dark HUD pixels' for f, dark in bad)
    print(f'Cotton bottom HUD flash: missing status panel in {preview}')
    sys.exit(1)

print('Cotton bottom HUD: status panel retained across frames 22018..22024')
sys.exit(0)
PY
}

# --- Panorama Cotton paused gameplay HUD stability ---
# User-observed reduced case for the bottom-HUD flash investigation: enter the
# first-stage bike/HUD scene, pause it, and verify that the static paused screen
# does not flicker or lose the bottom status panel.
test_cotton_paused_bottom_hud_stable() {
    local rom="build/Panorama Cotton (Japan).md"
    [ -f "$rom" ] || return 2

    write_cotton_gameplay_then_pause_input /tmp/test_cot_pause_input.txt

    CPZ_DUMP_FRAMES="21995:/tmp/test_cot_pause_21995.ppm,22000:/tmp/test_cot_pause_22000.ppm,22005:/tmp/test_cot_pause_22005.ppm,22023:/tmp/test_cot_pause_22023.ppm" \
        ./build/cpz_trace "$rom" none 22023 0 /dev/null \
            --input-file /tmp/test_cot_pause_input.txt 2>/dev/null

    python3 - <<'PY'
import sys

frames = [21995, 22000, 22005, 22023]
images = []
for f in frames:
    chunks = open(f'/tmp/test_cot_pause_{f}.ppm', 'rb').read().split(b'\n', 3)
    w = int(chunks[1].split()[0])
    h = int(chunks[1].split()[1])
    images.append((f, w, h, chunks[3]))

first_frame, first_w, first_h, first_px = images[0]
unstable = []
for f, w, h, px in images[1:]:
    if w != first_w or h != first_h or px != first_px:
        unstable.append(f)

if unstable:
    print(f'Cotton paused HUD changed after frame {first_frame}: {unstable}')
    sys.exit(1)

dark = 0
red_pause_pixels = 0
for y in range(184, 224):
    for x in range(first_w):
        off = (y * first_w + x) * 3
        r, g, b = first_px[off], first_px[off + 1], first_px[off + 2]
        if r < 40 and g < 40 and b < 40:
            dark += 1
        if y >= 200 and r > 150 and g < 80 and b < 80:
            red_pause_pixels += 1

if dark < 1000:
    print(f'Cotton paused HUD missing dark status panel: {dark} dark pixels')
    sys.exit(1)
if red_pause_pixels < 200:
    print(f'Cotton paused scene did not reach PAUSE HUD: {red_pause_pixels} red pause-panel pixels')
    sys.exit(1)

print(f'Cotton paused HUD stable across {len(frames)} frames; dark={dark}, pause-red={red_pause_pixels}')
sys.exit(0)
PY
}

# --- Top Gear 2 race screen black lines ---
# A buggy vscroll latch fix can introduce single all-black rows in the
# active display area at the horizon transition (typically rows 100-115).
# Active display in TG2 is rows 8-207 (rows 0-7 and 208-223 are letterbox).
# This test catches single-line all-black artifacts that the existing
# test_topgear2_road check (which only catches single-frame flickers)
# misses.
test_topgear2_race_black_lines() {
    local rom="build/Top Gear 2 (USA).md"
    local ss="$HOME/.genesis/saves/Top Gear 2 (USA).ss0"
    [ -f "$rom" ] && [ -f "$ss" ] || return 2

    ./build/cpz_trace "$rom" "$ss" 5 5 /tmp/test_tg2_race.ppm 2>/dev/null

    python3 -c "
import sys
data = open('/tmp/test_tg2_race.ppm','rb').read().split(b'\n',3)
w = int(data[1].split()[0])
h = int(data[1].split()[1])
px = data[3]

# Active display rows 8-207 must not be >95% black (TG2 has 200-line
# active area centered in 224, so rows 0-7 and 208-223 are intentional
# letterbox).
bad_rows = []
for y in range(8, 208):
    n_black = sum(1 for x in range(w)
                  if px[(y*w+x)*3:(y*w+x)*3+3] == b'\x00\x00\x00')
    if n_black > w * 0.95:
        bad_rows.append(y)

if bad_rows:
    print(f'TG2 race: all-black rows in active area: {bad_rows}')
    sys.exit(1)
print(f'TG2 race: 0 black-line artifacts in active area')
sys.exit(0)
"
}

# --- Panorama Cotton gameplay black sky bands (cold-boot integration) ---
# Cold-boot Panorama Cotton with held Start bursts to reach the first-stage
# bike/HUD scene. This is the scene the player described: Cotton on the bike,
# top status HUD, bottom EXP/SPEED/MAGIC HUD, and the desert road playfield.
#
# Oracle scene:
#   ours:    frame 22000, input from write_cotton_gameplay_held_start_input
#   reference: raw frame 44000, Start held for 20 raw frames every 60 raw frames
#
# Verified 2026-04-24: reference raw 44000 has no sparse top-playfield rows
# in rows 23..57; earlier builds had rows 23..57 mostly black because the
# visible scroll state was shifted by one line.
test_cotton_play_black_lines() {
    local rom="build/Panorama Cotton (Japan).md"
    [ -f "$rom" ] || return 2

    write_cotton_gameplay_held_start_input /tmp/test_cot_play_input.txt

    ./build/cpz_trace "$rom" none 22000 22000 /tmp/test_cot_play.ppm \
        --input-file /tmp/test_cot_play_input.txt 2>/dev/null

    python3 -c "
import sys
data = open('/tmp/test_cot_play.ppm','rb').read().split(b'\n',3)
w = int(data[1].split()[0])
px = data[3]

# Top playfield invariant: rows 23-57 are below the top HUD and should be
# dense sky/playfield content in the first-stage scene. Reference raw 44000
# has at least 269/320 non-black pixels on every row in this band. Earlier
# builds had only 0..25 non-black pixels on these rows.
top_sparse_rows = []
for y in range(23, 58):
    n_nb = sum(1 for x in range(w)
               if px[(y*w+x)*3:(y*w+x)*3+3] != b'\x00\x00\x00')
    if n_nb < w * 0.25:
        top_sparse_rows.append((y, n_nb))

pure_black_rows = 0
active_colors = set()
for y in range(23, 200):
    n_nb = sum(1 for x in range(w)
               if px[(y*w+x)*3:(y*w+x)*3+3] != b'\x00\x00\x00')
    for x in range(w):
        c = tuple(px[(y*w+x)*3:(y*w+x)*3+3])
        active_colors.add(c)
    if n_nb == 0:
        pure_black_rows += 1
if top_sparse_rows:
    preview = ', '.join(f'row {y}: {n}/320 non-black'
                        for y, n in top_sparse_rows[:12])
    print(f'Cotton gameplay top playfield too black: '
          f'{len(top_sparse_rows)}/35 sparse rows; {preview}')
    sys.exit(1)
if pure_black_rows > 10:
    print(f'Cotton gameplay sky broken: {pure_black_rows} pure-black rows '
          f'in 23..199 (expected <= 10; correct scene has <2)')
    sys.exit(1)
if len(active_colors) < 20:
    print(f'Cotton gameplay playfield too low-detail: {len(active_colors)} '
          f'unique active colors (expected >=20; reference has 30)')
    sys.exit(1)

# Floor invariant: rows 196-220 should be dense.
floor_bad = 0
for y in range(196, 221):
    n_nb = sum(1 for x in range(w)
               if px[(y*w+x)*3:(y*w+x)*3+3] != b'\x00\x00\x00')
    if n_nb < w * 0.5:
        floor_bad += 1
if floor_bad > 5:
    print(f'Floor region (rows 196-220) has {floor_bad}/25 rows >50% black')
    sys.exit(1)

print(f'Cotton play (frame 22000): {pure_black_rows} pure-black rows, '
      f'{len(active_colors)} active colors, top playfield dense, floor ok')
sys.exit(0)
"
}

# --- Panorama Cotton gameplay scroll-state oracle (cold-boot integration) ---
# Same held-Start path as the visual Cotton play smoke test, but this asserts
# VDP-visible scroll state before framebuffer composition. Reference raw
# frame 44000 reports the expected rows below for the first-stage bike/HUD
# scene. Earlier builds were shifted by one line at the top playfield boundary.
test_cotton_play_scroll_state_oracle() {
    local rom="build/Panorama Cotton (Japan).md"
    [ -f "$rom" ] || return 2

    write_cotton_gameplay_held_start_input /tmp/test_cot_play_scroll_input.txt

    GENESIS_LOG_SCROLL=1 \
    GENESIS_LOG_SCROLL_FRAME_FIRST=22000 \
    GENESIS_LOG_SCROLL_FRAME_LAST=22000 \
    GENESIS_LOG_SCROLL_LINE_FIRST=23 \
    GENESIS_LOG_SCROLL_LINE_LAST=60 \
    ./build/cpz_trace "$rom" none 22000 22000 /tmp/test_cot_play_scroll.ppm \
        --input-file /tmp/test_cot_play_scroll_input.txt \
        >/tmp/test_cot_play_scroll.out 2>/tmp/test_cot_play_scroll.err

    python3 - <<'PY'
import re
import sys

path = '/tmp/test_cot_play_scroll.err'
scroll = {}
pat = re.compile(r'\[SCROLL\] ln=(\d+) vsA=(-?\d+) vsB=(-?\d+)')
for line in open(path):
    m = pat.search(line)
    if not m:
        continue
    ln = int(m.group(1))
    scroll.setdefault(ln, (int(m.group(2)), int(m.group(3))))

expected = {23: (4, 0)}
for ln in range(24, 59):
    expected[ln] = (72 - ln, 71 - ln)
expected[59] = (125, 12)
expected[60] = (125, 11)

expected_lines = range(23, 61)
missing = [ln for ln in expected_lines if ln not in scroll]
if missing:
    print(f'Cotton scroll-state oracle missing SCROLL rows: {missing[:12]}')
    sys.exit(1)

bad = [(ln, scroll[ln][0], scroll[ln][1])
       for ln in expected_lines
       if scroll[ln] != expected[ln]]
if bad:
    preview = ', '.join(
        f'ln {ln}: got vsA={a} vsB={b}, '
        f'expected {expected[ln][0]}/{expected[ln][1]}'
        for ln, a, b in bad[:12])
    print(f'Cotton play scroll-state mismatch: {len(bad)} rows differ; {preview}')
    sys.exit(1)

print('Cotton play scroll-state oracle: rows 23..60 match reference raw 44000')
PY
}

# --- Clue game-level menu background ---
# The game-level menu should draw an opaque black panel behind the selectable
# text. The bad frame leaks stale/generated background tiles through the panel,
# especially in the central rectangle around rows 78..166.
test_clue_menu_background_clean() {
    local rom="build/Clue (USA).md"
    [ -f "$rom" ] || return 2

    write_clue_menu_start_input /tmp/test_clue_menu_input.txt

    ./build/cpz_trace "$rom" none 1600 1600 /tmp/test_clue_menu.ppm \
        --input-file /tmp/test_clue_menu_input.txt >/dev/null 2>/dev/null

    python3 - <<'PY'
import sys

chunks = open('/tmp/test_clue_menu.ppm', 'rb').read().split(b'\n', 3)
w = int(chunks[1].split()[0])
h = int(chunks[1].split()[1])
px = chunks[3]

if (w, h) != (256, 224):
    print(f'Clue menu viewport is {w}x{h}, expected 256x224')
    sys.exit(1)

# Rect covers the menu panel but avoids most surrounding board-room tiles.
x0, y0, x1, y1 = 56, 78, 199, 166
bad = 0
for y in range(y0, y1 + 1):
    for x in range(x0, x1 + 1):
        off = (y * w + x) * 3
        r, g, b = px[off], px[off + 1], px[off + 2]
        # The correct panel has black plus yellow/orange text. Green/blue/red
        # stale tiles in this rectangle indicate the menu background leak.
        if max(r, g, b) > 40 and not (r >= g >= b and r > 80):
            bad += 1

if bad > 100:
    print(f'Clue menu background has {bad} colored garbage pixels (expected <=100)')
    sys.exit(1)

print(f'Clue menu background clean: {bad} colored garbage pixels')
PY
}

# --- Darius title boot ---
# Darius uses the Z80 reset/BUSREQ handshake during startup. If BUSACK status
# treats reset as a granted bus, the game stays black before the SEGA logo.
test_darius_reaches_title() {
    local rom="build/Darius (World) (Genesis Mini, Mega Drive Mini).md"
    [ -f "$rom" ] || return 2

    ./build/cpz_trace "$rom" none 300 300 /tmp/test_darius_title.ppm >/dev/null 2>/dev/null

    python3 - <<'PY'
import sys

chunks = open('/tmp/test_darius_title.ppm', 'rb').read().split(b'\n', 3)
w = int(chunks[1].split()[0])
h = int(chunks[1].split()[1])
px = chunks[3]

if (w, h) != (320, 224):
    print(f'Darius title viewport is {w}x{h}, expected 320x224')
    sys.exit(1)

nonblack = 0
red = 0
for i in range(0, len(px), 3):
    r, g, b = px[i], px[i + 1], px[i + 2]
    if (r, g, b) != (0, 0, 0):
        nonblack += 1
    if r > 140 and g < 80 and b < 80:
        red += 1

if nonblack < 15000 or red < 300:
    print(f'Darius did not reach title frame: nonblack={nonblack}, red={red}')
    sys.exit(1)

def pixel(x, y):
    off = (y * w + x) * 3
    return px[off], px[off + 1], px[off + 2]

def dark(rgb):
    return sum(rgb) < 25

def bright(rgb):
    return sum(rgb) > 120

bad_dash_runs = 0
for y in range(50, 135):
    x = 16
    while x < 304:
        if not dark(pixel(x, y)):
            x += 1
            continue
        start = x
        while x < 304 and dark(pixel(x, y)):
            x += 1
        end = x - 1
        length = end - start + 1
        if 3 <= length <= 6 and start > 16 and end < 303:
            if not dark(pixel(start - 1, y)) and not dark(pixel(end + 1, y)):
                surrounded = 0
                for xx in range(start, end + 1):
                    if bright(pixel(xx, y - 1)) and bright(pixel(xx, y + 1)):
                        surrounded += 1
                if surrounded >= min(3, length):
                    bad_dash_runs += 1

if bad_dash_runs > 2:
    print(f'Darius title logo has FIFO-drop dash artifacts: runs={bad_dash_runs}')
    sys.exit(1)

print(f'Darius title reached: nonblack={nonblack}, red={red}, bad_dash_runs={bad_dash_runs}')
PY
}

# --- Flashback intro audio cadence ---
# Flashback's Z80 sound driver advances its music on the raw VBlank pulse.
# The pulse must occur once per frame even when the 68K-facing V-int path is
# masked or acknowledged; otherwise the early YM register batches drift late
# and the cutscene music tempo drags.
test_flashback_intro_audio_cadence() {
    local rom="build/Flashback - The Quest for Identity (USA) (En,Fr).md"
    [ -f "$rom" ] || return 2

    GENESIS_LOG_Z80_AUDIO=1 GENESIS_LOG_YM_WRITES=1 \
        ./build/cpz_trace "$rom" none 1200 1200 /tmp/test_flashback_intro.ppm \
        >/tmp/test_flashback_intro_audio.log 2>&1

    python3 - <<'PY'
import re
import sys

z80_rows = 0
z80_pulses = 0
ym_data_total = 0
ym_data_frames = []

for line in open('/tmp/test_flashback_intro_audio.log', 'r', encoding='utf-8', errors='replace'):
    if not line.startswith('[Z80-AUDIO]'):
        continue
    z80_rows += 1
    values = {name: int(value) for name, value in re.findall(r'(\w+)=(-?\d+)', line)}
    frame = values.get('frame')
    pulses = values.get('z80IntPulses', 0)
    data_writes = values.get('ymDataWrites', 0)
    z80_pulses += pulses
    ym_data_total += data_writes
    if data_writes:
        ym_data_frames.append((frame, data_writes))

if z80_rows != 1200:
    print(f'Flashback trace has {z80_rows} Z80 audio rows, expected 1200')
    sys.exit(1)

if z80_pulses != 1200:
    print(f'Flashback trace has {z80_pulses} Z80 V-int pulses, expected 1200')
    sys.exit(1)

if len(ym_data_frames) < 3:
    print(f'Flashback trace has only {len(ym_data_frames)} YM data-write frames')
    sys.exit(1)

expected_windows = [(320, 345), (540, 565), (625, 650)]
for idx, (lo, hi) in enumerate(expected_windows):
    frame, writes = ym_data_frames[idx]
    if not (lo <= frame <= hi):
        print(
            f'Flashback YM batch {idx + 1} landed at frame {frame} '
            f'({writes} data writes), expected frame {lo}..{hi}'
        )
        sys.exit(1)

print(
    f'Flashback intro audio cadence: pulses={z80_pulses}, '
    f'ymDataWrites={ym_data_total}, firstBatches={ym_data_frames[:3]}'
)
PY
}

# --- Register tests ---
echo "=== Rendering regression tests ==="

have_cotton_rom="no"
[ -f "build/Panorama Cotton (Japan).md" ] && have_cotton_rom="yes"
have_cotton_save="no"
[ -f "$HOME/.genesis/saves/Panorama Cotton (Japan).ss0" ] && have_cotton_save="yes"
have_tg2="no"
[ -f "build/Top Gear 2 (USA).md" ] && [ -f "$HOME/.genesis/saves/Top Gear 2 (USA).ss0" ] && have_tg2="yes"
have_outrun="no"
[ -f "build/OutRun (USA, Europe).md" ] && have_outrun="yes"
have_f1="no"
[ -f "build/F1 (Europe).md" ] && have_f1="yes"
have_clue="no"
[ -f "build/Clue (USA).md" ] && have_clue="yes"
have_darius="no"
[ -f "build/Darius (World) (Genesis Mini, Mega Drive Mini).md" ] && have_darius="yes"
have_flashback="no"
[ -f "build/Flashback - The Quest for Identity (USA) (En,Fr).md" ] && have_flashback="yes"

reg() {
    # Usage: reg "<name>" <func> <has-prereq:yes|no>
    local state="run"
    [ "$3" = "yes" ] || state="skip"
    register_test "$1" "$2" "$state"
}

# Gameplay regression coverage for HUD and floor rendering invariants.
reg "Panorama Cotton (HUD + floor rendering)" test_cotton "$have_cotton_rom"
reg "Panorama Cotton (boot pink line, frame 343)" test_cotton_boot_pink_line "$have_cotton_rom"
reg "Panorama Cotton (Cotton seq logging is non-invasive)" test_cotton_seq_logging_non_invasive "$have_cotton_rom"
reg "Panorama Cotton (boot pink line, frame 348)" test_cotton_boot_pink_line_348 "$have_cotton_rom"
reg "Panorama Cotton (intro pink stripe sweep)" test_cotton_intro_pink_stripes "$have_cotton_rom"
reg "Panorama Cotton (HUD/play-field boundary flicker)" test_cotton_hud_boundary_flicker "$have_cotton_rom"
reg "Panorama Cotton (bottom HUD does not flash)" test_cotton_bottom_hud_flash "$have_cotton_rom"
reg "Panorama Cotton (paused bottom HUD stable)" test_cotton_paused_bottom_hud_stable "$have_cotton_rom"
reg "Top Gear 2 road (120 frames)" test_topgear2_road "$have_tg2"
reg "Top Gear 2 race (no bottom red-line flash)" test_topgear2_race_bottom_red_flash "$have_tg2"
reg "Top Gear 2 race (no black-line artifacts)" test_topgear2_race_black_lines "$have_tg2"
reg "Top Gear 2 race (left edge col 0-7 not black)" test_topgear2_race_left_edge "$have_tg2"
reg "OutRun title (frame 400 row 24 not black)" test_outrun_title_row_24 "$have_outrun"
reg "OutRun title (sky rows uniform, no tile-edge stripes)" test_outrun_title_sky_stripes "$have_outrun"
reg "F1 race (PAL viewport bottom border)" test_f1_race_pal_bottom_border "$have_f1"
reg "Clue menu (game-level panel background clean)" test_clue_menu_background_clean "$have_clue"
reg "Darius title (Z80 reset/BUSACK startup)" test_darius_reaches_title "$have_darius"
reg "Flashback intro (Z80 audio cadence)" test_flashback_intro_audio_cadence "$have_flashback"
reg "Panorama Cotton play (scroll-state oracle, bike/HUD)" test_cotton_play_scroll_state_oracle "$have_cotton_rom"
reg "Panorama Cotton play (bike/HUD top playfield not black)" test_cotton_play_black_lines "$have_cotton_rom"

run_registered_tests

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ]
