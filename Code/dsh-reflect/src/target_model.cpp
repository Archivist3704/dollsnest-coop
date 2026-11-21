#include "dsh/reflect/target_model.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>

#include "dsh/platform/log.h"
#include "dsh/reflect/enemies.h"
#include "dsh/reflect/reflection.h"
#include "dsh/reflect/offsets.h"
#include "dsh/reflect/spawn.h"
#include "dsh/reflect/world.h"

namespace dsh::reflect {

namespace {

std::uint64_t elapsed_us(std::chrono::steady_clock::time_point from,
                         std::chrono::steady_clock::time_point to) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(to - from).count());
}

struct HeldDecision {
    std::string enemyName;
    std::string chosenName;
    float       score = 0.0f;
};

std::vector<HeldDecision>& held_decisions() {
    static std::vector<HeldDecision> s_held;
    return s_held;
}

HeldDecision* held_for(const std::string& enemyName) {
    for (HeldDecision& held : held_decisions()) {
        if (held.enemyName == enemyName) return &held;
    }
    return nullptr;
}

bool read_u64_at(std::uintptr_t address, std::uint64_t& out) {
    return read_process_bytes(address, &out, sizeof(out));
}

constexpr std::uint32_t kAiStateOffset = 0x0360;
constexpr std::uint32_t kScoringStride = 0x18;

int read_ai_state(std::uintptr_t controller) {
    std::uint8_t value = 0xFF;
    if (!read_process_bytes(controller + kAiStateOffset, &value, 1)) return -1;
    return value;
}

const char* ai_state_name(int state) {
    switch (state) {
        case 0: return "Idle(0)";
        case 1: return "Battle(1)";
        case 2: return "Return(2)";
        default: return "unreadable";
    }
}

bool read_simple_array(std::uintptr_t address, std::uint32_t cap, std::uintptr_t& data, std::uint32_t& num) {
    std::uint64_t dataPtr = 0;
    std::uint32_t count = 0;
    if (!read_process_bytes(address, &dataPtr, sizeof(dataPtr))) return false;
    if (!read_process_bytes(address + 0x08, &count, sizeof(count))) return false;
    if (dataPtr < 0x10000 || count > cap) return false;
    data = static_cast<std::uintptr_t>(dataPtr);
    num = count;
    return true;
}

bool enemy_holds_candidate(std::uintptr_t targeting, std::uintptr_t actor, std::uint32_t& listSize) {
    listSize = 0;
    std::uintptr_t data = 0;
    std::uint32_t num = 0;
    if (!read_simple_array(targeting + 0x00B0, 4096, data, num)) return false;
    listSize = num;
    for (std::uint32_t i = 0; i < num && i < 64; ++i) {
        std::uint64_t entry = 0;
        if (!read_u64_at(data + static_cast<std::uintptr_t>(i) * kScoringStride, entry)) continue;
        if (entry == actor) return true;
    }
    return false;
}

std::uint32_t fnv1a32(const unsigned char* data, std::size_t bytes) {
    std::uint32_t h = 2166136261u;
    for (std::size_t i = 0; i < bytes; ++i) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

bool leg_colour_hash(std::uintptr_t machine, std::string& legClass, std::uint32_t& hash, bool& ok) {
    MachineLoadout loadout;
    std::string note;
    if (!capture_machine_loadout(machine, loadout, note)) return false;
    legClass = loadout.className;
    if (!loadout.moduleColoringPresent) return false;
    const auto* bytes = reinterpret_cast<const unsigned char*>(&loadout.moduleColoring[kMobilityColorIndex][0][0]);
    constexpr std::size_t kLegColourBytes =
        static_cast<std::size_t>(kModuleColorChannels) * static_cast<std::size_t>(kModuleColorComponents) *
        sizeof(float);
    hash = fnv1a32(bytes, kLegColourBytes);
    ok = true;
    return true;
}

}

void forget_target_model() { held_decisions().clear(); }

