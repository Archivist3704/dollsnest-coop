#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "dsh/common.h"
#include "dsh/net/packet.h"
#include "dsh/net/transport.h"

namespace dsh::net {

struct SessionConfig {
    std::uint8_t  maxPlayers = 4;
    std::uint32_t keepAliveIntervalMs = 250;
    std::uint32_t handshakeRetryMs = 400;
    std::uint32_t handshakeTimeoutMs = 5000;
    std::uint32_t disconnectTimeoutMs = 3000;
    std::uint32_t clockSyncIntervalMs = 1000;

    std::uint32_t perClientBytesPerSecond = 20 * 1024;
    std::string   playerName = "player";
};

enum class SessionRole : std::uint8_t { none = 0, host = 1, client = 2 };

enum class PeerState : std::uint8_t {
    unknown = 0,
    connecting = 1,
    connected = 2,
    timedOut = 3,
};

[[nodiscard]] const char* to_string(PeerState s) noexcept;

struct Peer {
    PeerId        id = kInvalidPeer;
    Endpoint      endpoint;
    PeerState     state = PeerState::unknown;
    std::string   name;

    std::uint64_t firstSeenMs = 0;
    std::uint64_t lastHeardMs = 0;
    std::uint64_t lastSentMs = 0;

    bool          clockValid = false;
    std::int64_t  clockOffsetMs = 0;
    std::uint32_t clockRttMs = 0;
    std::uint32_t clockSamples = 0;

    bool          pingInFlight = false;
    std::uint64_t pingSentMs = 0;

    std::uint64_t windowStartMs = 0;
    std::uint32_t bytesThisWindow = 0;
    std::uint32_t peakBytesPerWindow = 0;
    std::uint64_t budgetExceededWindows = 0;

    [[nodiscard]] std::uint64_t ms_since_heard(std::uint64_t nowMs) const noexcept {
        return nowMs >= lastHeardMs ? nowMs - lastHeardMs : 0;
    }
};

enum class SessionEventKind : std::uint8_t {
    peerJoined = 0,
    peerLeft = 1,
    peerTimedOut = 2,
    handshakeFailed = 3,
    budgetExceeded = 4,
};

struct SessionEvent {
    SessionEventKind kind = SessionEventKind::peerJoined;
    PeerId           peer = kInvalidPeer;
    std::uint32_t    value = 0;
    std::string      text;
};

struct SessionStats {
    std::uint64_t datagramsSent = 0;
    std::uint64_t datagramsReceived = 0;
    std::uint64_t bytesSent = 0;
    std::uint64_t bytesReceived = 0;
    std::uint64_t malformed = 0;
    std::uint64_t wrongVersion = 0;
    std::uint64_t unknownSender = 0;
    std::uint64_t duplicatesIgnored = 0;
    std::uint64_t budgetAlarms = 0;

    std::uint64_t relayedDatagrams = 0;
    std::uint64_t relayedBytes = 0;
    std::uint64_t relayRefused = 0;
};

using SendFn = std::function<void(const Endpoint&, std::span<const std::uint8_t>)>;

class Session {
public:
    explicit Session(SessionConfig config = {});

    void   become_host(PeerId myId);
    void   become_client(PeerId myId, Endpoint hostEndpoint);
    void   set_send_function(SendFn fn);
    void   set_world_generation(WorldGeneration g) noexcept { worldGeneration_ = g; }

    [[nodiscard]] SessionRole   role() const noexcept { return role_; }
    [[nodiscard]] PeerId        local_id() const noexcept { return localId_; }
    [[nodiscard]] WorldGeneration world_generation() const noexcept { return worldGeneration_; }
    [[nodiscard]] std::uint8_t  connected_count() const noexcept;
    [[nodiscard]] const std::vector<Peer>& peers() const noexcept { return peers_; }
    [[nodiscard]] const Peer*   find_peer(PeerId id) const noexcept;
    [[nodiscard]] const Peer*   find_peer(const Endpoint& ep) const noexcept;
    [[nodiscard]] const SessionConfig& config() const noexcept { return config_; }
    [[nodiscard]] const SessionStats&  stats() const noexcept { return stats_; }

    void   on_datagram(std::uint64_t nowMs, const Endpoint& from,
                       std::span<const std::uint8_t> bytes);

    void   update(std::uint64_t nowMs);

    void   take_events(std::vector<SessionEvent>& out);

    [[nodiscard]] std::string status_line(std::uint64_t nowMs) const;

    struct RemoteTime {
        std::int64_t ms = 0;
        bool         valid = false;
    };
    [[nodiscard]] RemoteTime remote_time(std::uint64_t nowMs) const;

    void   send_state_to(PeerId to, std::uint32_t sequence,
                         std::span<const std::uint8_t> payload);

    void   send_appearance_to(PeerId to, std::uint32_t sequence,
                              std::span<const std::uint8_t> payload);

    void   send_appearance_ack_to(PeerId to, std::uint32_t sequence,
                                  std::span<const std::uint8_t> payload);

    void   send_action_to(PeerId to, std::uint32_t sequence,
                          std::span<const std::uint8_t> payload);

    void   send_ammo_spawn_to(PeerId to, std::uint32_t sequence,
                              std::span<const std::uint8_t> payload);

    void   send_mission_list_request_to(PeerId to, std::uint32_t sequence);
    void   send_mission_list_to(PeerId to, std::uint32_t sequence,
                                std::span<const std::uint8_t> payload);

    void   send_health_report_to(PeerId to, std::uint32_t sequence,
                                 std::span<const std::uint8_t> payload);
    void   send_health_broadcast_to(PeerId to, std::uint32_t sequence,
                                    std::span<const std::uint8_t> payload);

    void   send_player_health_to(PeerId to, std::uint32_t sequence,
                                 std::span<const std::uint8_t> payload);

    void   send_item_use_to(PeerId to, std::uint32_t sequence, std::span<const std::uint8_t> payload);

    void   relay_from_endpoint(const Endpoint& from, std::span<const std::uint8_t> datagram,
                               std::uint64_t nowMs);

    void   relay_to_peer(const Endpoint& from, PeerId to, std::span<const std::uint8_t> datagram,
                         std::uint64_t nowMs);

private:
    void   handle_packet(std::uint64_t nowMs, const Endpoint& from, const PacketHeader& h,
                         std::span<const std::uint8_t> payload);
    void   send_control(PeerId to, const Endpoint& ep, MessageType type, std::uint32_t sequence,
                        std::span<const std::uint8_t> payload);
    void   send_raw(const Peer& p, std::span<const std::uint8_t> bytes);
    void   note_bytes(Peer& p, std::uint32_t n, std::uint64_t nowMs);
    Peer*  ensure_peer_for(const Endpoint& from, std::uint64_t nowMs, const std::string& name,
                           bool createIfMissing);
    void   push_event(SessionEventKind kind, PeerId peer, std::uint32_t value, std::string text);

    SessionConfig        config_;
    SessionRole          role_ = SessionRole::none;
    PeerId               localId_ = kInvalidPeer;
    WorldGeneration      worldGeneration_ = kNoGeneration;
    Endpoint             hostEndpoint_;
    SendFn               send_;
    std::vector<Peer>    peers_;
    std::vector<SessionEvent> events_;
    SessionStats         stats_;

    std::uint32_t        nextSequence_ = 1;
    std::uint64_t        nextAnnounceMs_ = 0;
    std::uint64_t        handshakeStartMs_ = 0;
    std::uint8_t         nextPeerSlot_ = 1;
};

}
