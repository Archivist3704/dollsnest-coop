#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dsh/sync/interpolator.h"

namespace dsh::game {

inline constexpr std::size_t kPosePayloadBytes = 18;

[[nodiscard]] std::vector<std::uint8_t> encode_pose(std::uint64_t entityCounter,
                                                   const dsh::sync::PoseSample& pose);

[[nodiscard]] bool decode_pose(std::span<const std::uint8_t> bytes,
                               std::uint64_t& entityCounterOut, dsh::sync::PoseSample& poseOut,
                               std::string& note);

}
