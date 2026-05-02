// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "types.h"
#include <string>
#include <vector>

struct ROMHeader {
    char systemType[16];      // $100-$10F: "SEGA MEGA DRIVE" or "SEGA GENESIS"
    char copyright[16];       // $110-$11F: "(C)XXXX YYYY.ZZZ"
    char domesticName[48];    // $120-$14F: Japanese title
    char overseasName[48];    // $150-$17F: International title
    char serialNumber[14];    // $180-$18D: "GM XXXXXXXX-XX"
    u16 checksum;             // $18E-$18F: ROM checksum
    char deviceSupport[16];   // $190-$19F: Controller support
    u32 romStart;             // $1A0-$1A3: ROM start address
    u32 romEnd;               // $1A4-$1A7: ROM end address
    u32 ramStart;             // $1A8-$1AB: RAM start address
    u32 ramEnd;               // $1AC-$1AF: RAM end address
    char extraMemory[12];     // $1B0-$1BB: SRAM info
    char modem[12];           // $1BC-$1C7: Modem support
    char reserved[40];        // $1C8-$1EF: Reserved
    char region[3];           // $1F0-$1F2: Region codes
    char reserved2[13];       // $1F3-$1FF: Reserved
};

class Cartridge {
public:
    Cartridge();
    ~Cartridge();
    
    bool load(const char* filename);
    void unload();
    
    u8 read8(u32 addr) const;
    u16 read16(u32 addr) const;
    u8 readSRAM8(u32 addr) const;
    u16 readSRAM16(u32 addr) const;
    void write8(u32 addr, u8 val);
    void write16(u32 addr, u16 val);
    void writeSRAM8(u32 addr, u8 val);
    void writeSRAM16(u32 addr, u16 val);
    
    bool isLoaded() const { return !rom.empty(); }
    u32 size() const { return rom.size(); }
    
    const ROMHeader& getHeader() const { return header; }
    std::string getGameName() const;
    std::string getRegion() const;
    VideoStandard preferredVideoStandard() const;
    bool hasSRAM() const { return sramEnabled; }
    bool isSRAMAddress(u32 addr) const;
    bool isDirectMappedSRAMAddress(u32 addr) const;
    
    void printInfo() const;
    
private:
    std::vector<u8> rom;
    ROMHeader header;
    
    // SRAM
    bool sramEnabled;
    u32 sramStart;
    u32 sramEnd;
    u8 sramBusFlags;
    std::vector<u8> sram;
    
    void parseHeader();
    bool detectSMD();
    void convertSMD();
    u16 calculateChecksum() const;
    bool sramByteAccessible(u32 addr) const;
    u32 sramByteOffset(u32 addr) const;
};
