#pragma once

#include "dsh/common.h"

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dsh::sync {

struct EntityRecord {
    EntityId        id{};
    std::uint64_t   ownerHandle = 0;
    WorldGeneration generation = kNoGeneration;
    std::uint8_t    kind = 0;
    bool            alive = false;
};

enum class CreateResult {
    created,
    already_exists,
    rejected_invalid_id,
    rejected_counters_exhausted,
};

class EntityRegistry {
public:

    CreateResult create(PeerId authority, std::uint8_t kind, std::uint64_t ownerHandle,
                        WorldGeneration generation, EntityId& outId);

    bool destroy(EntityId id);

    [[nodiscard]] const EntityRecord* find(EntityId id) const;
    [[nodiscard]] std::size_t         size() const noexcept { return records_.size(); }

    std::size_t on_world_replaced();

    [[nodiscard]] WorldGeneration generation() const noexcept { return generation_; }

    [[nodiscard]] std::string_view stats_line() const noexcept { return stats_; }

private:
    WorldGeneration                        generation_ = 1;
    std::unordered_map<std::uint64_t, EntityRecord> records_;

    std::uint64_t                          nextCounter_[256] = {};
    mutable char                           stats_[160] = {};
};

[[nodiscard]] constexpr std::uint64_t pack(EntityId id) noexcept {
    return (static_cast<std::uint64_t>(id.authority.value) << 56) | (id.counter & 0x00FFFFFFFFFFFFFFull);
}

[[nodiscard]] constexpr EntityId unpack(std::uint64_t v) noexcept {
    EntityId id;
    id.authority.value = static_cast<std::uint8_t>(v >> 56);
    id.counter = v & 0x00FFFFFFFFFFFFFFull;
    return id;
}

}
