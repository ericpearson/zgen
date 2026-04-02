// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "cheats/cheat_types.h"
#include <filesystem>
#include <string>
#include <vector>

namespace Cheats {

bool saveCheatFile(const std::filesystem::path& path,
                   const std::vector<CheatEntry>& cheats,
                   std::string* errorText = nullptr);

bool loadCheatFile(const std::filesystem::path& path,
                   std::vector<CheatEntry>& cheats,
                   std::string* errorText = nullptr);

} // namespace Cheats
