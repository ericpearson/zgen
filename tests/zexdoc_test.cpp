// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

// ZEXDOC Z80 conformance test harness
// Loads zexdoc.com (CP/M .com file) and runs it against our Z80 core
// with minimal CP/M BDOS emulation for console output.

#include "cpu/z80.h"
#include "memory/bus.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

// Flat 64KB memory for standalone Z80 testing
static u8 memory[65536];

// --- Minimal Bus stubs (only z80Read/z80Write are used) ---
Bus::Bus() : z80Bank(0), z80BusRequested(false), z80Reset(false),
             m68k(nullptr), z80(nullptr), vdp(nullptr), psg(nullptr),
             ym2612(nullptr), cartridge(nullptr), controllers{nullptr, nullptr},
             rom(nullptr), romSize(0), sramMapped(false) {
    memset(ram, 0, sizeof(ram));
    memset(z80Ram, 0, sizeof(z80Ram));
    memset(ioData, 0, sizeof(ioData));
    memset(ioCtrl, 0, sizeof(ioCtrl));
    memset(padState, 0, sizeof(padState));
    memset(padState6, 0, sizeof(padState6));
    memset(thCounter, 0, sizeof(thCounter));
    memset(prevTH, 0, sizeof(prevTH));
    memset(romBankRegs, 0, sizeof(romBankRegs));
}
Bus::~Bus() {}
void Bus::reset() {}
u8 Bus::z80Read(u16 addr) { return memory[addr]; }
void Bus::z80Write(u16 addr, u8 val) { memory[addr] = val; }
u8 Bus::read8(u32) { return 0; }
u16 Bus::read16(u32) { return 0; }
void Bus::write8(u32, u8) {}
void Bus::write16(u32, u16) {}
bool Bus::loadROM(const char*) { return false; }
void Bus::setButtonState(int, int, bool) {}
void Bus::resetControllerCounters() {}
bool Bus::isM68KBusStalled() const { return false; }
u8 Bus::readControllerData(int) { return 0xFF; }

// --- Test harness ---
static bool loadCOM(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 65536 - 0x100) {
        fprintf(stderr, "File too large: %ld bytes\n", size);
        fclose(f);
        return false;
    }
    // CP/M loads .com files at 0x0100
    fread(&memory[0x100], 1, size, f);
    fclose(f);
    return true;
}

int main(int argc, char* argv[]) {
    const char* comFile = nullptr;

    if (argc >= 2) {
        comFile = argv[1];
    } else {
        // Try default paths
        const char* defaults[] = {
            "tests/zexdoc.com",
            "../tests/zexdoc.com",
            "zexdoc.com",
            nullptr
        };
        for (int i = 0; defaults[i]; i++) {
            FILE* f = fopen(defaults[i], "rb");
            if (f) { fclose(f); comFile = defaults[i]; break; }
        }
        if (!comFile) {
            fprintf(stderr, "Usage: %s <zexdoc.com>\n", argv[0]);
            return 1;
        }
    }

    memset(memory, 0, sizeof(memory));

    if (!loadCOM(comFile)) return 1;

    // CP/M warm boot at 0x0000: RET (we'll check PC == 0 to detect exit)
    memory[0x0000] = 0xC9; // RET (shouldn't reach here normally)

    // CP/M BDOS entry at 0x0005: RET (we intercept before execution)
    memory[0x0005] = 0xC9;

    // Set up initial SP (CP/M default TPA top)
    // Push 0x0000 as return address so RET from .com returns to warm boot
    memory[0xFFFE] = 0x00;
    memory[0xFFFF] = 0x00;

    Bus bus;
    Z80 z80;
    z80.setBus(&bus);
    z80.reset();

    z80.state.pc = 0x0100;
    z80.state.sp = 0xFFFE;

    long long totalCycles = 0;
    int testsCompleted = 0;
    bool running = true;
    bool allPassed = true;

    // Buffer to detect pass/fail in output
    char lineBuf[256];
    int linePos = 0;

    while (running) {
        // Intercept CP/M BDOS call at 0x0005
        if (z80.state.pc == 0x0005) {
            u8 func = z80.state.c;
            switch (func) {
                case 2: // C_WRITE: output character in E
                    putchar(z80.state.e);
                    if (z80.state.e == '\n') {
                        fflush(stdout);
                        lineBuf[linePos] = '\0';
                        if (strstr(lineBuf, "ERROR")) allPassed = false;
                        if (strstr(lineBuf, "OK") || strstr(lineBuf, "ERROR"))
                            testsCompleted++;
                        linePos = 0;
                    } else if (linePos < (int)sizeof(lineBuf) - 1) {
                        lineBuf[linePos++] = z80.state.e;
                    }
                    break;
                case 9: { // C_WRITESTR: output '$'-terminated string at DE
                    u16 addr = z80.getDE();
                    for (int i = 0; i < 4096; i++) {
                        u8 ch = memory[addr + i];
                        if (ch == '$') break;
                        putchar(ch);
                        if (ch == '\n') {
                            fflush(stdout);
                            lineBuf[linePos] = '\0';
                            if (strstr(lineBuf, "ERROR")) allPassed = false;
                            if (strstr(lineBuf, "OK") || strstr(lineBuf, "ERROR"))
                                testsCompleted++;
                            linePos = 0;
                        } else if (linePos < (int)sizeof(lineBuf) - 1) {
                            lineBuf[linePos++] = ch;
                        }
                    }
                    break;
                }
            }
            // Execute the RET at 0x0005 to return to caller
            z80.execute();
            continue;
        }

        // Detect warm boot (return to 0x0000) = test suite finished
        if (z80.state.pc == 0x0000) {
            running = false;
            break;
        }

        int cycles = z80.execute();
        totalCycles += cycles;

        // Safety: bail after ~100 billion cycles (way more than needed)
        if (totalCycles > 100000000000LL) {
            fprintf(stderr, "\nTIMEOUT after %lld cycles\n", totalCycles);
            running = false;
            allPassed = false;
        }
    }

    printf("\n--- ZEXDOC Summary ---\n");
    printf("Tests completed: %d\n", testsCompleted);
    printf("Total cycles: %lld\n", totalCycles);
    printf("Result: %s\n", allPassed ? "ALL PASSED" : "FAILURES DETECTED");

    return allPassed ? 0 : 1;
}
