#include "dsh/game/puppet_dolls.h"

namespace dsh::game {
namespace {

bool slot_for_peer(dsh::PeerId peer, std::size_t& index) noexcept {
    if (peer == dsh::kInvalidPeer) return false;
    if (peer.value >= PuppetDolls::kSlots) return false;
    index = static_cast<std::size_t>(peer.value);
    return true;
}

}

PuppetDoll* PuppetDolls::find(dsh::PeerId peer) noexcept {
    std::size_t index = 0;
    if (!slot_for_peer(peer, index)) return nullptr;
    if (!slots_[index]) return nullptr;
    return &*slots_[index];
}

const PuppetDoll* PuppetDolls::find(dsh::PeerId peer) const noexcept {
    std::size_t index = 0;
    if (!slot_for_peer(peer, index)) return nullptr;
    if (!slots_[index]) return nullptr;
    return &*slots_[index];
}

PuppetDoll* PuppetDolls::ensure(dsh::PeerId peer) noexcept {
    std::size_t index = 0;
    if (!slot_for_peer(peer, index)) return nullptr;
    auto& slot = slots_[index];
    if (!slot) {
        slot.emplace();
        slot->peer = peer;
    }
    return &*slot;
}

std::size_t PuppetDolls::size() const noexcept {
    std::size_t n = 0;
    for (const auto& slot : slots_) {
        if (slot) ++n;
    }
    return n;
}

std::size_t PuppetDolls::built_count() const noexcept {
    std::size_t n = 0;
    for (const auto& slot : slots_) {
        if (slot && slot->built) ++n;
    }
    return n;
}

void PuppetDolls::clear() noexcept {
    for (auto& slot : slots_) slot.reset();
}

std::uint8_t peers_needing_manifest(std::uint8_t connectedPeers, std::uint8_t alreadySent,
                                    dsh::PeerId owner) noexcept {

    static_assert(PuppetDolls::kSlots <= 8, "the peer mask is one byte");
    const std::uint8_t valid = static_cast<std::uint8_t>((1u << PuppetDolls::kSlots) - 1u);
    std::uint8_t need = static_cast<std::uint8_t>(connectedPeers & static_cast<std::uint8_t>(~alreadySent));
    need = static_cast<std::uint8_t>(need & valid);
    std::size_t ownerIndex = 0;
    if (slot_for_peer(owner, ownerIndex)) {
        need = static_cast<std::uint8_t>(need & static_cast<std::uint8_t>(~(1u << ownerIndex)));
    }
    return need;
}

}
