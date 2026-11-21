#include "dsh/game/pose_wire.h"

#include "dsh/net/bytes.h"

#include <cstring>
#include <string>

namespace dsh::game {
namespace {

void put_u16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
}

void put_u64(std::vector<std::uint8_t>& v, std::uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF));
}

std::uint16_t get_u16(std::span<const std::uint8_t> b, std::size_t off) {
    return static_cast<std::uint16_t>(b[off]) | (static_cast<std::uint16_t>(b[off + 1]) << 8);
}

std::uint64_t get_u64(std::span<const std::uint8_t> b, std::size_t off) {
    std::uint64_t x = 0;
    for (int i = 0; i < 8; ++i) x |= static_cast<std::uint64_t>(b[off + i]) << (8 * i);
    return x;
}

}

std::vector<std::uint8_t> encode_pose(std::uint64_t entityCounter,
                                     const dsh::sync::PoseSample& pose) {
    std::vector<std::uint8_t> out;
    out.reserve(kPosePayloadBytes);

    put_u64(out, entityCounter);
    put_u16(out, dsh::net::quantise_position_axis(pose.x));
    put_u16(out, dsh::net::quantise_position_axis(pose.y));
    put_u16(out, dsh::net::quantise_position_axis(pose.z));
    out.push_back(dsh::net::quantise_angle(pose.yaw));
    out.push_back(dsh::net::quantise_angle(pose.pitch));
    put_u16(out, static_cast<std::uint16_t>(pose.flags & 0xFFFFu));
    return out;
}

bool decode_pose(std::span<const std::uint8_t> bytes, std::uint64_t& entityCounterOut,
                 dsh::sync::PoseSample& poseOut, std::string& note) {
    if (bytes.size() != kPosePayloadBytes) {
        note = "pose payload is " + std::to_string(bytes.size()) + " bytes, expected " +
               std::to_string(kPosePayloadBytes);
        return false;
    }

    entityCounterOut = get_u64(bytes, 0);
    poseOut.x = dsh::net::dequantise_position_axis(get_u16(bytes, 8));
    poseOut.y = dsh::net::dequantise_position_axis(get_u16(bytes, 10));
    poseOut.z = dsh::net::dequantise_position_axis(get_u16(bytes, 12));
    poseOut.yaw = dsh::net::dequantise_angle(bytes[14]);
    poseOut.pitch = dsh::net::dequantise_angle(bytes[15]);
    poseOut.flags = get_u16(bytes, 16);

    poseOut.samplerTimeMs = 0;
    note = "ok";
    return true;
}

}
