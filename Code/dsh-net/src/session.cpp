#include "dsh/net/session.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dsh::net {
namespace {

void write_u32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 24) & 0xFF));
}

void write_u64(std::vector<std::uint8_t>& v, std::uint64_t x) {
    write_u32(v, static_cast<std::uint32_t>(x & 0xFFFFFFFFu));
    write_u32(v, static_cast<std::uint32_t>((x >> 32) & 0xFFFFFFFFu));
}

bool read_u32(std::span<const std::uint8_t> b, std::size_t off, std::uint32_t& out) {
    if (off + 4 > b.size()) return false;
    out = static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8) |
          (static_cast<std::uint32_t>(b[off + 2]) << 16) |
          (static_cast<std::uint32_t>(b[off + 3]) << 24);
    return true;
}

bool read_u64(std::span<const std::uint8_t> b, std::size_t off, std::uint64_t& out) {
    std::uint32_t lo = 0, hi = 0;
    if (!read_u32(b, off, lo) || !read_u32(b, off + 4, hi)) return false;
    out = static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
    return true;
}

}

const char* to_string(PeerState s) noexcept {
    switch (s) {
        case PeerState::unknown: return "unknown";
        case PeerState::connecting: return "connecting";
        case PeerState::connected: return "connected";
        case PeerState::timedOut: return "timedOut";
    }
    return "?";
}

Session::Session(SessionConfig config) : config_(std::move(config)) {
    if (config_.maxPlayers < 2) config_.maxPlayers = 2;
    if (config_.maxPlayers > 8) config_.maxPlayers = 8;
    peers_.reserve(config_.maxPlayers);
}

void Session::become_host(PeerId myId) {
    role_ = SessionRole::host;
    localId_ = myId;
    peers_.clear();
    nextPeerSlot_ = 1;
}

void Session::become_client(PeerId myId, Endpoint hostEndpoint) {
    role_ = SessionRole::client;

    localId_ = myId;
    hostEndpoint_ = std::move(hostEndpoint);
    peers_.clear();
    peers_.resize(1);
    peers_[0].endpoint = hostEndpoint_;
    peers_[0].state = PeerState::connecting;
    peers_[0].name = "host";

    peers_[0].id = PeerId{0};

    nextAnnounceMs_ = 0;
    handshakeStartMs_ = 0;
}

void Session::set_send_function(SendFn fn) { send_ = std::move(fn); }

std::uint8_t Session::connected_count() const noexcept {
    std::uint8_t n = 0;
    for (const auto& p : peers_) {
        if (p.state == PeerState::connected) ++n;
    }
    return n;
}

