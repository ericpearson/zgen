// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "config_dir.h"
#include <cstdlib>

static std::string configDirOverride;

void setConfigDir(const std::string& path) {
    configDirOverride = path;
}

std::filesystem::path getConfigDir() {
    if (!configDirOverride.empty()) {
        std::filesystem::path dir(configDirOverride);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }
#ifdef _WIN32
    const char* home = std::getenv("APPDATA");
    if (!home) home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (!home) home = ".";
    std::filesystem::path dir = std::filesystem::path(home) / ".genesis";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}
