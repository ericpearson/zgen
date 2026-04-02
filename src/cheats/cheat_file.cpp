// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "cheats/cheat_file.h"

#include "cheats/cheat_format.h"
#include <fstream>
#include <sstream>

namespace Cheats {

namespace {

std::string escapeField(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c; break;
        }
    }
    return out;
}

bool unescapeField(const std::string& text, std::string& out) {
    out.clear();
    out.reserve(text.size());
    bool escaping = false;
    for (char c : text) {
        if (!escaping) {
            if (c == '\\') {
                escaping = true;
            } else {
                out += c;
            }
            continue;
        }

        switch (c) {
            case '\\': out += '\\'; break;
            case 't': out += '\t'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            default:
                return false;
        }
        escaping = false;
    }
    return !escaping;
}

} // namespace

bool saveCheatFile(const std::filesystem::path& path,
                   const std::vector<CheatEntry>& cheats,
                   std::string* errorText) {
    if (errorText) errorText->clear();

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        if (errorText) *errorText = "Failed to open cheat file for writing";
        return false;
    }

    file << "# Genesis cheat file v1\n";
    for (const CheatEntry& cheat : cheats) {
        file << (cheat.enabled ? '1' : '0')
             << '\t'
             << cheat.code
             << '\t'
             << escapeField(cheat.name)
             << '\n';
    }

    if (!file.good()) {
        if (errorText) *errorText = "Failed while writing cheat file";
        return false;
    }
    return true;
}

bool loadCheatFile(const std::filesystem::path& path,
                   std::vector<CheatEntry>& cheats,
                   std::string* errorText) {
    if (errorText) errorText->clear();
    cheats.clear();

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return true;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        if (errorText) *errorText = "Failed to open cheat file for reading";
        return false;
    }

    std::string line;
    int lineNumber = 0;
    while (std::getline(file, line)) {
        lineNumber++;
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const size_t firstTab = line.find('\t');
        const size_t secondTab = firstTab == std::string::npos ? std::string::npos : line.find('\t', firstTab + 1);
        if (firstTab == std::string::npos || secondTab == std::string::npos) {
            if (errorText) *errorText = "Malformed cheat file line " + std::to_string(lineNumber);
            return false;
        }

        const std::string enabledText = line.substr(0, firstTab);
        const std::string codeText = line.substr(firstTab + 1, secondTab - firstTab - 1);
        const std::string nameText = line.substr(secondTab + 1);

        CheatEntry cheat;
        std::string parseError;
        if (!parseRawCheatCode(codeText, cheat, &parseError)) {
            if (errorText) {
                *errorText = "Invalid cheat on line " + std::to_string(lineNumber) + ": " + parseError;
            }
            return false;
        }

        if (enabledText == "1") {
            cheat.enabled = true;
        } else if (enabledText == "0") {
            cheat.enabled = false;
        } else {
            if (errorText) *errorText = "Invalid enabled flag on line " + std::to_string(lineNumber);
            return false;
        }

        if (!unescapeField(nameText, cheat.name)) {
            if (errorText) *errorText = "Invalid escape sequence on line " + std::to_string(lineNumber);
            return false;
        }

        cheats.push_back(cheat);
    }

    if (!file.eof() && file.fail()) {
        if (errorText) *errorText = "Failed while reading cheat file";
        return false;
    }

    return true;
}

} // namespace Cheats
