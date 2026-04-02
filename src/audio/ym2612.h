// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include "types.h"

struct FMOperator {
    u8 ar, dr, sr, rr;  // Attack, decay, sustain, release rates
    u8 sl;              // Sustain level
    u8 tl;              // Total level (volume)
    u8 ks;              // Key scale
    u8 mul;             // Frequency multiplier
    u8 dt;              // Detune
    u8 am;              // AM enable
    u8 ssgEg;           // SSG-EG register (bits 0-3)
    bool ssgInverted;   // Current SSG-EG inversion state

    u32 phase;          // Phase accumulator
    u32 phaseInc;       // Phase increment
    int egPhase;        // EG phase (0=off, 1=attack, 2=decay, 3=sustain, 4=release)
    int egLevel;        // Current envelope level (0-1023, 1023=silence)
    s32 output[2];      // Last two outputs for feedback
};

struct FMChannel {
    FMOperator op[4];
    u16 fnum;           // Frequency number
    u8 block;           // Octave block
    u8 keycode;         // Hardware keycode for key scaling and detune lookup
    u8 algorithm;       // Operator connection algorithm (0-7)
    u8 feedback;        // Operator 1 feedback level
    u8 ams, pms;        // AM/PM sensitivity
    u8 pan;             // Panning (bit 7=L, bit 6=R)
    bool keyOn[4];      // Key on state per operator
    u8 freqHiLatch;     // Latched $A4 write for frequency update on $A0 write
    s32 mem_value;      // Delayed operator output for inter-sample feedback
};

struct YMSample {
    s16 left;
    s16 right;
};

class YM2612Test;

class YM2612 {
public:
    YM2612();
    void reset();
    
    void writeAddress(u8 data, int port);  // Write to address port (port 0 or 1)
    void writeData(u8 data, int port);     // Write to data port
    u8 readStatus();                        // Read status register
    
    YMSample tick();
    void tickMany(int count, s32& leftAccum, s32& rightAccum);
    void getSamples(s16* left, s16* right, int count);
    
    bool isBusy() const { return busyCounter > 0; }

    friend class Genesis;
    friend class YM2612Test;

private:
    FMChannel channels[6];
    
    u8 addressLatch[2];  // Address latches for port 0 and 1
    
    // Global state
    u8 lfoFreq;
    bool lfoEnabled;
    u8 dacData;
    s32 dacOut;         // Pre-computed DAC output (14-bit signed), computed at write time
    bool dacEnabled;
    
    // Timers
    u16 timerA;
    u8 timerB;
    u8 timerControl;
    bool timerAOverflow;
    bool timerBOverflow;
    int timerACounter;
    int timerBCounter;
    int timerBSubCounter;

    int busyCounter;
    u32 egCounter;  // Global envelope generator counter
    u8 egSubCounter; // EG updates every 3 FM samples

    // CH3 special mode / CSM
    bool ch3SpecialMode;
    bool csmMode;           // CSM mode: Timer A overflow triggers key-on for CH3
    u16 ch3Fnum[3];
    u8 ch3Block[3];
    u8 ch3Keycode[3];
    u8 ch3FreqHiLatch[3];

    // LFO state
    u32 lfoCounter;
    u32 lfoPeriod;
    u8 lfoCnt;      // LFO step counter (0-127), advances once per lfoPeriod samples
    u8 lfoAM;       // Current AM attenuation (0-126, 7-bit)
    u8 lfoPM;       // Current PM index (0-31), quarter-rate of lfoCnt

    // Internal FM generation
    s32 generateChannel(int ch);
    s32 generateOperator(FMOperator& op, s32 modulation, u32 amAttenuation);
    void updateEnvelope(FMOperator& op, int keycode);
    void updatePhase(int chIdx);
    void keyOn(int ch, int op);
    void keyOff(int ch, int op);
    void advanceState();
    void advanceAttack(FMOperator& op, int rate);
    void advanceDecay(FMOperator& op, int rate);
    void advanceSustain(FMOperator& op, int rate);
    void advanceRelease(FMOperator& op, int rate);
    bool handleSSGEGBoundary(FMOperator& op);
    
    void writeRegister(u8 addr, u8 data, int bank);
};
