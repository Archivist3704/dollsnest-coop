#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace dsh::game {

inline constexpr std::uint16_t kMaxHealthEntries = 256;
inline constexpr std::uint8_t  kMaxHealthName = 96;

struct HealthEntry {
    std::string   enemyName;
    std::uint32_t sequence = 0;
    float         value = 0.0f;
};

[[nodiscard]] std::vector<std::uint8_t> encode_health_report(const std::vector<HealthEntry>& entries,
                                                             std::string& note);

[[nodiscard]] std::vector<std::uint8_t> encode_health_broadcast(const std::vector<HealthEntry>& entries,
                                                                std::string& note);

[[nodiscard]] bool decode_health(std::span<const std::uint8_t> bytes, bool requirePositiveValue,
                                 std::vector<HealthEntry>& out, std::string& note);

inline constexpr std::uint8_t kPlayerHealthFormat = 1;
inline constexpr std::size_t  kPlayerHealthBytes = 9;

struct PlayerHealth {
    float health = 0.0f;
    float maxHealth = 0.0f;
};

[[nodiscard]] std::vector<std::uint8_t> encode_player_health(const PlayerHealth& value, std::string& note);

[[nodiscard]] bool decode_player_health(std::span<const std::uint8_t> bytes, PlayerHealth& out,
                                        std::string& note);

}
