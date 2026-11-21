#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dsh::reflect {

struct GateActor {
    std::uintptr_t actor = 0;
    std::string    actorClass;
    std::string    actorName;
    bool           locationRead = false;
    float          x = 0.0f;
    float          y = 0.0f;
    float          z = 0.0f;
    std::string    locationNote;
};

struct GateSurvey {
    bool          worldFound = false;
    std::uint32_t levelsWalked = 0;
    std::uint64_t actorsWalked = 0;
    std::string   note;
    std::vector<GateActor> actors;

    std::vector<std::pair<std::string, std::uint32_t>> classHistogram;
    [[nodiscard]] std::vector<std::string> report_lines() const;
};

[[nodiscard]] GateSurvey survey_gates();

}
