// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "memory/cartridge.h"
#include <cstdio>
#include <cctype>
#include <cstring>
#include <algorithm>

Cartridge::Cartridge() : sramEnabled(false), sramStart(0), sramEnd(0) {
    std::memset(&header, 0, sizeof(header));
}

Cartridge::~Cartridge() {
    unload();
}

bool Cartridge::load(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) {
        printf("Error: Could not open file: %s\n", filename);
        return false;
    }
    
    fseek(f, 0, SEEK_END);
    size_t fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (fileSize < 0x200) {
        printf("Error: File too small to be a valid ROM\n");
        fclose(f);
        return false;
    }
    
    rom.resize(fileSize);
    size_t bytesRead = fread(rom.data(), 1, fileSize, f);
    fclose(f);
    
    if (bytesRead != fileSize) {
        printf("Error: Could not read entire file\n");
        rom.clear();
        return false;
    }
    
    // Check for SMD format and convert if needed
    if (detectSMD()) {
        printf("Detected SMD format, converting...\n");
        convertSMD();
    }

    // A second ROM load must not inherit any cartridge-side state from the
    // previous image.
    std::memset(&header, 0, sizeof(header));
    sramEnabled = false;
    sramStart = 0;
    sramEnd = 0;
    sram.clear();
    
    // Parse the header
    parseHeader();
    
    // Verify checksum (optional, just warn)
    u16 calcSum = calculateChecksum();
    if (calcSum != header.checksum) {
        printf("Warning: Checksum mismatch (header: %04X, calculated: %04X)\n", 
               header.checksum, calcSum);
    }
    
    printf("ROM loaded successfully: %zu bytes\n", rom.size());
    printInfo();
    
    return true;
}

void Cartridge::unload() {
    rom.clear();
    sram.clear();
    sramEnabled = false;
    std::memset(&header, 0, sizeof(header));
}

void Cartridge::parseHeader() {
    if (rom.size() < 0x200) return;
    
    // Copy header data
    std::memcpy(header.systemType, &rom[0x100], 16);
    std::memcpy(header.copyright, &rom[0x110], 16);
    std::memcpy(header.domesticName, &rom[0x120], 48);
    std::memcpy(header.overseasName, &rom[0x150], 48);
    std::memcpy(header.serialNumber, &rom[0x180], 14);
    
    header.checksum = (rom[0x18E] << 8) | rom[0x18F];
    
    std::memcpy(header.deviceSupport, &rom[0x190], 16);
    
    header.romStart = (rom[0x1A0] << 24) | (rom[0x1A1] << 16) | 
                      (rom[0x1A2] << 8) | rom[0x1A3];
    header.romEnd = (rom[0x1A4] << 24) | (rom[0x1A5] << 16) | 
                    (rom[0x1A6] << 8) | rom[0x1A7];
    header.ramStart = (rom[0x1A8] << 24) | (rom[0x1A9] << 16) | 
                      (rom[0x1AA] << 8) | rom[0x1AB];
    header.ramEnd = (rom[0x1AC] << 24) | (rom[0x1AD] << 16) | 
                    (rom[0x1AE] << 8) | rom[0x1AF];
    
    std::memcpy(header.extraMemory, &rom[0x1B0], 12);
    std::memcpy(header.region, &rom[0x1F0], 3);
    
    // Check for SRAM
    if (header.extraMemory[0] == 'R' && header.extraMemory[1] == 'A') {
        sramEnabled = true;
        sramStart = (rom[0x1B4] << 24) | (rom[0x1B5] << 16) | 
                    (rom[0x1B6] << 8) | rom[0x1B7];
        sramEnd = (rom[0x1B8] << 24) | (rom[0x1B9] << 16) | 
                  (rom[0x1BA] << 8) | rom[0x1BB];
        
        u32 sramSize = sramEnd - sramStart + 1;
        sram.resize(sramSize, 0xFF);
        printf("SRAM enabled: %08X-%08X (%u bytes)\n", sramStart, sramEnd, sramSize);
    }
}

bool Cartridge::detectSMD() {
    // SMD format has a 512-byte header
    // Check for typical SMD markers
    if (rom.size() < 512) return false;
    
    // SMD files have specific markers
    // Byte 8 is usually 0xAA, byte 9 is 0xBB
    if (rom[8] == 0xAA && rom[9] == 0xBB) {
        return true;
    }
    
    // Also check if first 16 bytes of ROM area don't look like 68K vectors
    // Valid 68K vectors start with stack pointer and reset vector
    // If they don't look valid, might be SMD
    if (rom.size() > 512) {
        u32 sp = (rom[0] << 24) | (rom[1] << 16) | (rom[2] << 8) | rom[3];
        u32 pc = (rom[4] << 24) | (rom[5] << 16) | (rom[6] << 8) | rom[7];
        
        // Valid Genesis ROMs have SP in RAM ($FF0000-$FFFFFF) and PC in ROM ($000000-$3FFFFF)
        bool validVectors = (sp >= 0xFF0000 && sp <= 0xFFFFFF) && 
                           (pc >= 0x000000 && pc <= 0x3FFFFF);
        
        if (!validVectors && rom[8] != 0xEA) {
            // Could be SMD, check after 512-byte header
            u32 sp2 = (rom[512] << 24) | (rom[513] << 16) | (rom[514] << 8) | rom[515];
            u32 pc2 = (rom[516] << 24) | (rom[517] << 16) | (rom[518] << 8) | rom[519];
            
            if ((sp2 >= 0xFF0000 && sp2 <= 0xFFFFFF) && 
                (pc2 >= 0x000000 && pc2 <= 0x3FFFFF)) {
                return true;
            }
        }
    }
    
    return false;
}