const Peer* Session::find_peer(PeerId id) const noexcept {
    for (const auto& p : peers_) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

const Peer* Session::find_peer(const Endpoint& ep) const noexcept {
    for (const auto& p : peers_) {
        if (p.endpoint == ep) return &p;
    }
    return nullptr;
}

Peer* Session::ensure_peer_for(const Endpoint& from, std::uint64_t nowMs, const std::string& name,
                               bool createIfMissing) {
    for (auto& p : peers_) {
        if (p.endpoint == from) return &p;
    }
    if (!createIfMissing) return nullptr;

    if (peers_.size() >= config_.maxPlayers) return nullptr;

    Peer p;
    p.endpoint = from;
    p.state = PeerState::unknown;
    p.name = name;
    p.firstSeenMs = nowMs;
    p.lastHeardMs = nowMs;
    p.windowStartMs = nowMs;
    peers_.push_back(std::move(p));
    return &peers_.back();
}

void Session::push_event(SessionEventKind kind, PeerId peer, std::uint32_t value,
                         std::string text) {
    SessionEvent e;
    e.kind = kind;
    e.peer = peer;
    e.value = value;
    e.text = std::move(text);
    events_.push_back(std::move(e));
}

void Session::take_events(std::vector<SessionEvent>& out) {
    out.insert(out.end(), events_.begin(), events_.end());
    events_.clear();
}

void Session::note_bytes(Peer& p, std::uint32_t n, std::uint64_t nowMs) {
    if (p.windowStartMs == 0) p.windowStartMs = nowMs;

    if (nowMs - p.windowStartMs >= 1000) {
        p.peakBytesPerWindow = std::max(p.peakBytesPerWindow, p.bytesThisWindow);
        p.bytesThisWindow = 0;
        p.windowStartMs = nowMs;
    }
    p.bytesThisWindow += n;
    if (p.bytesThisWindow > p.peakBytesPerWindow) {

    }
    if (p.bytesThisWindow > config_.perClientBytesPerSecond) {

        if (p.bytesThisWindow - n <= config_.perClientBytesPerSecond) {
            ++p.budgetExceededWindows;
            ++stats_.budgetAlarms;
            push_event(SessionEventKind::budgetExceeded, p.id, p.bytesThisWindow,
                       "per-client streaming budget exceeded");
        }
    }
}

void Session::send_raw(const Peer& p, std::span<const std::uint8_t> bytes) {
    if (!send_) return;
    send_(p.endpoint, bytes);
    stats_.datagramsSent += 1;
    stats_.bytesSent += bytes.size();
}

void Session::send_appearance_to(PeerId to, std::uint32_t sequence,
                                    std::span<const std::uint8_t> payload) {
    const Peer* p = find_peer(to);
    if (p == nullptr || p->state != PeerState::connected) return;
    PacketHeader h;
    h.version = kProtocolVersion;
    h.type = MessageType::playerStateBatch;
    h.senderPeer = localId_.value;
    h.sequence = sequence;
    const auto pkt = build_packet(h, payload);
    send_raw(*p, pkt);
    for (auto& q : peers_) {
        if (q.id == to) {
            note_bytes(q, static_cast<std::uint32_t>(pkt.size()), q.lastSentMs);
            break;
        }
    }
}

void Session::send_appearance_ack_to(PeerId to, std::uint32_t sequence,
                                     std::span<const std::uint8_t> payload) {
    const Peer* p = find_peer(to);
    if (p == nullptr || p->state != PeerState::connected) return;
    PacketHeader h;
    h.version = kProtocolVersion;
    h.type = MessageType::appearanceAck;
    h.senderPeer = localId_.value;
    h.sequence = sequence;
    const auto pkt = build_packet(h, payload);
    send_raw(*p, pkt);
    for (auto& q : peers_) {
        if (q.id == to) {
            note_bytes(q, static_cast<std::uint32_t>(pkt.size()), q.lastSentMs);
            break;
        }
    }
}

void Session::send_action_to(PeerId to, std::uint32_t sequence,
                             std::span<const std::uint8_t> payload) {
    const Peer* p = find_peer(to);
    if (p == nullptr || p->state != PeerState::connected) return;
    PacketHeader h;
    h.version = kProtocolVersion;
    h.type = MessageType::actionEdge;
    h.senderPeer = localId_.value;
    h.sequence = sequence;
    const auto pkt = build_packet(h, payload);
    send_raw(*p, pkt);
    for (auto& q : peers_) {
        if (q.id == to) {
            note_bytes(q, static_cast<std::uint32_t>(pkt.size()), q.lastSentMs);
            break;
        }
    }
}

void Session::send_ammo_spawn_to(PeerId to, std::uint32_t sequence,
                                 std::span<const std::uint8_t> payload) {
    const Peer* p = find_peer(to);
    if (p == nullptr || p->state != PeerState::connected) return;
    PacketHeader h;
    h.version = kProtocolVersion;
    h.type = MessageType::ammoSpawn;
    h.senderPeer = localId_.value;
    h.sequence = sequence;
    const auto pkt = build_packet(h, payload);
    send_raw(*p, pkt);
    for (auto& q : peers_) {
        if (q.id == to) {
            note_bytes(q, static_cast<std::uint32_t>(pkt.size()), q.lastSentMs);
            break;
        }
    }
}

void Session::send_mission_list_request_to(PeerId to, std::uint32_t sequence) {
    const Peer* p = find_peer(to);
    if (p == nullptr || p->state != PeerState::connected) return;
    PacketHeader h;
    h.version = kProtocolVersion;
    h.type = MessageType::missionListRequest;
    h.senderPeer = localId_.value;
    h.sequence = sequence;
    const auto pkt = build_packet(h);
    send_raw(*p, pkt);
    for (auto& q : peers_) {
        if (q.id == to) {
            note_bytes(q, static_cast<std::uint32_t>(pkt.size()), q.lastSentMs);
            break;
        }
    }
}

void Session::send_mission_list_to(PeerId to, std::uint32_t sequence,
                                   std::span<const std::uint8_t> payload) {
    const Peer* p = find_peer(to);
    if (p == nullptr || p->state != PeerState::connected) return;
    PacketHeader h;
    h.version = kProtocolVersion;
    h.type = MessageType::missionList;
    h.senderPeer = localId_.value;
    h.sequence = sequence;
    const auto pkt = build_packet(h, payload);
    send_raw(*p, pkt);
    for (auto& q : peers_) {
        if (q.id == to) {
            note_bytes(q, static_cast<std::uint32_t>(pkt.size()), q.lastSentMs);
            break;
        }
    }
}

void Session::send_control(PeerId to, const Endpoint& ep, MessageType type, std::uint32_t sequence,
                           std::span<const std::uint8_t> payload) {
    PacketHeader h;
    h.version = kProtocolVersion;
    h.type = type;

    h.senderPeer = localId_.value;
    h.flags = 0;
    h.sequence = sequence;
    const auto pkt = build_packet(h, payload);
    if (!send_) return;
    send_(ep, pkt);
    stats_.datagramsSent += 1;
    stats_.bytesSent += pkt.size();
}

void Session::send_state_to(PeerId to, std::uint32_t sequence,
                            std::span<const std::uint8_t> payload) {
    const Peer* p = find_peer(to);
    if (p == nullptr || p->state != PeerState::connected) return;
    PacketHeader h;
    h.version = kProtocolVersion;
    h.type = MessageType::playerState;
    h.senderPeer = localId_.value;
    h.sequence = sequence;
    const auto pkt = build_packet(h, payload);
    send_raw(*p, pkt);

    for (auto& q : peers_) {
        if (q.id == to) {
            note_bytes(q, static_cast<std::uint32_t>(pkt.size()), q.lastSentMs);
            break;
        }
    }
}

void Session::on_datagram(std::uint64_t nowMs, const Endpoint& from,
                          std::span<const std::uint8_t> bytes) {
    stats_.datagramsReceived += 1;
    stats_.bytesReceived += bytes.size();

    ByteReader r(bytes);
    PacketHeader h;
    if (!read_header(r, h)) {

        PacketHeader probe;
        ByteReader r2(bytes);
        if (read_header(r2, probe, static_cast<std::uint16_t>(h.version))) {
            ++stats_.malformed;
        } else {
            ++stats_.wrongVersion;
        }
        return;
    }

    handle_packet(nowMs, from, h, bytes.subspan(r.cursor()));
}

void Session::handle_packet(std::uint64_t nowMs, const Endpoint& from, const PacketHeader& h,
                            std::span<const std::uint8_t> payload) {

    Peer* peer = ensure_peer_for(from, nowMs, "",
                                  h.type == MessageType::hello && role_ == SessionRole::host);
    if (peer == nullptr) {
        ++stats_.unknownSender;
        return;
    }
    peer->lastHeardMs = nowMs;

    switch (h.type) {
        case MessageType::hello: {
            if (role_ != SessionRole::host) return;
            std::string name(reinterpret_cast<const char*>(payload.data()), payload.size());
            if (name.empty() || name.size() > 32) name = "client";

            if (peer->id == kInvalidPeer) {
                if (nextPeerSlot_ >= config_.maxPlayers) {

                    push_event(SessionEventKind::handshakeFailed, kInvalidPeer, 0,
                               "server full: no peer slot left");
                    return;
                }
                peer->id = PeerId{nextPeerSlot_++};
                peer->name = name;
                peer->state = PeerState::connected;
                push_event(SessionEventKind::peerJoined, peer->id, 0, name);
            } else {

                ++stats_.duplicatesIgnored;
            }

            std::vector<std::uint8_t> ack;
            ack.push_back(peer->id.value);
            write_u32(ack, worldGeneration_);
            write_u64(ack, nowMs);
            send_control(peer->id, from, MessageType::helloAck, nextSequence_++, ack);
            return;
        }

        case MessageType::helloAck: {
            if (role_ != SessionRole::client) return;
            if (peer->state == PeerState::connected) {
                ++stats_.duplicatesIgnored;
                return;
            }
            if (payload.size() >= 5) {

                localId_ = PeerId{payload[0]};
                std::uint32_t gen = 0;
                if (read_u32(payload, 1, gen)) worldGeneration_ = gen;
            }
            peer->name = "host";
            peer->state = PeerState::connected;

            push_event(SessionEventKind::peerJoined, PeerId{0}, 0, "host");
            return;
        }

        case MessageType::keepAlive: {

            if (peer->state == PeerState::timedOut) {
                peer->state = PeerState::connected;
            }
            return;
        }

        case MessageType::pingProbe: {

            std::uint64_t t0 = 0;
            read_u64(payload, 0, t0);
            std::vector<std::uint8_t> pong;
            write_u64(pong, t0);
            write_u64(pong, nowMs);
            send_control(localId_, from, MessageType::pongReply, nextSequence_++, pong);
            return;
        }

        case MessageType::pongReply: {
            std::uint64_t t0 = 0, t1 = 0;
            if (!read_u64(payload, 0, t0) || !read_u64(payload, 8, t1)) return;
            if (!peer->pingInFlight) {
                ++stats_.duplicatesIgnored;
                return;
            }
            peer->pingInFlight = false;
            const std::uint64_t t2 = nowMs;
            if (t2 < t0) return;
            const auto rtt = static_cast<std::uint32_t>(t2 - t0);

            const std::int64_t offset =
                static_cast<std::int64_t>(t1) - static_cast<std::int64_t>(t0 + (t2 - t0) / 2);

            if (!peer->clockValid || rtt < peer->clockRttMs) {
                peer->clockValid = true;
                peer->clockOffsetMs = offset;
                peer->clockRttMs = rtt;
            }
            ++peer->clockSamples;
            return;
        }

        case MessageType::playerState:
        case MessageType::playerStateBatch:
        case MessageType::gameplayEvent:

            return;

        default:
            return;
    }
}

void Session::update(std::uint64_t nowMs) {
    if (role_ == SessionRole::client) {
        if (handshakeStartMs_ == 0) handshakeStartMs_ = nowMs;
        Peer* host = peers_.empty() ? nullptr : &peers_[0];

        if (host != nullptr && host->state == PeerState::connecting) {
            if (nowMs - handshakeStartMs_ > config_.handshakeTimeoutMs) {
                push_event(SessionEventKind::handshakeFailed, kInvalidPeer,
                           static_cast<std::uint32_t>(nowMs - handshakeStartMs_),
                           "no helloAck within handshakeTimeoutMs");

                handshakeStartMs_ = nowMs;
            }
            if (nowMs >= nextAnnounceMs_) {
                nextAnnounceMs_ = nowMs + config_.handshakeRetryMs;
                const auto& nm = config_.playerName;
                send_control(localId_, host->endpoint, MessageType::hello, nextSequence_++,
                             std::span<const std::uint8_t>(
                                 reinterpret_cast<const std::uint8_t*>(nm.data()), nm.size()));
            }
        }
    }

    for (auto& p : peers_) {
        if (p.state == PeerState::connected) {

            if (nowMs - p.lastSentMs >= config_.keepAliveIntervalMs) {
                std::vector<std::uint8_t> none;
                send_control(localId_, p.endpoint, MessageType::keepAlive, nextSequence_++, none);
                p.lastSentMs = nowMs;
            }

            if (!p.pingInFlight && (p.clockSamples == 0 ||
                                    nowMs - p.lastSentMs >= config_.clockSyncIntervalMs)) {
                std::vector<std::uint8_t> probe;
                write_u64(probe, nowMs);
                send_control(localId_, p.endpoint, MessageType::pingProbe, nextSequence_++, probe);
                p.pingInFlight = true;
                p.pingSentMs = nowMs;
            }

            if (nowMs - p.lastHeardMs > config_.disconnectTimeoutMs) {
                p.state = PeerState::timedOut;
                push_event(SessionEventKind::peerTimedOut, p.id,
                           static_cast<std::uint32_t>(nowMs - p.lastHeardMs),
                           "no datagram within disconnectTimeoutMs");
            }
        } else if (p.state == PeerState::connecting) {
            if (nowMs - p.lastHeardMs > config_.disconnectTimeoutMs && p.lastHeardMs != 0) {
                p.state = PeerState::timedOut;
                push_event(SessionEventKind::peerTimedOut, p.id,
                           static_cast<std::uint32_t>(nowMs - p.lastHeardMs), "handshake stalled");
            }
        }

        if (p.windowStartMs == 0) p.windowStartMs = nowMs;
        if (nowMs - p.windowStartMs >= 1000) {
            p.peakBytesPerWindow = std::max(p.peakBytesPerWindow, p.bytesThisWindow);
            p.bytesThisWindow = 0;
            p.windowStartMs = nowMs;
        }
    }
}

Session::RemoteTime Session::remote_time(std::uint64_t nowMs) const {
    RemoteTime t;
    for (const auto& p : peers_) {
        if (p.state == PeerState::connected && p.clockValid) {
            t.ms = static_cast<std::int64_t>(nowMs) + p.clockOffsetMs;
            t.valid = true;
            return t;
        }
    }
    t.ms = static_cast<std::int64_t>(nowMs);
    t.valid = false;
    return t;
}

std::string Session::status_line(std::uint64_t nowMs) const {
    char buf[256];
    std::string out;
    std::snprintf(buf, sizeof(buf), "role=%s me=%u peers=%zu connected=%u",
                  role_ == SessionRole::host ? "host"
                                             : (role_ == SessionRole::client ? "client" : "none"),
                  static_cast<unsigned>(localId_.value), peers_.size(),
                  static_cast<unsigned>(connected_count()));
    out += buf;
    for (const auto& p : peers_) {
        std::snprintf(buf, sizeof(buf), " | #%u %s heard=%llums rtt=%ums off=%lldms",
                      static_cast<unsigned>(p.id.value), to_string(p.state),
                      static_cast<unsigned long long>(p.ms_since_heard(nowMs)),
                      static_cast<unsigned>(p.clockRttMs),
                      static_cast<long long>(p.clockOffsetMs));
        out += buf;
    }
    std::snprintf(buf, sizeof(buf), " | tx=%llu rx=%llu malformed=%llu alarms=%llu relayed=%llu relayRefused=%llu",
                  static_cast<unsigned long long>(stats_.datagramsSent),
                  static_cast<unsigned long long>(stats_.datagramsReceived),
                  static_cast<unsigned long long>(stats_.malformed),
                  static_cast<unsigned long long>(stats_.budgetAlarms),
                  static_cast<unsigned long long>(stats_.relayedDatagrams),
                  static_cast<unsigned long long>(stats_.relayRefused));
    out += buf;
    return out;
}

void Session::send_health_report_to(PeerId to, std::uint32_t sequence,
                                    std::span<const std::uint8_t> payload) {
    const Peer* p = find_peer(to);
    if (p == nullptr || p->state != PeerState::connected) return;
    PacketHeader h;
    h.version = kProtocolVersion;
    h.type = MessageType::healthReport;
    h.senderPeer = localId_.value;
    h.sequence = sequence;
    const auto pkt = build_packet(h, payload);
    send_raw(*p, pkt);
}

void Session::send_health_broadcast_to(PeerId to, std::uint32_t sequence,
                                       std::span<const std::uint8_t> payload) {
    const Peer* p = find_peer(to);
    if (p == nullptr || p->state != PeerState::connected) return;
    PacketHeader h;
    h.version = kProtocolVersion;
    h.type = MessageType::healthBroadcast;
    h.senderPeer = localId_.value;
    h.sequence = sequence;
    const auto pkt = build_packet(h, payload);
    send_raw(*p, pkt);
}

void Session::send_player_health_to(PeerId to, std::uint32_t sequence,
                                    std::span<const std::uint8_t> payload) {
    const Peer* p = find_peer(to);
    if (p == nullptr || p->state != PeerState::connected) return;
    PacketHeader h;
    h.version = kProtocolVersion;
    h.type = MessageType::playerHealth;
    h.senderPeer = localId_.value;
    h.sequence = sequence;
    const auto pkt = build_packet(h, payload);
    send_raw(*p, pkt);
}

void Session::send_item_use_to(PeerId to, std::uint32_t sequence,
                               std::span<const std::uint8_t> payload) {
    const Peer* p = find_peer(to);
    if (p == nullptr || p->state != PeerState::connected) return;
    PacketHeader h;
    h.version = kProtocolVersion;
    h.type = MessageType::itemUse;
    h.senderPeer = localId_.value;
    h.sequence = sequence;
    const auto pkt = build_packet(h, payload);
    send_raw(*p, pkt);
}

void Session::relay_from_endpoint(const Endpoint& from, std::span<const std::uint8_t> datagram,
                                  std::uint64_t nowMs) {
    if (role_ != SessionRole::host) {
        ++stats_.relayRefused;
        return;
    }
    Peer* origin = nullptr;
    for (auto& p : peers_) {
        if (p.endpoint == from) {
            origin = &p;
            break;
        }
    }
    if (origin == nullptr || origin->state != PeerState::connected || origin->id == kInvalidPeer) {
        ++stats_.relayRefused;
        return;
    }

    ByteReader r(datagram);
    PacketHeader h;
    if (!read_header(r, h)) {
        ++stats_.relayRefused;
        return;
    }
    PacketHeader out = h;
    out.senderPeer = origin->id.value;
    const auto pkt = build_packet(out, datagram.subspan(r.cursor()));

    std::size_t copies = 0;
    for (auto& p : peers_) {
        if (p.state != PeerState::connected) continue;
        if (p.id == origin->id) continue;
        if (p.id == localId_) continue;
        send_raw(p, pkt);
        note_bytes(p, static_cast<std::uint32_t>(pkt.size()), nowMs);
        ++copies;
    }
    if (copies != 0) {
        ++stats_.relayedDatagrams;
        stats_.relayedBytes += static_cast<std::uint64_t>(pkt.size() * copies);
    }
}

void Session::relay_to_peer(const Endpoint& from, PeerId to, std::span<const std::uint8_t> datagram,
                            std::uint64_t nowMs) {
    if (role_ != SessionRole::host) {
        ++stats_.relayRefused;
        return;
    }
    Peer* origin = nullptr;
    for (auto& p : peers_) {
        if (p.endpoint == from) {
            origin = &p;
            break;
        }
    }
    if (origin == nullptr || origin->state != PeerState::connected || origin->id == kInvalidPeer) {
        ++stats_.relayRefused;
        return;
    }
    Peer* target = nullptr;
    for (auto& p : peers_) {
        if (p.id == to && p.state == PeerState::connected) {
            target = &p;
            break;
        }
    }

    if (target == nullptr || target->id == origin->id) {
        ++stats_.relayRefused;
        return;
    }
    ByteReader r(datagram);
    PacketHeader h;
    if (!read_header(r, h)) {
        ++stats_.relayRefused;
        return;
    }
    PacketHeader out = h;
    out.senderPeer = origin->id.value;
    const auto pkt = build_packet(out, datagram.subspan(r.cursor()));
    send_raw(*target, pkt);
    note_bytes(*target, static_cast<std::uint32_t>(pkt.size()), nowMs);
    ++stats_.relayedDatagrams;
    stats_.relayedBytes += static_cast<std::uint64_t>(pkt.size());
}

}
