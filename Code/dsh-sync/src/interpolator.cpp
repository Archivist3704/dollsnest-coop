#include "dsh/sync/interpolator.h"

#include <algorithm>
#include <cmath>

namespace dsh::sync {
namespace {

constexpr float kPi = 3.14159265358979f;

float lerp_angle(float a, float b, float t) {
    float delta = b - a;
    while (delta > kPi) delta -= 2.0f * kPi;
    while (delta < -kPi) delta += 2.0f * kPi;
    return a + delta * t;
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

void lerp_basis(PoseOutput& out, const PoseSample& a, const PoseSample& b, float t) {
    out.fx = lerp(a.fx, b.fx, t);
    out.fy = lerp(a.fy, b.fy, t);
    out.fz = lerp(a.fz, b.fz, t);
    out.rx = lerp(a.rx, b.rx, t);
    out.ry = lerp(a.ry, b.ry, t);
    out.rz = lerp(a.rz, b.rz, t);

    const auto norm = [](float& x, float& y, float& z) {
        const float len = std::sqrt(x * x + y * y + z * z);
        if (len > 1e-6f) {
            x /= len;
            y /= len;
            z /= len;
        }
    };
    norm(out.fx, out.fy, out.fz);
    norm(out.rx, out.ry, out.rz);
}

}

void PoseInterpolator::push(const PoseSample& s) {
    ++stats_.pushed;

    for (auto& existing : samples_) {
        if (existing.samplerTimeMs == s.samplerTimeMs) {
            existing = s;
            ++stats_.replacedDuplicate;
            return;
        }
    }

    auto it = samples_.begin();
    while (it != samples_.end() && it->samplerTimeMs < s.samplerTimeMs) ++it;
    samples_.insert(it, s);

    while (samples_.size() > config_.maxSamples) {
        samples_.pop_front();
        ++stats_.discardedOverflow;
    }
}

PoseOutput PoseInterpolator::sample(std::uint64_t renderTimeMs, std::uint64_t nowMs) {
    PoseOutput out;

    if (samples_.empty()) {
        out.mode = PoseOutput::Mode::noData;
        ++stats_.noDataFrames;
        return out;
    }

    const std::uint64_t newestStamp = samples_.back().samplerTimeMs;

    const PoseSample newestPose = samples_.back();

    const std::uint64_t cutoff =
        (nowMs > config_.maxSampleAgeMs) ? (nowMs - config_.maxSampleAgeMs) : 0;
    while (!samples_.empty() && samples_.front().samplerTimeMs < cutoff) {
        samples_.pop_front();
        ++stats_.discardedTooOld;
    }

    out.newestSampleAgeMs = (nowMs >= newestStamp)
                                ? static_cast<float>(nowMs - newestStamp)
                                : 0.0f;
    if (out.newestSampleAgeMs > config_.staleAfterMs) ++stats_.staleFrames;

    if (samples_.empty()) {

        out.x = newestPose.x; out.y = newestPose.y; out.z = newestPose.z;
        out.yaw = newestPose.yaw; out.pitch = newestPose.pitch;
        out.fx = newestPose.fx; out.fy = newestPose.fy; out.fz = newestPose.fz;
        out.rx = newestPose.rx; out.ry = newestPose.ry; out.rz = newestPose.rz;
        out.flags = newestPose.flags;
        out.mode = PoseOutput::Mode::frozen;
        ++stats_.frozenFrames;
        return out;
    }

    const PoseSample& newest = samples_.back();

    if (renderTimeMs <= samples_.front().samplerTimeMs) {
        const PoseSample& s = samples_.front();
        out.x = s.x; out.y = s.y; out.z = s.z;
        out.yaw = s.yaw; out.pitch = s.pitch;
        out.fx = s.fx; out.fy = s.fy; out.fz = s.fz;
        out.rx = s.rx; out.ry = s.ry; out.rz = s.rz;
        out.flags = s.flags;
        out.mode = PoseOutput::Mode::interpolated;
        ++stats_.interpolatedFrames;
        return out;
    }

    if (renderTimeMs >= newest.samplerTimeMs) {
        const float overshootMs = static_cast<float>(renderTimeMs - newest.samplerTimeMs);

        out.x = newest.x; out.y = newest.y; out.z = newest.z;
        out.yaw = newest.yaw; out.pitch = newest.pitch;
        out.fx = newest.fx; out.fy = newest.fy; out.fz = newest.fz;
        out.rx = newest.rx; out.ry = newest.ry; out.rz = newest.rz;
        out.flags = newest.flags;

        if (overshootMs <= config_.maxExtrapolationMs && samples_.size() >= 2) {

            const PoseSample& prev = samples_[samples_.size() - 2];
            const float dtMs = static_cast<float>(newest.samplerTimeMs - prev.samplerTimeMs);
            if (dtMs > 1.0f) {
                const float k = overshootMs / dtMs;
                out.x = newest.x + (newest.x - prev.x) * k;
                out.y = newest.y + (newest.y - prev.y) * k;
                out.z = newest.z + (newest.z - prev.z) * k;
                out.yaw = newest.yaw + (newest.yaw - prev.yaw) * k;
                out.pitch = newest.pitch + (newest.pitch - prev.pitch) * k;
                out.mode = PoseOutput::Mode::extrapolated;
                ++stats_.extrapolatedFrames;
                return out;
            }
        }

        out.mode = PoseOutput::Mode::frozen;
        ++stats_.frozenFrames;
        return out;
    }

    for (std::size_t i = 0; i + 1 < samples_.size(); ++i) {
        const PoseSample& a = samples_[i];
        const PoseSample& b = samples_[i + 1];
        if (renderTimeMs < a.samplerTimeMs || renderTimeMs > b.samplerTimeMs) continue;

        const float span = static_cast<float>(b.samplerTimeMs - a.samplerTimeMs);
        const float t = (span > 1e-3f)
                            ? static_cast<float>(renderTimeMs - a.samplerTimeMs) / span
                            : 0.0f;

        out.x = lerp(a.x, b.x, t);
        out.y = lerp(a.y, b.y, t);
        out.z = lerp(a.z, b.z, t);
        out.yaw = lerp_angle(a.yaw, b.yaw, t);
        out.pitch = lerp_angle(a.pitch, b.pitch, t);
        lerp_basis(out, a, b, t);

        out.flags = (t < 0.5f) ? a.flags : b.flags;
        out.mode = PoseOutput::Mode::interpolated;
        ++stats_.interpolatedFrames;
        return out;
    }

    out.x = newest.x; out.y = newest.y; out.z = newest.z;
    out.yaw = newest.yaw; out.pitch = newest.pitch;
    out.fx = newest.fx; out.fy = newest.fy; out.fz = newest.fz;
    out.rx = newest.rx; out.ry = newest.ry; out.rz = newest.rz;
    out.flags = newest.flags;
    out.mode = PoseOutput::Mode::frozen;
    ++stats_.frozenFrames;
    return out;
}

void PoseInterpolator::clear() {
    samples_.clear();
    stats_ = Stats{};
}

}
