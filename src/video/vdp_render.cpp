// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "video/vdp.h"
#include <algorithm>
#include <cstring>

namespace {
constexpr int kPlaneBLowRank = 1;
constexpr int kPlaneALowRank = 2;
constexpr int kSpriteLowRank = 3;
constexpr int kWindowLowRank = 4;
constexpr int kPlaneBHighRank = 5;
constexpr int kPlaneAHighRank = 6;
constexpr int kSpriteHighRank = 7;
constexpr int kWindowHighRank = 8;

struct WindowSpan {
    int start = 0;
    int end = 0;

    bool active() const {
        return start < end;
    }
};

inline u16 readVramWord(const u8* vram, u16 addr) {
    return (static_cast<u16>(vram[addr & 0xFFFF]) << 8) | vram[(addr + 1) & 0xFFFF];
}

inline void loadPatternRow(const u8* vram, u16 rowAddr, u8 (&rowBytes)[4]) {
    rowBytes[0] = vram[rowAddr & 0xFFFF];
    rowBytes[1] = vram[(rowAddr + 1) & 0xFFFF];
    rowBytes[2] = vram[(rowAddr + 2) & 0xFFFF];
    rowBytes[3] = vram[(rowAddr + 3) & 0xFFFF];
}

inline bool rowIsTransparent(const u8 (&rowBytes)[4]) {
    return (rowBytes[0] | rowBytes[1] | rowBytes[2] | rowBytes[3]) == 0;
}

inline int tilePixelFromRow(const u8 (&rowBytes)[4], int px) {
    const u8 packed = rowBytes[px >> 1];
    return (px & 1) ? (packed & 0x0F) : (packed >> 4);
}

inline int planeRank(int layer, bool highPriority) {
    // Ordering from back to front:
    // Plane B low, Plane A low, Sprite low, Window low,
    // Plane B high, Plane A high, Sprite high, Window high.
    if (layer == 1) {
        return highPriority ? kPlaneBHighRank : kPlaneBLowRank;
    }
    return highPriority ? kPlaneAHighRank : kPlaneALowRank;
}

inline int windowRank(bool highPriority) {
    return highPriority ? kWindowHighRank : kWindowLowRank;
}

inline WindowSpan horizontalWindowSpan(int activeWidth, int windowHP, bool windowRight) {
    int windowXCell = windowHP * 2;
    if (windowRight) {
        int start = (windowHP == 0) ? 0 : std::min(windowXCell * 8, activeWidth);
        return {start, activeWidth};
    }

    if (windowHP == 0) {
        return {};
    }

    return {0, std::min(windowXCell * 8, activeWidth)};
}

inline WindowSpan computeWindowSpanForLine(int line, int activeWidth, u8 reg11, u8 reg12) {
    const int windowHP = reg11 & 0x1F;
    const bool windowRight = (reg11 & 0x80) != 0;
    const int windowVP = reg12 & 0x1F;
    const bool windowDown = (reg12 & 0x80) != 0;

    // The VDP window is not a simple rectangle. The vertical setting selects
    // full-width window lines from the top or bottom, and the horizontal
    // setting applies on the remaining lines.
    bool fullLineWindow = false;
    if (windowVP == 0) {
        fullLineWindow = windowDown;
    } else {
        const int boundaryLine = windowVP * 8;
        fullLineWindow = windowDown ? (line >= (boundaryLine - 1))
                                    : (line < boundaryLine);
    }

    if (fullLineWindow) {
        return {0, activeWidth};
    }

    return horizontalWindowSpan(activeWidth, windowHP, windowRight);
}

inline bool hasLeftWindowHScrollBug(int layer, const WindowSpan& windowSpan,
                                    int activeWidth, int hscroll) {
    return layer == 0 &&
           windowSpan.active() &&
           windowSpan.start == 0 &&
           windowSpan.end > 0 &&
           windowSpan.end < activeWidth &&
           (hscroll & 0x0F) != 0;
}
}

void VDP::renderScanline(int line) {
    renderScanlineSegment(line, 0, activeWidth);
}

