#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace dsh::reflect {

inline constexpr float kTargetClosenessRangeCm = 8000.0f;
inline constexpr float kTargetIncumbentBonus = 0.05f;
inline constexpr float kSwitchMargin = 0.10f;
inline constexpr float kDecisionFloor = 0.10f;

struct TargetScoreInputs {
    float distanceCm = 0.0f;
    bool  incumbent = false;
};

inline float target_score(const TargetScoreInputs& in) {
    const float closeness = std::clamp(1.0f - in.distanceCm / kTargetClosenessRangeCm, 0.0f, 1.0f);
    return closeness + (in.incumbent ? kTargetIncumbentBonus : 0.0f);
}

inline bool target_should_switch(float challenger, float incumbent) {
    return challenger > incumbent + kSwitchMargin;
}

inline bool target_keep_incumbent(float challenger, float incumbent) {
    return incumbent >= kDecisionFloor && !target_should_switch(challenger, incumbent);
}

struct TargetCandidate {
    std::uintptr_t actor = 0;
    std::string    name;
    std::string    legClass;
    std::uint32_t  colourHash = 0;
    bool           colourKnown = false;
    bool           hasLocation = false;
    float          x = 0.0f, y = 0.0f, z = 0.0f;
    bool           isPuppet = false;
};

struct TargetEnemy {
    std::uintptr_t actor = 0;
    std::uintptr_t targeting = 0;
    std::string    name;
    bool           hasLocation = false;
    float          x = 0.0f, y = 0.0f, z = 0.0f;
    std::string    currentTargetName;
    std::string    chosenName;
    float          chosenScore = 0.0f;
};

struct TargetDecision {
    std::string enemyName;
    std::string fromName;
    std::string toName;

    float       fromScore = 0.0f;
    bool        fromScoreKnown = false;
    float       toScore = 0.0f;
    bool        switched = false;
};

struct TargetModelReport {
    bool        worldFound = false;
    std::string note;
    std::uint32_t enemies = 0;
    std::uint32_t candidates = 0;

    std::uint32_t forced = 0;
    std::uint32_t verified = 0;
    std::uint32_t refused = 0;

    std::uint32_t npcSkipped = 0;

    std::uint32_t belowFloor = 0;
    std::uint32_t ambiguous = 0;

    std::uint32_t engaged = 0;
    std::uint32_t notEngaged = 0;

    std::uint64_t usAggro = 0;
    std::uint64_t usCandidates = 0;
    std::uint64_t usEnemies = 0;
    std::uint64_t usDecide = 0;

    std::vector<TargetDecision> decisions;
    std::vector<std::string> lines;
    [[nodiscard]] std::vector<std::string> report_lines() const;
};

[[nodiscard]] TargetModelReport ai_enforce_targets(std::uint64_t nowMs, bool asHost);

void forget_target_model();

}
