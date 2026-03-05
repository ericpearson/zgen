// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "cheats/cheat_types.h"
#include <string>

namespace Cheats {

bool parseRawCheatCode(const std::string& input, CheatEntry& outEntry, std::string* errorText = nullptr);
std::string formatRawCheatCode(u32 address, u32 value, CheatValueType type);
bool parseSearchValueText(const std::string& input, s32& outValue);

} // namespace Cheats
