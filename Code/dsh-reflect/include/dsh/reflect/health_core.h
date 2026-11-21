#ifndef DSH_REFLECT_HEALTH_CORE_H
#define DSH_REFLECT_HEALTH_CORE_H

#include <cstdint>

namespace dsh::reflect {

class HealthDeltaTracker {
public:

    void adopt(float authoritative);

    bool observe(float observed, float* damageOut) const;

    void reset();

    [[nodiscard]] bool  hasBaseline() const { return hasBaseline_; }
    [[nodiscard]] float baseline() const { return baseline_; }

private:
    bool  hasBaseline_ = false;
    float baseline_ = 0.0f;
};

class SequenceGate {
public:

    bool accept(std::uint32_t sequence);

    void reset();

    [[nodiscard]] bool     hasApplied() const { return hasApplied_; }
    [[nodiscard]] std::uint32_t lastApplied() const { return lastApplied_; }

    static constexpr std::uint32_t kMaxSkew = 4096;

private:
    bool          hasApplied_ = false;
    std::uint32_t lastApplied_ = 0;
};

[[nodiscard]] float clamp_reported_damage(float reported, float health, float shield);

[[nodiscard]] bool report_is_plausible(float reported);

}

#endif
