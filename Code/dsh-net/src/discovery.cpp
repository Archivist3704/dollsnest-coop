#include "dsh/net/discovery.h"

#include "dsh/net/bytes.h"

#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace dsh::net {
namespace {

inline void put16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

inline std::uint16_t get16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

constexpr char kMagic[4] = {'D', 'S', 'H', 'D'};

constexpr char kProbeMagic[4] = {'D', 'S', 'H', 'Q'};

struct Ipv4Interface {
    std::uint32_t address = 0;
    std::uint32_t mask = 0;
    std::string   name;
};

std::string narrow_wchar(const wchar_t* w) {
    std::string out;
    if (w == nullptr) return out;
    for (const wchar_t* p = w; *p != L'\0'; ++p) {
        out.push_back((*p > 0 && *p < 128) ? static_cast<char>(*p) : '?');
    }
    return out;
}

std::vector<Ipv4Interface> ipv4_interfaces() {
    std::vector<Ipv4Interface> out;

    ULONG size = 16 * 1024;
    std::vector<std::uint8_t> buffer(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;

    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                    reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.assign(size, 0);
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (rc != NO_ERROR) return out;

    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter != nullptr;
         adapter = adapter->Next) {

        if (adapter->OperStatus != IfOperStatusUp) continue;
        const std::string name = narrow_wchar(adapter->FriendlyName);

        for (auto* uni = adapter->FirstUnicastAddress; uni != nullptr; uni = uni->Next) {
            if (uni->Address.lpSockaddr == nullptr) continue;
            if (uni->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* sin = reinterpret_cast<const sockaddr_in*>(uni->Address.lpSockaddr);
            const std::uint32_t address = ntohl(sin->sin_addr.s_addr);
            if ((address & 0xFF000000u) == 0x7F000000u) continue;
            const unsigned prefix = uni->OnLinkPrefixLength;
            if (prefix > 32) continue;
            const std::uint32_t mask =
                prefix == 0 ? 0u : (0xFFFFFFFFu << static_cast<unsigned>(32 - prefix));
            out.push_back(Ipv4Interface{address, mask, name});
        }
    }
    return out;
}

std::string dotted(std::uint32_t hostOrder) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (hostOrder >> 24) & 0xFFu, (hostOrder >> 16) & 0xFFu,
                  (hostOrder >> 8) & 0xFFu, hostOrder & 0xFFu);
    return buf;
}

int prefix_length(std::uint32_t mask) {
    int bits = 0;
    for (int i = 31; i >= 0; --i) {
        if ((mask & (1u << i)) == 0) break;
        ++bits;
    }
    return bits;
}

std::vector<BroadcastTarget> g_cachedTargets;
std::uint32_t                g_cacheStampMs = 0;
bool                         g_cacheValid = false;

}

std::uint32_t directed_broadcast(std::uint32_t ip, std::uint32_t mask) {
    if (mask == 0u || mask == 0xFFFFFFFFu) return 0u;

    const std::uint32_t inverted = ~mask;
    if ((inverted & (inverted + 1u)) != 0u) return 0u;
    return ip | inverted;
}

std::vector<BroadcastTarget> broadcast_targets() {
    std::vector<BroadcastTarget> out;

    BroadcastTarget limited;
    limited.address = 0xFFFFFFFFu;
    limited.text = "255.255.255.255 (the limited broadcast -- reaches the physical LAN only)";
    out.push_back(limited);

    for (const Ipv4Interface& iface : ipv4_interfaces()) {
        const std::uint32_t target = directed_broadcast(iface.address, iface.mask);
        if (target == 0u || target == 0xFFFFFFFFu) continue;
        BroadcastTarget t;
        t.address = target;
        t.localAddress = iface.address;
        t.mask = iface.mask;
        t.iface = iface.name;
        t.text = dotted(target) + "/" + std::to_string(prefix_length(iface.mask)) + " on '" + iface.name +
                 "' (this machine is " + dotted(iface.address) + ")";
        out.push_back(std::move(t));
    }
    return out;
}

const std::vector<BroadcastTarget>& broadcast_targets_cached(std::uint32_t cacheMs) {
    const std::uint32_t now = GetTickCount();
    if (!g_cacheValid || cacheMs == 0 || (now - g_cacheStampMs) >= cacheMs) {
        g_cachedTargets = broadcast_targets();
        g_cacheStampMs = now;
        g_cacheValid = true;
    }
    return g_cachedTargets;
}

std::string local_ipv4_summary() {
    std::string out;
    for (const Ipv4Interface& iface : ipv4_interfaces()) {
        if (!out.empty()) out += "; ";
        out += dotted(iface.address) + "/" + std::to_string(prefix_length(iface.mask)) + " on '" +
               iface.name + "'";
    }
    if (out.empty()) out = "no IPv4 interface could be read";
    return out;
}

