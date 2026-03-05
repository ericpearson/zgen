// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

enum class VideoStandard {
    NTSC = 0,
    PAL = 1,
};

enum class VideoStandardMode {
    Auto = 0,
    NTSC = 1,
    PAL = 2,
};

constexpr u8 BIT0 = 0x01;
constexpr u8 BIT1 = 0x02;
constexpr u8 BIT2 = 0x04;
constexpr u8 BIT3 = 0x08;
constexpr u8 BIT4 = 0x10;
constexpr u8 BIT5 = 0x20;
constexpr u8 BIT6 = 0x40;
constexpr u8 BIT7 = 0x80;

inline bool testBit(u8 val, int bit) { return (val >> bit) & 1; }
inline u8 setBit(u8 val, int bit) { return val | (1 << bit); }
inline u8 clearBit(u8 val, int bit) { return val & ~(1 << bit); }

inline u16 makeWord(u8 high, u8 low) { return (high << 8) | low; }
inline u8 highByte(u16 val) { return val >> 8; }
inline u8 lowByte(u16 val) { return val & 0xFF; }

inline u32 makeLong(u16 high, u16 low) { return (static_cast<u32>(high) << 16) | low; }
inline u16 highWord(u32 val) { return val >> 16; }
inline u16 lowWord(u32 val) { return val & 0xFFFF; }
