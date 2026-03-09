<!-- Copyright (C) 2026 pagefault -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Genesis Emulator Design Notes

This document describes the current architecture in the source tree and the major planned areas that do not exist yet.

It is intended to stay aligned with the implementation in `src/`, not serve as a historical implementation checklist.

## Scope

- Current desktop frontend: SDL2 + Dear ImGui
- Current rendering backends: OpenGL by default, Vulkan as a build-time option
- Current emulator core: 68000, Z80, VDP, YM2612, PSG, cartridge/bus, save states, cheats
- Planned but not yet implemented: debugger subsystem

## Source Layout

```text
src/
  main.cpp
  genesis.cpp
  genesis.h
  cpu/
    m68k.cpp
    m68k.h
    m68k_opcodes.cpp
    m68k_ops_data.cpp
    m68k_ops_arith.cpp
    m68k_ops_logic.cpp
    m68k_ops_branch.cpp
    m68k_ops_shift.cpp
    z80.cpp
    z80.h
  memory/
    bus.cpp
    bus.h
    cartridge.cpp
    cartridge.h
  cheats/
    cheat_types.cpp
    cheat_types.h
    cheat_format.cpp
    cheat_format.h
    cheat_file.cpp
    cheat_file.h
    ram_search.cpp
    ram_search.h
    cheat_engine.cpp
    cheat_engine.h
  video/
    vdp.cpp
    vdp.h
    vdp_slot_table.h
    vdp_dma.cpp
    vdp_render.cpp
    vdp_scanline.cpp
  audio/
    ym2612.cpp
    ym2612.h
    psg.cpp
    psg.h
  ui/
    app_ui.cpp
    app_ui.h
    keybindings.cpp
    keybindings.h
    game_renderer.h
    game_renderer_gl.cpp
    game_renderer_vk.cpp
    vulkan_context.cpp
    vulkan_context.h
```

## High-Level Architecture

### App Layer

`src/main.cpp` owns:

- SDL initialization
- window creation
- OpenGL or Vulkan presentation setup
- Dear ImGui setup
- config load/save
- main event loop
- hotkeys, menu actions, audio queueing, frame pacing
- on-screen profiler aggregation

The app layer is desktop-specific. The emulator core is not yet separated into a standalone library target.

### Core Coordinator

`src/genesis.cpp` and `src/genesis.h` own:

- component construction and wiring
- ROM loading/reset
- per-frame and per-scanline scheduling
- audio sample generation and mixing
- cheat engine integration
- save-state serialization
- detailed emulator-side profiling hooks

The `Genesis` object directly owns:

- `M68K`
- `Z80`
- `VDP`
- `Bus`
- `Cartridge`
- `YM2612`
- `PSG`

### CPU Cores

`src/cpu/m68k.*` implements the main 68000 core.

- full register state lives in `M68KState`
- the opcode handlers are split across several source files
- interrupt acknowledge is routed back into the VDP with a callback

`src/cpu/z80.*` implements the sound CPU.

- alternate registers, IX/IY, IM modes, and interrupt lines are modeled
- the core is strong enough to pass the included ZEXDOC-based test target

### Bus and Cartridge

`src/memory/bus.*` handles:

- 68K memory decode
- Z80 memory decode
- VDP port access
- YM2612 and PSG access
- controller I/O
- Z80 bus request/reset
- cartridge mapper and SRAM mapping support

There is no standalone `Controller` class in the current tree. Controller protocol state is handled inside `Bus`.

`src/memory/cartridge.*` handles:

- ROM loading
- SMD detection/conversion
- ROM header parsing
- SRAM detection and backing storage

### Video

`src/video/vdp.*`, `src/video/vdp_slot_table.h`, `src/video/vdp_dma.cpp`, `src/video/vdp_render.cpp`, and `src/video/vdp_scanline.cpp` implement the current VDP.

Supported features in the current renderer:

- planes A and B
- window layer
- sprites
- H32 and H40 widths
- 224 and 240 line modes
- shadow/highlight
- horizontal and vertical scrolling modes used by Genesis software
- DMA modes
- H-int and V-int timing
- interlace mode 3 double-resolution output

The current renderer path is the timed scanline renderer only. The old legacy renderer has been removed from the tree.

### Cheats

`src/cheats/*` owns the modular cheat subsystem:

- raw typed cheat parsing and formatting
- freeze cheat storage and application
- writable-memory abstraction for cheat access
- RAM search snapshots, filtering, and typed result actions
- ROM-sidecar cheat file save/load

