// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "cheats/cheat_types.h"
#include <cstddef>
#include <optional>
#include <vector>

namespace Cheats {

class CheatMemoryInterface {
public:
    virtual ~CheatMemoryInterface() = default;

    virtual bool read8(u32 address, u8& out) const = 0;
    virtual bool read16(u32 address, u16& out) const = 0;
    virtual bool write8(u32 address, u8 value) = 0;
    virtual bool write16(u32 address, u16 value) = 0;
    virtual void enumerateWritableRanges(std::vector<MemoryRange>& out) const = 0;
};

class RamSearchSession {
public:
    void reset();

    bool startKnownValue(CheatMemoryInterface& memory,
                         CheatValueType type,
                         s32 targetValue,
                         SearchHeuristic heuristic);

    bool startUnknown(CheatMemoryInterface& memory,
                      CheatValueType type);

    bool refine(CheatMemoryInterface& memory,
                SearchCompareMode mode,
                std::optional<s32> compareValue,
                SearchHeuristic heuristic);

    size_t resultCount() const { return addresses_.size(); }
    bool getResult(size_t index, SearchCandidate& out) const;
    void getResults(size_t offset, size_t limit, std::vector<SearchCandidate>& out) const;

    bool active() const { return active_; }
    CheatValueType valueType() const { return type_; }
    bool getResultAddress(size_t index, SearchAddress& out) const;

private:
    bool active_ = false;
    CheatValueType type_ = CheatValueType::U16;
    std::vector<SearchAddress> addresses_;
    std::vector<u32> baseline_;
    std::vector<u32> current_;
};

void enumerateSearchAddresses(const std::vector<MemoryRange>& ranges,
                              CheatValueType type,
                              std::vector<SearchAddress>& out);

bool readTypedValue(const CheatMemoryInterface& memory,
                    u32 address,
                    CheatValueType type,
                    u32& outRawValue);

bool writeTypedValue(CheatMemoryInterface& memory,
                     u32 address,
                     CheatValueType type,
                     u32 rawValue);

bool matchesTarget(u32 rawValue,
                   s32 targetValue,
                   CheatValueType type,
                   SearchHeuristic heuristic);

bool compareValues(u32 previousValue,
                   u32 currentValue,
                   CheatValueType type,
                   SearchCompareMode mode,
                   const std::optional<s32>& compareValue,
                   SearchHeuristic heuristic);

} // namespace Cheats
