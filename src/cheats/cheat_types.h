// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "types.h"
#include <string>

namespace Cheats {

enum class CheatValueType {
    U8,
    S8,
    U16,
    S16,
    U32,
    S32,
};

enum class CheatMode {
    Freeze,
};

enum class SearchRegionKind {
    MainRam,
    Z80Ram,
    SRAM,
};

enum class SearchCompareMode {
    Changed,
    Unchanged,
    Increased,
    Decreased,
    EqualToValue,
    NotEqualToValue,
};

enum class SearchHeuristic {
    Off,
    BooleanLike,
    Tolerance1,
    Tolerance2,
};

struct MemoryRange {
    u32 start = 0;
    u32 end = 0; // inclusive
    SearchRegionKind region = SearchRegionKind::MainRam;
};

struct CheatEntry {
    u32 address = 0;
    u32 value = 0;
    CheatValueType type = CheatValueType::U16;
    CheatMode mode = CheatMode::Freeze;
    bool enabled = true;
    std::string code;
    std::string name;
};

struct SearchAddress {
    u32 address = 0;
    SearchRegionKind region = SearchRegionKind::MainRam;
};

struct SearchCandidate {
    SearchAddress where;
    u32 previousValue = 0;
    u32 currentValue = 0;
};

const char* valueTypeLabel(CheatValueType type);
const char* regionLabel(SearchRegionKind region);
int valueTypeByteWidth(CheatValueType type);
bool isSignedType(CheatValueType type);
s32 reinterpretSigned(u32 rawValue, CheatValueType type);
std::string formatValueDecimal(u32 rawValue, CheatValueType type);
std::string formatValueHex(u32 rawValue, CheatValueType type);

} // namespace Cheats
