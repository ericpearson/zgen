// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "video/vdp.h"

void VDP::beginScanlineTimed() { beginScanlineCommon(); }

void VDP::endScanlineTimed() {
    // Advance VDP to end of line so remaining FIFO/DMA slots are processed
    // and H-int fires at the correct hardware time, even if the M68K budget
    // was reduced by overshoot debt from the previous line.
    if (lineCycles < VDP_MAX_M68K_CYCLES) {
        clockM68K(VDP_MAX_M68K_CYCLES - lineCycles);
    }
    flushCurrentLineToPixel(activeWidth);
}

void VDP::captureBoundaryVscrollForNextLine() {
    int nextLine = scanline + 1;
    if (nextLine >= getTotalScanlines()) {
        nextLine = 0;
    }

    if (activeWidth == 320 && vscrollMode == 0 && nextLine < activeHeight) {
        boundaryVscrollLatch_[0] = vsram[0];
        boundaryVscrollLatch_[1] = vsram[1];
        boundaryVscrollLatchValid_ = true;
    } else {
        boundaryVscrollLatchValid_ = false;
    }
}
