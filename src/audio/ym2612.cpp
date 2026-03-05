// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "audio/ym2612.h"
#include <cstring>
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Log-sine table: 256 entries covering one quarter-wave
// Output is 12-bit (4.8 fixed-point) representing -log2(sin(x))
static u16 logsinTable[256];

// Exponential table: 256 entries, 11-bit output
// Converts from log domain back to linear: 2^(-x) in 0.11 fixed-point
static u16 expTable[256];

// Detune table: indexed by [detune 0-3][keycode 0-31]
// DT 4-7 are negatives of DT 0-3 (dt&3 indexes, dt&4 = negate)
static const s32 dt_tab[4][32] = {
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,8,8 },
    { 1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,16,16,16 },
    { 2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,17,19,20,22,22,22,22 },
};

// Hardware-accurate EG rate increment table (19 rows x 8 columns)
// Each row is selected by effective rate, columns by (egCounter >> shift) & 7
static const u8 eg_inc[19 * 8] = {
    // Rate 0-3: no increment
    0,0,0,0,0,0,0,0,  // row 0
    // Rates 4-47 (grouped by 4):
    0,1,0,1,0,1,0,1,  // row 1
    0,1,0,1,1,1,0,1,  // row 2
    0,1,1,1,0,1,1,1,  // row 3
    0,1,1,1,1,1,1,1,  // row 4
    // Rate 48-51:
    1,1,1,1,1,1,1,1,  // row 5
    1,1,1,2,1,1,1,2,  // row 6
    1,2,1,2,1,2,1,2,  // row 7
    1,2,2,2,1,2,2,2,  // row 8
    // Rate 52-55:
    2,2,2,2,2,2,2,2,  // row 9
    2,2,2,4,2,2,2,4,  // row 10
    2,4,2,4,2,4,2,4,  // row 11
    2,4,4,4,2,4,4,4,  // row 12
    // Rate 56-59:
    4,4,4,4,4,4,4,4,  // row 13
    4,4,4,8,4,4,4,8,  // row 14
    4,8,4,8,4,8,4,8,  // row 15
    4,8,8,8,4,8,8,8,  // row 16
    // Rate 60-63:
    8,8,8,8,8,8,8,8,  // row 17
    8,8,8,8,8,8,8,8,  // row 18
};

// Map effective rate (0-63) to shift amount for counter
static const u8 eg_rate_shift[64] = {
    11,11,11,11, // 0-3
    10,10,10,10, // 4-7
     9, 9, 9, 9, // 8-11
     8, 8, 8, 8, // 12-15
     7, 7, 7, 7, // 16-19
     6, 6, 6, 6, // 20-23
     5, 5, 5, 5, // 24-27
     4, 4, 4, 4, // 28-31
     3, 3, 3, 3, // 32-35
     2, 2, 2, 2, // 36-39
     1, 1, 1, 1, // 40-43
     0, 0, 0, 0, // 44-47
     0, 0, 0, 0, // 48-51
     0, 0, 0, 0, // 52-55
     0, 0, 0, 0, // 56-59
     0, 0, 0, 0, // 60-63
};

// Map effective rate (0-63) to row index in eg_inc table
static const u8 eg_rate_select[64] = {
     0, 0, 0, 0, // 0-3: no increment
     1, 2, 3, 4, // 4-7
     1, 2, 3, 4, // 8-11
     1, 2, 3, 4, // 12-15
     1, 2, 3, 4, // 16-19
     1, 2, 3, 4, // 20-23
     1, 2, 3, 4, // 24-27
     1, 2, 3, 4, // 28-31
     1, 2, 3, 4, // 32-35
     1, 2, 3, 4, // 36-39
     1, 2, 3, 4, // 40-43
     1, 2, 3, 4, // 44-47
     5, 6, 7, 8, // 48-51
     9,10,11,12, // 52-55
    13,14,15,16, // 56-59
    17,17,18,18, // 60-63
};

// LFO period table (in FM samples)
// Frequencies: 3.98, 5.56, 6.02, 6.37, 6.88, 9.63, 48.1, 72.2 Hz
// FM sample rate ~53267 Hz, period = rate/freq
static const u32 lfo_period[8] = {
    13389, 9581, 8848, 8362, 7745, 5530, 1108, 738
};

// Hardware-accurate keycode calculation (Nuked-OPN2 fn_note table)
// Indexed by fnum >> 7 (upper 4 bits of fnum), produces note value 0-3
static const u8 fn_note[16] = {0,0,0,0,0,0,0,1,2,3,3,3,3,3,3,3};

static inline int calcKeycode(u16 fnum, u8 block) {
    return (block << 2) | fn_note[fnum >> 7];
}

static bool tablesInitialized = false;

