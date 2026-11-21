#pragma once

#include <cstdint>

namespace dsh::sync {

[[nodiscard]] std::uint64_t tick_index(std::uint64_t epochMs, std::uint64_t nowMs,
                                       std::uint64_t intervalMs);

[[nodiscard]] std::uint64_t ticks_skipped(std::uint64_t previousIndex, std::uint64_t currentIndex);

}
