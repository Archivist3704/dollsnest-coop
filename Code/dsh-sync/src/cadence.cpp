#include "dsh/sync/cadence.h"

namespace dsh::sync {

std::uint64_t tick_index(std::uint64_t epochMs, std::uint64_t nowMs, std::uint64_t intervalMs) {
    if (intervalMs == 0 || nowMs <= epochMs) return 0;
    return (nowMs - epochMs) / intervalMs;
}

std::uint64_t ticks_skipped(std::uint64_t previousIndex, std::uint64_t currentIndex) {
    return currentIndex > previousIndex + 1 ? currentIndex - previousIndex - 1 : 0;
}

}