void VDP::renderScanlineSegment(int line, int startX, int endX) {
    if (startX < 0) startX = 0;
    if (endX > activeWidth) endX = activeWidth;
    if (startX >= endX) {
        return;
    }

    bool im2 = (interlaceMode == 3);
    // Double-field progressive: in IM2, render both field 0 and field 1
    // per scanline using the same VDP state.  This eliminates combing from
    // field-to-field scroll differences (standard approach — GP-GX, BlastEm).
    int numFields = im2 ? 2 : 1;

    for (int field = 0; field < numFields; field++) {
        // Temporarily set oddFrame so sub-render functions pick up
        // the correct field for their effectiveY calculations.
        bool savedOddFrame = oddFrame;
        if (im2) {
            oddFrame = (field == 1);
        }

        u16* lineBuffer = scanlineColorBuffer;
        u8* rankBuffer = scanlineRankBuffer;
        const int outputXOffset = (320 - activeWidth) / 2;

        std::fill_n(rankBuffer + startX, endX - startX, static_cast<u8>(0));
        std::fill_n(lineBuffer + startX, endX - startX, bgColorIndex);

        // When display is disabled the VDP outputs the border/backdrop stage
        // instead of the normal plane/sprite composition.
        if (displayEnabled) {
            renderBackground(line, 1, startX, endX, lineBuffer, rankBuffer);
            renderBackground(line, 0, startX, endX, lineBuffer, rankBuffer);
            renderWindow(line, startX, endX, lineBuffer, rankBuffer);
            renderSprites(line, startX, endX, lineBuffer, rankBuffer);
        }

        int fbLine = im2 ? (line * 2 + field) : line;
        u32* fbRow = framebuffer + (fbLine * 320);
        if (startX == 0 && activeWidth < 320) {
            std::fill_n(fbRow, outputXOffset, cramColors[bgColorIndex]);
            std::fill_n(fbRow + outputXOffset + activeWidth, 320 - outputXOffset - activeWidth,
                        cramColors[bgColorIndex]);
        }
        std::fill_n(fbRow + outputXOffset + startX, endX - startX, cramColors[bgColorIndex]);

        if (shadowHighlightEnabled) {
            for (int x = startX; x < endX; x++) {
                u16 colorIndex = lineBuffer[x] & 0x3F;
                u8 rank = rankBuffer[x];

                bool spriteHighlight = (lineBuffer[x] & 0x8000) != 0;
                bool spriteShadow = (lineBuffer[x] & 0x4000) != 0;

                if (spriteHighlight) {
                    fbRow[outputXOffset + x] = cramHighlightColors[colorIndex];
                } else if (spriteShadow) {
                    fbRow[outputXOffset + x] = cramShadowColors[colorIndex];
                } else if (rank >= kPlaneBHighRank) {
                    fbRow[outputXOffset + x] = cramColors[colorIndex];
                } else {
                    fbRow[outputXOffset + x] = cramShadowColors[colorIndex];
                }
            }
        } else {
            for (int x = startX; x < endX; x++) {
                u16 colorIndex = lineBuffer[x] & 0x3F;
                fbRow[outputXOffset + x] = cramColors[colorIndex];
            }
        }

        oddFrame = savedOddFrame;
    }
}

