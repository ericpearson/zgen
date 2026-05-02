// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "types.h"

#include <cstdint>

struct AudioQueueSnapshot {
    int producerQueuedBytes = 0;
    int convertedAvailableBytes = 0;
};

inline u32 audioProducerQueueLimitBytes(u32 bytesPerSecond, int bufferMs) {
    if (bytesPerSecond == 0 || bufferMs <= 0) {
        return 0;
    }
    return static_cast<u32>((static_cast<std::uint64_t>(bytesPerSecond) *
                             static_cast<std::uint64_t>(bufferMs)) / 1000);
}

inline bool shouldClearAudioProducerQueue(const AudioQueueSnapshot& snapshot,
                                          u32 bytesPerSecond,
                                          int bufferMs) {
    if (snapshot.producerQueuedBytes < 0) {
        return false;
    }

    const u32 limit = audioProducerQueueLimitBytes(bytesPerSecond, bufferMs);
    return limit > 0 && snapshot.producerQueuedBytes > static_cast<int>(limit);
}