`Genesis` integrates this subsystem through a narrow memory adapter and thin forwarding methods instead of owning cheat logic directly.

### Audio

`src/audio/ym2612.*` implements:

- 6 FM channels
- LFO
- SSG-EG
- DAC
- timers
- channel 3 special mode
- stereo panning

`src/audio/psg.*` implements the SN76489-compatible PSG.

Audio mixing is currently done inside `Genesis::runScanline()` rather than in a separate `audio_mixer.cpp` module.

### UI

`src/ui/app_ui.*` owns the Dear ImGui frontend:

- ROM browser
- save-state browser
- video/audio/emulation/input settings
- controls reference
- cheat manager
- RAM search UI
- status overlay
- FPS/profiler overlay

`src/ui/keybindings.*` owns default hotkeys and persistence.

`src/ui/game_renderer_gl.cpp` and `src/ui/game_renderer_vk.cpp` own the game framebuffer presentation path.

## Timing Model

The current scheduler is scanline-based with cycle-aware CPU and VDP interleaving.

Constants in `src/genesis.h`:

- master clock: `53,693,175`
- 68000 divider: `7`
- Z80 divider: `15`
- scanlines per frame: `262`
- master cycles per scanline: `3420`

Per scanline, `Genesis::runScanline()` does this:

1. apply cheats
2. begin the VDP scanline
3. raise pending H-int and V-int to the CPUs
4. run the 68K and Z80 with per-line budget accounting
5. stall the 68K during 68K-to-VDP DMA and FIFO backpressure
6. clock PSG and YM2612 timers in sync with CPU progress
7. generate audio samples for the line
8. end the VDP scanline

FIFO drains and DMA transfers are driven by a slot-table model that places external access windows at hardware-accurate positions within each scanline. The renderer itself is still segment-based rather than per-slot pixel output.

## Current VDP Rendering Model

The current VDP path is important to understand because it mixes accurate timing work with a few deliberate approximations.

### What it does now

- `beginScanlineTimed()` starts the line without pre-rendering the whole scanline
- the VDP tracks a per-line render frontier in visible pixels
- CPU and DMA writes that affect visible state flush the line up to the current cycle before mutating VDP state
- `endScanlineTimed()` flushes the remaining visible pixels for the line

The segment-render rule lives across `src/video/vdp.cpp`, `src/video/vdp_dma.cpp`, `src/video/vdp_render.cpp`, and `src/video/vdp_scanline.cpp`.

### What is accurate enough today

- HBlank status is derived from slot-table boundaries that match hardware-documented positions
- H-counter is derived from the slot table rather than a linear lineCycles mapping
- H-int delivery and acknowledge ordering are handled in the active VDP path
- visible VDP writes are applied in-order within a scanline rather than by full-line replay
- window-plane coverage and priority no longer use the earlier rectangle-style approximation
- Sonic 2 2P-style mid-frame split handling works well enough with the timed scanline renderer
- FIFO entries drain at exact external-access slot positions (VRAM: 2 slots per word, CRAM/VSRAM: 1 slot per word)
- DMA transfers (68K→VDP, fill, copy) are driven by the same external slot positions with correct per-mode slot costs
- DMA bandwidth matches Sega Software Manual figures (e.g. H40 active: 18 ext slots; H40 blank: 204 ext slots)

### VDP Slot Table

`src/video/vdp_slot_table.h` defines four prebuilt slot tables (H40/H32 × active/blank) containing:

- slot type (external, refresh, pattern) at each position
- master-clock offset per slot
- precomputed next-external-slot jump table for O(external) iteration in `clockM68K()`
- pixel position and H-counter value per slot
- reverse lookup from M68K cycle to slot index

`clockM68K()` uses a fast path (bulk advance when FIFO and DMA are idle) and a slow path that jumps directly between external slots using the precomputed lookup, processing each slot exactly once per scanline.

### What is still approximate

- the renderer is still segment-based, not slot-accurate at the VDP fetch level
- 68K-to-VDP DMA visibility during active display is still modeled as segment flush plus immediate memory mutation, not true fetch-slot timing
- sprite evaluation and composition are still performed at scanline granularity inside each rendered segment

## Audio Model

### YM2612

The YM2612 implementation separates:

- FM/audio clocking via `clock()`
- timer advancement via `clockTimers(int ticks)`
- sample output via `getSamples(...)`

`Genesis` computes the required YM tick density from the current audio sample rate and averages YM samples across an output sample interval.

