#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace dsh::net {

using PeerIndex = std::uint8_t;
inline constexpr PeerIndex kUnknownPeer = 0xFF;

struct Endpoint {
    std::vector<std::uint8_t> bytes;

    [[nodiscard]] bool valid() const noexcept { return !bytes.empty(); }
    [[nodiscard]] bool operator==(const Endpoint& o) const noexcept { return bytes == o.bytes; }
};

struct Datagram {
    Endpoint                  from;
    std::vector<std::uint8_t> payload;
};

class ITransport {
public:
    virtual ~ITransport() = default;

    virtual bool        listen(std::string_view bindAddress, std::uint16_t port) = 0;
    virtual void        close() = 0;

    virtual bool        send(const Endpoint& to, std::span<const std::uint8_t> payload) = 0;

    virtual void        poll(std::vector<Datagram>& out) = 0;

    [[nodiscard]] virtual bool     is_open() const = 0;
    [[nodiscard]] virtual Endpoint local_endpoint() const = 0;

    [[nodiscard]] virtual std::string_view stats_line() const = 0;

    virtual bool        start_background() { return false; }
    virtual void        stop_background() {}

};

[[nodiscard]] std::unique_ptr<ITransport> make_udp_transport();

}