static void initTables() {
    if (tablesInitialized) return;

    // Build log-sine table: -log2(sin(x)) in 4.8 fixed-point
    for (int i = 0; i < 256; i++) {
        double angle = ((i * 2 + 1) * M_PI) / (4.0 * 256.0);
        double logsin = -log2(sin(angle));
        logsinTable[i] = static_cast<u16>(logsin * 256.0 + 0.5);
    }

    // Build exponential table: 2^(1-x/256) in 0.11 fixed-point
    for (int i = 0; i < 256; i++) {
        double value = pow(2.0, 1.0 - i / 256.0);
        expTable[i] = static_cast<u16>(value * 1024.0 + 0.5);
    }

    tablesInitialized = true;
}

YM2612::YM2612() {
    initTables();
    reset();
}

void YM2612::reset() {
    std::memset(channels, 0, sizeof(channels));
    std::memset(addressLatch, 0, sizeof(addressLatch));

    lfoFreq = 0;
    lfoEnabled = false;
    dacData = 0;
    dacEnabled = false;

    timerA = 0;
    timerB = 0;
    timerControl = 0;
    timerAOverflow = false;
    timerBOverflow = false;
    timerACounter = 0;
    timerBCounter = 0;
    timerBSubCounter = 0;
    busyCounter = 0;
    egCounter = 0;
    egSubCounter = 0;

    lfoCounter = 0;
    lfoPeriod = lfo_period[0];
    lfoAM = 0;
    lfoPM = 0;

    ch3SpecialMode = false;
    csmMode = false;
    std::memset(ch3Fnum, 0, sizeof(ch3Fnum));
    std::memset(ch3Block, 0, sizeof(ch3Block));
    std::memset(ch3Keycode, 0, sizeof(ch3Keycode));
    std::memset(ch3FreqHiLatch, 0, sizeof(ch3FreqHiLatch));

    for (int ch = 0; ch < 6; ch++) {
        for (int op = 0; op < 4; op++) {
            channels[ch].op[op].egLevel = 1023;
            channels[ch].op[op].egPhase = 0;
            channels[ch].op[op].ssgEg = 0;
            channels[ch].op[op].ssgInverted = false;
        }
        channels[ch].pan = 0xC0;
        channels[ch].freqHiLatch = 0;
    }
}

void YM2612::writeAddress(u8 data, int port) {
    addressLatch[port & 1] = data;
}

void YM2612::writeData(u8 data, int port) {
    writeRegister(addressLatch[port & 1], data, port & 1);
    busyCounter = 32;
}

u8 YM2612::readStatus() {
    u8 status = 0;
    if (busyCounter > 0) status |= 0x80;  // Bit 7: busy flag
    if (timerAOverflow) status |= 0x01;
    if (timerBOverflow) status |= 0x02;
    return status;
}

