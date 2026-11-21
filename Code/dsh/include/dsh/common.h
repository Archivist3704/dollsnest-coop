#pragma once

#include <cstdint>
#include <cstddef>
#include <string_view>

namespace dsh {

struct PeerId {
    std::uint8_t value = 0;
    [[nodiscard]] constexpr bool operator==(const PeerId& o) const noexcept { return value == o.value; }
    [[nodiscard]] constexpr bool operator!=(const PeerId& o) const noexcept { return value != o.value; }
};

inline constexpr PeerId kInvalidPeer{0xFF};
inline constexpr PeerId kHostPeer{0};

struct EntityId {
    PeerId        authority{};
    std::uint64_t counter = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return authority != kInvalidPeer && counter != 0; }
    [[nodiscard]] constexpr bool operator==(const EntityId& o) const noexcept {
        return authority == o.authority && counter == o.counter;
    }
    [[nodiscard]] constexpr bool operator!=(const EntityId& o) const noexcept { return !(*this == o); }
};

using WorldGeneration = std::uint32_t;
inline constexpr WorldGeneration kNoGeneration = 0;

using Sequence = std::uint32_t;

struct EventKey {
    PeerId          peer{};
    WorldGeneration generation = kNoGeneration;
    Sequence        seq = 0;
};

[[nodiscard]] constexpr std::string_view to_string(PeerId p) noexcept {
    return p.value == kInvalidPeer.value ? std::string_view{"invalid"} : std::string_view{"peer"};
}

}
