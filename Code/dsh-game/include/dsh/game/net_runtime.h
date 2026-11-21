#pragma once

#include <cstdint>
#include <memory>
#include <cstddef>
#include <string>

namespace dsh::game {

struct LoopbackResult {
    bool          ok = false;
    int           samplesSent = 0;
    int           samplesReceived = 0;
    float         worstPositionErrorM = 0.0f;
    float         worstYawErrorDeg = 0.0f;
    std::uint64_t handshakeMs = 0;
    std::string   detail;
};

[[nodiscard]] LoopbackResult run_loopback_pose_test(int seconds);

}