void YM2612::writeRegister(u8 addr, u8 data, int bank) {
    // Global registers (bank 0 only, $20-$2F)
    if (addr >= 0x20 && addr < 0x30 && bank == 0) {
        switch (addr) {
            case 0x22:
                lfoEnabled = data & 0x08;
                lfoFreq = data & 0x07;
                lfoPeriod = lfo_period[lfoFreq];
                break;
            case 0x24:
                timerA = (timerA & 0x03) | (data << 2);
                break;
            case 0x25:
                timerA = (timerA & 0x3FC) | (data & 0x03);
                break;
            case 0x26:
                timerB = data;
                break;
            case 0x27: {
                // Bits 7:6: 00=normal, 01=CH3 special, 10=CSM (implies special), 11=CSM
                u8 mode = (data >> 6) & 0x03;
                ch3SpecialMode = (mode != 0);
                csmMode = (mode >= 2);

                // Reload counter when timer transitions from disabled to enabled
                if ((data & 0x01) && !(timerControl & 0x01)) {
                    timerACounter = timerA;
                }
                if ((data & 0x02) && !(timerControl & 0x02)) {
                    timerBCounter = timerB;
                    timerBSubCounter = 0;
                }
                timerControl = data;
                if (data & 0x10) timerAOverflow = false;
                if (data & 0x20) timerBOverflow = false;

                // Recalculate CH3 phase increments when mode changes
                updatePhase(2);
                break;
            }
            case 0x28: {
                int ch = data & 0x07;
                if (ch == 3 || ch == 7) break; // Invalid channel values
                if (ch >= 4) ch -= 1;
                if (ch < 6) {
                    // Bits 4-7 = slots 1-4. Slot-to-operator mapping must match
                    // the register mapping: slot1→op[0], slot2→op[2], slot3→op[1], slot4→op[3]
                    static const int slotToOp[4] = {0, 2, 1, 3};
                    for (int slot = 0; slot < 4; slot++) {
                        int op = slotToOp[slot];
                        if (data & (0x10 << slot)) {
                            keyOn(ch, op);
                        } else {
                            keyOff(ch, op);
                        }
                    }
                }
                break;
            }
            case 0x2A:
                dacData = data;
                break;
            case 0x2B:
                dacEnabled = data & 0x80;
                break;
        }
        return;
    }

    // Channel registers ($30-$B6)
    if (addr >= 0x30) {
        int ch = addr & 0x03;
        if (ch == 3) return;
        ch += bank * 3;

        if (addr < 0xA0) {
            // Operator registers ($30-$9F)
            int op = ((addr >> 3) & 1) | ((addr >> 1) & 2);
            FMOperator& oper = channels[ch].op[op];

            switch (addr & 0xF0) {
                case 0x30:
                    oper.dt = (data >> 4) & 0x07;
                    oper.mul = data & 0x0F;
                    updatePhase(ch);
                    break;
                case 0x40:
                    oper.tl = data & 0x7F;
                    break;
                case 0x50:
                    oper.ks = (data >> 6) & 0x03;
                    oper.ar = data & 0x1F;
                    break;
                case 0x60:
                    oper.am = (data >> 7) & 0x01;
                    oper.dr = data & 0x1F;
                    break;
                case 0x70:
                    oper.sr = data & 0x1F;
                    break;
                case 0x80:
                    oper.sl = (data >> 4) & 0x0F;
                    oper.rr = data & 0x0F;
                    break;
                case 0x90:
                    oper.ssgEg = data & 0x0F;
                    break;
            }
        } else {
            FMChannel& channel = channels[ch];

            switch (addr & 0xFC) {
                case 0xA0: // Frequency LSB — apply latched MSB + this LSB
                    channel.block = (channel.freqHiLatch >> 3) & 0x07;
                    channel.fnum = ((channel.freqHiLatch & 0x07) << 8) | data;
                    updatePhase(ch);
                    break;
                case 0xA4: // Block/Frequency MSB — latch only, don't update yet
                    channel.freqHiLatch = data;
                    break;
                case 0xA8: { // CH3 special mode frequency LSB (bank 0 only)
                    if (bank != 0) break;
                    int idx = addr & 3;
                    if (idx == 3) break;
                    // Map register sub-address to operator index:
                    // $A8 (idx 0) → op[2] (OP3), $A9 (idx 1) → op[0] (OP1), $AA (idx 2) → op[1] (OP2)
                    static const int ch3RegToOp[3] = {2, 0, 1};
                    int opIdx = ch3RegToOp[idx];
                    ch3Block[opIdx] = (ch3FreqHiLatch[opIdx] >> 3) & 0x07;
                    ch3Fnum[opIdx] = ((ch3FreqHiLatch[opIdx] & 0x07) << 8) | data;
                    ch3Keycode[opIdx] = calcKeycode(ch3Fnum[opIdx], ch3Block[opIdx]);
                    if (ch3SpecialMode) updatePhase(2);
                    break;
                }
                case 0xAC: { // CH3 special mode frequency MSB (bank 0 only, latch)
                    if (bank != 0) break;
                    int idx = addr & 3;
                    if (idx == 3) break;
                    static const int ch3RegToOp[3] = {2, 0, 1};
                    int opIdx = ch3RegToOp[idx];
                    ch3FreqHiLatch[opIdx] = data;
                    break;
                }
                case 0xB0:
                    channel.feedback = (data >> 3) & 0x07;
                    channel.algorithm = data & 0x07;
                    break;
                case 0xB4:
                    channel.pan = data & 0xC0;
                    channel.ams = (data >> 4) & 0x03;
                    channel.pms = data & 0x07;
                    break;
            }
        }
    }
}

void YM2612::keyOn(int ch, int op) {
    FMOperator& oper = channels[ch].op[op];
    if (!channels[ch].keyOn[op]) {
        channels[ch].keyOn[op] = true;
        oper.egPhase = 1;
        oper.phase = 0;
        // SSG-EG: initial inversion state from Attack bit (bit 2)
        oper.ssgInverted = (oper.ssgEg & 0x04) != 0;
    }
}

void YM2612::keyOff(int ch, int op) {
    if (channels[ch].keyOn[op]) {
        channels[ch].keyOn[op] = false;
        FMOperator& oper = channels[ch].op[op];
        // SSG-EG: key-off forces specific state
        if (oper.ssgEg & 0x08) {
            // Force inversion based on attack bit and current state
            oper.ssgInverted = (oper.ssgEg & 0x04) != 0;
            oper.egLevel = 0x200;
        }
        oper.egPhase = 4;
    }
}

