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

We use BlastEm as the primary reference emulator (cycle-accurate, hardware-tested). GPGX is a secondary reference (simplified timing models).

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

### BlastEm (primary reference)

```bash
# Build (one-time)
cd third_party/blastem-src/blastem-884de5ef1263 && make -j8

# Run windowed
./third_party/blastem-src/blastem-884de5ef1263/blastem "build/ROM.md"

# Headless N frames + frame dump
BLASTEM_DUMP_PPM=/tmp/out.ppm ./third_party/blastem-src/blastem-884de5ef1263/blastem "build/ROM.md" -b N

# Headless with input injection (same format as cpz_trace)
./third_party/blastem-src/blastem-884de5ef1263/blastem "build/ROM.md" -b N --input-file /tmp/input.txt

# Per-frame cycle logging
BLASTEM_LOG_FRAME_CYCLES=1 ./third_party/blastem-src/blastem-884de5ef1263/blastem "build/ROM.md" -b N

# Per-line scroll logging
BLASTEM_LOG_SCROLL=1 ./third_party/blastem-src/blastem-884de5ef1263/blastem "build/ROM.md" -b N
```

Note: `-b N` = headless exit after N frames. `-n` disables Z80 (NOT headless).

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
- **Use BlastEm as the primary reference emulator.** BlastEm is cycle-accurate and hardware-tested. GPGX uses simplified models (no DMA stall, table-driven timing, game-specific hacks). Use BlastEm for all timing, scroll, and rendering comparisons. GPGX is a secondary reference only.

## Known Issues

### Sonic 2 2-player transition flash

Single-frame blue flash when transitioning from 2-player mode to title screen. Save state at `~/.genesis/saves/Sonic The Hedgehog 2 (World) (Rev A).ss0` — press Start, flash appears at frame 36.

**What happens:** The game's V-int handler writes vsram[0]=720 (vsA=800 after 10-bit mask) at V-blank line 231 for the 2-player split screen effect. On normal frames, vsram[0] is reset to 0 at V-blank line 224 before active display. On the transition frame, the reset doesn't happen until the NEXT frame's V-blank. Result: active display renders with vsA=800 pointing to empty nametable rows = solid blue.

