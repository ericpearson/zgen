// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <filesystem>
#include <string>

// Set a custom config directory (overrides ~/.genesis).
// Must be called before any getConfigDir() calls (e.g. from main() CLI parsing).
void setConfigDir(const std::string& path);

// Returns the config directory path, creating it if necessary.
// Uses the override if set, otherwise ~/.genesis (or %APPDATA%/.genesis on Windows).
std::filesystem::path getConfigDir();