void YM2612::updatePhase(int chIdx) {
    FMChannel& ch = channels[chIdx];

    // Hardware-accurate keycode calculation
    int chanKeycode = calcKeycode(ch.fnum, ch.block);
    ch.keycode = chanKeycode;

    for (int op = 0; op < 4; op++) {
        u16 fnum = ch.fnum;
        u8 block = ch.block;
        int keycode = chanKeycode;

        // CH3 special mode: per-operator frequency for ops 0-2
        if (chIdx == 2 && ch3SpecialMode && op < 3) {
            fnum = ch3Fnum[op];
            block = ch3Block[op];
            keycode = ch3Keycode[op];
        }

        // Hardware-accurate phase increment (Nuked-OPN2):
        // 1. Base frequency: (fnum << block) >> 1 → 17-bit intermediate
        // 2. Apply detune at this scale (detune table values are designed for it)
        // 3. Mask to 17 bits
        // 4. MUL=0: freq >> 1, else freq * MUL
        // 5. Mask to 20 bits
        s32 freq = static_cast<s32>((fnum << block) >> 1);
        int dt = ch.op[op].dt;
        if (dt) {
            int dtIdx = dt & 3;
            s32 detune = dt_tab[dtIdx][keycode];
            if (dt & 4) detune = -detune;  // DT 4-7 = negative of 0-3
            freq += detune;
        }
        freq &= 0x1FFFF;  // 17-bit mask after detune

        // Apply multiplier
        u32 phaseInc;
        int mul = ch.op[op].mul;
        if (mul == 0) {
            phaseInc = static_cast<u32>(freq >> 1);  // ×0.5
        } else {
            phaseInc = static_cast<u32>(freq * mul);  // direct multiply
        }

        // 20-bit phase increment (hardware precision)
        phaseInc &= 0xFFFFF;

        ch.op[op].phaseInc = phaseInc;
    }
}

// Get EG increment for the current counter and effective rate
static inline int getEgIncrement(int rate, u32 egCounter) {
    if (rate <= 0) return 0;
    if (rate > 63) rate = 63;
    int shift = eg_rate_shift[rate];
    // Only update on specific counter alignments
    if ((egCounter & ((1u << shift) - 1)) != 0) return 0;
    int row = eg_rate_select[rate];
    int col = (egCounter >> shift) & 7;
    return eg_inc[row * 8 + col];
}

bool YM2612::handleSSGEGBoundary(FMOperator& op) {
    if ((op.ssgEg & 0x08) == 0 || op.egLevel < 0x200) {
        return false;
    }

    if (op.ssgEg & 0x01) {
        op.egLevel = 0x200;
        return true;
    }

    if (op.ssgEg & 0x02) {
        op.ssgInverted = !op.ssgInverted;
    }
    op.egLevel = 0;
    op.egPhase = 1;
    op.phase = 0;
    return true;
}

void YM2612::advanceAttack(FMOperator& op, int rate) {
    if (rate >= 62) {
        op.egLevel = 0;
        op.egPhase = 2;
        return;
    }

    int step = getEgIncrement(rate, egCounter);
    if (step != 0) {
        op.egLevel += (~op.egLevel * step) >> 4;
    }
    if (op.egLevel <= 0) {
        op.egLevel = 0;
        op.egPhase = 2;
    }
}

void YM2612::advanceDecay(FMOperator& op, int rate) {
    int target = (op.sl == 15) ? 1023 : (op.sl << 5);
    if (op.egLevel >= target) {
        op.egLevel = target;
        op.egPhase = 3;
        return;
    }

    int step = getEgIncrement(rate, egCounter);
    if (step == 0) {
        if ((op.ssgEg & 0x08) && op.egLevel > 0x200) {
            handleSSGEGBoundary(op);
        }
        return;
    }

    op.egLevel += step;
    if (handleSSGEGBoundary(op)) {
        return;
    }
    if (op.egLevel >= target) {
        op.egLevel = target;
        op.egPhase = 3;
    }
}

void YM2612::advanceSustain(FMOperator& op, int rate) {
    int step = getEgIncrement(rate, egCounter);
    if (step == 0) {
        if ((op.ssgEg & 0x08) && op.egLevel > 0x200) {
            handleSSGEGBoundary(op);
        }
        if (op.egLevel > 1023) {
            op.egLevel = 1023;
        }
        return;
    }

    op.egLevel += step;
    if (handleSSGEGBoundary(op)) {
        return;
    }
    if (op.egLevel > 1023) {
        op.egLevel = 1023;
    }
}

void YM2612::advanceRelease(FMOperator& op, int rate) {
    int step = getEgIncrement(rate, egCounter);
    if (step != 0) {
        op.egLevel += step;
    }
    if (op.egLevel > 1023) {
        op.egLevel = 1023;
    }
    if (op.egLevel >= 1023) {
        op.egPhase = 0;
    }
}

void YM2612::updateEnvelope(FMOperator& op, int keycode) {
    int rate = 0;
    int ksVal = keycode >> (3 - op.ks);  // Key scale contribution

    switch (op.egPhase) {
        case 0:
            op.egLevel = 1023;
            return;

        case 1: {
            rate = op.ar ? (op.ar * 2 + ksVal) : 0;
            if (rate > 63) rate = 63;
            advanceAttack(op, rate);
            break;
        }

        case 2: {
            rate = op.dr ? (op.dr * 2 + ksVal) : 0;
            if (rate > 63) rate = 63;
            advanceDecay(op, rate);
            break;
        }

        case 3: {
            rate = op.sr ? (op.sr * 2 + ksVal) : 0;
            if (rate > 63) rate = 63;
            advanceSustain(op, rate);
            break;
        }

        case 4: {
            rate = op.rr * 4 + 2 + ksVal;
            if (rate > 63) rate = 63;
            advanceRelease(op, rate);
            break;
        }
    }
}

