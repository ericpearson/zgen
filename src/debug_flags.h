// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Global debug mode flag. When false, all GENESIS_LOG_* environment variable
// checks short-circuit to false, eliminating logging overhead entirely.
// Set to true by passing --debug on the command line.
extern bool g_debugMode;