void Cartridge::convertSMD() {
    // SMD format: 512-byte header + interleaved 16KB blocks
    // Each block has even bytes first, then odd bytes
    
    std::vector<u8> converted;
    size_t dataSize = rom.size() - 512;
    converted.resize(dataSize);
    
    size_t blockSize = 16384;
    size_t numBlocks = dataSize / blockSize;
    
    for (size_t block = 0; block < numBlocks; block++) {
        size_t srcOffset = 512 + block * blockSize;
        size_t dstOffset = block * blockSize;
        size_t halfBlock = blockSize / 2;
        
        for (size_t i = 0; i < halfBlock; i++) {
            converted[dstOffset + i * 2 + 1] = rom[srcOffset + i];           // Odd bytes
            converted[dstOffset + i * 2] = rom[srcOffset + halfBlock + i];   // Even bytes
        }
    }
    
    rom = std::move(converted);
}

u16 Cartridge::calculateChecksum() const {
    if (rom.size() < 0x200) return 0;
    
    u32 sum = 0;
    for (size_t i = 0x200; i < rom.size(); i += 2) {
        u16 word = (rom[i] << 8);
        if (i + 1 < rom.size()) {
            word |= rom[i + 1];
        }
        sum += word;
    }
    
    return sum & 0xFFFF;
}

u8 Cartridge::read8(u32 addr) const {
    if (addr < rom.size()) {
        return rom[addr];
    }
    
    // SRAM access
    if (sramEnabled && addr >= sramStart && addr <= sramEnd) {
        u32 sramAddr = addr - sramStart;
        if (sramAddr < sram.size()) {
            return sram[sramAddr];
        }
    }
    
    return 0xFF;
}

u16 Cartridge::read16(u32 addr) const {
    if (addr + 1 < rom.size()) {
        return (static_cast<u16>(rom[addr]) << 8) | rom[addr + 1];
    }

    if (sramEnabled && addr >= sramStart && addr < sramEnd) {
        u32 sramAddr = addr - sramStart;
        if (sramAddr + 1 < sram.size()) {
            return (static_cast<u16>(sram[sramAddr]) << 8) | sram[sramAddr + 1];
        }
    }

    return (read8(addr) << 8) | read8(addr + 1);
}

void Cartridge::write8(u32 addr, u8 val) {
    // SRAM write
    if (sramEnabled && addr >= sramStart && addr <= sramEnd) {
        u32 sramAddr = addr - sramStart;
        if (sramAddr < sram.size()) {
            sram[sramAddr] = val;
        }
    }
}

void Cartridge::write16(u32 addr, u16 val) {
    if (sramEnabled && addr >= sramStart && addr < sramEnd) {
        u32 sramAddr = addr - sramStart;
        if (sramAddr + 1 < sram.size()) {
            sram[sramAddr] = static_cast<u8>(val >> 8);
            sram[sramAddr + 1] = static_cast<u8>(val & 0xFF);
            return;
        }
    }

    write8(addr, val >> 8);
    write8(addr + 1, val & 0xFF);
}

std::string Cartridge::getGameName() const {
    // Use overseas name, trim trailing spaces
    std::string name(header.overseasName, 48);
    size_t end = name.find_last_not_of(' ');
    if (end != std::string::npos) {
        name.erase(end + 1);
    }
    return name;
}

std::string Cartridge::getRegion() const {
    std::string region;
    for (int i = 0; i < 3; i++) {
        char c = header.region[i];
        if (c == 'J') region += "Japan ";
        else if (c == 'U' || c == '4') region += "USA ";
        else if (c == 'E' || c == 'A') region += "Europe ";
    }
    if (region.empty()) region = "Unknown";
    return region;
}

VideoStandard Cartridge::preferredVideoStandard() const {
    bool ntsc = false;
    bool pal = false;

    for (char raw : header.region) {
        unsigned char uc = static_cast<unsigned char>(raw);
        if (uc == 0 || uc == ' ') {
            continue;
        }

        const char c = static_cast<char>(std::toupper(uc));
        bool handledAsLetter = false;
        switch (c) {
            case 'J':
            case 'U':
                ntsc = true;
                handledAsLetter = true;
                break;
            case 'E':
            case 'A':
                pal = true;
                handledAsLetter = true;
                break;
            default:
                break;
        }

        // Hex-digit region codes (used by newer games): bit 0=Japan, bit 2=USA, bit 3=Europe.
        // Only apply if the character wasn't already handled as a letter region code,
        // since 'E' and 'A' are both valid hex digits AND letter codes.
        if (!handledAsLetter && std::isxdigit(uc)) {
            int bits = 0;
            if (c >= '0' && c <= '9') {
                bits = c - '0';
            } else {
                bits = 10 + (c - 'A');
            }
            if (bits & 0x01) ntsc = true; // Japan
            if (bits & 0x04) ntsc = true; // USA
            if (bits & 0x08) pal = true;  // Europe
        }
    }

    if (pal && !ntsc) {
        return VideoStandard::PAL;
    }
    return VideoStandard::NTSC;
}

void Cartridge::printInfo() const {
    printf("\n=== ROM Information ===\n");
    printf("System: %.16s\n", header.systemType);
    printf("Copyright: %.16s\n", header.copyright);
    printf("Game (JP): %.48s\n", header.domesticName);
    printf("Game (US): %.48s\n", header.overseasName);
    printf("Serial: %.14s\n", header.serialNumber);
    printf("Checksum: %04X\n", header.checksum);
    printf("ROM: %08X - %08X (%u KB)\n", header.romStart, header.romEnd, 
           (header.romEnd - header.romStart + 1) / 1024);
    printf("Region: %s\n", getRegion().c_str());
    printf("=======================\n\n");
}
