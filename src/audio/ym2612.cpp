// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "audio/ym2612.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static constexpr int SINE_TABLE_SIZE = 512;
static constexpr int POW_TABLE_SIZE = 8192;

// Hardware-accurate OPN2 log-sine and power tables.
// sineTable: half-wave (0..pi), 4.8 fixed-point -log2(sin)
// powTable: log-to-linear, 11-bit mantissa with integer shift
static u16 sineTable[SINE_TABLE_SIZE];
static u16 powTable[POW_TABLE_SIZE];

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

// LFO samples per step (YM2612 die-shot verified values).
// There are 128 steps per full LFO cycle; one step lasts this many FM samples.
// Frequencies (NTSC): 3.85, 5.40, 5.86, 6.21, 6.71, 9.46, 52.0, 83.2 Hz
static const u32 lfo_samples_per_step[8] = {
    108, 77, 71, 67, 62, 44, 8, 5
};

// Hardware-accurate keycode calculation (YM2612 fn_note lookup)
// Indexed by fnum >> 7 (upper 4 bits of fnum), produces note value 0-3
static const u8 fn_note[16] = {0,0,0,0,0,0,0,1,2,3,3,3,3,3,3,3};

static inline int calcKeycode(u16 fnum, u8 block) {
    return (block << 2) | fn_note[fnum >> 7];
}

static bool tablesInitialized = false;

static void initTables() {
    if (tablesInitialized) return;

    // Log-sine: half-wave (0..pi), 4.8 fixed-point -log2(sin)
    for (int i = 0; i < SINE_TABLE_SIZE; i++) {
        double sine = sin(((double)(i * 2 + 1) / (SINE_TABLE_SIZE * 2)) * M_PI);
        sineTable[i] = static_cast<u16>(-log2(sine) * 256.0 + 0.5);
    }

    // Power table: log-to-linear, 11-bit mantissa with integer shift
    for (int i = 0; i < POW_TABLE_SIZE; i++) {
        double linear = pow(2.0, -((double)((i & 0xFF) + 1) / 256.0));
        s32 mantissa = static_cast<s32>(linear * 2048.0 + 0.5);
        int shift = (i >> 8) - 2;
        if (shift < 0) mantissa <<= -shift;
        else           mantissa >>= shift;
        powTable[i] = static_cast<u16>(mantissa);
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
    dacOut = 0;
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
    lfoPeriod = lfo_samples_per_step[0];
    lfoCnt = 0;
    lfoAM = 126;  // Max AM attenuation when LFO disabled
    lfoPM = 0;

    ch3SpecialMode = false;
    csmMode = false;
    std::memset(ch3Fnum, 0, sizeof(ch3Fnum));
    std::memset(ch3Block, 0, sizeof(ch3Block));
    std::memset(ch3Keycode, 0, sizeof(ch3Keycode));
    std::memset(ch3FreqHiLatch, 0, sizeof(ch3FreqHiLatch));

    // YM2612 ladder-effect scaling with zero_offset enabled:
    // zero_offset = 0x70, volume_mult = 79, volume_div = 120.
    // The older 2/3 constants only apply when zero_offset is disabled.
    zeroOffset_ = 0x70;
    volumeMult_ = 79;
    volumeDiv_ = 120;

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
    static FILE* ymLog = nullptr;
    static bool ymLogChecked = false;
    if (!ymLogChecked) {
        const char* p = std::getenv("GENESIS_LOG_YM_WRITES");
        if (p && p[0] != '0') ymLog = stderr;
        ymLogChecked = true;
    }
    if (ymLog) {
        std::fprintf(ymLog, "[YM] p%d r%02X=%02X\n", port & 1, addressLatch[port & 1], data);
    }
    writeRegister(addressLatch[port & 1], data, port & 1);
    // YM2612 busy flag lasts 32 FM internal clock cycles after a data write
    // (confirmed by Nuked OPN2 cycle-accurate core: write_busy_cnt counts 0-31).
    // One FM sample = 24 internal clocks, so 32 clocks ≈ 1.33 samples → round up to 2.
    busyCounter = 2;
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
                if (data & 0x08) {
                    lfoEnabled = true;
                    lfoFreq = data & 0x07;
                    lfoPeriod = lfo_samples_per_step[lfoFreq];
                } else {
                    // Hold LFO in reset state when disabled
                    lfoEnabled = false;
                    lfoCounter = 0;
                    lfoCnt = 0;
                    lfoPM = 0;
                    lfoAM = 126;  // Max AM attenuation
                }
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
                dacOut = (static_cast<s32>(data) - 128) << 6;
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
        oper.phase = 0;

        int keycode = channels[ch].keycode;
        if (ch == 2 && ch3SpecialMode && op < 3) {
            keycode = ch3Keycode[op];
        }

        int ksVal = keycode >> (3 - oper.ks);
        int rate = oper.ar ? (oper.ar * 2 + ksVal) : 0;
        if (rate > 63) rate = 63;

        if (rate >= 62) {
            oper.egLevel = 0;
            oper.egPhase = (oper.sl == 0) ? 3 : 2;
        } else if (oper.egLevel <= 0) {
            oper.egPhase = (oper.sl == 0) ? 3 : 2;
        } else {
            oper.egPhase = 1;
        }

        // SSG-EG: initial inversion state from Attack bit (bit 2)
        oper.ssgInverted = (oper.ssgEg & 0x04) != 0;
    }
}