void VDP::renderBackground(int line, int layer, int startX, int endX, u16* lineBuffer, u8* rankBuffer) {
    u16 ntBase = (layer == 0) ? scrollABase : scrollBBase;
    int planeWidthPixels = scrollWidth * 8;
    int planeHeightPixels = scrollHeight * 8;
    bool im2 = (interlaceMode == 3);
    // IM2 (double resolution): Y coordinate is doubled, tiles are 8x16
    int effectiveY = im2 ? (line * 2 + (oddFrame ? 1 : 0)) : line;
    int planeHeightPx = im2 ? (planeHeightPixels * 2) : planeHeightPixels;

    int hscrollOffset = hscrollBase + layer * 2;
    switch (hscrollMode) {
        case 2: // Per 8-line cell
            hscrollOffset += (line & ~7) * 4;
            break;
        case 3: // Per line
            hscrollOffset += line * 4;
            break;
        case 1: // Reserved; handle as full-screen.
        case 0:
        default:
            break;
    }

    int hscroll = readVramWord(vram, static_cast<u16>(hscrollOffset)) & 0x03FF;

    WindowSpan windowSpan;
    if (layer == 0) {
        windowSpan = computeWindowSpanForLine(line, activeWidth, regs[0x11], regs[0x12]);
    }
    const bool leftWindowHScrollBug = hasLeftWindowHScrollBug(layer, windowSpan, activeWidth, hscroll);
    const int leftWindowBugEnd = leftWindowHScrollBug
        ? std::min(windowSpan.end + 16, activeWidth)
        : 0;

    int fullVScroll = vsram[layer] & 0x03FF;

    int x = startX;
    while (x < endX) {
        if (layer == 0 && windowSpan.active() && x >= windowSpan.start && x < windowSpan.end) {
            x = std::min(windowSpan.end, endX);
            continue;
        }

        int vscroll = fullVScroll;
        int spanEnd = endX;
        if (layer == 0 && windowSpan.active() && x < windowSpan.start) {
            spanEnd = std::min(spanEnd, windowSpan.start);
        }
        if (leftWindowHScrollBug && x >= windowSpan.end && x < leftWindowBugEnd) {
            spanEnd = std::min(spanEnd, leftWindowBugEnd);
        }
        if (vscrollMode == 1) {
            int vsIndex = ((x >> 4) * 2) + layer;
            if (vsIndex < 40) {
                vscroll = vsram[vsIndex] & 0x03FF;
            }
            spanEnd = std::min(spanEnd, ((x >> 4) + 1) << 4);
        }

        int planeScreenX = x;
        if (leftWindowHScrollBug && x >= windowSpan.end && x < leftWindowBugEnd) {
            // Real VDP quirk: with a left-side window and a Plane A h-scroll
            // that is not 16-pixel aligned, the first two Plane A cells to
            // the right of the boundary are fetched from a restarted origin.
            planeScreenX -= windowSpan.end;
        }

        int scrolledX = (planeScreenX - hscroll) & (planeWidthPixels - 1);
        int scrolledY = (effectiveY + vscroll) & (planeHeightPx - 1);

        int tileCol = scrolledX >> 3;
        int tileX = scrolledX & 7;
        spanEnd = std::min(spanEnd, x + (8 - tileX));
        int tileRow, tileY;
        if (im2) {
            tileRow = scrolledY >> 4;   // 16-pixel tiles
            tileY = scrolledY & 15;
        } else {
            tileRow = scrolledY >> 3;
            tileY = scrolledY & 7;
        }

        u16 ntOffset = static_cast<u16>(ntBase + ((tileRow * scrollWidth + tileCol) * 2));
        u16 ntEntry = readVramWord(vram, ntOffset);

        bool priority = (ntEntry & 0x8000) != 0;
        int palette = (ntEntry >> 13) & 0x03;
        bool vflip = (ntEntry & 0x1000) != 0;
        bool hflip = (ntEntry & 0x0800) != 0;
        int pattern = ntEntry & 0x07FF;

        int py;
        if (im2) {
            // IM2: tiles are 8x16 (64 bytes). Address shift doubles
            // (pattern * 64 instead of * 32). py ranges 0-15 sequentially
            // within the 64-byte tile. No sub-tile interleaving.
            py = vflip ? (15 - tileY) : tileY;
        } else {
            py = vflip ? (7 - tileY) : tileY;
        }

        int patternSize = im2 ? 64 : 32;
        u8 rowBytes[4];
        loadPatternRow(vram, static_cast<u16>((pattern * patternSize) + (py * 4)), rowBytes);
        if (rowIsTransparent(rowBytes)) {
            x = spanEnd;
            continue;
        }

        int rank = planeRank(layer, priority);
        int baseColor = palette * 16;
        int pxCursor = tileX;
        for (int screenX = x; screenX < spanEnd; screenX++, pxCursor++) {
            int px = hflip ? (7 - pxCursor) : pxCursor;
            int colorIndex = tilePixelFromRow(rowBytes, px);
            if (colorIndex == 0 || rank < rankBuffer[screenX]) {
                continue;
            }
            lineBuffer[screenX] = static_cast<u16>(baseColor + colorIndex);
            rankBuffer[screenX] = static_cast<u8>(rank);
        }
        x = spanEnd;
    }
}