s32 YM2612::generateOperator(FMOperator& op, s32 modulation, u32 amAttenuation) {
    if (op.egPhase == 0) return 0;

    // Extract 10-bit phase from 20-bit accumulator, add modulation
    u32 phase = ((op.phase >> 10) + modulation) & 0x3FF;

    // Bit 9 = sign, bit 8 = mirror, bits 7-0 = table index
    bool sign = phase & 0x200;
    bool mirror = phase & 0x100;
    u16 index = phase & 0xFF;

    // Mirror for second quarter of sine wave
    if (mirror) index = 255 - index;

    // Log-sine lookup → 12-bit log attenuation
    u16 logAtten = logsinTable[index];

    // Apply SSG-EG inversion: when inverted, effective level = 0x1FF - egLevel
    int egOut = op.egLevel;
    if ((op.ssgEg & 0x08) && op.ssgInverted) {
        egOut = 0x1FF - egOut;
        if (egOut < 0) egOut = 0;
    }

    // Add envelope and total level attenuation (convert to 4.8 fixed-point)
    // egLevel is 10-bit (0-1023), TL is 7-bit (0-127)
    u32 totalAtten = logAtten + (static_cast<u32>(egOut) << 2) + (static_cast<u32>(op.tl) << 5);

    // Add LFO AM attenuation for operators with AM enabled
    if (op.am) totalAtten += amAttenuation;

    // Convert from log domain back to linear via exp table
    // Low 8 bits → exp table index, upper bits → right-shift
    u16 expIndex = totalAtten & 0xFF;
    int intPart = totalAtten >> 8;

    s32 output;
    if (intPart >= 13) {
        output = 0;
    } else {
        // Exp table produces 11-bit; shift left 2 for 13-bit unsigned (hardware precision)
        output = (expTable[expIndex] >> intPart) << 2;
    }

    // Apply sign → signed 14-bit output (±8192)
    if (sign) output = -output;

    op.phase += op.phaseInc;

    op.output[0] = op.output[1];
    op.output[1] = output;

    return output;
}

// Genesis Plus GX / Nuked-OPN2 hardware-accurate LFO PM output table
// Indexed by [fnum_bit * 8 + pms], column = lfo_step (0-7)
// For each of 7 fnum bits (bits 4-10), 8 PMS values, and 8 LFO steps,
// this encodes the per-bit contribution to the PM offset.
// Hardware examines each fnum bit independently and sums contributions,
// producing non-linear vibrato that depends on which specific fnum bits are set.
static const u8 lfo_pm_output[7*8][8] = {
    // fnum bit 4
    /* PMS 0 */ {0,0,0,0,0,0,0,0}, /* PMS 1 */ {0,0,0,0,0,0,0,0},
    /* PMS 2 */ {0,0,0,0,0,0,0,0}, /* PMS 3 */ {0,0,0,0,0,0,0,0},
    /* PMS 4 */ {0,0,0,0,0,0,0,0}, /* PMS 5 */ {0,0,0,0,0,0,0,0},
    /* PMS 6 */ {0,0,0,0,0,0,0,0}, /* PMS 7 */ {0,0,0,0,1,1,1,1},
    // fnum bit 5
    /* PMS 0 */ {0,0,0,0,0,0,0,0}, /* PMS 1 */ {0,0,0,0,0,0,0,0},
    /* PMS 2 */ {0,0,0,0,0,0,0,0}, /* PMS 3 */ {0,0,0,0,0,0,0,0},
    /* PMS 4 */ {0,0,0,0,0,0,0,0}, /* PMS 5 */ {0,0,0,0,0,0,0,0},
    /* PMS 6 */ {0,0,0,0,1,1,1,1}, /* PMS 7 */ {0,0,1,1,2,2,2,3},
    // fnum bit 6
    /* PMS 0 */ {0,0,0,0,0,0,0,0}, /* PMS 1 */ {0,0,0,0,0,0,0,0},
    /* PMS 2 */ {0,0,0,0,0,0,0,0}, /* PMS 3 */ {0,0,0,0,0,0,0,0},
    /* PMS 4 */ {0,0,0,0,0,0,0,0}, /* PMS 5 */ {0,0,0,0,1,1,1,1},
    /* PMS 6 */ {0,0,1,1,2,2,2,3}, /* PMS 7 */ {0,0,2,3,4,4,5,6},
    // fnum bit 7
    /* PMS 0 */ {0,0,0,0,0,0,0,0}, /* PMS 1 */ {0,0,0,0,0,0,0,0},
    /* PMS 2 */ {0,0,0,0,0,0,0,0}, /* PMS 3 */ {0,0,0,0,0,0,0,0},
    /* PMS 4 */ {0,0,0,0,1,1,1,1}, /* PMS 5 */ {0,0,1,1,2,2,2,3},
    /* PMS 6 */ {0,0,2,3,4,4,5,6}, /* PMS 7 */ {0,0,4,6,8,8,10,12},
    // fnum bit 8
    /* PMS 0 */ {0,0,0,0,0,0,0,0}, /* PMS 1 */ {0,0,0,0,0,0,0,0},
    /* PMS 2 */ {0,0,0,0,0,0,0,0}, /* PMS 3 */ {0,0,0,0,1,1,1,1},
    /* PMS 4 */ {0,0,1,1,2,2,2,3}, /* PMS 5 */ {0,0,2,3,4,4,5,6},
    /* PMS 6 */ {0,0,4,6,8,8,10,12}, /* PMS 7 */ {0,0,8,12,16,16,20,24},
    // fnum bit 9
    /* PMS 0 */ {0,0,0,0,0,0,0,0}, /* PMS 1 */ {0,0,0,0,0,0,0,0},
    /* PMS 2 */ {0,0,0,0,1,1,1,1}, /* PMS 3 */ {0,0,1,1,2,2,2,3},
    /* PMS 4 */ {0,0,2,3,4,4,5,6}, /* PMS 5 */ {0,0,4,6,8,8,10,12},
    /* PMS 6 */ {0,0,8,12,16,16,20,24}, /* PMS 7 */ {0,0,16,24,32,32,40,48},
    // fnum bit 10
    /* PMS 0 */ {0,0,0,0,0,0,0,0}, /* PMS 1 */ {0,0,0,0,1,1,1,1},
    /* PMS 2 */ {0,0,1,1,2,2,2,3}, /* PMS 3 */ {0,0,2,3,4,4,5,6},
    /* PMS 4 */ {0,0,4,6,8,8,10,12}, /* PMS 5 */ {0,0,8,12,16,16,20,24},
    /* PMS 6 */ {0,0,16,24,32,32,40,48}, /* PMS 7 */ {0,0,32,48,64,64,80,96},
};

