# Genesis Emulator

## Build

```bash
make -j7 -C build
```

## Run

```bash
./build/genesis "build/ROM Name.md"
```

Save states auto-load from `~/.genesis/saves/`. Press F5 to save state.

## GPGX Comparison Testing

We use Genesis-Plus-GX (GPGX) as the reference emulator for debugging rendering and timing issues.

### Setup (one-time)

```bash
# Build GPGX SDL2 (required before other GPGX tools)
cd third_party/Genesis-Plus-GX/sdl && make -f Makefile.sdl2

# Build headless frame dumper
cd third_party/Genesis-Plus-GX/sdl && ./build_headless.sh

# Build VDP state comparison tool
cd third_party/Genesis-Plus-GX/sdl && ./build_vdp_compare.sh
```

### GPGX headless frame dumper

Runs a ROM for N frames from cold boot, dumps the framebuffer as PPM.

```bash
./third_party/Genesis-Plus-GX/sdl/headless_dump <rom> <frames> <output.ppm> [--input-file <path>] [frame:buttons ...]
```

Button codes: `U`=Up `D`=Down `L`=Left `R`=Right `A` `B` `C` `S`=Start

Supports `--input-file <path>` for long input sequences (one `frame:buttons` per line).

### cpz_trace (our headless tracer)

```bash
./build/cpz_trace <rom> <state|none> [frames] [dump_frame dump_path] [--input-file <path>] [frame:buttons ...]
```

- Use `none` as state path for cold-boot (no save state)
- Supports `--input-file <path>` for long input sequences
- Button codes: `U`=Up `D`=Down `L`=Left `R`=Right `A` `B` `C` `S`=Start

### VDP state cross-renderer comparison

Definitively test if a rendering difference is a rendering bug or VDP state divergence:

```bash
# 1. Dump VDP state from our save state
./build/vdp_state_dump "build/ROM.md" ~/.genesis/saves/ROM.ss0 /tmp/vdp.bin /tmp/ours.ppm

# 2. Render same VDP state with GPGX
./third_party/Genesis-Plus-GX/sdl/vdp_compare "build/ROM.md" /tmp/vdp.bin /tmp/gpgx.ppm

# 3. Compare pixel-by-pixel
python3 -c "
a=open('/tmp/ours.ppm','rb').read(); b=open('/tmp/gpgx.ppm','rb').read()
ha=a.split(b'\n',3); hb=b.split(b'\n',3)
da,db=ha[3],hb[3]; w=int(ha[1].split()[0]); h=int(ha[1].split()[1])
d=sum(1 for i in range(0,len(da),3) if da[i:i+3]!=db[i:i+3])
print(f'{d}/{w*h} pixels differ ({100*d/(w*h):.1f}%)')
"
```

If 0% differ → rendering is correct, issue is VDP state divergence (CPU timing).
If >0% differ → rendering code bug.

### Per-line scroll logging

Compare VSRAM/HSCROLL values between emulators per scanline:

```bash
# Our emulator
GENESIS_LOG_SCROLL=1 ./build/cpz_trace rom state 1 2>/tmp/ours_scroll.log

# GPGX
GPGX_LOG_SCROLL=1 ./third_party/Genesis-Plus-GX/sdl/headless_dump rom 1 /dev/null 2>/tmp/gpgx_scroll.log

# Compare
diff <(grep SCROLL /tmp/ours_scroll.log) <(grep SCROLL /tmp/gpgx_scroll.log)
```

### Input file for long automated runs

For games with long intros (like Panorama Cotton), use `--input-file`:

```bash
python3 -c "
for f in range(1250,1256): print(f'{f}:A')
print('1256:')
for f in range(1400,60000,10): print(f'{f}:A'); print(f'{f+3}:')
" > /tmp/input.txt

./build/cpz_trace rom none 60000 60000 /tmp/out.ppm --input-file /tmp/input.txt
```

### Panorama Cotton input sequence

