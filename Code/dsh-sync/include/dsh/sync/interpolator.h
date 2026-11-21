#pragma once

#include <cstdint>
#include <deque>
#include <vector>

namespace dsh::sync {

struct PoseSample {
    std::uint64_t samplerTimeMs = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f, pitch = 0.0f;
    std::uint32_t flags = 0;

    float fx = 1.0f, fy = 0.0f, fz = 0.0f;
    float rx = 0.0f, ry = 1.0f, rz = 0.0f;
};

struct PoseOutput {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f, pitch = 0.0f;
    float fx = 1.0f, fy = 0.0f, fz = 0.0f;
    float rx = 0.0f, ry = 1.0f, rz = 0.0f;
    std::uint32_t flags = 0;

    enum class Mode : std::uint8_t {
        noData,
        interpolated,
        extrapolated,
        frozen,
    } mode = Mode::noData;

    float newestSampleAgeMs = 0.0f;

    [[nodiscard]] bool renderable() const noexcept { return mode != Mode::noData; }
};

struct InterpolatorConfig {

    float interpDelayMs = 100.0f;

    float maxExtrapolationMs = 250.0f;

    float staleAfterMs = 1000.0f;

    std::uint64_t maxSampleAgeMs = 2000;

    std::size_t maxSamples = 32;
};

class PoseInterpolator {
public:
    explicit PoseInterpolator(InterpolatorConfig config = {}) : config_(config) {}

    void push(const PoseSample& s);

    [[nodiscard]] PoseOutput sample(std::uint64_t renderTimeMs, std::uint64_t nowMs);

    void clear();

    struct Stats {
        std::uint64_t pushed = 0;
        std::uint64_t discardedTooOld = 0;
        std::uint64_t discardedOverflow = 0;
        std::uint64_t replacedDuplicate = 0;
        std::uint64_t interpolatedFrames = 0;
        std::uint64_t extrapolatedFrames = 0;
        std::uint64_t frozenFrames = 0;
        std::uint64_t noDataFrames = 0;
        std::uint64_t staleFrames = 0;
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t buffered() const noexcept { return samples_.size(); }
    [[nodiscard]] bool empty() const noexcept { return samples_.empty(); }

private:
    InterpolatorConfig     config_;
    std::deque<PoseSample> samples_;
    Stats                  stats_{};
};

}