std::vector<std::string> TargetModelReport::report_lines() const {
    std::vector<std::string> out;
    if (!worldFound) {
        out.push_back("targets: " + note);
        return out;
    }
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "targets: %u enemy/enemies, %u candidate(s) (%u NPC(s) filtered out); %u engaged, %u not "
                  "engaged (left alone); %u added as a candidate (never a clear), %u already there, %u below "
                  "the floor, %u refused, %u indistinguishable pair(s)",
                  enemies, candidates, npcSkipped, engaged, notEngaged, forced, verified, belowFloor, refused,
                  ambiguous);
    out.push_back(buf);

    char timing[224];
    std::snprintf(timing, sizeof(timing),
                  "targets: timing: aggro=%.1f ms, candidates=%.1f ms, enemies=%.1f ms, decide=%.1f ms",
                  static_cast<double>(usAggro) / 1000.0, static_cast<double>(usCandidates) / 1000.0,
                  static_cast<double>(usEnemies) / 1000.0, static_cast<double>(usDecide) / 1000.0);
    out.push_back(timing);
    for (const TargetDecision& d : decisions) {
        if (!d.switched) continue;
        char fromText[24];
        if (d.fromScoreKnown) {
            std::snprintf(fromText, sizeof(fromText), "%.2f", static_cast<double>(d.fromScore));
        } else {
            std::snprintf(fromText, sizeof(fromText), "NOT MEASURED");
        }
        char line[480];
        std::snprintf(line, sizeof(line), "targets: '%s' %s -> %s (%s -> %.2f)", d.enemyName.c_str(),
                      d.fromName.empty() ? "NOTHING" : d.fromName.c_str(),
                      d.toName.empty() ? "NOTHING" : d.toName.c_str(), fromText,
                      static_cast<double>(d.toScore));
        out.push_back(line);
    }
    for (const std::string& one : lines) out.push_back("targets: " + one);
    return out;
}