void YM2612::keyOff(int ch, int op) {
    if (channels[ch].keyOn[op]) {
        channels[ch].keyOn[op] = false;
        FMOperator& oper = channels[ch].op[op];
        // SSG-EG key-off: invert level when ssgInverted differs from attack bit,
        // then force to OFF if level >= 0x200 (hardware-verified)
        if (oper.ssgEg & 0x08) {
            bool attackBit = (oper.ssgEg & 0x04) != 0;
            if (oper.ssgInverted != attackBit) {
                oper.egLevel = (0x200 - oper.egLevel) & 0x3FF;
            }
            if (oper.egLevel >= 0x200) {
                oper.egLevel = 1023;
                oper.egPhase = 0; // OFF — already at max attenuation
                return;
            }
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

        // Hardware-accurate phase increment (YM2612 application manual):
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
        // Hold mode (hardware-verified SSG-EG behavior):
        // 1. If alternate bit set: force inversion flag on
        if (op.ssgEg & 0x02) {
            op.ssgInverted = (op.ssgEg & 0x04) == 0;
        }
        // 2. Force max attenuation if output is NOT inverted and not in attack
        if (op.egPhase != 1 && !op.ssgInverted) {
            op.egLevel = 1023;
        }
        return true;
    }

    // Loop mode: toggle inversion OR reset phase (mutually exclusive)
    if (op.ssgEg & 0x02) {
        op.ssgInverted = !op.ssgInverted;
    } else {
        op.phase = 0;
    }

    // Restart envelope (same as key-on) if not already in attack
    if (op.egPhase != 1) {
        op.egLevel = 0;
        op.egPhase = 1;
    }
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
    // SL=15 maps to 31<<5 = 992, not 1023 (hardware-verified)
    int target = (op.sl == 15) ? 992 : (op.sl << 5);
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

    // SSG-EG: 4x increment rate when below 0x200 threshold (hardware-verified)
    if (op.ssgEg & 0x08) {
        if (op.egLevel < 0x200) {
            op.egLevel += 4 * step;
        }
    } else {
        op.egLevel += step;
    }
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

    // SSG-EG: 4x increment rate when below 0x200 threshold (hardware-verified)
    if (op.ssgEg & 0x08) {
        if (op.egLevel < 0x200) {
            op.egLevel += 4 * step;
        }
    } else {
        op.egLevel += step;
    }
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
        // SSG-EG: 4x increment rate when below 0x200 threshold (hardware-verified)
        if ((op.ssgEg & 0x08) && op.egLevel < 0x200) {
            op.egLevel += 4 * step;
            // Snap to max attenuation at SSG-EG boundary
            if (op.egLevel >= 0x200) {
                op.egLevel = 1023;
                op.egPhase = 0;
                return;
            }
        } else {
            op.egLevel += step;
        }
    }
    if (op.egLevel > 1023) {
        op.egLevel = 1023;
    }
    if (op.egLevel >= 1023) {
        op.egPhase = 0;
    }
}

