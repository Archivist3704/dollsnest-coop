#include "dsh/sync/playback_clock.h"

#include <cmath>

namespace dsh::sync {

void PlaybackClock::note_arrival(std::uint64_t stampMs, std::uint64_t arrivalMs) {
    ++stats_.arrivals;
    if (haveArrival_) {
        const std::uint64_t stampGap = stampMs >= lastStampMs_ ? stampMs - lastStampMs_ : 0;
        const std::uint64_t arrivalGap = arrivalMs >= lastArrivalMs_ ? arrivalMs - lastArrivalMs_ : 0;

        if (stampGap != 0 && arrivalGap != 0) {
            double estimate = static_cast<double>(stampGap) / static_cast<double>(arrivalGap);
            if (estimate < config_.minRate) estimate = config_.minRate;
            if (estimate > config_.maxRate) estimate = config_.maxRate;

            if (stats_.rateEstimates == 0) {
                rate_ = estimate;
            } else {
                rate_ = (1.0 - config_.rateSmoothing) * rate_ + config_.rateSmoothing * estimate;
            }
            ++stats_.rateEstimates;
        }
    }
    lastStampMs_ = stampMs;
    lastArrivalMs_ = arrivalMs;
    haveArrival_ = true;
}

PlaybackClock::Frame PlaybackClock::frame(std::uint64_t nowMs, std::uint64_t delayMs) {
    Frame out;

    if (!haveArrival_ || stats_.rateEstimates == 0) {
        out.renderMs = 0;
        out.samplerNowMs = 0;
        return out;
    }

    const double elapsedMs =
        nowMs > lastArrivalMs_ ? static_cast<double>(nowMs - lastArrivalMs_) : 0.0;
    const double samplerNow = static_cast<double>(lastStampMs_) + elapsedMs * rate_;
    out.samplerNowMs = samplerNow > 0.0 ? static_cast<std::uint64_t>(samplerNow) : 0;

    const double wanted = samplerNow - static_cast<double>(delayMs);
    const std::uint64_t targetMs = wanted > 0.0 ? static_cast<std::uint64_t>(wanted) : 0;

    if (!running_) {
        renderMs_ = targetMs;
        lastNowMs_ = nowMs;
        running_ = true;
        ++stats_.advances;
        out.renderMs = renderMs_;
        return out;
    }

    if (nowMs > lastNowMs_) {
        const double advance = static_cast<double>(nowMs - lastNowMs_) * rate_;
        renderMs_ += advance > 0.0 ? static_cast<std::uint64_t>(advance) : 0;
    }
    lastNowMs_ = nowMs;

    const std::int64_t current = static_cast<std::int64_t>(renderMs_);
    const std::int64_t wantedI = static_cast<std::int64_t>(targetMs);
    std::int64_t error = wantedI - current;
    if (error < 0) {

        ++stats_.backwardRequests;
        stats_.backwardTotalMs += -error;
        if (-error > stats_.worstBackwardMs) stats_.worstBackwardMs = -error;
    }
    std::int64_t step = error;
    if (error > config_.resyncMs || error < -config_.resyncMs) {

        ++stats_.resyncs;
    } else {
        if (step > config_.maxCorrectionPerAdvanceMs) step = config_.maxCorrectionPerAdvanceMs;
        if (step < -config_.maxCorrectionPerAdvanceMs) step = -config_.maxCorrectionPerAdvanceMs;
    }
    if (step < 0) {
        ++stats_.backSteps;
        if (-step > stats_.worstStepBackMs) stats_.worstStepBackMs = -step;
    }
    const std::int64_t next = current + step;
    renderMs_ = next > 0 ? static_cast<std::uint64_t>(next) : 0;
    stats_.correctedTotalMs += step;
    ++stats_.advances;

    out.renderMs = renderMs_;
    return out;
}

void PlaybackClock::reset() {
    lastStampMs_ = 0;
    lastArrivalMs_ = 0;
    haveArrival_ = false;
    rate_ = 1.0;
    renderMs_ = 0;
    lastNowMs_ = 0;
    running_ = false;
    stats_ = Stats{};
}

}
