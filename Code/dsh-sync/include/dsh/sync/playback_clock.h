#pragma once

#include <cstdint>

namespace dsh::sync {

class PlaybackClock {
public:
    struct Config {

        double minRate = 0.4;
        double maxRate = 1.0;

        double rateSmoothing = 0.1;

        std::int64_t maxCorrectionPerAdvanceMs = 2;

        std::int64_t resyncMs = 500;
    };

    struct Frame {
        std::uint64_t renderMs = 0;
        std::uint64_t samplerNowMs = 0;
    };

    struct Stats {
        std::uint64_t arrivals = 0;
        std::uint64_t rateEstimates = 0;
        std::uint64_t advances = 0;

        std::uint64_t backwardRequests = 0;
        std::int64_t  backwardTotalMs = 0;
        std::int64_t  worstBackwardMs = 0;

        std::uint64_t backSteps = 0;
        std::int64_t  worstStepBackMs = 0;
        std::uint64_t resyncs = 0;
        std::int64_t  correctedTotalMs = 0;
    };

    explicit PlaybackClock(Config config = {}) : config_(config) {}

    void note_arrival(std::uint64_t stampMs, std::uint64_t arrivalMs);

    [[nodiscard]] Frame frame(std::uint64_t nowMs, std::uint64_t delayMs);

    void reset();

    [[nodiscard]] bool   running() const noexcept { return running_; }
    [[nodiscard]] double rate() const noexcept { return rate_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    Config config_;

    std::uint64_t lastStampMs_ = 0;
    std::uint64_t lastArrivalMs_ = 0;
    bool          haveArrival_ = false;

    double        rate_ = 1.0;

    std::uint64_t renderMs_ = 0;
    std::uint64_t lastNowMs_ = 0;
    bool          running_ = false;
    Stats         stats_{};
};

}