void YM2612::updateEnvelope(FMOperator& op, int keycode) {
    int ksVal = keycode >> (3 - op.ks);  // Key scale contribution

    switch (op.egPhase) {
        case 0:
            op.egLevel = 1023;
            return;

        case 1: {
            int rate = op.ar ? (op.ar * 2 + ksVal) : 0;
            if (rate > 63) rate = 63;
            advanceAttack(op, rate);
            break;
        }

        case 2: {
            int rate = op.dr ? (op.dr * 2 + ksVal) : 0;
            if (rate > 63) rate = 63;
            advanceDecay(op, rate);
            break;
        }

        case 3: {
            int rate = op.sr ? (op.sr * 2 + ksVal) : 0;
            if (rate > 63) rate = 63;
            advanceSustain(op, rate);
            break;
        }

        case 4: {
            int rate = op.rr * 4 + 2 + ksVal;
            if (rate > 63) rate = 63;
            advanceRelease(op, rate);
            break;
        }
    }
}

s32 YM2612::generateOperator(FMOperator& op, s32 modulation, u32 amAttenuation) {
    // Always advance phase and update output history regardless of EG state.
    // This prevents stale feedback values when operators are re-keyed.
    s32 output = 0;

    if (op.egPhase != 0) {
        // Apply SSG-EG output inversion: (0x200 - volume) & 0x3FF
        // ssgInverted XOR attack bit determines whether output is currently inverted
        int egOut = op.egLevel;
        if ((op.ssgEg & 0x08) && (op.ssgInverted != ((op.ssgEg & 0x04) != 0))) {
            egOut = (0x200 - egOut) & 0x3FF;
        }

        u32 env = static_cast<u32>(egOut) + (static_cast<u32>(op.tl) << 3);
        if (op.am) env += amAttenuation;
        if (env > 0x1FFF) env = 0x1FFF;

        u16 phase = static_cast<u16>(((op.phase >> 10) + static_cast<u32>(modulation)) & 0x3FF);

        // Envelope is 10-bit (0-1023 base + TL/AM). Convert to 4.8 fixed-point
        // log domain by shifting left 2 (each env unit = 4 powTable index steps,
        // matching hardware's 0.09375 dB/step → ~4 units in 4.8 log2 space).
        u32 logVal = sineTable[phase & 0x1FF] + (env << 2);
        if (logVal >= static_cast<u32>(POW_TABLE_SIZE)) {
            output = 0;
        } else {
            output = powTable[logVal];
        }
        if (phase & 0x200) {
            output = -output;
        }
    }

    op.phase += op.phaseInc;
    op.output[0] = op.output[1];
    op.output[1] = output;

    return output;
}

