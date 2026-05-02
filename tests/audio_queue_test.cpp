#include "audio/audio_queue.h"

#include <cstdio>

static int tests_run = 0;
static int tests_failed = 0;

static void check(bool condition, const char* message) {
    tests_run++;
    if (condition) {
        std::printf("PASS: %s\n", message);
    } else {
        std::printf("FAIL: %s\n", message);
        tests_failed++;
    }
}

static void testProducerQueuedBytesDriveOverflowDecision() {
    AudioQueueSnapshot snapshot{};
    snapshot.producerQueuedBytes = 48001;
    snapshot.convertedAvailableBytes = 0;

    check(shouldClearAudioProducerQueue(snapshot, 48000 * 2 * 2, 250),
          "producer queued bytes trigger clear even when converted availability is empty");
}

static void testQueueLimitIsExclusive() {
    AudioQueueSnapshot snapshot{};
    snapshot.producerQueuedBytes = 48000;
    snapshot.convertedAvailableBytes = 48000;

    check(!shouldClearAudioProducerQueue(snapshot, 48000 * 2 * 2, 250),
          "queue clear threshold allows exactly the configured buffer size");
}

static void testInvalidMeasurementsDoNotClear() {
    AudioQueueSnapshot snapshot{};
    snapshot.producerQueuedBytes = -1;
    snapshot.convertedAvailableBytes = -1;

    check(!shouldClearAudioProducerQueue(snapshot, 48000 * 2 * 2, 250),
          "invalid SDL queue measurements do not clear audio");
}

int main() {
    testProducerQueuedBytesDriveOverflowDecision();
    testQueueLimitIsExclusive();
    testInvalidMeasurementsDoNotClear();

    std::printf("\nResults: %d/%d passed\n", tests_run - tests_failed, tests_run);
    return tests_failed == 0 ? 0 : 1;
}
