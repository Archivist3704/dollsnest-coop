#include "dsh/game/net_runtime.h"

#include "dsh/game/pawn.h"
#include "dsh/game/pose_wire.h"
#include "dsh/sync/interpolator.h"

#include "dsh/net/session.h"
#include "dsh/net/transport.h"
#include "dsh/platform/log.h"

#include <winsock2.h>

#include <windows.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace dsh::game {
namespace {

dsh::net::Endpoint make_endpoint(const sockaddr_in& a) {
    dsh::net::Endpoint ep;
    ep.bytes.resize(sizeof(a));
    std::memcpy(ep.bytes.data(), &a, sizeof(a));
    return ep;
}

}

LoopbackResult run_loopback_pose_test(int seconds) {
    LoopbackResult r;
    if (seconds < 1) seconds = 1;
    if (seconds > 30) seconds = 30;

    const auto handle = locate_local_pawn();
    if (!handle.valid) {
        r.detail = "no local pawn: " + handle.note;
        DSH_ERROR("loopback: %s", r.detail.c_str());
        return r;
    }
    DSH_INFO("loopback: tracking pawn 0x%llX (%s)", static_cast<unsigned long long>(handle.pawn),
             handle.pawnClass.c_str());

    auto hostTransport = dsh::net::make_udp_transport();
    auto clientTransport = dsh::net::make_udp_transport();
    if (!hostTransport || !clientTransport || !hostTransport->listen("", 0) ||
        !clientTransport->listen("", 0)) {
        r.detail = "could not open the loopback sockets";
        DSH_ERROR("loopback: %s", r.detail.c_str());
        return r;
    }

    sockaddr_in hostAddr{};
    {
        const auto local = hostTransport->local_endpoint();
        if (local.bytes.size() != sizeof(sockaddr_in)) {
            r.detail = "host endpoint is not a sockaddr_in";
            DSH_ERROR("loopback: %s", r.detail.c_str());
            hostTransport->close();
            clientTransport->close();
            return r;
        }
        std::memcpy(&hostAddr, local.bytes.data(), sizeof(hostAddr));
        hostAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    dsh::net::Endpoint hostEp = make_endpoint(hostAddr);

    dsh::net::SessionConfig hc;
    hc.playerName = "loopback-host";
    dsh::net::Session host(hc);
    host.become_host(dsh::PeerId{0});
    host.set_send_function([&](const dsh::net::Endpoint& to, std::span<const std::uint8_t> b) {
        hostTransport->send(to, b);
    });

    dsh::net::SessionConfig cc;
    cc.playerName = "loopback-client";
    dsh::net::Session client(cc);
    client.become_client(dsh::PeerId{1}, hostEp);
    client.set_send_function([&](const dsh::net::Endpoint& to, std::span<const std::uint8_t> b) {
        clientTransport->send(to, b);
    });

    std::vector<dsh::net::Datagram> inbox;
    const std::uint64_t t0 = GetTickCount64();

    while (GetTickCount64() - t0 < 4000 && client.connected_count() == 0) {
        Sleep(5);
        const std::uint64_t now = GetTickCount64();
        inbox.clear();
        hostTransport->poll(inbox);
        for (const auto& d : inbox) host.on_datagram(now, d.from, d.payload);
        inbox.clear();
        clientTransport->poll(inbox);
        for (const auto& d : inbox) client.on_datagram(now, d.from, d.payload);
        host.update(now);
        client.update(now);
    }
    r.handshakeMs = GetTickCount64() - t0;
    if (client.connected_count() == 0) {
        r.detail = "handshake over loopback failed within 4 s";
        DSH_ERROR("loopback: %s", r.detail.c_str());
        hostTransport->close();
        clientTransport->close();
        return r;
    }
    DSH_INFO("loopback: handshake completed in %llu ms", static_cast<unsigned long long>(r.handshakeMs));

    const auto peerId = host.peers().empty() ? dsh::kInvalidPeer : host.peers()[0].id;
    dsh::sync::PoseInterpolator interp;
    const std::uint64_t runStart = GetTickCount64();
    std::uint64_t nextSample = runStart;
    std::uint32_t seq = 1;

    while (GetTickCount64() - runStart < static_cast<std::uint64_t>(seconds) * 1000) {
        const std::uint64_t now = GetTickCount64();
        if (now >= nextSample) {
            nextSample += 50;

            dsh::sync::PoseSample pose;
            std::string note;
            if (read_pawn_pose(handle, pose, note)) {
                const auto payload = encode_pose(1, pose);
                host.send_state_to(peerId, seq++, payload);
                ++r.samplesSent;
            }
        }

        inbox.clear();
        hostTransport->poll(inbox);
        for (const auto& d : inbox) host.on_datagram(now, d.from, d.payload);
        inbox.clear();
        clientTransport->poll(inbox);
        for (const auto& d : inbox) {
            client.on_datagram(now, d.from, d.payload);

            dsh::net::ByteReader reader(d.payload);
            dsh::net::PacketHeader hdr;
            if (!dsh::net::read_header(reader, hdr)) continue;
            if (hdr.type != dsh::net::MessageType::playerState) continue;

            std::uint64_t counter = 0;
            dsh::sync::PoseSample got;
            std::string dnote;
            const std::span<const std::uint8_t> whole(d.payload);
            if (!decode_pose(whole.subspan(reader.cursor()), counter, got, dnote)) continue;
            ++r.samplesReceived;

            dsh::sync::PoseSample current;
            std::string cnote;
            if (read_pawn_pose(handle, current, cnote)) {
                const float dx = got.x - current.x, dy = got.y - current.y, dz = got.z - current.z;
                const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (dist > r.worstPositionErrorM) r.worstPositionErrorM = dist;
                float dyaw = std::abs(got.yaw - current.yaw);
                if (dyaw > 3.14159265f) dyaw = 6.2831853f - dyaw;
                const float deg = dyaw * 180.0f / 3.14159265f;
                if (deg > r.worstYawErrorDeg) r.worstYawErrorDeg = deg;
            }
            interp.push(got);
        }

        host.update(now);
        client.update(now);
        Sleep(2);
    }

    r.ok = (r.samplesSent > 0 && r.samplesReceived > 0);
    r.detail = "sent " + std::to_string(r.samplesSent) + ", received " +
               std::to_string(r.samplesReceived);

    DSH_INFO("loopback: handshake=%llu ms sent=%d received=%d worstPos=%.4f m worstYaw=%.4f deg "
             "(%d%% delivered)",
             static_cast<unsigned long long>(r.handshakeMs), r.samplesSent, r.samplesReceived,
             static_cast<double>(r.worstPositionErrorM), static_cast<double>(r.worstYawErrorDeg),
             r.samplesSent ? (100 * r.samplesReceived / r.samplesSent) : 0);
    DSH_INFO("loopback: session stats host[%s]", host.status_line(GetTickCount64()).c_str());
    DSH_INFO("loopback: result=%s", r.ok ? "PASS -- pose travelled over real UDP and came back"
                                         : "FAIL");

    hostTransport->close();
    clientTransport->close();
    return r;
}

}