// Hardware-accurate LFO PM output table (YM2612 die-shot verified)
// Indexed by [fnum_bit * 8 + pms], column = lfo_step (0-7)
// For each of 7 fnum bits (bits 4-10), 8 PMS values, and 8 LFO steps,
// this encodes the per-bit contribution to the PM offset.
// Hardware examines each fnum bit independently and sums contributions,
// producing non-linear vibrato that depends on which specific fnum bits are set.
static const u8 lfo_pm_output[7*8][8] = {
    // fnum bit 4 (000 0001xxxx)
    /* PMS 0 */ {0,0,0,0,0,0,0,0}, /* PMS 1 */ {0,0,0,0,0,0,0,0},
    /* PMS 2 */ {0,0,0,0,0,0,0,0}, /* PMS 3 */ {0,0,0,0,0,0,0,0},
    /* PMS 4 */ {0,0,0,0,0,0,0,0}, /* PMS 5 */ {0,0,0,0,0,0,0,0},
    /* PMS 6 */ {0,0,0,0,0,0,0,0}, /* PMS 7 */ {0,0,0,0,1,1,1,1},
    // fnum bit 5 (000 0010xxxx)
    /* PMS 0 */ {0,0,0,0,0,0,0,0}, /* PMS 1 */ {0,0,0,0,0,0,0,0},
    /* PMS 2 */ {0,0,0,0,0,0,0,0}, /* PMS 3 */ {0,0,0,0,0,0,0,0},
    /* PMS 4 */ {0,0,0,0,0,0,0,0}, /* PMS 5 */ {0,0,0,0,0,0,0,0},
    /* PMS 6 */ {0,0,0,0,1,1,1,1}, /* PMS 7 */ {0,0,1,1,2,2,2,3},
    // fnum bit 6 (000 0100xxxx)
    /* PMS 0 */ {0,0,0,0,0,0,0,0}, /* PMS 1 */ {0,0,0,0,0,0,0,0},
    /* PMS 2 */ {0,0,0,0,0,0,0,0}, /* PMS 3 */ {0,0,0,0,0,0,0,0},
    /* PMS 4 */ {0,0,0,0,0,0,0,1}, /* PMS 5 */ {0,0,0,0,1,1,1,1},
    /* PMS 6 */ {0,0,1,1,2,2,2,3}, /* PMS 7 */ {0,0,2,3,4,4,5,6},
    // fnum bit 7 (000 1000xxxx)
    /* PMS 0 */ {0,0,0,0,0,0,0,0}, /* PMS 1 */ {0,0,0,0,0,0,0,0},
    /* PMS 2 */ {0,0,0,0,0,0,1,1}, /* PMS 3 */ {0,0,0,0,1,1,1,1},
    /* PMS 4 */ {0,0,0,1,1,1,1,2}, /* PMS 5 */ {0,0,1,1,2,2,2,3},
    /* PMS 6 */ {0,0,2,3,4,4,5,6}, /* PMS 7 */ {0,0,4,6,8,8,10,12},
    // fnum bit 8 (001 0000xxxx)
    /* PMS 0 */ {0,0,0,0,0,0,0,0}, /* PMS 1 */ {0,0,0,0,1,1,1,1},
    /* PMS 2 */ {0,0,0,1,1,1,2,2}, /* PMS 3 */ {0,0,1,1,2,2,3,3},
    /* PMS 4 */ {0,0,1,2,2,2,3,4}, /* PMS 5 */ {0,0,2,3,4,4,5,6},
    /* PMS 6 */ {0,0,4,6,8,8,10,12}, /* PMS 7 */ {0,0,8,12,16,16,20,24},
    // fnum bit 9 (010 0000xxxx)
    /* PMS 0 */ {0,0,0,0,0,0,0,0}, /* PMS 1 */ {0,0,0,0,2,2,2,2},
    /* PMS 2 */ {0,0,0,2,2,2,4,4}, /* PMS 3 */ {0,0,2,2,4,4,6,6},
    /* PMS 4 */ {0,0,2,4,4,4,6,8}, /* PMS 5 */ {0,0,4,6,8,8,10,12},
    /* PMS 6 */ {0,0,8,12,16,16,20,24}, /* PMS 7 */ {0,0,16,24,32,32,40,48},
    // fnum bit 10 (100 0000xxxx)
    /* PMS 0 */ {0,0,0,0,0,0,0,0}, /* PMS 1 */ {0,0,0,0,4,4,4,4},
    /* PMS 2 */ {0,0,0,4,4,4,8,8}, /* PMS 3 */ {0,0,4,4,8,8,12,12},
    /* PMS 4 */ {0,0,4,8,8,8,12,16}, /* PMS 5 */ {0,0,8,12,16,16,20,24},
    /* PMS 6 */ {0,0,16,24,32,32,40,48}, /* PMS 7 */ {0,0,32,48,64,64,80,96},
};