**Our emulator:** Mash Start every 10 frames to get through intros and into gameplay:
```bash
python3 -c "
for f in range(100, 80000, 10):
    print(f'{f}:S')
    print(f'{f+3}:')
" > /tmp/start_mash.txt
./build/cpz_trace "build/Panorama Cotton (Japan).md" none 32000 32000 /tmp/ours.ppm --input-file /tmp/start_mash.txt
```
Flying section with floor is reached by ~frame 32000.

**GPGX:** Start goes to options — use A at title instead:
```bash
python3 -c "
print('1250:A')
print('1255:')
for f in range(1400, 80000, 10):
    print(f'{f}:A')
    print(f'{f+3}:')
" > /tmp/a_mash.txt
./third_party/Genesis-Plus-GX/sdl/headless_dump "build/Panorama Cotton (Japan).md" 60000 /tmp/gpgx.ppm --input-file /tmp/a_mash.txt
```
Note: GPGX and our emulator respond differently to Start at the title screen — this is itself a behavioral difference worth investigating.

### Cold-boot frame comparison

Quick check for rendering correctness:
```bash
# Frame 5 should be 0% different (both emulators in simple state)
./build/cpz_trace rom none 5 5 /tmp/ours.ppm 2>/dev/null
./third_party/Genesis-Plus-GX/sdl/headless_dump rom 5 /tmp/gpgx.ppm 2>/dev/null
# Compare with python script above
```

## Core Principles

- **No game-specific hacks.** All fixes must be generic timing/hardware correctness.
- **Back up changes with references.** Cite hardware docs, test ROMs, or other emulator implementations.

## Known Issues

### Panorama Cotton floor rendering (partially fixed)

**Tile corruption: FIXED** (bus refresh + H counter tables). **Road position: still off-center** from ~5 cycle/frame timing drift.

- Rendering code is CORRECT (0% pixel diff at frame 5 cold boot with same VDP state)
- M68K instruction cycles match Musashi's cycle tables
- Bus refresh delay added (ce95db5): 2 M68K cycles every ~128 for DRAM refresh
- Per-master-clock H counter tables added (f988868): 3420-entry lookup in `src/video/hvc_tables.h`
- Game reads HV counter every scanline at ~cycle 120 in H-int handler
- Remaining ~5 cycle/frame drift is architectural (per-instruction vs burst model)
- Drift compounds chaotically: 4.7/frame at frame 10 → 10.1/frame by frame 50

### Mega Turrican letterbox bars (NOT FIXED)

**Current diagnosis: this is primarily a late-VBlank state/timing bug, not a confirmed window-mask or simple per-line VSRAM renderer bug.**

The old hypothesis in this file was too narrow. We now have stronger evidence that the visible top/bottom popping comes from CPU/VDP state diverging before the bad frame is rendered.

**What has been proven so far:**
- Frame `585` intro viewport handling is fixed. The game really does switch between `256x224` and `320x224`, and our export/debug path now respects that.
- VINT timing was wrong and has been corrected to match GPGX's timing window:
  - H40: `788` master clocks
  - H32: `770` master clocks
- HINT pending behavior across VBlank was also wrong and has been corrected:
  - we previously dropped HINT pending when HINT was disabled
  - GPGX carries the raw pending latch into VBlank
  - we now do too
- Neither of those fixes changed the visible Mega Turrican bad frame.

**What the traces now show:**
- By frame `1000`, our state is already ahead of GPGX before the scene is drawn.
- At line `224` (first VBlank line), both emulators are at PC `09DA80`, but our RAM values are already ahead:
  - ours: `FF0004=0010`, `FF04B4=005C`, `FF04E4=007C`, `FF116C=FFDC`
  - GPGX: `FF0004=000F`, `FF04B4=005A`, `FF04E4=007A`, `FF116C=FFDA`
- Through lines `246-255`, our 68K also diverges in the late-VBlank work window, and the object-driving RAM remains one update ahead.
- Sprite-line snapshotting was tested and was a complete no-op for the bad frame.
- The bad frame remains `7506 / 71680` pixels off GPGX after the VINT and HINT fixes.

