#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace dsh::game {

inline constexpr std::uint16_t kMaxMissionListEntries = 64;
inline constexpr std::uint8_t  kMaxMissionIdLength    = 64;

struct MissionListEntry {
    std::string  id;
    std::uint8_t state = 0;
};

[[nodiscard]] std::vector<std::uint8_t> encode_mission_list(const std::vector<MissionListEntry>& entries,
                                                            std::string& note);

[[nodiscard]] bool decode_mission_list(std::span<const std::uint8_t> bytes,
                                       std::vector<MissionListEntry>& out, bool& noneReported,
                                       std::string& note);

}
