#include "dsh/net/transport.h"

#include "dsh/platform/log.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <thread>
#include <utility>
#include <cstring>
#include <mutex>

namespace dsh::net {
namespace {

std::atomic<int> g_wsaRefs{0};

bool wsa_acquire() {
    if (g_wsaRefs.fetch_add(1) == 0) {
        WSADATA wsa{};
        const int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (rc != 0) {

            DSH_FATAL("WSAStartup failed rc=%d -- all networking disabled", rc);
            g_wsaRefs.fetch_sub(1);
            return false;
        }
        DSH_INFO("Winsock up: version %u.%u", LOBYTE(wsa.wVersion), HIBYTE(wsa.wVersion));
    }
    return true;
}

void wsa_release() {
    if (g_wsaRefs.fetch_sub(1) == 1) {
        WSACleanup();
    }
}

Endpoint endpoint_from(const sockaddr_in& a) {
    Endpoint e;
    e.bytes.resize(sizeof(a));
    std::memcpy(e.bytes.data(), &a, sizeof(a));
    return e;
}

bool endpoint_to(const Endpoint& e, sockaddr_in& out, bool& isBroadcast) {
    isBroadcast = false;
    if (e.bytes.size() != sizeof(sockaddr_in)) return false;
    std::memcpy(&out, e.bytes.data(), sizeof(out));
    if (out.sin_family != AF_INET) return false;

    const std::uint32_t host = ntohl(out.sin_addr.s_addr);
    isBroadcast = (host == 0xFFFFFFFFu) || ((host & 0xFFu) == 0xFFu);
    return true;
}

class UdpTransport final : public ITransport {
public:
    ~UdpTransport() override { close(); }

    bool listen(std::string_view bindAddress, std::uint16_t port) override {
        if (!wsa_acquire()) return false;

        const std::string addr(bindAddress);
        const char* node = addr.empty() ? nullptr : addr.c_str();
        return open_socket(node, port, nullptr);
    }

    void close() override {
        stop_background();
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
        if (wsaHeld_) {
            wsa_release();
            wsaHeld_ = false;
        }
    }

    bool send(const Endpoint& to, std::span<const std::uint8_t> payload) override {
        if (socket_ == INVALID_SOCKET) return false;
        if (payload.empty()) return false;

        if (pumping_.load()) {
            std::lock_guard<std::mutex> lock(outMutex_);
            if (outQueue_.size() >= 2048) {
                ++sendErrors_;
                return false;
            }
            outQueue_.emplace_back(to, std::vector<std::uint8_t>(payload.begin(), payload.end()));
            return true;
        }

        sockaddr_in dst{};
        bool broadcast = false;
        if (!endpoint_to(to, dst, broadcast)) {
            ++malformed_;
            DSH_WARN("send: bad endpoint (%zu bytes) -- dropped", to.bytes.size());
            return false;
        }

        const int rc = sendto(socket_, reinterpret_cast<const char*>(payload.data()),
                              static_cast<int>(payload.size()), 0,
                              reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
        if (rc == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            ++sendErrors_;

            if (sendErrors_ <= 5 || (sendErrors_ % 500) == 0) {
                DSH_WARN("sendto failed err=%d (count=%llu, %zu bytes) -- packet lost",
                         err, static_cast<unsigned long long>(sendErrors_), payload.size());
            }
            return false;
        }
        ++sent_;
        bytesSent_ += payload.size();
        return true;
    }

    void poll(std::vector<Datagram>& out) override {
        if (socket_ == INVALID_SOCKET) return;

        if (pumping_.load()) {
            std::lock_guard<std::mutex> lock(inMutex_);
            for (auto& d : inQueue_) out.push_back(std::move(d));
            inQueue_.clear();
            return;
        }

        for (int i = 0; i < kMaxDatagramsPerPoll; ++i) {
            sockaddr_in from{};
            int fromLen = sizeof(from);
            const int n = recvfrom(socket_, reinterpret_cast<char*>(recvBuf_),
                                   static_cast<int>(sizeof(recvBuf_)), 0,
                                   reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n == SOCKET_ERROR) {
                const int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK) {
                    ++recvErrors_;
                    if (recvErrors_ <= 5 || (recvErrors_ % 500) == 0) {
                        DSH_WARN("recvfrom failed err=%d (count=%llu)",
                                 err, static_cast<unsigned long long>(recvErrors_));
                    }
                }
                break;
            }
            if (n == 0) continue;

            Datagram d;
            d.from = endpoint_from(from);
            d.payload.assign(recvBuf_, recvBuf_ + n);
            out.push_back(std::move(d));
            ++received_;
            bytesReceived_ += static_cast<std::uint64_t>(n);
        }
    }

