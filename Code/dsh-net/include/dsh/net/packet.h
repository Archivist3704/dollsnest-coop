#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "dsh/net/bytes.h"

namespace dsh::net {

enum class MessageType : std::uint16_t {
    invalid = 0,

    hello = 1,
    helloAck = 2,
    keepAlive = 3,

    playerState = 16,
    playerStateBatch = 17,

    appearanceAck = 20,

    actionEdge = 21,

    ammoSpawn = 22,

    gameplayEvent = 32,

    healthReport = 33,
    healthBroadcast = 34,

    missionListRequest = 52,
    missionList = 53,

    playerHealth = 54,

    itemUse = 55,

    pingProbe = 48,
    pongReply = 49,
};

inline constexpr std::uint8_t kFlagHasAck = 0x01;

struct PacketHeader {
    std::uint16_t version = kProtocolVersion;
    MessageType   type = MessageType::invalid;
    std::uint8_t  senderPeer = 0;
    std::uint8_t  flags = 0;
    std::uint32_t sequence = 0;
    std::uint32_t ackThrough = 0;
    std::uint32_t ackSelective = 0;

    [[nodiscard]] bool has_ack() const noexcept { return (flags & kFlagHasAck) != 0; }
};

void write_header(ByteWriter& w, const PacketHeader& h);

bool read_header(ByteReader& r, PacketHeader& out, std::uint16_t expectedVersion = kProtocolVersion);

[[nodiscard]] std::vector<std::uint8_t> build_packet(const PacketHeader& h,
                                                    std::span<const std::uint8_t> payload = {});

[[nodiscard]] bool message_is_relayed(MessageType type) noexcept;

}