TargetModelReport ai_enforce_targets(std::uint64_t nowMs, bool asHost) {
    TargetModelReport out;

    (void)asHost;
    const std::uintptr_t actorClass = resolve_uclass_address("Actor");
    if (actorClass == 0) {
        out.note = "the Actor UClass is not in the object array";
        return out;
    }
    const FunctionRef fnLocation = reflect_resolve_function(actorClass, "K2_GetActorLocation");
    if (!fnLocation.usable) {
        out.note = "K2_GetActorLocation is not usable: " + fnLocation.note;
        return out;
    }
    const std::uintptr_t targetingClass = resolve_uclass_address("NPTargetingSystemComponent");
    if (targetingClass == 0) {
        out.note = "NPTargetingSystemComponent is not in the object array";
        return out;
    }
    const FunctionRef fnMainTarget = reflect_resolve_function(targetingClass, "GetMainTarget");
    const FunctionRef fnAddTarget = reflect_resolve_function(targetingClass, "AddTarget");
    if (!fnAddTarget.usable) {
        out.note = "AddTarget could not be resolved: " + fnAddTarget.note;
        return out;
    }

    std::vector<TargetCandidate> candidates;
    const auto tAggroStart = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point tAggroDone{};
    std::chrono::steady_clock::time_point tCandidatesDone{};
    EnemySurvey survey;
    {

        std::vector<AggroMachine> machines;
        survey = survey_enemies(&machines);
        tAggroDone = std::chrono::steady_clock::now();
        if (!survey.worldFound) {
            out.note = "the live world is not known: " + survey.worldNote;
            return out;
        }
        out.worldFound = true;
        for (const AggroMachine& m : machines) {

            if (!npc_exclusion_reason(m.cls, m.controllerCls).empty()) {
                ++out.npcSkipped;
                continue;
            }
            TargetCandidate c;
            c.actor = m.actor;
            c.name = m.name;

            std::uint32_t hash = 0;
            bool colourOk = false;
            if (leg_colour_hash(m.actor, c.legClass, hash, colourOk)) {
                c.colourHash = hash;
                c.colourKnown = colourOk;
            }
            if (c.legClass.empty()) c.legClass = m.cls;
            c.hasLocation = m.hasLocation;
            c.x = m.x;
            c.y = m.y;
            c.z = m.z;
            c.isPuppet = !m.hasController;
            candidates.push_back(std::move(c));
        }
    }
    tCandidatesDone = std::chrono::steady_clock::now();
    out.usAggro = elapsed_us(tAggroStart, tAggroDone);
    out.usCandidates = elapsed_us(tAggroDone, tCandidatesDone);
    out.candidates = static_cast<std::uint32_t>(candidates.size());
    if (candidates.size() < 2) {
        out.note = "only " + std::to_string(candidates.size()) +
                   " player machine(s) in the levels -- the model needs both, so there is nothing to choose";
        out.worldFound = true;
        return out;
    }

    for (std::size_t i = 0; i < candidates.size(); ++i) {
        for (std::size_t j = i + 1; j < candidates.size(); ++j) {
            const TargetCandidate& a = candidates[i];
            const TargetCandidate& b = candidates[j];
            if (a.name == b.name && a.colourKnown && b.colourKnown && a.colourHash == b.colourHash) {
                ++out.ambiguous;
            }
        }
    }
    for (const TargetCandidate& c : candidates) {
        char one[320];
        std::snprintf(one, sizeof(one), "candidate '%s' (leg class '%s', colours %s%s)", c.name.c_str(),
                      c.legClass.c_str(), c.colourKnown ? "0x" : "NOT MEASURED",
                      c.colourKnown ? [&] { static thread_local char hex[16];
                                            std::snprintf(hex, sizeof(hex), "%08X", c.colourHash);
                                            return std::string(hex); }().c_str() : std::string().c_str());
        if (out.lines.size() < 8) out.lines.push_back(one);
    }
    if (out.ambiguous != 0) {
        out.note = "two player machines share BOTH the leg class name and its four colours, so they are one "
                   "person as far as any name or colour can tell -- refusing rather than guessing";
        out.worldFound = true;
        out.lines.push_back(out.note);
        return out;
    }

    const auto tEnemiesDone = std::chrono::steady_clock::now();
    out.usEnemies = elapsed_us(tCandidatesDone, tEnemiesDone);
    std::uint32_t decided = 0;

    std::uint32_t notEngagedLines = 0;
    for (const EnemyRecord& e : survey.enemies) {
        if (e.actorName.empty()) continue;
        if (!npc_exclusion_reason(e.actorClass, e.controllerClass).empty()) continue;
        if (e.controller < 0x10000) continue;
        ++out.enemies;
        std::uint64_t targeting = 0;
        if (!read_u64_at(e.controller + 0x0358, targeting) || targeting < 0x10000) continue;
        const auto targetingPtr = static_cast<std::uintptr_t>(targeting);

        const int aiState = read_ai_state(static_cast<std::uintptr_t>(e.controller));
        if (aiState != 1) {
            ++out.notEngaged;
            if (notEngagedLines < 3 && out.lines.size() < 8) {
                ++notEngagedLines;
                out.lines.push_back("'" + e.actorName + "' is " + ai_state_name(aiState) +
                                    " -- not steered: a main target is not a fight, and the tree walks");
            }
            continue;
        }
        ++out.engaged;

        std::string currentName;
        {
            unsigned char ret[0x10] = {};
            std::string note;
            if (fnMainTarget.usable && reflect_invoke_function(targetingPtr, fnMainTarget, ret, note)) {
                std::uint64_t held = 0;
                if (read_u64_at(reinterpret_cast<std::uintptr_t>(ret), held) && held >= 0x10000) {
                    currentName = object_own_name(static_cast<std::uintptr_t>(held));
                }
            }
        }

        const float ex = e.x, ey = e.y, ez = e.z;
        const bool enemyHasLocation = e.locationRead;
        TargetCandidate* best = nullptr;
        float bestScore = -1.0f;
        std::vector<std::pair<TargetCandidate*, float>> scored;
        HeldDecision* held = held_for(e.actorName);
        for (TargetCandidate& c : candidates) {
            TargetScoreInputs in;
            in.distanceCm = (enemyHasLocation && c.hasLocation)
                                ? std::sqrt((ex - c.x) * (ex - c.x) + (ey - c.y) * (ey - c.y) +
                                            (ez - c.z) * (ez - c.z))
                                : 100000.0f;
            in.incumbent = (held != nullptr && held->chosenName == c.name);
            const float score = target_score(in);
            scored.push_back({&c, score});
            if (score > bestScore) {
                bestScore = score;
                best = &c;
            }
        }
        if (best == nullptr) continue;
        if (bestScore < kDecisionFloor) {

            for (std::size_t i = 0; i < held_decisions().size();) {
                if (held_decisions()[i].enemyName == e.actorName) {
                    held_decisions().erase(held_decisions().begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
            ++out.belowFloor;
            continue;
        }

        if (out.lines.size() < 6) {
            std::string detail = "'" + e.actorName + "'";
            for (const auto& pair : scored) {
                char piece[200];
                const float distance = (enemyHasLocation && pair.first->hasLocation)
                                           ? std::sqrt((ex - pair.first->x) * (ex - pair.first->x) +
                                                       (ey - pair.first->y) * (ey - pair.first->y) +
                                                       (ez - pair.first->z) * (ez - pair.first->z))
                                           : -1.0f;
                std::snprintf(piece, sizeof(piece), " %s: %.2f (%s)",
                              pair.first->name.c_str(), static_cast<double>(pair.second),
                              distance < 0.0f
                                  ? "distance NOT MEASURED"
                                  : (std::to_string(static_cast<int>(distance)) + " cm").c_str());
                detail += piece;
            }
            out.lines.push_back(detail);
        }

        TargetCandidate* chosen = best;
        float chosenScore = bestScore;
        if (held != nullptr) {
            TargetCandidate* incumbent = nullptr;
            float incumbentScore = 0.0f;
            for (const auto& pair : scored) {
                if (pair.first->name == held->chosenName) {
                    incumbent = pair.first;
                    incumbentScore = pair.second;
                }
            }

            if (incumbent != nullptr && target_keep_incumbent(bestScore, incumbentScore)) {
                chosen = incumbent;
                chosenScore = incumbentScore;
            }
        }

        TargetDecision decision;
        decision.enemyName = e.actorName;
        decision.fromName = currentName;

        if (!currentName.empty()) {
            for (const auto& pair : scored) {
                if (pair.first->name == currentName) {
                    decision.fromScore = pair.second;
                    decision.fromScoreKnown = true;
                    break;
                }
            }
        }
        decision.toName = chosen->name;
        decision.toScore = chosenScore;
        if (!held) {
            HeldDecision fresh;
            fresh.enemyName = e.actorName;
            fresh.chosenName = chosen->name;
            fresh.score = chosenScore;
            held_decisions().push_back(std::move(fresh));
        } else {
            held->score = chosenScore;
            if (held->chosenName != chosen->name) {
                decision.switched = true;
                held->chosenName = chosen->name;
            }
        }
        ++decided;

        std::uint32_t listSize = 0;
        const bool holdsChosen = enemy_holds_candidate(targetingPtr, chosen->actor, listSize);
        if (currentName == chosen->name || holdsChosen) {
            ++out.verified;
        } else {
            unsigned char frame[0x10] = {};
            const std::uint64_t asArgument = chosen->actor;
            std::memcpy(frame + 0x00, &asArgument, sizeof(asArgument));
            std::string addNote;
            if (!reflect_invoke_function(targetingPtr, fnAddTarget, frame, addNote)) {
                ++out.refused;
                if (out.lines.size() < 8) out.lines.push_back("'" + e.actorName + "' AddTarget refused: " + addNote);
            } else {
                ++out.forced;
                decision.switched = true;
                if (out.lines.size() < 8) {
                    out.lines.push_back("'" + e.actorName + "' offered '" + chosen->name + "' (" +
                                        std::to_string(listSize) + " entry/entries before)");
                }
            }
        }

        out.decisions.push_back(std::move(decision));
    }
    if (decided == 0) out.lines.push_back("not one AI-driven character had a targeting component to ask");
    out.usDecide = elapsed_us(tEnemiesDone, std::chrono::steady_clock::now());
    return out;
}

}
