<!-- Copyright (C) 2026 pagefault -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# References

This file tracks both third-party libraries used by the project and the
hardware/reference material that informed the emulator implementation.

## Third-Party Libraries

- **SDL3**
  - Window creation, input, audio device output, timing, and display mode queries
  - https://www.libsdl.org/

- **Dear ImGui**
  - In-app UI framework used for menus, settings, overlays, and tools
  - Uses the official SDL3 platform backend and OpenGL3 or Vulkan renderer backends
  - https://github.com/ocornut/imgui

- **OpenGL** / **Vulkan**
  - System graphics APIs used by the renderer backends
  - OpenGL is the default desktop backend; Vulkan is an optional build target

## Hardware Documentation

- **Charles MacDonald's VDP Documentation (v1.5f)**
  - HINT counter reload rules, register timing, VDP status register behavior
  - Source for the register #10 note used in `src/video/vdp.cpp`
  - https://www.neperos.com/article/picoxo1fc33c8979

- **Sega Genesis Technical Manual (Sega of America)**
  - VDP register descriptions, DMA modes, controller and interrupt behavior

- **Yacht v1.1 (Yet Another Cycle Hunting Table)**
  - M68000 instruction timing, bus-cycle ordering, and effective-address timing
  - Used by CPU timing tests and comments that lock instruction-cycle behavior

## Emulator and Audio Reference Implementations

- **Genesis Plus GX** (ekeeke)
  - Reference for VDP timing details, DMA behavior, HINT/VINT handling, and IM2 tile addressing
  - Also referenced in YM2612 comments and behavior notes
  - https://github.com/ekeeke/Genesis-Plus-GX

- **BlastEm** (Mike Pavone)
  - VDP line advance logic, HINT counter behavior on non-active lines
  - Useful architectural reference for more hardware-timed VDP behavior
  - https://www.retrodev.com/blastem/

- **Nuked-OPN2**
  - YM2612 behavior reference for ladder-effect DAC non-linearity and hardware-oriented FM details
  - Referenced directly by comments in `src/audio/ym2612.cpp`
  - https://github.com/nukeykt/Nuked-OPN2

## Community and Reverse-Engineering Resources

- **SpritesMind Forums** (`gendev.spritesmind.net`)
  - HBlank timing discussion: `/forum/viewtopic.php?t=388`
  - VDP register timing: `/forum/viewtopic.php?t=291`
  - HInt counter timing: `/forum/viewtopic.php?t=1511`
  - Horizontal interruption behavior: `/forum/viewtopic.php?t=3290`
  - Sonic 2 double-screen split technique: `/forum/viewtopic.php?t=1034`
  - Near's (byuu/higan) IM2 tile addressing explanation

- **MegaDrive Wiki**
  - VDP documentation and register reference
  - https://md.railgun.works/index.php?title=VDP

- **jsgroth blog - Emulator Bugs: Fatal Rewind**
  - HINT/VINT pending flag behavior independent of enable bits
  - https://jsgroth.dev/blog/posts/emulator-bugs-fatal-rewind/

- **Sega Retro / Plutiedev - TAS Instruction Write-Back Suppression**
  - The 68000 TAS instruction cannot complete its write-back cycle on the Genesis external bus (no bus lock support for read-modify-write)
  - Documented hardware quirk affecting Gargoyles, Ex-Mutants, and others
  - https://segaretro.org/Mega_Drive_Quirks
  - https://plutiedev.com/hardware-issues
