#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "dsh/common.h"
#include "dsh/game/appearance_wire.h"
#include "dsh/sync/interpolator.h"
#include "dsh/sync/playback_clock.h"

namespace dsh::game {

struct PuppetDoll {
    dsh::PeerId peer = dsh::kInvalidPeer;

    AppearanceAssembler assembler;
    dsh::reflect::MachineLoadout loadout;
    bool haveLoadout = false;

    bool ackScheduled = false;
    int  ackPending = 0;

    bool buildAttempted = false;
    bool built = false;
    std::vector<std::uintptr_t> actors;

    dsh::sync::PoseInterpolator interp;
    bool interpHave = false;

    dsh::sync::PlaybackClock clock;

    float latestSampleM[3] = {0.0f, 0.0f, 0.0f};
    bool  latestSampleHave = false;

    std::uint32_t teleportsSeen = 0;
    std::uint64_t lastTeleportLogMs = 0;

    float watchHealthLast = -1.0f;
    int   watchLines = 0;
    float watchMaxDipCm = 0.0f;

    std::uint32_t damageBlockedWrites = 0;

    std::uint32_t lastActionSequence = 0;
    std::uint8_t lastExclusiveTypes = 0;
    std::uint8_t lastExclusiveOwnerKind = 0;

    std::vector<std::vector<std::uint8_t>> manifestDatagrams;

    std::uint8_t manifestSentMask = 0;
};

class PuppetDolls {
public:

    static constexpr std::size_t kSlots = 8;

    [[nodiscard]] PuppetDoll* find(dsh::PeerId peer) noexcept;
    [[nodiscard]] const PuppetDoll* find(dsh::PeerId peer) const noexcept;

    [[nodiscard]] PuppetDoll* ensure(dsh::PeerId peer) noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t built_count() const noexcept;

    [[nodiscard]] std::array<std::optional<PuppetDoll>, kSlots>& slots() noexcept { return slots_; }
    [[nodiscard]] const std::array<std::optional<PuppetDoll>, kSlots>& slots() const noexcept {
        return slots_;
    }

    void clear() noexcept;

private:
    std::array<std::optional<PuppetDoll>, kSlots> slots_{};
};

[[nodiscard]] std::uint8_t peers_needing_manifest(std::uint8_t connectedPeers, std::uint8_t alreadySent,
                                                  dsh::PeerId owner) noexcept;

}