**Normal frame VSRAM sequence:**
- Line 108 (active): vsram[0]=720 (bottom player split)
- Line 224 (V-blank): vsram[0]=0 (reset for next frame's top half)

**Flash frame VSRAM sequence:**
- Line 231 (V-blank): vsram[0]=720 (2-player value written during V-blank instead of line 108)
- NO reset to 0 — next vsram[0]=0 happens at the FOLLOWING frame's V-blank line 224

**Root cause:** Game state divergence. The V-int handler takes a different code path on the transition frame in our emulator vs BlastEm, writing vsram during V-blank instead of during active display. Same class of micro-timing divergence as the Panorama Cotton entry-1003 issue.

### Panorama Cotton HUD rendering (FIXED)

The bottom HUD (EXP/SPEED/COTTON/PAUSE) in the flying level was missing.

**Root cause:** The VDP has a transitional "PREPARING" state at vcounter 0x1FF (last V-blank line before active display) that DECREMENTS the H-int counter instead of reloading from reg[0x0A]. We were reloading at scanline 0, which left the counter 1 too high entering active display.

**Why it matters:** Panorama Cotton uses a multi-stage H-int handler chain ($5690 → $569C → $56B0) configured during V-blank. Handler $56B0 reads a per-line scroll table via `(A6)+` and disables H-int at V counter $B7 (line 183). With the counter 1 too high, the chain fires 1 line late, so `$56B0` starts at line 24 instead of line 23. This shifts the scroll table pointer by 1 entry. At line 183, the handler reads entry 183 (floor, vsA=240) instead of entry 184 (HUD, vsA=868).

**The fix (`src/video/vdp.cpp` beginScanlineCommon):** At scanline 0, decrement hblankCounter instead of reloading from reg[0x0A]. If the counter reaches 0, reload but do NOT assert hblankIRQ (H-int delivery happens at hblankStartCycle_ in clockM68K).

**Regression risk:** Changing the scanline 0 counter behavior affects ALL games. If the decrement is replaced with a reload, or if hblankIRQ is asserted at scanline 0, Cotton's HUD breaks. Top Gear 2 (reg[0x0A]=0, every-line H-int) also depends on the counter NOT firing at scanline 0 — double-firing would shift its raster effect.

**Test:** `./build/cpz_trace "build/Panorama Cotton (Japan).md" none 32000 32000 /tmp/test.ppm --input-file /tmp/start_mash.txt` — bottom HUD (EXP/COTTON/SPEED) must be visible.

### Top Gear 2 road flickering (FIXED)

Flickering black horizontal bands in the road during gameplay (every ~3 frames). Also affected: line 13 in H32 mode (sky band near top of screen).

**Root cause:** The game uses full-screen vscroll with H-int writing vsram[0]/vsram[1] every line for the road perspective. The H-int handler fires at hblankStartCycle_ (~cycle 483) and writes to the VDP data port, which enqueues in the FIFO. The FIFO drains at external slots. The LAST external slot in H40 active is at mclk 3380 (cycle ~483). If the handler's write misses this slot, the FIFO entry can't drain during the current line — no external slots remain.

The entry should drain at the NEXT line's first external slot (mclk 352, cycle ~50). But `beginScanlineCommon` latches `vsramSnapshot` BEFORE the FIFO drains. The snapshot captures the stale pre-write value. On some frames this maps to an empty nametable row = black band.

There are two variants depending on when the H-int handler's VDP write completes:

**Variant A — FIFO entry enqueued on current line, drains on next line:** The VDP write instruction completes within the current line's M68K budget. The FIFO entry is enqueued but can't drain (no external slots left). At the next line's `beginScanlineCommon`, `drainPendingFIFOForSnapshot()` processes the entry at an early external slot before latching `vsramSnapshot`.

**Variant B — H-int handler straddles the scanline boundary:** The H-int fires late enough that the handler's VDP write instruction doesn't execute until the NEXT line's M68K loop (not carry-in — actual M68K instruction execution). The FIFO entry is enqueued on the new line. `beginScanlineCommon` already ran (FIFO was empty), so the snapshot has a stale value. The carry-in drain also misses it (carry-in only advances VDP time, the instruction hasn't executed yet).

**The fix (three parts):**

1. **`src/video/vdp.cpp` drainPendingFIFOForSnapshot():** Called from beginScanlineCommon — processes ALL pending FIFO entries at the new line's early external slots BEFORE latching vsramSnapshot. Handles variant A.

2. **`src/genesis.cpp` after carry-in:** Calls drainPendingFIFOForSnapshot() + relatchVsramSnapshot() after carry-in cycles. Handles FIFO entries enqueued DURING carry-in.

3. **`src/video/vdp.cpp` applyFIFOEntry() cross-scanline relatch:** Each FIFO entry records its `enqueueScanline`. When a VSRAM entry (idx 0 or 1) drains on a DIFFERENT scanline than it was enqueued on, AND no pixels have been rendered yet (`renderedPixels == 0`), the vsramSnapshot is relatched. Handles variant B.

