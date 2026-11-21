#include "dsh/net/packet.h"

namespace dsh::net {

void write_header(ByteWriter& w, const PacketHeader& h) {
    w.u16(h.version);
    w.u16(static_cast<std::uint16_t>(h.type));
    w.u8(h.senderPeer);
    w.u8(h.flags);
    w.u32(h.sequence);
    if (h.has_ack()) {
        w.u32(h.ackThrough);
        w.u32(h.ackSelective);
    }
}

bool read_header(ByteReader& r, PacketHeader& out, std::uint16_t expectedVersion) {
    std::uint16_t version = 0;
    std::uint16_t typeValue = 0;
    if (!r.u16(version)) return false;

    out.version = version;

    if (!r.u16(typeValue) || !r.u8(out.senderPeer) || !r.u8(out.flags) || !r.u32(out.sequence)) {
        return false;
    }
    out.type = static_cast<MessageType>(typeValue);

    if (out.has_ack()) {
        if (!r.u32(out.ackThrough) || !r.u32(out.ackSelective)) return false;
    }

    if (version != expectedVersion) return false;

    return true;
}

std::vector<std::uint8_t> build_packet(const PacketHeader& h, std::span<const std::uint8_t> payload) {
    ByteWriter w;
    write_header(w, h);
    if (!payload.empty()) w.raw(payload.data(), payload.size());
    return w.take();
}

bool message_is_relayed(MessageType type) noexcept {
    switch (type) {
        case MessageType::playerState:
        case MessageType::playerStateBatch:
        case MessageType::actionEdge:
        case MessageType::ammoSpawn:
        case MessageType::playerHealth:
            return true;
        default:

            return false;
    }
}

}