// Compute hardware-accurate LFO PM delta in fnum units.
// lfo_pm_idx is 0-31 (lfoCnt >> 2), encoding a signed triangle:
//   0-7: positive rising, 8-15: positive falling,
//   16-23: negative rising, 24-31: negative falling.
// Matches hardware LFO PM waveform structure.
static s32 calcLfoPM(u16 fnum, int pms, u8 lfo_pm_idx) {
    if (pms == 0) return 0;

    int quarter = lfo_pm_idx >> 3;   // 0-3: which quarter of triangle
    int s = lfo_pm_idx & 7;
    bool negate = quarter >= 2;
    int step = (quarter & 1) ? (s ^ 7) : s;  // mirror on odd quarters

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

    // Compute LFO AM attenuation in native EG units.
    u32 amAtten = 0;
    if (lfoEnabled && channel.ams > 0) {
        amAtten = static_cast<u32>(lfoAM) >> ams_shift[channel.ams];
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

    // Feedback for operator 1: average of last two outputs, shifted by feedback level.
    // True 2-sample averaging is required here; using only the previous
    // sample as a shortcut produces the wrong feedback level.
    s32 feedback = 0;
    if (channel.feedback) {
        feedback = (channel.op[0].output[0] + channel.op[0].output[1]) >> (10 - channel.feedback);
    }

    s32 out[4];
    s32 result = 0;

    // Hardware evaluation order: SLOT1(M1=op[0]) -> SLOT3(M2=op[2]) -> SLOT2(C1=op[1]) -> SLOT4(C2=op[3])
    // Algorithms 0-3, 5 use a one-sample delay (mem_value) for inter-operator
    // modulation where the modulated operator runs before the modulator in
    // hardware order. Our op[N] is in slot-number order (op[0]=SLOT1,
    // op[1]=SLOT2, op[2]=SLOT3, op[3]=SLOT4), so the wiring below is the
    // correct translation for our convention. Some references index operators
    // in register-address order, with op[1]/op[2] swapped relative to ours.
    s32 mem = channel.mem_value;

    switch (channel.algorithm) {
        case 0: // M1->C1->(delay)->M2->C2
            out[0] = generateOperator(channel.op[0], feedback, amAtten);
            out[2] = generateOperator(channel.op[2], mem >> 1, amAtten);
            out[1] = generateOperator(channel.op[1], out[0] >> 1, amAtten);
            out[3] = generateOperator(channel.op[3], out[2] >> 1, amAtten);
            channel.mem_value = out[1];
            result = out[3];
            break;

        case 1: // (M1+C1)->(delay)->M2->C2
            out[0] = generateOperator(channel.op[0], feedback, amAtten);
            out[2] = generateOperator(channel.op[2], mem >> 1, amAtten);
            out[1] = generateOperator(channel.op[1], 0, amAtten);
            out[3] = generateOperator(channel.op[3], out[2] >> 1, amAtten);
            channel.mem_value = out[0] + out[1];
            result = out[3];
            break;

        case 2: // C1->(delay)->M2, (M1+M2)->C2
            out[0] = generateOperator(channel.op[0], feedback, amAtten);
            out[2] = generateOperator(channel.op[2], mem >> 1, amAtten);
            out[1] = generateOperator(channel.op[1], 0, amAtten);
            out[3] = generateOperator(channel.op[3], (out[0] + out[2]) >> 1, amAtten);
            channel.mem_value = out[1];
            result = out[3];
            break;

        case 3: // M1->C1, (C1_delayed + M2)->C2
            out[0] = generateOperator(channel.op[0], feedback, amAtten);
            out[2] = generateOperator(channel.op[2], 0, amAtten);
            out[1] = generateOperator(channel.op[1], out[0] >> 1, amAtten);
            out[3] = generateOperator(channel.op[3], (mem + out[2]) >> 1, amAtten);
            channel.mem_value = out[1];
            result = out[3];
            break;

        case 4: // (M1→C1)+(M2→C2)
            // op[2] unmodulated
            // op[1] modulated by op[0]
            // op[3] modulated by op[2]
            out[0] = generateOperator(channel.op[0], feedback, amAtten);
            out[2] = generateOperator(channel.op[2], 0, amAtten);
            out[1] = generateOperator(channel.op[1], out[0] >> 1, amAtten);
            out[3] = generateOperator(channel.op[3], out[2] >> 1, amAtten);
            result = (out[1] & ~0x1F) + (out[3] & ~0x1F);
            if (result > 0x1FE0) result = 0x1FE0;
            else if (result < -0x1FF0) result = -0x1FF0;
            break;

        case 5: // M1->(delay)->M2, M1->C1, M1->C2
            out[0] = generateOperator(channel.op[0], feedback, amAtten);
            out[2] = generateOperator(channel.op[2], mem >> 1, amAtten);
            out[1] = generateOperator(channel.op[1], out[0] >> 1, amAtten);
            out[3] = generateOperator(channel.op[3], out[0] >> 1, amAtten);
            channel.mem_value = out[0];
            result = (out[1] & ~0x1F) + (out[2] & ~0x1F) + (out[3] & ~0x1F);
            if (result > 0x1FE0) result = 0x1FE0;
            else if (result < -0x1FF0) result = -0x1FF0;
            break;

        case 6: // (M1→C1)+M2+C2
            // op[2] unmodulated
            // op[1] modulated by op[0]
            // op[3] unmodulated
            out[0] = generateOperator(channel.op[0], feedback, amAtten);
            out[2] = generateOperator(channel.op[2], 0, amAtten);
            out[1] = generateOperator(channel.op[1], out[0] >> 1, amAtten);
            out[3] = generateOperator(channel.op[3], 0, amAtten);
            result = (out[1] & ~0x1F) + (out[2] & ~0x1F) + (out[3] & ~0x1F);
            if (result > 0x1FE0) result = 0x1FE0;
            else if (result < -0x1FF0) result = -0x1FF0;
            break;

        case 7: // M1+M2+C1+C2
            // All operators unmodulated (except feedback on op[0])
            out[0] = generateOperator(channel.op[0], feedback, amAtten);
            out[2] = generateOperator(channel.op[2], 0, amAtten);
            out[1] = generateOperator(channel.op[1], 0, amAtten);
            out[3] = generateOperator(channel.op[3], 0, amAtten);
            result = (out[0] & ~0x1F) + (out[1] & ~0x1F) + (out[2] & ~0x1F) + (out[3] & ~0x1F);
            if (result > 0x1FE0) result = 0x1FE0;
            else if (result < -0x1FF0) result = -0x1FF0;
            break;
    }

    // DAC replaces channel 5 FM output (operators still run above for envelope progression)
    // DAC byte is unsigned 8-bit, maps to signed 14-bit: (byte - 128) << 6
    if (ch == 5 && dacEnabled) {
        result = dacOut;
    } else if (channel.algorithm < 4) {
        // 9-bit DAC: single-carrier algos (0-3) truncate low 5 bits
        result &= ~0x1F;
    }
    // Multi-carrier algos (4-7): already accumulated with & ~0x1F per operator.

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

        // Envelope advancement happens after the sample is generated.
        for (int ch = 0; ch < 6; ch++) {
            FMChannel& channel = channels[ch];
            for (int op = 0; op < 4; op++) {
                int keycode = channel.keycode;
                if (ch == 2 && ch3SpecialMode && op < 3) {
                    keycode = ch3Keycode[op];
                }
                updateEnvelope(channel.op[op], keycode);
            }
        }
    }
    if (busyCounter > 0) busyCounter--;

    // LFO update: hardware has 128 discrete steps per cycle.
    // Hardware uses 128 discrete steps per cycle, not continuous phase.
    if (lfoEnabled) {
        if (++lfoCounter >= lfoPeriod) {
            lfoCounter = 0;
            // There are 128 LFO steps per full cycle
            lfoCnt = (lfoCnt + 1) & 127;

            // AM: inverted triangle 126→0→0→126 (hardware-verified)
            // Hardware AM starts at max attenuation and dips to zero.
            if (lfoCnt < 64)
                lfoAM = static_cast<u8>((lfoCnt ^ 63) << 1);
            else
                lfoAM = static_cast<u8>((lfoCnt & 63) << 1);

            // PM: 32 values (128 steps / 4), indexes into lfo_pm_output table
            // Maps to signed triangle via the table structure.
            lfoPM = static_cast<s8>(lfoCnt >> 2);
        }
    } else {
        // Hold LFO in reset state when disabled
        lfoCounter = 0;
        lfoCnt = 0;
        lfoPM = 0;
        lfoAM = 126;  // Max AM attenuation when LFO disabled
    }

    // Timer A: 10-bit counter, increments once per FM sample (~53 kHz).
    // Overflows after (1024 - timerA) samples.
    if (timerControl & 0x01) {
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

    if (timerControl & 0x02) {
        timerBSubCounter++;
        if (timerBSubCounter >= 16) {
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
    static FILE* chanFiles[6] = {};
    static bool chanDumpChecked = false;
    if (!chanDumpChecked) {
        const char* e = std::getenv("GENESIS_DUMP_YM_CHANNELS");
        if (e && e[0] != '0') {
            for (int i = 0; i < 6; i++) {
                char path[256];
                std::snprintf(path, sizeof(path), "/tmp/our_ym_ch%d.raw", i);
                chanFiles[i] = std::fopen(path, "wb");
            }
        }
        chanDumpChecked = true;
    }

    s32 left = 0;
    s32 right = 0;

    for (int ch = 0; ch < 6; ch++) {
        s32 sample = generateChannel(ch);

        // DAC output model: bias by +/- zero_offset (the YM2612 ladder-effect
        // DC bias) before the panned/unpanned branches, then mix at 79/120.
        s32 value = sample;
        if (value >= 0) value += zeroOffset_;
        else            value -= zeroOffset_;

        // Per-channel debug dump writes the biased value, not the raw sample.
        if (chanFiles[ch]) {
            s16 s = static_cast<s16>(value > 32767 ? 32767 : (value < -32768 ? -32768 : value));
            std::fwrite(&s, sizeof(s16), 1, chanFiles[ch]);
        }

        bool panL = channels[ch].pan & 0x80;
        bool panR = channels[ch].pan & 0x40;

        if (panL) {
            left += (value * volumeMult_) / volumeDiv_;
        } else if (zeroOffset_) {
            if (value >= 0) {
                left += (zeroOffset_ * volumeMult_) / volumeDiv_;
            } else {
                left -= (zeroOffset_ * volumeMult_) / volumeDiv_;
            }
            left += (value * volumeMult_) / (60 * volumeDiv_);
        }

        if (panR) {
            right += (value * volumeMult_) / volumeDiv_;
        } else if (zeroOffset_) {
            if (value >= 0) {
                right += (zeroOffset_ * volumeMult_) / volumeDiv_;
            } else {
                right -= (zeroOffset_ * volumeMult_) / volumeDiv_;
            }
            right += (value * volumeMult_) / (60 * volumeDiv_);
        }
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