**Implication:**
- The primary bug is not yet proven to be a renderer/window-path issue.
- The higher-confidence problem is CPU/VDP scheduling during late VBlank:
  - interrupt-edge ordering
  - exception entry timing
  - DMA stall/bus scheduling during the VBlank work window

**Renderer/window status:**
- A secondary renderer bug is still possible.
- Window mask is currently low confidence because the trace mismatch appears first in state, not only in final pixels.
- The decisive renderer check is still the same-state cross-render test:
  1. dump a bad Mega Turrican state with `vdp_state_dump`
  2. render the exact same state with GPGX `vdp_compare`
  3. if the same state differs, there is a real renderer/window bug
  4. if the same state matches, the bug is timing/state only

**Highest-confidence next targets:**
1. Same-state cross-render on the bad intro frame to definitively clear or confirm window/render involvement.
2. If same-state matches, continue on late-VBlank timing:
   - interrupt entry / autovector timing
   - DMA stall duration and scheduling in the VBlank DMA bursts
   - instruction-boundary progression across VBlank lines

**Latest findings (2026-03-25):**
- At frame `960` (star scrolling section), GPGX has 24 black lines top + 24 bottom (176-line visible area). Our emulator renders stars into those 48 lines.
- **VSRAM is CONSTANT and IDENTICAL in both emulators** (vsA=1004 on all 224 lines). The letterbox is NOT from per-line VSRAM changes.
- **Palettes 0-2 are ALL BLACK. Palette 3 has colors.** The nametable tiles on lines 0-23 and 200-223 should map to tiles using palette 0-2 (black), while lines 24-199 map to palette 3 (colors).
- With the same static VSRAM, both emulators scroll to the SAME nametable rows. But GPGX renders the top/bottom as black while we render stars there.
- **This points to a rendering code bug**: same VDP state (VRAM, CRAM, VSRAM, regs) produces different output. The nametable tiles at the top/bottom scroll positions may use palette 0-2 (all black) but our renderer might be using the wrong palette index, or the nametable lookup is off by one row.

## Architecture Notes

- Our M68K runs one instruction at a time with VDP clocking between each (more granular than GPGX's burst-to-cycle-target model)
- GPGX runs M68K in bursts (`m68k_run(target_cycles)`) and checks H-int once per line boundary
- VDP uses slot-based timing with 4 static tables (H40/H32 × active/blank) in `vdp_slot_table.h`
- DMA bandwidth matches GPGX (18 slots/line H40 active, 204 H40 blank)
- Bus refresh: 2 M68K cycles every ~128 cycles, tracked via `masterCycles_` counter in M68K class
- H counter: per-master-clock precision via `cycle2hc40[3420]`/`cycle2hc32[3420]` in `src/video/hvc_tables.h`
- Per-frame cycle drift vs GPGX: ~5-8 M68K cycles/frame (compounds chaotically over time)

## Debugging Tools

- `GENESIS_LOG_SCROLL=1` — per-line VSRAM/HSCROLL values during rendering
- `GENESIS_LOG_HINT_TIMING=1` — H-int counter events with cycle info
- `GENESIS_LOG_FRAME_CYCLES=1` — per-frame cumulative M68K cycle count
- `M68K_TRACE_INSN=N` — log first N M68K instructions (opcode + cycles)
- `GPGX_LOG_SCROLL=1` — same as above for GPGX headless dumper
- `GPGX_LOG_FRAME_CYCLES=1` — per-frame cycle count for GPGX
- `cpz_trace` — headless frame dumper with `--input-file` support and `none` state for cold boot
- `vdp_state_dump` — dumps VRAM/CRAM/VSRAM/regs/RAM/M68K state for cross-renderer comparison
- GPGX instrumentation: `gpgx_instrumentation.patch` (apply in `third_party/Genesis-Plus-GX/`)
- GPGX headless dumper: `third_party/Genesis-Plus-GX/sdl/headless_dump` with `--load-state` support