std::vector<std::uint32_t> subnet_probe_addresses(std::uint32_t ip, std::uint32_t mask,
                                                  std::uint32_t maxPerSubnet) {
    std::vector<std::uint32_t> out;
    const std::uint32_t broadcast = directed_broadcast(ip, mask);
    if (broadcast == 0u) return out;
    const std::uint32_t network = ip & mask;

    const std::uint64_t span = static_cast<std::uint64_t>(broadcast) - static_cast<std::uint64_t>(network);
    if (span < 3u) return out;
    const std::uint64_t hosts = span - 1u;
    if (hosts > maxPerSubnet) return out;

    out.reserve(static_cast<std::size_t>(hosts));
    for (std::uint64_t a = static_cast<std::uint64_t>(network) + 1u;
         a < static_cast<std::uint64_t>(broadcast); ++a) {
        if (a == ip) continue;
        out.push_back(static_cast<std::uint32_t>(a));
    }
    return out;
}

std::vector<std::uint32_t> unicast_probe_addresses(std::uint32_t maxPerSubnet) {
    std::vector<std::uint32_t> out;
    for (const Ipv4Interface& iface : ipv4_interfaces()) {
        for (const std::uint32_t a : subnet_probe_addresses(iface.address, iface.mask, maxPerSubnet)) {
            out.push_back(a);
        }
    }
    return out;
}

namespace {

bool parse_dotted_quad(std::string_view text, std::uint32_t& out) {
    std::uint32_t octet[4] = {0, 0, 0, 0};
    int index = 0;
    std::uint32_t value = 0;
    bool haveDigit = false;
    for (const char c : text) {
        if (c == '.') {
            if (!haveDigit || index >= 3) return false;
            octet[index++] = value;
            value = 0;
            haveDigit = false;
        } else if (c >= '0' && c <= '9') {
            value = value * 10u + static_cast<std::uint32_t>(c - '0');
            if (value > 255u) return false;
            haveDigit = true;
        } else {
            return false;
        }
    }
    if (!haveDigit || index != 3) return false;
    octet[3] = value;
    const std::uint32_t address = (octet[0] << 24) | (octet[1] << 16) | (octet[2] << 8) | octet[3];

    if (address == 0u || address == 0xFFFFFFFFu) return false;
    out = address;
    return true;
}

}

bool parse_peer_addresses(std::string_view text, std::vector<std::uint32_t>& out, std::string& note) {
    out.clear();
    note.clear();

    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    std::size_t lineNo = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string_view raw =
            text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;
        ++lineNo;

        const auto trim = [](std::string_view& v) {
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r')) {
                v.remove_prefix(1);
            }
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) {
                v.remove_suffix(1);
            }
        };
        trim(raw);
        const std::size_t hash = raw.find('#');
        if (hash != std::string_view::npos) {
            raw = raw.substr(0, hash);
            trim(raw);
        }
        if (raw.empty()) continue;

        std::uint32_t address = 0;
        if (!parse_dotted_quad(raw, address)) {
            note = "line " + std::to_string(lineNo) + " ('" + std::string(raw) +
                   "') is not an IPv4 address";
            out.clear();
            return false;
        }
        out.push_back(address);
    }
    if (note.empty()) note = std::to_string(out.size()) + " address(es)";
    return true;
}

std::vector<std::uint8_t> encode_beacon(std::uint16_t sessionPort, std::string_view name,
                                        std::string_view mapKey) {
    const std::size_t len = name.size() > kBeaconMaxNameBytes ? kBeaconMaxNameBytes : name.size();
    const std::size_t mapLen =
        mapKey.size() > kBeaconMaxMapBytes ? kBeaconMaxMapBytes : mapKey.size();
    std::vector<std::uint8_t> out;
    out.reserve(kBeaconHeaderBytes + len + 1 + mapLen);
    out.insert(out.end(), kMagic, kMagic + 4);
    put16(out, kProtocolVersion);
    put16(out, sessionPort);
    out.push_back(static_cast<std::uint8_t>(len));
    out.insert(out.end(), name.begin(), name.begin() + static_cast<std::ptrdiff_t>(len));

    out.push_back(static_cast<std::uint8_t>(mapLen));
    out.insert(out.end(), mapKey.begin(), mapKey.begin() + static_cast<std::ptrdiff_t>(mapLen));
    return out;
}

bool decode_beacon(std::span<const std::uint8_t> payload, std::uint16_t& sessionPort,
                   std::string& name, std::string& mapKey) {
    sessionPort = 0;
    name.clear();
    mapKey.clear();
    if (payload.size() < kBeaconHeaderBytes) return false;
    if (std::memcmp(payload.data(), kMagic, 4) != 0) return false;

    sessionPort = get16(payload.data() + 6);
    const std::size_t len = payload[8];
    if (len > payload.size() - kBeaconHeaderBytes) return false;
    name.assign(reinterpret_cast<const char*>(payload.data() + kBeaconHeaderBytes), len);

    const std::size_t afterName = kBeaconHeaderBytes + len;
    if (payload.size() <= afterName) return true;
    const std::size_t mapLen = payload[afterName];
    if (mapLen == 0 || mapLen > payload.size() - afterName - 1) return true;
    mapKey.assign(reinterpret_cast<const char*>(payload.data() + afterName + 1), mapLen);
    return true;
}