// Compute hardware-accurate LFO PM delta in fnum units
// Examines each fnum bit (4-10) independently and sums per-bit contributions
// Returns a signed delta to be applied to fnum before block/MUL calculation
static s32 calcLfoPM(u16 fnum, int pms, s8 lfoPM) {
    if (pms == 0) return 0;

    // lfoPM is a signed triangle wave: -126..+126
    int abspm = lfoPM < 0 ? -lfoPM : lfoPM;
    bool negate = lfoPM < 0;

    // Map |lfoPM| (0-126) to step (0-7)
    int step = abspm >> 4;
    if (step > 7) step = 7;

    // Sum per-bit PM contributions for fnum bits 4-10
    s32 offset = 0;
    for (int bit = 0; bit < 7; bit++) {
        if (fnum & (1 << (bit + 4))) {
            offset += lfo_pm_output[bit * 8 + pms][step];
        }
    }

    if (negate) offset = -offset;
    return offset;
}

// AM depth shift table: hardware uses right-shift of lfoAM value
// AMS 0: >>8 (effectively 0), AMS 1: >>3 (max 15), AMS 2: >>1 (max 63), AMS 3: >>0 (max 126)
static const u8 ams_shift[4] = {8, 3, 1, 0};

s32 YM2612::generateChannel(int ch) {
    FMChannel& channel = channels[ch];

    // Update envelopes only on EG ticks (every 3 FM samples)
    // The EG counter only advances every 3 samples, so calling updateEnvelope
    // every sample would apply increments 3x (same counter value passes the
    // rate-gating check on all 3 consecutive samples).
    if (egSubCounter == 0) {
        for (int op = 0; op < 4; op++) {
            // CH3 special mode: per-operator keycode for EG key scaling
            int keycode = channel.keycode;
            if (ch == 2 && ch3SpecialMode && op < 3) {
                keycode = ch3Keycode[op];
            }
            updateEnvelope(channel.op[op], keycode);
        }
    }

    // Compute LFO AM attenuation (4.8 fixed-point, added to totalAtten for am-enabled ops)
    // Hardware uses a simple right-shift of the LFO AM triangle value
    // The shift produces an EG-scale value (10-bit); shift left 2 to convert to 4.8 fixed-point
    u32 amAtten = 0;
    if (lfoEnabled && channel.ams > 0) {
        amAtten = (static_cast<u32>(lfoAM) >> ams_shift[channel.ams]) << 2;
    }

    // Apply LFO PM: temporarily adjust phase increments for vibrato
    // Hardware modifies F-number bits directly, producing non-linear vibrato
    // PM delta is computed in fnum units, then scaled by block and MUL per-operator
    u32 savedPhaseInc[4];
    bool applyPM = lfoEnabled && channel.pms > 0;
    if (applyPM) {
        for (int op = 0; op < 4; op++) {
            savedPhaseInc[op] = channel.op[op].phaseInc;
            u16 fnum = channel.fnum;
            u8 block = channel.block;
            if (ch == 2 && ch3SpecialMode && op < 3) {
                fnum = ch3Fnum[op];
                block = ch3Block[op];
            }
            // Get PM delta in fnum units
            s32 pmDelta = calcLfoPM(fnum, channel.pms, lfoPM);
            // Convert to phase increment delta: ((delta << block) >> 1) then apply MUL
            s32 freqDelta = (pmDelta << block) >> 1;
            int mul = channel.op[op].mul;
            s32 phaseIncDelta = (mul == 0) ? (freqDelta >> 1) : (freqDelta * mul);
            channel.op[op].phaseInc = static_cast<u32>(
                static_cast<s32>(channel.op[op].phaseInc) + phaseIncDelta) & 0xFFFFF;
        }
    }

    // Feedback for operator 1: average of last two outputs, shifted by feedback level
    // Modulation input is operator output >> 1 (produces 10-bit value added to phase)
    s32 feedback = 0;
    if (channel.feedback) {
        feedback = (channel.op[0].output[0] + channel.op[0].output[1]) >> (10 - channel.feedback);
    }

    s32 out[4];
    s32 result = 0;

    // Hardware evaluates operators in order OP1→OP3→OP2→OP4.
    // For algorithms where OP2 modulates OP3 (algos 0,1,2), OP3 is evaluated
    // before OP2 and uses OP2's previous sample output (delayed modulation).
    switch (channel.algorithm) {
        case 0: // OP1→OP2→OP3→OP4→out (hw: OP3 uses OP2 prev)
            out[0] = generateOperator(channel.op[0], feedback, amAtten);
            out[2] = generateOperator(channel.op[2], channel.op[1].output[1] >> 1, amAtten);
            out[1] = generateOperator(channel.op[1], out[0] >> 1, amAtten);
            out[3] = generateOperator(channel.op[3], out[2] >> 1, amAtten);
            result = out[3];
            break;

        case 1: // (OP1+OP2)→OP3→OP4→out (hw: OP3 uses OP2 prev)
            out[0] = generateOperator(channel.op[0], feedback, amAtten);
            out[2] = generateOperator(channel.op[2], (out[0] + channel.op[1].output[1]) >> 1, amAtten);
            out[1] = generateOperator(channel.op[1], 0, amAtten);
            out[3] = generateOperator(channel.op[3], out[2] >> 1, amAtten);
            result = out[3];
            break;

        case 2: // (OP1+(OP2→OP3))→OP4→out (hw: OP3 uses OP2 prev)
            out[0] = generateOperator(channel.op[0], feedback, amAtten);
            out[2] = generateOperator(channel.op[2], channel.op[1].output[1] >> 1, amAtten);
            out[1] = generateOperator(channel.op[1], 0, amAtten);
            out[3] = generateOperator(channel.op[3], (out[0] + out[2]) >> 1, amAtten);
            result = out[3];
            break;

        case 3: // ((OP1→OP2)+OP3)→OP4→out
            out[0] = generateOperator(channel.op[0], feedback, amAtten);
            out[2] = generateOperator(channel.op[2], 0, amAtten);
            out[1] = generateOperator(channel.op[1], out[0] >> 1, amAtten);
            out[3] = generateOperator(channel.op[3], (out[1] + out[2]) >> 1, amAtten);
            result = out[3];
            break;

        case 4: // (OP1→OP2)+(OP3→OP4)→out
            out[0] = generateOperator(channel.op[0], feedback, amAtten);
            out[2] = generateOperator(channel.op[2], 0, amAtten);
            out[1] = generateOperator(channel.op[1], out[0] >> 1, amAtten);
            out[3] = generateOperator(channel.op[3], out[2] >> 1, amAtten);
            // Per-carrier 9-bit truncation before summing (hardware DAC behavior)
            result = (out[1] >> 5) + (out[3] >> 5);
            break;

        case 5: // OP1→(OP2+OP3+OP4)→out
            out[0] = generateOperator(channel.op[0], feedback, amAtten);
            out[2] = generateOperator(channel.op[2], out[0] >> 1, amAtten);
            out[1] = generateOperator(channel.op[1], out[0] >> 1, amAtten);
            out[3] = generateOperator(channel.op[3], out[0] >> 1, amAtten);
            result = (out[1] >> 5) + (out[2] >> 5) + (out[3] >> 5);
            break;

        case 6: // (OP1→OP2)+OP3+OP4→out
            out[0] = generateOperator(channel.op[0], feedback, amAtten);
            out[2] = generateOperator(channel.op[2], 0, amAtten);
            out[1] = generateOperator(channel.op[1], out[0] >> 1, amAtten);
            out[3] = generateOperator(channel.op[3], 0, amAtten);
            result = (out[1] >> 5) + (out[2] >> 5) + (out[3] >> 5);
            break;

        case 7: // OP1+OP2+OP3+OP4→out
            out[0] = generateOperator(channel.op[0], feedback, amAtten);
            out[2] = generateOperator(channel.op[2], 0, amAtten);
            out[1] = generateOperator(channel.op[1], 0, amAtten);
            out[3] = generateOperator(channel.op[3], 0, amAtten);
            result = (out[0] >> 5) + (out[1] >> 5) + (out[2] >> 5) + (out[3] >> 5);
            break;
    }

    // DAC replaces channel 5 FM output (operators still run above for envelope progression)
    // DAC byte is unsigned 8-bit, maps to signed 14-bit: (byte - 128) << 6
    if (ch == 5 && dacEnabled) {
        result = (static_cast<s32>(dacData) - 128) << 6;
        // Single value, treat as single-carrier for truncation
        result >>= 5;
    } else if (channel.algorithm < 4) {
        // 9-bit DAC: single-carrier algos (0-3) need truncation
        result >>= 5;  // 14-bit → 9-bit
    }
    // Multi-carrier algos (4-7) already accumulated in 9-bit above

    // YM2612 "ladder effect" DAC non-linearity (Nuked-OPN2):
    // Non-negative 9-bit values get +1 offset; negative values unchanged.
    // This creates the characteristic warm distortion of the original chip.
    if (result > 0) result += 1;

    // Scale back to internal resolution
    result <<= 5;

    // Restore phase increments after PM
    if (applyPM) {
        for (int op = 0; op < 4; op++) {
            channel.op[op].phaseInc = savedPhaseInc[op];
        }
    }

    return result;
}