void VDP::renderWindow(int line, int startX, int endX, u16* lineBuffer, u8* rankBuffer) {
    WindowSpan windowSpan = computeWindowSpanForLine(line, activeWidth, regs[0x11], regs[0x12]);
    if (!windowSpan.active()) {
        return;
    }

    bool im2 = (interlaceMode == 3);
    int effectiveY = im2 ? (line * 2 + (oddFrame ? 1 : 0)) : line;

    int tableWidth = (activeWidth == 320) ? 64 : 32;
    int cellY, tileY;
    if (im2) {
        cellY = effectiveY >> 4;   // 16-pixel tiles
        tileY = effectiveY & 15;
    } else {
        cellY = line >> 3;
        tileY = line & 7;
    }
    if (cellY >= 32) {
        return;
    }

    for (int x = std::max(windowSpan.start, startX); x < std::min(windowSpan.end, endX); ) {
        int cellX = x >> 3;
        int tileEnd = std::min(std::min(windowSpan.end, endX), ((cellX + 1) << 3));
        int winCellX = cellX;
        if (winCellX >= tableWidth) {
            x = tileEnd;
            continue;
        }

        u16 ntOffset = static_cast<u16>(windowBase + ((cellY * tableWidth + winCellX) * 2));
        u16 ntEntry = readVramWord(vram, ntOffset);

        bool priority = (ntEntry & 0x8000) != 0;
        int palette = (ntEntry >> 13) & 0x03;
        bool vflip = (ntEntry & 0x1000) != 0;
        bool hflip = (ntEntry & 0x0800) != 0;
        int pattern = ntEntry & 0x07FF;

        int py;
        if (im2) {
            py = vflip ? (15 - tileY) : tileY;
        } else {
            py = vflip ? (7 - tileY) : tileY;
        }

        int patternSize = im2 ? 64 : 32;
        u8 rowBytes[4];
        loadPatternRow(vram, static_cast<u16>((pattern * patternSize) + (py * 4)), rowBytes);
        if (rowIsTransparent(rowBytes)) {
            x = tileEnd;
            continue;
        }

        int rank = windowRank(priority);
        int baseColor = palette * 16;
        for (int screenX = x; screenX < tileEnd; screenX++) {
            int tileX = screenX & 7;
            int px = hflip ? (7 - tileX) : tileX;
            int colorIndex = tilePixelFromRow(rowBytes, px);
            if (colorIndex == 0 || rank < rankBuffer[screenX]) {
                continue;
            }
            lineBuffer[screenX] = static_cast<u16>(baseColor + colorIndex);
            rankBuffer[screenX] = static_cast<u8>(rank);
        }
        x = tileEnd;
    }
}

void VDP::parseSprites(int line, SpriteEntry* sprites, int& count) {
    count = 0;
    int maxSprites = (activeWidth == 320) ? 80 : 64;
    int maxPerLine = (activeWidth == 320) ? 20 : 16;
    bool im2 = (interlaceMode == 3);
    // IM2: sprite comparison uses doubled line, Y offset is 256, cells are 16px
    int effectiveLine = im2 ? (line * 2 + (oddFrame ? 1 : 0)) : line;
    int yOffset = im2 ? 256 : 128;
    int cellHeight = im2 ? 16 : 8;

    int spriteIndex = 0;
    for (int i = 0; i < maxSprites; i++) {
        u16 satAddr = static_cast<u16>(spriteBase + spriteIndex * 8);

        s16 ypos = static_cast<s16>(readVramWord(vram, satAddr) & 0x03FF);
        ypos -= yOffset;

        u8 size = vram[(satAddr + 2) & 0xFFFF];
        int width = ((size >> 2) & 0x03) + 1;
        int height = (size & 0x03) + 1;

        u8 link = vram[(satAddr + 3) & 0xFFFF] & 0x7F;

        u16 attr = readVramWord(vram, static_cast<u16>(satAddr + 4));
        bool priority = (attr & 0x8000) != 0;
        int palette = (attr >> 13) & 0x03;
        bool vflip = (attr & 0x1000) != 0;
        bool hflip = (attr & 0x0800) != 0;
        int pattern = attr & 0x07FF;

        s16 xpos = static_cast<s16>(readVramWord(vram, static_cast<u16>(satAddr + 6)) & 0x03FF);
        xpos -= 128;

        int spriteHeightPx = height * cellHeight;
        if (effectiveLine >= ypos && effectiveLine < ypos + spriteHeightPx) {
            if (count >= maxPerLine) {
                // Sprite overflow: set flag and stop processing
                spriteOverflow = true;
                break;
            }
            sprites[count].ypos = ypos;
            sprites[count].width = static_cast<u8>(width);
            sprites[count].height = static_cast<u8>(height);
            sprites[count].link = link;
            sprites[count].priority = priority;
            sprites[count].palette = static_cast<u8>(palette);
            sprites[count].vflip = vflip;
            sprites[count].hflip = hflip;
            sprites[count].pattern = static_cast<u16>(pattern);
            sprites[count].xpos = xpos;
            count++;
        }

        if (link == 0) {
            break;
        }
        spriteIndex = link;
    }
}

