// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "cheats/ram_search.h"

#include <algorithm>
#include <cstdlib>

namespace Cheats {

namespace {

bool addressFitsRange(u32 address, int byteWidth, const MemoryRange& range) {
    if (address < range.start || address > range.end) {
        return false;
    }
    const u32 last = address + static_cast<u32>(byteWidth - 1);
    return last >= address && last <= range.end;
}

bool isAddressStepValid(u32 address, CheatValueType type) {
    switch (type) {
        case CheatValueType::U8:
        case CheatValueType::S8:
            return true;
        case CheatValueType::U16:
        case CheatValueType::S16:
        case CheatValueType::U32:
        case CheatValueType::S32:
            return (address & 1u) == 0;
    }
    return false;
}

} // namespace

void enumerateSearchAddresses(const std::vector<MemoryRange>& ranges,
                              CheatValueType type,
                              std::vector<SearchAddress>& out) {
    out.clear();
    const int width = valueTypeByteWidth(type);
    for (const MemoryRange& range : ranges) {
        if (range.end < range.start) {
            continue;
        }
        u32 step = (width == 1) ? 1u : 2u;
        u32 start = range.start;
        if (step == 2u && (start & 1u)) {
            start++;
        }
        for (u32 address = start; address <= range.end; ) {
            if (addressFitsRange(address, width, range) && isAddressStepValid(address, type)) {
                out.push_back({address, range.region});
            }
            if (range.end - address < step) {
                break;
            }
            address += step;
        }
    }
}

bool readTypedValue(const CheatMemoryInterface& memory,
                    u32 address,
                    CheatValueType type,
                    u32& outRawValue) {
    switch (type) {
        case CheatValueType::U8:
        case CheatValueType::S8: {
            u8 value = 0;
            if (!memory.read8(address, value)) return false;
            outRawValue = value;
            return true;
        }
        case CheatValueType::U16:
        case CheatValueType::S16: {
            u16 value = 0;
            if (!memory.read16(address, value)) return false;
            outRawValue = value;
            return true;
        }
        case CheatValueType::U32:
        case CheatValueType::S32: {
            u16 hi = 0;
            u16 lo = 0;
            if (!memory.read16(address, hi) || !memory.read16(address + 2, lo)) return false;
            outRawValue = (static_cast<u32>(hi) << 16) | static_cast<u32>(lo);
            return true;
        }
    }
    return false;
}

bool writeTypedValue(CheatMemoryInterface& memory,
                     u32 address,
                     CheatValueType type,
                     u32 rawValue) {
    switch (type) {
        case CheatValueType::U8:
        case CheatValueType::S8:
            return memory.write8(address, static_cast<u8>(rawValue & 0xFFu));
        case CheatValueType::U16:
        case CheatValueType::S16:
            return memory.write16(address, static_cast<u16>(rawValue & 0xFFFFu));
        case CheatValueType::U32:
        case CheatValueType::S32:
            return memory.write16(address, static_cast<u16>((rawValue >> 16) & 0xFFFFu)) &&
                   memory.write16(address + 2, static_cast<u16>(rawValue & 0xFFFFu));
    }
    return false;
}

bool matchesTarget(u32 rawValue,
                   s32 targetValue,
                   CheatValueType type,
                   SearchHeuristic heuristic) {
    const s64 currentValue = isSignedType(type)
        ? reinterpretSigned(rawValue, type)
        : static_cast<s64>(rawValue & ((valueTypeByteWidth(type) == 1) ? 0xFFu :
                                       (valueTypeByteWidth(type) == 2) ? 0xFFFFu : 0xFFFFFFFFu));
    const s64 target = static_cast<s64>(targetValue);

    switch (heuristic) {
        case SearchHeuristic::Off:
            return currentValue == target;
        case SearchHeuristic::BooleanLike:
            if (target == 0 || target == 1) {
                return currentValue == 0 || currentValue == 1;
            }
            if (target == 2 || target == 3) {
                return currentValue >= (target - 1) && currentValue <= target;
            }
            return currentValue == target;
        case SearchHeuristic::Tolerance1:
            return std::llabs(currentValue - target) <= 1;
        case SearchHeuristic::Tolerance2:
            return std::llabs(currentValue - target) <= 2;
    }
    return false;
}

bool compareValues(u32 previousValue,
                   u32 currentValue,
                   CheatValueType type,
                   SearchCompareMode mode,
                   const std::optional<s32>& compareValue,
                   SearchHeuristic heuristic) {
    const auto valueOf = [&](u32 raw) -> s64 {
        return isSignedType(type)
            ? static_cast<s64>(reinterpretSigned(raw, type))
            : static_cast<s64>(raw & ((valueTypeByteWidth(type) == 1) ? 0xFFu :
                                       (valueTypeByteWidth(type) == 2) ? 0xFFFFu : 0xFFFFFFFFu));
    };

    switch (mode) {
        case SearchCompareMode::Changed:
            return valueOf(currentValue) != valueOf(previousValue);
        case SearchCompareMode::Unchanged:
            return valueOf(currentValue) == valueOf(previousValue);
        case SearchCompareMode::Increased:
            return valueOf(currentValue) > valueOf(previousValue);
        case SearchCompareMode::Decreased:
            return valueOf(currentValue) < valueOf(previousValue);
        case SearchCompareMode::EqualToValue:
            return compareValue.has_value() && matchesTarget(currentValue, *compareValue, type, heuristic);
        case SearchCompareMode::NotEqualToValue:
            return compareValue.has_value() && !matchesTarget(currentValue, *compareValue, type, heuristic);
    }
    return false;
}

void RamSearchSession::reset() {
    active_ = false;
    type_ = CheatValueType::U16;
    addresses_.clear();
    baseline_.clear();
    current_.clear();
}

bool RamSearchSession::startKnownValue(CheatMemoryInterface& memory,
                                       CheatValueType type,
                                       s32 targetValue,
                                       SearchHeuristic heuristic) {
    std::vector<MemoryRange> ranges;
    memory.enumerateWritableRanges(ranges);

    std::vector<SearchAddress> addresses;
    enumerateSearchAddresses(ranges, type, addresses);
    if (addresses.empty()) {
        reset();
        return false;
    }

    std::vector<SearchAddress> filteredAddresses;
    std::vector<u32> values;
    filteredAddresses.reserve(addresses.size());
    values.reserve(addresses.size());

    for (const SearchAddress& candidate : addresses) {
        u32 rawValue = 0;
        if (!readTypedValue(memory, candidate.address, type, rawValue)) {
            continue;
        }
        if (matchesTarget(rawValue, targetValue, type, heuristic)) {
            filteredAddresses.push_back(candidate);
            values.push_back(rawValue);
        }
    }

    active_ = true;
    type_ = type;
    addresses_ = std::move(filteredAddresses);
    baseline_ = values;
    current_ = values;
    return true;
}

bool RamSearchSession::startUnknown(CheatMemoryInterface& memory,
                                    CheatValueType type) {
    std::vector<MemoryRange> ranges;
    memory.enumerateWritableRanges(ranges);

    std::vector<SearchAddress> addresses;
    enumerateSearchAddresses(ranges, type, addresses);
    if (addresses.empty()) {
        reset();
        return false;
    }

    std::vector<u32> values;
    values.reserve(addresses.size());
    for (const SearchAddress& candidate : addresses) {
        u32 rawValue = 0;
        if (!readTypedValue(memory, candidate.address, type, rawValue)) {
            continue;
        }
        values.push_back(rawValue);
    }

    if (values.size() != addresses.size()) {
        reset();
        return false;
    }

    active_ = true;
    type_ = type;
    addresses_ = std::move(addresses);
    baseline_ = values;
    current_ = values;
    return true;
}

bool RamSearchSession::refine(CheatMemoryInterface& memory,
                              SearchCompareMode mode,
                              std::optional<s32> compareValue,
                              SearchHeuristic heuristic) {
    if (!active_) {
        return false;
    }

    std::vector<SearchAddress> filteredAddresses;
    std::vector<u32> filteredBaseline;
    std::vector<u32> filteredCurrent;
    filteredAddresses.reserve(addresses_.size());
    filteredBaseline.reserve(addresses_.size());
    filteredCurrent.reserve(addresses_.size());

    for (size_t i = 0; i < addresses_.size(); i++) {
        u32 rawValue = 0;
        if (!readTypedValue(memory, addresses_[i].address, type_, rawValue)) {
            continue;
        }
        current_[i] = rawValue;
        if (compareValues(baseline_[i], current_[i], type_, mode, compareValue, heuristic)) {
            filteredAddresses.push_back(addresses_[i]);
            filteredBaseline.push_back(current_[i]);
            filteredCurrent.push_back(current_[i]);
        }
    }

    addresses_ = std::move(filteredAddresses);
    baseline_ = std::move(filteredBaseline);
    current_ = std::move(filteredCurrent);
    return true;
}

bool RamSearchSession::getResult(size_t index, SearchCandidate& out) const {
    if (index >= addresses_.size()) {
        return false;
    }
    out.where = addresses_[index];
    out.previousValue = baseline_[index];
    out.currentValue = current_[index];
    return true;
}

void RamSearchSession::getResults(size_t offset, size_t limit, std::vector<SearchCandidate>& out) const {
    out.clear();
    if (offset >= addresses_.size()) {
        return;
    }
    const size_t end = std::min(addresses_.size(), offset + limit);
    out.reserve(end - offset);
    for (size_t i = offset; i < end; i++) {
        SearchCandidate candidate;
        candidate.where = addresses_[i];
        candidate.previousValue = baseline_[i];
        candidate.currentValue = current_[i];
        out.push_back(candidate);
    }
}

bool RamSearchSession::getResultAddress(size_t index, SearchAddress& out) const {
    if (index >= addresses_.size()) {
        return false;
    }
    out = addresses_[index];
    return true;
}

} // namespace Cheats