void YM2612::advanceState() {
    // EG updates every 3 FM sample periods
    if (++egSubCounter >= 3) {
        egSubCounter = 0;
        egCounter++;
        // Hardware 12-bit counter wraps from 0xFFF to 1 (skips 0)
        egCounter &= 0xFFF;
        if (egCounter == 0) egCounter = 1;
    }
    if (busyCounter > 0) busyCounter--;

    // LFO update
    if (lfoEnabled) {
        if (++lfoCounter >= lfoPeriod) {
            lfoCounter = 0;
        }
        // Normalize LFO counter to 0-255 phase
        u32 lfoPhase = (lfoCounter * 256) / lfoPeriod;

        // AM: unsigned triangle 0→126→0 (always non-negative, used as attenuation)
        if (lfoPhase < 128) {
            lfoAM = static_cast<u8>(lfoPhase);
        } else {
            lfoAM = static_cast<u8>(255 - lfoPhase);
        }

        // PM: signed triangle 0→+126→0→-126→0 (vibrato oscillates both directions)
        if (lfoPhase < 64) {
            lfoPM = static_cast<s8>(lfoPhase * 2);              // 0 to +126
        } else if (lfoPhase < 128) {
            lfoPM = static_cast<s8>((127 - lfoPhase) * 2);      // +126 to 0
        } else if (lfoPhase < 192) {
            lfoPM = static_cast<s8>(-((lfoPhase - 128) * 2));   // 0 to -126
        } else {
            lfoPM = static_cast<s8>(-((255 - lfoPhase) * 2));   // -126 to 0
        }
    } else {
        lfoAM = 0;
        lfoPM = 0;
    }

    if (timerControl & 0x01) {
        for (int i = 0; i < 2; i++) {
            timerACounter++;
            if (timerACounter >= 1024) {
                timerACounter = timerA;
                if (timerControl & 0x04) timerAOverflow = true;
                if (csmMode) {
                    for (int op = 0; op < 4; op++) {
                        keyOff(2, op);
                    }
                    for (int op = 0; op < 4; op++) {
                        keyOn(2, op);
                    }
                }
            }
        }
    }

    if (timerControl & 0x02) {
        timerBSubCounter++;
        if (timerBSubCounter >= 8) {
            timerBSubCounter = 0;
            timerBCounter++;
            if (timerBCounter >= 256) {
                timerBCounter = timerB;
                if (timerControl & 0x08) timerBOverflow = true;
            }
        }
    }
}

YMSample YM2612::tick() {
    s32 left = 0;
    s32 right = 0;

    for (int ch = 0; ch < 6; ch++) {
        s32 sample = generateChannel(ch);

        if (channels[ch].pan & 0x80) left += sample;
        if (channels[ch].pan & 0x40) right += sample;
    }

    if (left > 32767) left = 32767;
    if (left < -32768) left = -32768;
    if (right > 32767) right = 32767;
    if (right < -32768) right = -32768;

    advanceState();
    return {static_cast<s16>(left), static_cast<s16>(right)};
}

void YM2612::tickMany(int count, s32& leftAccum, s32& rightAccum) {
    for (int i = 0; i < count; i++) {
        YMSample sample = tick();
        leftAccum += sample.left;
        rightAccum += sample.right;
    }
}

void YM2612::getSamples(s16* left, s16* right, int count) {
    for (int i = 0; i < count; i++) {
        YMSample sample = tick();
        left[i] = sample.left;
        right[i] = sample.right;
    }
}
