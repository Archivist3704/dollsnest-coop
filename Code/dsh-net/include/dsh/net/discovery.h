#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace dsh::net {

inline constexpr std::uint16_t kDiscoveryPort = 47778;

struct BroadcastTarget {
    std::uint32_t address = 0;
    std::uint32_t localAddress = 0;
    std::uint32_t mask = 0;
    std::string   iface;
    std::string   text;
};

[[nodiscard]] std::uint32_t directed_broadcast(std::uint32_t ip, std::uint32_t mask);

[[nodiscard]] std::vector<BroadcastTarget> broadcast_targets();

[[nodiscard]] const std::vector<BroadcastTarget>& broadcast_targets_cached(std::uint32_t cacheMs);

[[nodiscard]] std::string local_ipv4_summary();

inline constexpr std::uint32_t kBeaconIntervalMs = 1000;

inline constexpr std::size_t kMaxRooms = 16;

inline constexpr std::size_t kBeaconHeaderBytes = 9;
inline constexpr std::size_t kBeaconMaxNameBytes = 200;
inline constexpr std::size_t kBeaconMaxMapBytes = 96;

[[nodiscard]] std::vector<std::uint8_t> encode_beacon(std::uint16_t sessionPort, std::string_view name,
                                                     std::string_view mapKey = {});

[[nodiscard]] bool decode_beacon(std::span<const std::uint8_t> payload, std::uint16_t& sessionPort,
                                 std::string& name, std::string& mapKey);

struct BeaconState {
    bool          active     = false;
    std::uint16_t sessionPort = 0;
    std::string   name;
    std::string   mapKey;
    std::uint32_t intervalMs = kBeaconIntervalMs;
    std::uint64_t nextSendMs = 0;
    std::uint64_t sent       = 0;

    void start(std::uint16_t port, std::string sessionName, std::string map, std::uint64_t nowMs);
    void stop();

    [[nodiscard]] bool tick(std::uint64_t nowMs);

    [[nodiscard]] std::vector<std::uint8_t> payload() const;
};

inline constexpr std::uint32_t kProbeIntervalMs = 1000;

[[nodiscard]] std::vector<std::uint8_t> encode_probe();

[[nodiscard]] bool is_probe(std::span<const std::uint8_t> payload);

[[nodiscard]] std::vector<std::uint32_t> subnet_probe_addresses(std::uint32_t ip, std::uint32_t mask,
                                                                std::uint32_t maxPerSubnet);

[[nodiscard]] std::vector<std::uint32_t> unicast_probe_addresses(std::uint32_t maxPerSubnet);

[[nodiscard]] bool parse_peer_addresses(std::string_view text, std::vector<std::uint32_t>& out,
                                        std::string& note);

struct Room {
    std::string   address;
    std::uint16_t endpointPort = 0;
    std::uint16_t sessionPort  = 0;
    std::string   name;
    std::string   mapKey;
    std::uint32_t announcements = 0;
};

struct SearchState {
    bool          active      = false;
    std::uint32_t windowMs    = 5000;
    std::uint64_t deadlineMs  = 0;

    std::string   mapKey;
    std::vector<Room> rooms;
    std::uint32_t packetsSeen     = 0;
    std::uint32_t packetsRejected = 0;

    std::uint32_t roomsOnAnotherMap = 0;

    std::uint64_t nextProbeMs = 0;
    std::uint32_t probesSent  = 0;

    void start(std::uint32_t window, std::uint64_t nowMs);
    void stop();

    [[nodiscard]] bool probe_due(std::uint64_t nowMs);

    [[nodiscard]] std::vector<std::uint8_t> probe_payload() const;

    [[nodiscard]] bool observe(std::span<const std::uint8_t> fromEndpointBytes,
                               std::span<const std::uint8_t> payload);

    [[nodiscard]] bool done(std::uint64_t nowMs) const;

    [[nodiscard]] std::string summary() const;
};

}
