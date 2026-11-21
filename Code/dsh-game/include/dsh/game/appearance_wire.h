#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dsh/reflect/spawn.h"

namespace dsh::game {

inline constexpr std::uint8_t kAppearanceVersion = 5;

inline constexpr std::size_t kAppearanceAckBytes = 1;

inline constexpr std::size_t kAppearanceChunkBytes = 1100;

inline constexpr std::size_t kAppearanceChunkHeader = 8;

inline constexpr std::uint16_t kAppearanceMaxChunks = 64;

[[nodiscard]] std::vector<std::uint8_t> encode_loadout_manifest(
    const dsh::reflect::MachineLoadout* loadout = nullptr);

[[nodiscard]] bool decode_loadout_manifest(std::span<const std::uint8_t> bytes,
                                     std::string& note,
                                     dsh::reflect::MachineLoadout* loadoutOut = nullptr);

[[nodiscard]] std::vector<std::vector<std::uint8_t>> chunk_appearance(
    const std::vector<std::uint8_t>& manifest, std::uint32_t manifestId);

class AppearanceAssembler {
public:

    bool add_chunk(std::span<const std::uint8_t> payload, std::string& note);

    [[nodiscard]] bool complete() const { return complete_; }
    [[nodiscard]] std::uint32_t manifest_id() const { return manifestId_; }

    bool take(std::string& note,
              dsh::reflect::MachineLoadout* loadoutOut = nullptr);

    [[nodiscard]] std::uint32_t chunksReceived() const { return received_; }

private:
    void reset();

    std::uint32_t                 manifestId_ = 0;
    std::uint32_t                 total_ = 0;
    std::uint32_t                 received_ = 0;
    std::uint32_t                 completed_ = 0;
    bool                          complete_ = false;
    std::vector<std::vector<std::uint8_t>> chunks_;
    std::vector<bool>             have_;
};

}
