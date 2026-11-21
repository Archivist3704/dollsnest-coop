#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace dsh::game {

inline constexpr std::size_t kMaxItemPath = 240;

inline constexpr std::size_t kItemUseCapacity = 32;

struct ItemUseSample {
    std::uintptr_t itemClass = 0;
    std::uint64_t  ms = 0;
};

void note_item_use(std::uintptr_t itemClass, std::uint64_t ms);

[[nodiscard]] bool take_item_use(ItemUseSample& out);

struct ItemUseStats {
    std::uint64_t noted = 0;
    std::uint64_t dropped = 0;
    std::uint64_t drained = 0;
    std::uint64_t sent = 0;
    std::uint64_t refused = 0;
    std::uint64_t refusedForeign = 0;
};
[[nodiscard]] ItemUseStats item_use_stats();

void note_item_use_refused();

void note_item_use_foreign();

void note_item_use_sent();

inline constexpr std::uint8_t kItemWireFormat = 1;

[[nodiscard]] std::vector<std::uint8_t> encode_item_use(const std::string& classPath, std::string& note);

[[nodiscard]] bool decode_item_use(std::span<const std::uint8_t> bytes, std::string& classPath,
                                   std::string& note);

}
