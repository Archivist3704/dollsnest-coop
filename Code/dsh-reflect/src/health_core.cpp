#include "dsh/reflect/health_core.h"

#include <cmath>

namespace dsh::reflect {

void HealthDeltaTracker::adopt(float authoritative) {

    if (!std::isfinite(authoritative)) return;
    baseline_ = authoritative;
    hasBaseline_ = true;
}

bool HealthDeltaTracker::observe(float observed, float* damageOut) const {
    if (damageOut != nullptr) *damageOut = 0.0f;
    if (!hasBaseline_ || !std::isfinite(observed)) return false;

    if (!(observed < baseline_)) return false;
    if (damageOut != nullptr) *damageOut = baseline_ - observed;
    return true;
}

void HealthDeltaTracker::reset() {
    hasBaseline_ = false;
    baseline_ = 0.0f;
}

bool SequenceGate::accept(std::uint32_t sequence) {
    if (!hasApplied_) {
        hasApplied_ = true;
        lastApplied_ = sequence;
        return true;
    }

    if (sequence == lastApplied_) return false;
    if (static_cast<std::int32_t>(sequence - lastApplied_) <= 0) return false;
    if (static_cast<std::uint32_t>(sequence - lastApplied_) > kMaxSkew) return false;
    lastApplied_ = sequence;
    return true;
}

void SequenceGate::reset() {
    hasApplied_ = false;
    lastApplied_ = 0;
}

bool report_is_plausible(float reported) {
    if (!std::isfinite(reported)) return false;
    if (reported <= 0.0f) return false;
    return true;
}

float clamp_reported_damage(float reported, float health, float shield) {
    if (!std::isfinite(reported) || reported <= 0.0f) return 0.0f;
    float ceiling = 0.0f;
    if (std::isfinite(health) && health > 0.0f) ceiling += health;
    if (std::isfinite(shield) && shield > 0.0f) ceiling += shield;
    if (reported > ceiling) return ceiling;
    return reported;
}

}
