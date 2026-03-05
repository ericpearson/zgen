<!-- Copyright (C) 2026 pagefault -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Genesis Test ROMs

Focused VDP/timing test ROMs downloaded from the Exodus Mega Drive software archive.

Run them with:

```sh
./build/genesis testroms/vdp-port-access-test/VDPFIFOTesting.bin
```

Included ROMs:

- `testroms/vdp-port-access-test/VDPFIFOTesting.bin`
  - General VDP port/FIFO behavior.
- `testroms/direct-color-dma/Direct-Color-DMA.bin`
  - DMA timing and active-display color/DMA behavior.
- `testroms/cram-flicker-test/cram flicker.bin`
  - CRAM write visibility and flicker timing.
- `testroms/v-counter-test/vctest.bin`
  - V counter and scanline edge behavior.
- `testroms/window-test/Window Test by Fonzie (PD).bin`
  - Window plane behavior.

The original downloads landed as ZIP archives from Google Drive wrappers and were extracted in place.