void VDP::renderSprites(int line, int startX, int endX, u16* lineBuffer, u8* rankBuffer) {
    // Latch sprite list once per scanline/field so SAT updates do not become
    // visible mid-line. This matches VDP line-buffer behavior more closely.
    int fieldIndex = 0;
    if (interlaceMode == 3) {
        fieldIndex = oddFrame ? 1 : 0;
    }
    if (!spriteLineValid[fieldIndex]) {
        parseSprites(line, spriteLineCache[fieldIndex], spriteLineCount[fieldIndex]);
        spriteLineValid[fieldIndex] = true;
    }

    SpriteEntry* sprites = spriteLineCache[fieldIndex];
    int spriteCount = spriteLineCount[fieldIndex];

    // Track which pixels already have a sprite for collision detection
    std::fill_n(spritePixelBuffer + startX, endX - startX, false);

    bool im2 = (interlaceMode == 3);
    int effectiveLine = im2 ? (line * 2 + (oddFrame ? 1 : 0)) : line;

    // Draw later SAT entries first so lower indices appear in front.
    for (int i = spriteCount - 1; i >= 0; i--) {
        SpriteEntry& spr = sprites[i];

        int spriteY = effectiveLine - spr.ypos;
        int totalHeight = spr.height * (im2 ? 16 : 8);
        if (spr.vflip) {
            spriteY = (totalHeight - 1) - spriteY;
        }

        int tileRow, py;
        if (im2) {
            tileRow = spriteY >> 4;           // 16-pixel cells
            py = spriteY & 15;               // 0-15 sequential within 64-byte tile
        } else {
            tileRow = spriteY >> 3;
            py = spriteY & 7;
        }

        int patternSize = im2 ? 64 : 32;
        int colStride = spr.height;

        for (int tileCol = 0; tileCol < spr.width; tileCol++) {
            int screenXBase = spr.xpos + (tileCol * 8);
            if (screenXBase >= activeWidth || screenXBase + 7 < 0) {
                continue;
            }

            // Sprite patterns are laid out column-major (top-to-bottom, then left-to-right).
            int sourceTileCol = spr.hflip ? (spr.width - 1 - tileCol) : tileCol;
            int tileIndex = spr.pattern + tileRow + (sourceTileCol * colStride);
            u8 rowBytes[4];
            loadPatternRow(vram, static_cast<u16>(((tileIndex & 0x07FF) * patternSize) + (py * 4)), rowBytes);
            if (rowIsTransparent(rowBytes)) {
                continue;
            }

            for (int localX = 0; localX < 8; localX++) {
                int screenX = screenXBase + localX;
                if (screenX < startX || screenX >= endX) {
                    continue;
                }

                int tileX = spr.hflip ? (7 - localX) : localX;
                int colorIndex = tilePixelFromRow(rowBytes, tileX);
                if (colorIndex == 0) {
                    continue;
                }

                // Sprite collision: two non-transparent sprite pixels overlap
                if (spritePixelBuffer[screenX]) {
                    spriteCollision = true;
                }
                spritePixelBuffer[screenX] = true;

                // Shadow/Highlight operators: palette 3, colors 14 and 15
                if (shadowHighlightEnabled && spr.palette == 3) {
                    if (colorIndex == 14) {
                        // Highlight operator: mark pixel for highlight, don't draw sprite
                        lineBuffer[screenX] |= 0x8000;
                        continue;
                    } else if (colorIndex == 15) {
                        // Shadow operator: mark pixel for shadow, don't draw sprite
                        lineBuffer[screenX] |= 0x4000;
                        continue;
                    }
                }

                int rank = spr.priority ? kSpriteHighRank : kSpriteLowRank;
                if (rank >= rankBuffer[screenX]) {
                    // Clear S/H flags when sprite pixel replaces
                    lineBuffer[screenX] = static_cast<u16>((spr.palette * 16) + colorIndex);
                    rankBuffer[screenX] = static_cast<u8>(rank);
                }
            }
        }
    }
}