**Regression risk:** The relatch in applyFIFOEntry MUST only fire for cross-scanline entries (previous line's handler straddling). Same-scanline entries from the current line's H-int are for the NEXT line and must NOT relatch — this would corrupt Panorama Cotton's per-line scroll table. The `enqueueScanline != scanline` check ensures this: Cotton's H-int handler enqueues and drains within the same line, so the scanlines always match.

**vsramSnapshot architectural limitation:** Real VDP hardware reads VSRAM live at each tile-fetch slot — there is no discrete "snapshot." Our batched renderer can't do this: `flushCurrentLineToCurrentCycle()` renders pixel ranges at once, so it needs a stable VSRAM reference for the line. The snapshot captures VSRAM at line start for full-screen vscroll. This is correct when VSRAM changes are fully settled before the line begins, but breaks when a FIFO drain crosses the snapshot boundary (variant B above). A pixel-accurate fix would track VSRAM changes with cycle timestamps and have the renderer pick the right value per pixel range, but the `enqueueScanline` check handles the practical case that breaks: H-int handler writes that straddle the scanline boundary.

**Test:** `./build/cpz_trace "build/Top Gear 2 (USA).md" ~/.genesis/saves/"Top Gear 2 (USA).ss0" 120 0 /dev/null` — all 120 frames must have identical hash (no flickering). Note: save state must be version 16+ (includes `enqueueScanline` in FIFO struct).

### Panorama Cotton floor rendering (FIXED)

Tile corruption fixed via bus refresh + H counter tables.

### Mega Turrican letterbox bars (FIXED)

Fixed by implementing sprite X=0 masking.

### Sunset Riders (FIXED)

Sega/Konami logos fixed. Player select screen fixed — byte-write fill trigger was missing.

## Architecture Notes

- Our M68K runs one instruction at a time with VDP sync after each instruction (`syncToLineCycle`)
- VDP uses slot-based timing with 4 static tables (H40/H32 × active/blank) in `vdp_slot_table.h`
- DMA bandwidth: 18 external slots/line H40 active, ~204 H40 blank
- Bus refresh: 2 M68K cycles every 128 M68K cycles (896 master clocks). Sync-point batched model matching BlastEm: penalties applied at VDP/IO bus accesses (free-access model), scanline boundaries, and pre-interrupt delivery. ROM/RAM accesses are penalty-free. H-int handlers execute without mid-handler refresh stalls.
- H counter: per-master-clock precision via `cycle2hc40[3420]`/`cycle2hc32[3420]` in `src/video/hvc_tables.h`
- Z80 runs sequentially after each M68K burst (not interleaved like BlastEm). Z80 bank reads ($8000+) access 68K bus without stalling M68K.
- Per-frame cycle count matches BlastEm exactly on average (128005.7). Per-instruction cycle counts differ due to bus refresh model (per-instruction vs batched).

## Debugging Tools

- `GENESIS_LOG_SCROLL=1` — per-line VSRAM/HSCROLL values during rendering
- `GENESIS_LOG_HINT_TIMING=1` — H-int counter events with cycle info
- `GENESIS_LOG_FRAME_CYCLES=1` — per-frame cumulative M68K cycle count
- `GENESIS_LOG_FIFO_TIMING=1` — FIFO enqueue/apply/VSRAM visibility per entry
- `GENESIS_LOG_SCANLINE_EVENTS=first-last` — unified per-scanline event trace (FIFO-DRAIN, DMA-SLOT, HINT-ASSERT, VINT-ASSERT)
- `GENESIS_LOG_REG_CHANGES=1` — VDP register write logging
- `M68K_TRACE_INSN=N` — log first N M68K instructions (opcode + cycles)
- `M68K_TRACE_FRAME_FIRST=N` / `M68K_TRACE_FRAME_LAST=M` — gate instruction trace to specific frames
- `GPGX_LOG_SCROLL=1` — same as above for GPGX headless dumper
- `GPGX_LOG_FRAME_CYCLES=1` — per-frame cycle count for GPGX
- `BLASTEM_LOG_FRAME_CYCLES=1` — per-frame cycle count for BlastEm
- `BLASTEM_DUMP_PPM=/path/to/out.ppm` — dump frame to PPM at exit (BlastEm `-b N` mode)
- `cpz_trace` — headless frame dumper with `--input-file` support and `none` state for cold boot
- `cpz_trace` immediate-dump mode: `frames=0` dumps framebuffer without running
- `vdp_state_dump` — dumps VRAM/CRAM/VSRAM/regs/RAM/M68K state for cross-renderer comparison
- GPGX instrumentation: `gpgx_instrumentation.patch` (apply in `third_party/Genesis-Plus-GX/`)
- GPGX headless dumper: `third_party/Genesis-Plus-GX/sdl/headless_dump` with `--load-state` support
- `GENESIS_LOG_RAM_WRITES=ADDR-ADDR` — watch RAM byte/word writes to specified address range
- BlastEm: built at `third_party/blastem-src/blastem-884de5ef1263/blastem` with frame dump, cycle logging, and `--input-file` support
- `BLASTEM_TRACE_INSN=1` + `BLASTEM_TRACE_FRAME_FIRST/LAST` — per-instruction trace in BlastEm (m68k.c)