### PSG

The PSG is clocked from 68K-side timing via `clock(int cycles)` and mixed into the stereo output in `Genesis`.

## Persistence

### Config

`src/main.cpp` persists runtime config in `~/.genesis/config.ini`.

Current persisted settings include:

- window scale
- scaling mode
- aspect ratio
- fullscreen mode
- exclusive fullscreen resolution
- bilinear filtering
- VSync
- FPS overlay enable
- profiler mode
- frame limiter
- audio queue size

### Keybindings

`src/ui/keybindings.cpp` persists keybindings in `~/.genesis/keybindings.txt`.

### Save States

`Genesis` save states include:

- CPU state
- VDP memory and control state
- bus state
- YM2612 state
- PSG state
- timing accumulators
- thumbnail image

Thumbnails are stored as `320x224` ARGB data at the end of the save file.

### Cheats

Cheats are currently persisted as ROM-sidecar `.cht` files next to the loaded ROM path.

The sidecar file currently stores:

- enabled state
- raw typed cheat code
- cheat name

## Current Frontend Features

The current app layer supports:

- responsive ImGui scaling based on DPI and actual window size
- windowed, borderless fullscreen, and exclusive fullscreen modes
- fullscreen resolution selection for exclusive fullscreen
- configurable scaling and aspect presentation
- runtime PAL/NTSC video standard selection with ROM-header auto mode
- on-screen FPS overlay with `Simple` and `Detailed` profiler modes
- save-state browser with thumbnails
- cheat entry and toggling
- RAM search with known-value and unknown-initial-state workflows
- ROM-sidecar cheat save/load
- rebindable emulator hotkeys and controller keys
- held fast-forward

## Known Gaps and Current Limitations

These are real current limitations from the source, not wishlist items:

- no debugger subsystem exists yet
- Vulkan VSync swapchain recreation is not implemented in the current settings path
- Vulkan save-state thumbnails are not yet uploaded as ImGui textures
- the VDP renderer is segment-based, not slot-accurate at the pixel-fetch level
- sprite evaluation is per-scanline, not per-slot

## Planned Debugger

The project still needs a debugger, and the design should match the current architecture instead of the old document's imaginary `src/debug/` tree.

### Goals

- inspect 68000 and Z80 state live
- disassemble 68000 and Z80 code
- add execution breakpoints
- add memory watchpoints
- inspect RAM, VRAM, CRAM, VSRAM, and sprite state
- step CPU execution while paused
- make raster and interrupt debugging practical

### Recommended Architecture

The debugger should be a new module layered on the existing `Genesis` object, not a parallel execution path.

Recommended pieces:

- `src/debug/` for debugger-specific code
- a `Debugger` controller object owned by `main.cpp` or `AppUI`, with read-only access to `Genesis` plus explicit stepping hooks
- CPU-facing helpers for disassembly and breakpoint checks
- bus-facing hooks for watchpoints and traced reads/writes
- VDP inspection helpers for VRAM/CRAM/VSRAM/SAT visualization

### Suggested First Milestone

1. pause/run/step controls backed by the existing `Genesis::step()`
2. 68000 register window
3. 68000 disassembly window around PC
4. RAM hex viewer
5. execution breakpoints on 68000 PC

### Suggested Second Milestone

1. Z80 register and disassembly windows
2. VRAM/CRAM/VSRAM viewers
3. sprite table viewer
4. watchpoints on bus reads/writes
5. interrupt and VDP event trace panes

### Constraints

- debugger hooks must be cheap when disabled
- watchpoints should live near bus access, not be duplicated in every caller
- VDP inspection should reuse existing internal state instead of recomputing everything from framebuffer output
- the debugger should not require a separate emulator core or alternate scheduler

## Testing

The current repo includes:

- `m68k_test`
- `zexdoc_test`
- `psg_test`
- `ym2612_test`
- `cheat_engine_test`

These are the current baseline checks for CPU/audio/cheat regressions. There is not yet a dedicated automated VDP regression suite in-tree.

## Audit Notes

This document replaces an older planning document that no longer matched the implementation.

Notable mismatches corrected here:

- there is no standalone `Controller` class in the current source
- there is no implemented `src/debug/` subsystem yet
- there is no SDL texture/renderer frontend path anymore; the frontend uses ImGui plus OpenGL or Vulkan
- the VDP uses the timed scanline path only
- save states, profiler modes, fullscreen modes, and responsive UI scaling are current shipped features and are now documented here
