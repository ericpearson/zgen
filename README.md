<!-- Copyright (C) 2026 pagefault -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Genesis

[![GitHub CI](https://github.com/ericpearson/zgen/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/ericpearson/zgen/actions/workflows/ci.yml)
[![CI test coverage](https://img.shields.io/badge/CI%20test%20coverage-ctest%20%2B%20ROM%20regressions-blue)](#testing)

A Sega Genesis / Mega Drive emulator written in C++17 with SDL3 and Dear ImGui.

## What This Project Is

A desktop emulator with cycle-aware Genesis / Mega Drive hardware emulation:

- **Motorola 68000** main CPU (~7.67 MHz)
- **Zilog Z80** sound CPU (~3.58 MHz) — passes ZEXDOC conformance suite
- **VDP** — slot-driven renderer with per-column tile output, planes A/B, window, sprites, scrolling, DMA, shadow/highlight, H32/H40, interlace mode 2 (320x448 double-resolution), FIFO write latency, deferred VSRAM latch, H-interrupt driven mid-frame splits
- **YM2612** 6-channel FM synthesis with LFO, SSG-EG, DAC, timers, CH3 special mode
- **SN76489** PSG — 3 tone channels + noise
- **Memory bus** — full 24-bit address space, bank switching, SRAM save support
- **Controllers** — 3-button and 6-button with TH counter detection

## Project Status

**Playable** — boots and runs games reliably. VDP uses a slot-driven rendering pipeline where each scanline's tiles, FIFO drains, and VSRAM latches are processed at their correct hardware slot positions. Audio and timing accuracy are under active improvement.

## Downloads

Prebuilt packages are published on the [GitHub Releases](https://github.com/ericpearson/zgen/releases) page:

- `zgen-linux-x86_64.tar.gz`
- `zgen-macos-arm64.tar.gz`
- `zgen-windows-x64.zip` — default OpenGL build
- `zgen-windows-x64-vulkan.zip` — Windows build with the Vulkan renderer enabled

## Build

Requires:

- CMake 3.16+
- a C++17 compiler
- `pkg-config`
- SDL3 development files

Default build uses OpenGL 3.2.

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

### Vulkan backend (optional)

Requires a Vulkan SDK/loader in addition to SDL3.

```bash
cmake -S . -B build -DUSE_VULKAN=ON
cmake --build build -j$(nproc)
```

## Run

```bash
./build/genesis path/to/rom.bin
```

Supported formats: `.bin`, `.md`, `.gen` (auto-detects and converts SMD interleaved format).

Set `GENESIS_MAX_FRAMES=N` to stop after N frames (useful for testing).

## Controls

All controls are rebindable via Settings > Input.

### Game

| Key | Action |
|-----|--------|
| Arrow keys | D-pad |
| Z / X / C | A / B / C |
| Enter | Start |
| A / W / D / Q | X / Y / Z / Mode (6-button) |

### Emulator

| Key | Action |
|-----|--------|
| Escape | Toggle menu |
| F5 | Quick save |
| F7 | Quick load |
| F6 | Open save state browser |
| F4 / F8 | Previous / next save slot |
| Space | Pause |
| F | Step frame (while paused) |
| S | Step instruction (while paused) |
| T | Toggle frame limiter |
| F12 | Toggle FPS counter |
| Tab | Hold to fast forward |
| F10 | Quit |

## Features

### GUI

Dear ImGui overlay with a menu bar, draggable windows, and responsive DPI-aware rendering that stays readable in small windows without ballooning in large ones.

- **File** — open ROM, recent ROMs (last 10), reset, resume, quit
- **Settings** — video, audio, emulation, and input configuration
- **Tools** — cheat manager, save state browser
- **Help** — controls reference, about

### Video Settings

- Display modes: windowed, borderless fullscreen, or exclusive fullscreen
- Exclusive fullscreen resolution selection when SDL reports display modes
- Window size presets (1x-6x, 320x224 base) in windowed mode
- Scaling mode: integer-only, fit, or stretch
- Aspect ratio: auto, 4:3, 16:9, or stretch
- Bilinear filtering toggle
- VSync and frame limiter toggles
- FPS counter with `Simple` and `Detailed` profiler modes
- On-screen performance breakdown for emulation, render, swap/present, UI, SDL, audio, and idle time

### Audio Settings

- Buffer size presets (75ms / 150ms / 300ms radio buttons) with fine-tune slider (50–500ms)
- 48 kHz stereo output (respects system sample rate)

### Save States

- 10 slots per game with thumbnail previews (320x224 screenshots)
- Quick save/load hotkeys (F5/F7), slot browser (F6), and slot navigation (F4/F8)
- Full state serialization: CPU, RAM, VDP, audio chips

### Cheat Manager

- Format: `FFFFFF:YY` (byte) or `FFFFFF:YYYY` (word)
- Per-cheat enable/disable toggles
- RAM search with known-value and unknown-initial-state workflows
- Result refinement (`Changed`, `Unchanged`, `Increased`, `Decreased`, `Equal To Value`, `Not Equal To Value`)
- `Set Once` memory editing and `Freeze` cheat creation from search results
- ROM-sidecar cheat save/load (`.cht` next to the ROM)
- Real-time freeze application during emulation

### Input Rebinding

- Game controls and emulator hotkeys are rebindable with visual key-capture UI
- Persistent storage (`~/.genesis/keybindings.txt`)
- Reset to defaults with confirmation

### Rendering Backends

- **OpenGL 3.2+** (default) — shader-based framebuffer rendering
- **Vulkan 1.0+** (compile-time option) — full swapchain, render pass, and descriptor set pipeline

## Testing

```bash
cmake --build build --target all
ctest --test-dir build --output-on-failure
```

GitHub CI builds and runs CTest on Linux, macOS, and Windows. Local regression
coverage is provided by `tests/test_regressions.sh` when the required ROMs are
available.

Test suites: M68K instruction and timing tests, Z80 ZEXDOC conformance, VDP
slot/rendering tests, PSG, YM2612, audio mixing, cheat engine, cartridge
loading, and ROM-backed rendering regressions.

## Notes

- Hardware documentation sources are in `REFERENCES.md`
- Config and save data stored in `~/.genesis/`
- License is `GPL-3.0-only`; see `LICENSE`