    [[nodiscard]] bool is_open() const override { return socket_ != INVALID_SOCKET; }

    [[nodiscard]] Endpoint local_endpoint() const override {
        Endpoint e;
        if (socket_ == INVALID_SOCKET) return e;
        sockaddr_in a{};
        int len = sizeof(a);
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&a), &len) == 0) {
            e = endpoint_from(a);
        }
        return e;
    }

    [[nodiscard]] std::string_view stats_line() const override {
        std::lock_guard<std::mutex> lock(statsMutex_);
        std::snprintf(statsBuf_, sizeof(statsBuf_),
                      "udp sent=%llu(%lluB) recv=%llu(%lluB) sendErr=%llu recvErr=%llu malformed=%llu",
                      static_cast<unsigned long long>(sent_),
                      static_cast<unsigned long long>(bytesSent_),
                      static_cast<unsigned long long>(received_),
                      static_cast<unsigned long long>(bytesReceived_),
                      static_cast<unsigned long long>(sendErrors_),
                      static_cast<unsigned long long>(recvErrors_),
                      static_cast<unsigned long long>(malformed_));
        return statsBuf_;
    }

private:
    static constexpr int kMaxDatagramsPerPoll = 256;

    bool open_socket(const char* node, std::uint16_t port, const Endpoint* connectTo) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        hints.ai_flags = AI_PASSIVE;

        char portStr[8] = {};
        std::snprintf(portStr, sizeof(portStr), "%u", static_cast<unsigned>(port));

        addrinfo* res = nullptr;
        const int rc = getaddrinfo(node, portStr, &hints, &res);
        if (rc != 0 || !res) {
            DSH_FATAL("getaddrinfo('%s', %s) failed rc=%d -- transport not listening",
                      node ? node : "0.0.0.0", portStr, rc);
            wsa_release();
            wsaHeld_ = false;
            return false;
        }

        socket_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (socket_ == INVALID_SOCKET) {
            DSH_FATAL("socket failed err=%d -- transport not listening", WSAGetLastError());
            freeaddrinfo(res);
            wsa_release();
            wsaHeld_ = false;
            return false;
        }
        wsaHeld_ = true;

        u_long nonBlocking = 1;
        ioctlsocket(socket_, FIONBIO, &nonBlocking);

        BOOL yes = TRUE;
        setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&yes), sizeof(yes));

        setsockopt(socket_, SOL_SOCKET, SO_BROADCAST,
                   reinterpret_cast<const char*>(&yes), sizeof(yes));

        int rcvBuf = 1 << 20;
        setsockopt(socket_, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&rcvBuf), sizeof(rcvBuf));

        if (bind(socket_, res->ai_addr, static_cast<int>(res->ai_addrlen)) == SOCKET_ERROR) {
            DSH_FATAL("bind(%s:%s) failed err=%d -- transport not listening",
                      node ? node : "0.0.0.0", portStr, WSAGetLastError());
            freeaddrinfo(res);
            close();
            return false;
        }
        freeaddrinfo(res);

        if (connectTo) {
            sockaddr_in dst{};
            bool bc = false;
            if (endpoint_to(*connectTo, dst, bc) &&
                connect(socket_, reinterpret_cast<const sockaddr*>(&dst), sizeof(dst)) == SOCKET_ERROR) {
                DSH_WARN("connect to peer failed err=%d -- will use sendto instead",
                         WSAGetLastError());
            }
        }

        DSH_INFO("udp transport listening on port %u (non-blocking, rcvbuf=%d KiB)",
                 static_cast<unsigned>(port), rcvBuf / 1024);
        return true;
    }

    bool start_background() override {
        if (socket_ == INVALID_SOCKET) return false;
        if (pumping_.load()) return true;
        stopRequested_.store(false);
        pumping_.store(true);
        worker_ = std::thread([this]() { pump_loop(); });
        DSH_INFO("udp transport: the socket now has its own thread -- the session loop can block for "
                 "seconds without the socket going unread");
        return true;
    }

    void stop_background() override {
        if (!pumping_.load()) return;
        stopRequested_.store(true);
        if (worker_.joinable()) worker_.join();
        pumping_.store(false);
        std::lock_guard<std::mutex> a(inMutex_);
        std::lock_guard<std::mutex> b(outMutex_);
        DSH_INFO("udp transport: the socket thread stopped with %zu receive and %zu send item(s) still "
                 "queued", inQueue_.size(), outQueue_.size());
    }

    void pump_loop() {
        while (!stopRequested_.load()) {

            for (int i = 0; i < 64; ++i) {
                std::pair<Endpoint, std::vector<std::uint8_t>> item;
                {
                    std::lock_guard<std::mutex> lock(outMutex_);
                    if (outQueue_.empty()) break;
                    item = std::move(outQueue_.front());
                    outQueue_.pop_front();
                }
                sockaddr_in dst{};
                bool broadcast = false;
                if (!endpoint_to(item.first, dst, broadcast)) {
                    ++malformed_;
                    continue;
                }
                const int rc = sendto(socket_, reinterpret_cast<const char*>(item.second.data()),
                                      static_cast<int>(item.second.size()), 0,
                                      reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
                if (rc == SOCKET_ERROR) {
                    ++sendErrors_;
                    if (sendErrors_ <= 5 || (sendErrors_ % 500) == 0) {
                        DSH_WARN("sendto (socket thread) failed err=%d (count=%llu)",
                                 WSAGetLastError(),
                                 static_cast<unsigned long long>(sendErrors_));
                    }
                } else {
                    ++sent_;
                    bytesSent_ += item.second.size();
                }
            }

            bool got = false;
            for (int i = 0; i < kMaxDatagramsPerPoll; ++i) {
                sockaddr_in from{};
                int fromLen = sizeof(from);
                const int n = recvfrom(socket_, reinterpret_cast<char*>(recvBuf_),
                                       static_cast<int>(sizeof(recvBuf_)), 0,
                                       reinterpret_cast<sockaddr*>(&from), &fromLen);
                if (n == SOCKET_ERROR) {
                    const int err = WSAGetLastError();
                    if (err != WSAEWOULDBLOCK) {
                        ++recvErrors_;
                        if (recvErrors_ <= 5 || (recvErrors_ % 500) == 0) {
                            DSH_WARN("recvfrom (socket thread) failed err=%d (count=%llu)", err,
                                     static_cast<unsigned long long>(recvErrors_));
                        }
                    }
                    break;
                }
                if (n == 0) continue;
                Datagram d;
                d.from = endpoint_from(from);
                d.payload.assign(recvBuf_, recvBuf_ + n);
                ++received_;
                bytesReceived_ += static_cast<std::uint64_t>(n);
                got = true;
                std::lock_guard<std::mutex> lock(inMutex_);
                if (inQueue_.size() >= kMaxQueuedIn) {
                    ++malformed_;
                    continue;
                }
                inQueue_.push_back(std::move(d));
            }

            if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    SOCKET socket_ = INVALID_SOCKET;
    bool   wsaHeld_ = false;

    std::thread              worker_;
    std::atomic<bool>        pumping_{false};
    std::atomic<bool>        stopRequested_{false};
    std::mutex                inMutex_;
    std::mutex                outMutex_;
    std::deque<Datagram>      inQueue_;
    std::deque<std::pair<Endpoint, std::vector<std::uint8_t>>> outQueue_;
    static constexpr std::size_t kMaxQueuedIn = 4096;

    std::uint8_t recvBuf_[65536] = {};

    std::uint64_t sent_ = 0;
    std::uint64_t received_ = 0;
    std::uint64_t bytesSent_ = 0;
    std::uint64_t bytesReceived_ = 0;
    std::uint64_t sendErrors_ = 0;
    std::uint64_t recvErrors_ = 0;
    std::uint64_t malformed_ = 0;

    mutable std::mutex statsMutex_;
    mutable char       statsBuf_[256] = {};
};

}

std::unique_ptr<ITransport> make_udp_transport() {
    return std::make_unique<UdpTransport>();
}

}