void BeaconState::start(std::uint16_t port, std::string sessionName, std::string map,
                        std::uint64_t nowMs) {
    active = true;
    sessionPort = port;
    name = std::move(sessionName);
    mapKey = std::move(map);
    sent = 0;

    nextSendMs = nowMs;
}

void BeaconState::stop() {
    active = false;
    nextSendMs = 0;
}

bool BeaconState::tick(std::uint64_t nowMs) {
    if (!active) return false;
    if (nowMs < nextSendMs) return false;
    ++sent;

    const std::uint64_t interval = intervalMs == 0 ? kBeaconIntervalMs : intervalMs;
    nextSendMs += interval;
    if (nextSendMs <= nowMs) {

        nextSendMs = nowMs + interval;
    }
    return true;
}

std::vector<std::uint8_t> BeaconState::payload() const {
    return encode_beacon(sessionPort, name, mapKey);
}

void SearchState::start(std::uint32_t window, std::uint64_t nowMs) {
    active = true;
    windowMs = window == 0 ? 5000 : window;
    deadlineMs = nowMs + windowMs;
    rooms.clear();
    packetsSeen = 0;
    packetsRejected = 0;
    roomsOnAnotherMap = 0;
    probesSent = 0;

    nextProbeMs = nowMs;
}

void SearchState::stop() {
    active = false;
    deadlineMs = 0;
    nextProbeMs = 0;
}

std::vector<std::uint8_t> encode_probe() {
    std::vector<std::uint8_t> out;
    out.insert(out.end(), kProbeMagic, kProbeMagic + 4);
    put16(out, kProtocolVersion);
    return out;
}

bool is_probe(std::span<const std::uint8_t> payload) {
    if (payload.size() != 6) return false;
    if (std::memcmp(payload.data(), kProbeMagic, 4) != 0) return false;

    return get16(payload.data() + 4) == kProtocolVersion;
}

bool SearchState::probe_due(std::uint64_t nowMs) {
    if (!active) return false;
    if (nowMs < nextProbeMs) return false;
    ++probesSent;
    nextProbeMs += kProbeIntervalMs;
    if (nextProbeMs <= nowMs) nextProbeMs = nowMs + kProbeIntervalMs;
    return true;
}

std::vector<std::uint8_t> SearchState::probe_payload() const { return encode_probe(); }

bool SearchState::observe(std::span<const std::uint8_t> fromEndpointBytes,
                          std::span<const std::uint8_t> payload) {
    if (!active) return false;
    ++packetsSeen;

    std::uint16_t sessionPort = 0;
    std::string name;
    std::string roomMap;
    if (!decode_beacon(payload, sessionPort, name, roomMap)) {
        ++packetsRejected;
        return false;
    }

    if (roomMap != mapKey) {
        ++roomsOnAnotherMap;
        return false;
    }

    std::string key(reinterpret_cast<const char*>(fromEndpointBytes.data()), fromEndpointBytes.size());

    for (Room& r : rooms) {
        if (r.address == key) {
            ++r.announcements;

            r.sessionPort = sessionPort;
            return false;
        }
    }

    if (rooms.size() >= kMaxRooms) {
        ++packetsRejected;
        return false;
    }

    Room r;
    r.address = key;
    r.sessionPort = sessionPort;
    r.name = name;
    r.mapKey = std::move(roomMap);
    r.announcements = 1;
    rooms.push_back(std::move(r));
    return true;
}

bool SearchState::done(std::uint64_t nowMs) const {
    return !active || nowMs >= deadlineMs;
}

std::string SearchState::summary() const {

    if (!active && packetsSeen == 0 && rooms.empty()) return "the search is not running";
    const std::string asked = std::to_string(probesSent) + " probe(s) sent; ";

    const std::string elsewhere =
        roomsOnAnotherMap == 0
            ? std::string()
            : (std::to_string(roomsOnAnotherMap) + " announcement(s) came from a host on ANOTHER map ('" +
               "ours: " + (mapKey.empty() ? std::string("<unreadable>") : mapKey) +
               "'), and none of them was allowed into the table; ");
    if (!rooms.empty()) {
        return elsewhere + std::to_string(rooms.size()) + " room(s) from " +
               std::to_string(packetsSeen) + " announcement(s) (" + asked +
               "answered or overheard)";
    }
    if (packetsSeen == 0) {

        return "heard NOTHING -- " + asked +
               "no answer and no announcement reached this machine. That means nothing is hosting on a "
               "network this machine can reach, or a firewall dropped the traffic in BOTH directions. It "
               "does NOT mean the search failed";
    }
    if (!elsewhere.empty()) {
        return elsewhere + "and the other " + std::to_string(packetsSeen) +
               " datagram(s) were not announcements (" + std::to_string(packetsRejected) + " refused)";
    }
    return "heard " + std::to_string(packetsSeen) + " datagram(s) and NOT ONE was an announcement (" +
           std::to_string(packetsRejected) + " refused) -- something is talking on this port that is not "
           "this mod, which is a different problem from silence";
}

}
