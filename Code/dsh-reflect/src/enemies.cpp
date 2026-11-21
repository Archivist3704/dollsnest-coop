#include "dsh/reflect/enemies.h"

#include "dsh/reflect/scanner.h"
#include "dsh/reflect/gamethread.h"
#include "dsh/platform/log.h"
#include "dsh/reflect/reflection.h"
#include "dsh/reflect/offsets.h"
#include "dsh/reflect/spawn.h"
#include "dsh/reflect/world.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace dsh::reflect {
namespace {

using namespace dsh::reflect::offsets;

constexpr std::uint32_t kWorldLevels = 0x0138;
constexpr std::uint32_t kLevelActors = 0x0098;
constexpr std::uint32_t kPawnController = 0x0258;

constexpr std::uint32_t kMaxLevels = 4096;
constexpr std::uint32_t kMaxActorsPerLevel = 200000;
constexpr int           kMaxSuperChain = 32;

constexpr std::uint32_t kLocationFrameBytes = 0x10;

std::uint64_t read_u64(std::uintptr_t addr) {
    std::uint64_t v = 0;
    if (!try_read_u64(addr, v)) return 0;
    return v;
}

constexpr std::uint32_t kMaxMachinesListed = 8;
bool read_actor_location(std::uintptr_t actor, const FunctionRef& fn, float& x, float& y, float& z,
                         std::string& note);

std::string name_of_object(std::uintptr_t obj) {
    const std::uint64_t nameField = read_u64(obj + kUObjectNameOffset);
    if (nameField == 0) return {};
    std::string out;
    std::string note;
    if (!resolve_fname(static_cast<std::uint32_t>(nameField & 0xFFFFFFFFu), out, note)) return {};
    return out;
}

struct ArrayHeader {
    std::uintptr_t data = 0;
    std::uint32_t  num = 0;
    std::uint32_t  max = 0;
};

bool read_tarray(std::uintptr_t addr, std::uint32_t limit, ArrayHeader& out) {
    const std::uint64_t data = read_u64(addr);
    std::uint32_t num = 0;
    std::uint32_t max = 0;
    if (!try_read_u32(addr + 8, num) || !try_read_u32(addr + 12, max)) return false;
    if (num == 0) {
        out = ArrayHeader{};
        return true;
    }
    if (num > limit) return false;
    if (max < num || data < 0x10000) return false;
    out.data = static_cast<std::uintptr_t>(data);
    out.num = num;
    out.max = max;
    return true;
}

bool class_derives_from_named(std::uintptr_t cls, const char* baseName,
                              std::unordered_map<std::uintptr_t, bool>& cache) {
    if (cls < 0x10000) return false;

    std::uint64_t nameHash = 1469598103934665603ull;
    for (const char* p = baseName; p != nullptr && *p != '\0'; ++p) {
        nameHash ^= static_cast<unsigned char>(*p);
        nameHash *= 1099511628211ull;
    }
    const std::uintptr_t key = static_cast<std::uintptr_t>(cls ^ nameHash);
    const auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    bool found = false;
    std::uintptr_t cur = cls;
    for (int depth = 0; depth < kMaxSuperChain; ++depth) {
        if (cur < 0x10000) break;
        if (name_of_object(cur) == baseName) {
            found = true;
            break;
        }
        const std::uint64_t super = read_u64(cur + kUStructSuperOffset);
        if (super == 0) break;
        cur = static_cast<std::uintptr_t>(super);
    }

    cache.emplace(key, found);
    return found;
}

bool name_is_enemy_ish(const std::string& cls) {
    return cls.find("Enemy") != std::string::npos || cls.find("AI") != std::string::npos;
}

}

bool is_npc_character(const std::string& actorClass, const std::string& controllerClass) {
    return actorClass.find("NPC") != std::string::npos ||
           controllerClass.find("NPC") != std::string::npos;
}

std::string npc_exclusion_reason(const std::string& actorClass, const std::string& controllerClass) {
    const bool byActor = actorClass.find("NPC") != std::string::npos;
    const bool byController = controllerClass.find("NPC") != std::string::npos;
    if (!byActor && !byController) return {};
    if (byActor && byController) {
        return "the actor's class name and its controller's class name both say NPC ('" + actorClass +
               "' / '" + controllerClass + "')";
    }
    if (byActor) return "the actor's own class name says NPC ('" + actorClass + "')";
    return "its controller's class name says NPC ('" + controllerClass + "')";
}

EnemySurvey survey_enemies(std::vector<AggroMachine>* machinesOut) {
    EnemySurvey s;

    const LiveObjects live = find_live_objects();
    if (live.world == 0 || live.level == 0) {
        s.worldNote = "the live world or its persistent level is not known, so no level was walked";
        return s;
    }
    s.worldFound = true;

    s.baseClass = resolve_uclass_address("DAAIController");

    ArrayHeader levels;
    if (!read_tarray(live.world + kWorldLevels, kMaxLevels, levels)) {
        s.worldNote = "UWorld::Levels could not be read, so no level was walked";
        return s;
    }

    FunctionRef getLocation;
    {
        const std::uintptr_t actorClass = resolve_uclass_address("Actor");
        if (actorClass != 0) {
            getLocation = reflect_resolve_function(actorClass, "K2_GetActorLocation");
        } else {
            getLocation.note = "the Actor UClass is not in the object array";
        }
    }

    std::unordered_map<std::uintptr_t, bool> aiControllerCache;
    std::unordered_map<std::uintptr_t, bool> pawnCache;
    std::unordered_map<std::uintptr_t, bool> machineCache;
    std::unordered_map<std::string, std::uint32_t> candidates;
    std::uint64_t actorsSeen = 0;
    std::uint64_t pawnsSeen = 0;

    for (std::uint32_t li = 0; li < levels.num; ++li) {
        const std::uint64_t levelPtr = read_u64(levels.data + li * 8);
        if (levelPtr < 0x10000) continue;
        ++s.levelsWalked;

        ArrayHeader actors;
        if (!read_tarray(static_cast<std::uintptr_t>(levelPtr) + kLevelActors, kMaxActorsPerLevel,
                         actors)) {
            continue;
        }
        for (std::uint32_t ai = 0; ai < actors.num; ++ai) {
            const std::uint64_t actorPtr = read_u64(actors.data + ai * 8);
            if (actorPtr < 0x10000) continue;
            ++actorsSeen;

            const auto actor = static_cast<std::uintptr_t>(actorPtr);
            const std::string cls = object_class_name(actor);
            if (name_is_enemy_ish(cls)) ++candidates[cls];

            const std::uint64_t classPtr = read_u64(actor + kUObjectClassOffset);
            if (classPtr < 0x10000) continue;

            if (machinesOut != nullptr && machinesOut->size() < kMaxMachinesListed &&
                class_derives_from_named(static_cast<std::uintptr_t>(classPtr), "DAMachine", machineCache)) {
                AggroMachine m;
                m.actor = actor;
                m.name = name_of_object(actor);
                m.cls = cls;
                std::uint64_t machineController = 0;
                if (try_read_u64(actor + kPawnController, machineController) &&
                    machineController >= 0x10000) {
                    m.hasController = true;
                    m.controllerCls = object_class_name(static_cast<std::uintptr_t>(machineController));
                }
                m.hasLocation = read_actor_location(actor, getLocation, m.x, m.y, m.z, m.locationNote);
                machinesOut->push_back(std::move(m));
            }

            if (!class_derives_from_named(static_cast<std::uintptr_t>(classPtr), "Pawn", pawnCache)) {
                continue;
            }
            ++pawnsSeen;

            const std::uint64_t controller = read_u64(actor + kPawnController);
            if (controller < 0x10000) {
                continue;
            }
            const auto controllerAddr = static_cast<std::uintptr_t>(controller);
            const std::uint64_t controllerClass = read_u64(controllerAddr + kUObjectClassOffset);
            if (controllerClass < 0x10000) continue;
            if (!class_derives_from_named(static_cast<std::uintptr_t>(controllerClass), "DAAIController",
                                          aiControllerCache)) {
                continue;
            }

            EnemyRecord e;
            e.actor = actor;
            e.actorClass = cls;
            e.actorName = name_of_object(actor);
            e.controller = controllerAddr;
            e.controllerRead = true;
            e.controllerClass = object_class_name(controllerAddr);
            if (e.controllerClass.empty()) {
                e.controllerNote = "the controller's class name could not be resolved";
            }

            if (getLocation.usable) {
                unsigned char frame[kLocationFrameBytes] = {0};
                std::string note;
                if (!reflect_invoke_function(actor, getLocation, frame, note)) {
                    e.locationNote = "the call was NOT made: " + note;
                } else {
                    float xyz[3] = {0.0f, 0.0f, 0.0f};
                    if (try_read_bytes(reinterpret_cast<std::uintptr_t>(frame), xyz, sizeof(xyz))) {
                        e.x = xyz[0];
                        e.y = xyz[1];
                        e.z = xyz[2];
                        e.locationRead = true;
                    } else {
                        e.locationNote = "K2_GetActorLocation WAS called but its answer could not be read "
                                         "back out of the frame";
                    }
                }
            } else {
                e.locationNote = "K2_GetActorLocation could not be resolved, so NO location was read: " +
                                 getLocation.note;
            }

            s.enemies.push_back(std::move(e));
        }
    }

    s.actorsWalked = actorsSeen;
    s.pawnsWalked = pawnsSeen;
    s.candidateClasses.assign(candidates.begin(), candidates.end());
    std::sort(s.candidateClasses.begin(), s.candidateClasses.end(),
              [](const std::pair<std::string, std::uint32_t>& a,
                 const std::pair<std::string, std::uint32_t>& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });
    return s;
}

std::vector<std::string> EnemySurvey::report_lines() const {
    std::vector<std::string> out;
    char buf[512];

    if (baseClass == 0) {
        out.push_back("the DAAIController UClass is NOT in the object array, so the controller-chain check "
                      "could not be made at all -- nothing below is a statement about AI-driven characters");
    }
    std::snprintf(buf, sizeof(buf),
                  "world: %u level(s) walked, %llu actor(s) seen of which %llu pawn(s); %zu AI-driven "
                  "character(s) found (possessed by a DAAIController descendant)",
                  levelsWalked, static_cast<unsigned long long>(actorsWalked),
                  static_cast<unsigned long long>(pawnsWalked), enemies.size());
    out.push_back(buf);
    if (!worldNote.empty()) out.push_back("world: " + worldNote);

    if (!candidateClasses.empty()) {
        std::string line = "classes whose NAME mentions 'Enemy' or 'AI' (accepted or not):";
        for (std::size_t i = 0; i < candidateClasses.size() && i < 12; ++i) {
            const std::string& cls = candidateClasses[i].first;
            const bool accepted = std::any_of(enemies.begin(), enemies.end(),
                                              [&](const EnemyRecord& e) { return e.actorClass == cls; });
            line += " " + cls + "=" + std::to_string(candidateClasses[i].second) +
                    (accepted ? " [accepted]" : " [NOT accepted]");
        }
        out.push_back(line);
    } else if (worldFound) {
        out.push_back("classes whose NAME mentions 'Enemy' or 'AI': NONE -- so either this level holds no "
                      "AI-driven characters, or the controller rule and this heuristic are both looking at "
                      "the wrong thing");
    }

    for (std::size_t i = 0; i < enemies.size(); ++i) {
        const EnemyRecord& e = enemies[i];
        std::string line = " [" + std::to_string(i + 1) + "] " + e.actorClass + " '" + e.actorName + "'";
        if (e.locationRead) {
            std::snprintf(buf, sizeof(buf), " at (%.1f, %.1f, %.1f) cm", static_cast<double>(e.x),
                          static_cast<double>(e.y), static_cast<double>(e.z));
            line += buf;
        } else {
            line += " at a location that was NOT MEASURED (" + e.locationNote + ")";
        }
        if (e.controllerRead) {
            line += " controller='" + e.controllerClass + "'";
            if (!e.controllerNote.empty()) line += " (" + e.controllerNote + ")";
        } else {
            line += " controller=NONE";
        }
        std::snprintf(buf, sizeof(buf), " @0x%llX", static_cast<unsigned long long>(e.actor));
        line += buf;
        out.push_back(line);
    }

    out.push_back(
        "identity: put THIS list beside the other machine's. If the same name AND the same (x, y, z) appear "
        "on both, the actor's own name is a usable id. If the transforms match but the names do not, the "
        "transform is the id. If neither matches, this walk is looking at the wrong class -- and the "
        "histogram above says so.");
    return out;
}

namespace {

constexpr std::uint32_t kControllerTargeting = 0x0358;

bool read_actor_location(std::uintptr_t actor, const FunctionRef& fn, float& x, float& y, float& z,
                         std::string& note) {
    if (!fn.usable) {
        note = "the call could not be made because K2_GetActorLocation was not resolved: " + fn.note;
        return false;
    }
    unsigned char frame[kLocationFrameBytes] = {0};
    std::string callNote;
    if (!reflect_invoke_function(actor, fn, frame, callNote)) {
        note = "the call was NOT made: " + callNote;
        return false;
    }
    float xyz[3] = {0.0f, 0.0f, 0.0f};
    if (!try_read_bytes(reinterpret_cast<std::uintptr_t>(frame), xyz, sizeof(xyz))) {
        note = "K2_GetActorLocation WAS called but its answer could not be read back out of the frame";
        return false;
    }
    x = xyz[0];
    y = xyz[1];
    z = xyz[2];
    return true;
}

}

namespace {

constexpr std::uint32_t kTargetingTargetDataList = 0x00B0;
constexpr std::uint32_t kTargetingMainTarget = 0x00D8;
constexpr std::uint32_t kScoringStride = 0x18;
constexpr std::uint32_t kScoringScore = 0x08;
constexpr std::uint32_t kScoringTargetedTime = 0x0C;
constexpr std::uint32_t kScoringFiltered = 0x10;
constexpr std::uint32_t kMaxCandidatesListed = 24;
constexpr std::uint32_t kMaxPerceptionNamesListed = 8;

bool read_f32(std::uintptr_t addr, float& out) {
    return try_read_bytes(addr, &out, sizeof(out));
}

std::string classify_machine_or_npc(std::uintptr_t actor, std::unordered_map<std::uintptr_t, bool>& cache,
                                    bool& isMachine, bool& isNpc) {
    isMachine = false;
    isNpc = false;
    const std::string cls = object_class_name(actor);
    if (cls.find("NPC") != std::string::npos) isNpc = true;
    const std::uint64_t classPtr = read_u64(actor + kUObjectClassOffset);
    if (classPtr >= 0x10000 &&
        class_derives_from_named(static_cast<std::uintptr_t>(classPtr), "DAMachine", cache)) {
        isMachine = true;
    }
    return cls;
}

std::vector<AggroMachine> collect_machines(const FunctionRef& fnLocation,
                                           std::unordered_map<std::uintptr_t, bool>& machineCache,
                                           std::string& note) {
    std::vector<AggroMachine> out;
    note.clear();
    const LiveObjects live = find_live_objects();
    if (live.world == 0 || live.level == 0) {
        note = "the live world or its persistent level is not known, so no machine was listed";
        return out;
    }
    ArrayHeader levels;
    if (!read_tarray(live.world + kWorldLevels, kMaxLevels, levels)) {
        note = "UWorld::Levels could not be read";
        return out;
    }
    for (std::uint32_t li = 0; li < levels.num; ++li) {
        const std::uint64_t levelPtr = read_u64(levels.data + li * 8);
        if (levelPtr < 0x10000) continue;
        ArrayHeader actors;
        if (!read_tarray(static_cast<std::uintptr_t>(levelPtr) + kLevelActors, kMaxActorsPerLevel,
                         actors)) {
            continue;
        }
        for (std::uint32_t ai = 0; ai < actors.num; ++ai) {
            const std::uint64_t actorPtr = read_u64(actors.data + ai * 8);
            if (actorPtr < 0x10000) continue;
            const auto actor = static_cast<std::uintptr_t>(actorPtr);
            const std::uint64_t classPtr = read_u64(actor + kUObjectClassOffset);
            if (classPtr < 0x10000) continue;
            if (!class_derives_from_named(static_cast<std::uintptr_t>(classPtr), "DAMachine",
                                          machineCache)) {
                continue;
            }
            AggroMachine m;
            m.actor = actor;
            m.name = name_of_object(actor);
            m.cls = object_class_name(actor);
            std::uint64_t controller = 0;
            if (try_read_u64(actor + kPawnController, controller) && controller >= 0x10000) {
                m.hasController = true;
                m.controllerCls = object_class_name(static_cast<std::uintptr_t>(controller));
            }
            m.hasLocation = read_actor_location(actor, fnLocation, m.x, m.y, m.z, m.locationNote);
            out.push_back(std::move(m));
            if (out.size() >= kMaxMachinesListed) return out;
        }
    }
    return out;
}

}

AggroSurvey survey_aggro() {
    AggroSurvey s;

    EnemySurvey survey = survey_enemies(&s.machines);
    if (!survey.worldFound) {
        s.worldNote = "the live world is not known, so no level was walked: " + survey.worldNote;
        return s;
    }
    s.worldFound = true;
    s.enemiesListed = static_cast<std::uint32_t>(survey.enemies.size());

    std::unordered_map<std::uintptr_t, bool> machineCache;
    FunctionRef fnMainTarget;
    FunctionRef fnKnownPerceived;
    FunctionRef fnLocation;
    {
        const std::uintptr_t targetingClass = resolve_uclass_address("NPTargetingSystemComponent");
        if (targetingClass != 0) {
            fnMainTarget = reflect_resolve_function(targetingClass, "GetMainTarget");
        }
        const std::uintptr_t perceptionClass = resolve_uclass_address("AIPerceptionComponent");
        if (perceptionClass != 0) {
            fnKnownPerceived = reflect_resolve_function(perceptionClass, "GetKnownPerceivedActors");
        }
        const std::uintptr_t actorClass = resolve_uclass_address("Actor");
        if (actorClass != 0) {
            fnLocation = reflect_resolve_function(actorClass, "K2_GetActorLocation");
        }
    }

    for (const EnemyRecord& e : survey.enemies) {
        if (e.actorName.empty()) continue;

        const std::string npcWhy = npc_exclusion_reason(e.actorClass, e.controllerClass);
        if (!npcWhy.empty()) {
            ++s.npcSkipped;
            if (s.npcNames.size() < 8) s.npcNames.push_back(e.actorName + " (" + npcWhy + ")");
            continue;
        }

        AggroEnemy a;
        a.enemyName = e.actorName;
        a.enemyClass = e.actorClass;
        a.controllerClass = e.controllerClass;
        a.hasLocation = read_actor_location(e.actor, fnLocation, a.x, a.y, a.z, a.locationNote);
        bool sawAMachine = false;

        if (e.controller >= 0x10000) {
            std::uint64_t targeting = 0;
            if (try_read_u64(e.controller + kControllerTargeting, targeting) && targeting >= 0x10000) {
                a.targeting = static_cast<std::uintptr_t>(targeting);

                if (fnMainTarget.usable) {
                    unsigned char ret[0x10] = {};
                    std::string note;
                    if (reflect_invoke_function(a.targeting, fnMainTarget, ret, note)) {
                        std::uint64_t mainTarget = 0;
                        if (try_read_u64(reinterpret_cast<std::uintptr_t>(ret), mainTarget) &&
                            mainTarget >= 0x10000) {
                            a.mainTargetName = name_of_object(static_cast<std::uintptr_t>(mainTarget));
                            a.mainTargetClass = object_class_name(static_cast<std::uintptr_t>(mainTarget));
                        } else {
                            a.mainTargetName.clear();
                        }
                    } else {
                        a.mainTargetNote = "GetMainTarget was NOT called: " + note;
                    }
                } else {
                    a.mainTargetNote = "GetMainTarget could not be resolved: " + fnMainTarget.note;
                }

                ArrayHeader list;
                if (read_tarray(a.targeting + kTargetingTargetDataList, 4096, list)) {
                    a.listRead = true;
                    for (std::uint32_t i = 0; i < list.num && i < kMaxCandidatesListed; ++i) {
                        const std::uintptr_t entry =
                            list.data + static_cast<std::uintptr_t>(i) * kScoringStride;
                        std::uint64_t target = 0;
                        if (!try_read_u64(entry, target) || target < 0x10000) {
                            ++a.candidatesUnreadable;
                            continue;
                        }
                        AggroCandidate c;
                        c.actor = static_cast<std::uintptr_t>(target);
                        c.name = name_of_object(static_cast<std::uintptr_t>(target));
                        c.cls = classify_machine_or_npc(static_cast<std::uintptr_t>(target), machineCache,
                                                        c.isMachine, c.isNpc);
                        read_f32(entry + kScoringScore, c.score);
                        read_f32(entry + kScoringTargetedTime, c.targetedTime);
                        std::uint8_t filtered = 0;
                        if (try_read_bytes(entry + kScoringFiltered, &filtered, 1)) {
                            c.filtered = filtered != 0;
                        }
                        if (c.isMachine) a.holdsMachine = true;
                        if (c.isNpc) a.holdsNpc = true;
                        ++s.candidatesTotal;
                        if (c.filtered) ++s.candidatesFiltered;
                        a.candidates.push_back(std::move(c));
                    }
                } else {
                    a.listNote = "TargetDataList could not be read as a TArray";
                }

                std::uint64_t perception = 0;
                if (try_read_u64(e.controller + 0x0350, perception) && perception >= 0x10000) {
                    if (fnKnownPerceived.usable) {
                        unsigned char frame[0x20] = {};

                        std::string note;
                        if (reflect_invoke_function(static_cast<std::uintptr_t>(perception), fnKnownPerceived,
                                                    frame, note)) {

                            ArrayHeader known;
                            if (read_tarray(reinterpret_cast<std::uintptr_t>(frame) + 0x08, 65536, known)) {
                                a.perceptionRead = true;
                                a.perceptionKnown = known.num;
                                for (std::uint32_t i = 0;
                                     i < known.num && i < kMaxPerceptionNamesListed; ++i) {
                                    std::uint64_t actor = 0;
                                    if (!try_read_u64(known.data + static_cast<std::uintptr_t>(i) * 8,
                                                      actor) ||
                                        actor < 0x10000) {
                                        continue;
                                    }
                                    a.perceptionNames.push_back(
                                        object_class_name(static_cast<std::uintptr_t>(actor)) + " '" +
                                        name_of_object(static_cast<std::uintptr_t>(actor)) + "'");
                                    a.perceptionActorNames.push_back(
                                        name_of_object(static_cast<std::uintptr_t>(actor)));
                                }
                            } else {
                                a.perceptionNote = "the call returned but its out-array could not be read";
                            }
                        } else {
                            a.perceptionNote = "the call was NOT made: " + note;
                        }
                    } else {
                        a.perceptionNote = "GetKnownPerceivedActors could not be resolved: " +
                                           fnKnownPerceived.note;
                    }
                } else {
                    a.perceptionNote = "the controller has nothing at (no perception component)";
                }
            } else {
                a.listNote = "the controller has nothing at (no targeting component)";
            }
        } else {
            a.listNote = "the survey gave this character no controller to ask";
        }

        for (const AggroMachine& m : s.machines) {
            AggroEnemy::MachineVerdict v;
            v.name = m.name;
            v.inPerception = std::find(a.perceptionActorNames.begin(), a.perceptionActorNames.end(),
                                       m.name) != a.perceptionActorNames.end();
            for (const AggroCandidate& c : a.candidates) {
                if (c.name == m.name) {
                    v.inCandidates = true;
                    break;
                }
            }
            if (a.hasLocation && m.hasLocation) {
                const float dx = a.x - m.x;
                const float dy = a.y - m.y;
                const float dz = a.z - m.z;
                v.distanceCm = std::sqrt(dx * dx + dy * dy + dz * dz);
                v.distanceKnown = true;
            }
            if (v.inPerception) sawAMachine = true;
            a.machines.push_back(std::move(v));
        }

        if (sawAMachine) ++s.enemiesWithMachinePerceived;
        if (a.holdsMachine) ++s.enemiesWithMachineCandidate;
        if (a.holdsNpc) ++s.enemiesWithNpcCandidate;
        if (!a.perceptionRead) ++s.perceptionUnavailable;
        s.enemies.push_back(std::move(a));
    }

    return s;
}

namespace {

struct WatchEnemy {
    std::string    name;
    std::uintptr_t targeting = 0;
    std::uintptr_t lastMain = 0;
    std::uint64_t  lastChangeMs = 0;
    std::uint64_t  longestHoldMs = 0;
    std::uint32_t  changes = 0;
    std::uint32_t  samples = 0;
    std::uint32_t  gone = 0;
};

struct WatchDoll {
    std::string    name;
    std::uintptr_t actor = 0;
};

std::vector<WatchEnemy> g_watchEnemies;
std::vector<WatchDoll>  g_watchDolls;
bool                    g_watchArmed = false;
std::uint64_t           g_watchStartedMs = 0;
std::uint32_t           g_watchLinesPrinted = 0;
constexpr std::uint32_t kWatchMaxLines = 80;
constexpr std::uint32_t kWatchMaxEnemies = 16;
constexpr std::uint32_t kWatchMaxDolls = 4;

std::string friendly_actor(std::uintptr_t actor) {
    if (actor < 0x10000) return "NOTHING";
    return "'" + name_of_object(actor) + "'";
}

std::string doll_scores(std::uintptr_t targeting) {
    ArrayHeader list;
    if (!read_tarray(targeting + kTargetingTargetDataList, 4096, list)) {
        return "candidate list UNREADABLE";
    }
    std::string out;
    for (const WatchDoll& d : g_watchDolls) {
        bool found = false;
        for (std::uint32_t i = 0; i < list.num && i < kMaxCandidatesListed; ++i) {
            const std::uintptr_t entry = list.data + static_cast<std::uintptr_t>(i) * kScoringStride;
            std::uint64_t target = 0;
            if (!try_read_u64(entry, target) || target != d.actor) continue;
            float score = 0.0f;
            read_f32(entry + kScoringScore, score);
            std::uint8_t filtered = 0;
            try_read_bytes(entry + kScoringFiltered, &filtered, 1);
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s %.2f%s", d.name.c_str(), static_cast<double>(score),
                          filtered != 0 ? " FILTERED" : "");
            if (!out.empty()) out += " | ";
            out += buf;
            found = true;
            break;
        }
        if (!found) {
            if (!out.empty()) out += " | ";
            out += d.name + " absent";
        }
    }
    return out.empty() ? "no doll to compare against" : out;
}

}

std::vector<std::string> TargetOfferReport::report_lines() const {
    std::vector<std::string> out;
    if (!note.empty()) out.push_back("offer: " + note);
    char buf[400];
    std::snprintf(buf, sizeof(buf),
                  "offer: %u enemy/enemies, %u puppet(s) on this machine, %u added, %u already held, "
                  "%u refused, %u NPC(s) skipped, %u NPC machine(s) never offered",
                  enemies, puppets, added, alreadyHeld, refused, npcSkipped, npcMachines);
    out.push_back(buf);
    for (const std::string& one : lines) out.push_back("offer: " + one);
    return out;
}

TargetOfferReport offer_puppets_as_candidates() {
    TargetOfferReport out;

    const AggroSurvey before = survey_aggro();
    out.worldFound = before.worldFound;
    if (!before.worldFound) {
        out.note = "the live world is not known: " + before.worldNote;
        return out;
    }
    out.npcSkipped = before.npcSkipped;
    out.enemies = static_cast<std::uint32_t>(before.enemies.size());

    std::vector<const AggroMachine*> puppets;
    for (const AggroMachine& m : before.machines) {
        if (m.hasController) continue;
        if (m.cls.find("NPC") != std::string::npos || m.name.find("NPC") != std::string::npos) {
            ++out.npcMachines;
            continue;
        }
        if (puppets.size() >= kWatchMaxDolls) break;
        puppets.push_back(&m);
    }
    out.puppets = static_cast<std::uint32_t>(puppets.size());
    if (puppets.empty()) {
        out.note = "this machine holds no OTHER player's doll, so there is nothing to offer -- run it inside a "
                   "session (a client offers the host's doll, the host offers the client's)";
        return out;
    }

    FunctionRef fnAdd;
    {
        const std::uintptr_t targetingClass = resolve_uclass_address("NPTargetingSystemComponent");
        if (targetingClass != 0) fnAdd = reflect_resolve_function(targetingClass, "AddTarget");
    }
    if (!fnAdd.usable) {
        out.note = "AddTarget could not be resolved: " + fnAdd.note;
        return out;
    }

    std::vector<std::string> touched;
    for (const AggroEnemy& e : before.enemies) {
        if (e.targeting < 0x10000) continue;
        std::uint32_t addedHere = 0;
        for (const AggroMachine* p : puppets) {
            bool present = false;
            for (const AggroCandidate& c : e.candidates) {
                if (c.actor == p->actor) {
                    present = true;
                    break;
                }
            }
            if (present) {
                ++out.alreadyHeld;
                continue;
            }
            unsigned char frame[0x10] = {};
            const std::uint64_t asArgument = p->actor;
            std::memcpy(frame + 0x00, &asArgument, sizeof(asArgument));
            std::string callNote;
            if (reflect_invoke_function(e.targeting, fnAdd, frame, callNote)) {
                ++out.added;
                ++addedHere;
            } else {
                ++out.refused;
                if (out.lines.size() < 8) {
                    out.lines.push_back("'" + e.enemyName + "' AddTarget('" + p->name + "') was REFUSED: " +
                                        callNote);
                }
            }
        }
        if (addedHere != 0 && touched.size() < kWatchMaxEnemies) touched.push_back(e.enemyName);
    }

    if (!touched.empty()) {
        const AggroSurvey after = survey_aggro();
        for (const std::string& enemyName : touched) {
            const AggroEnemy* was = nullptr;
            const AggroEnemy* is = nullptr;
            for (const AggroEnemy& e : before.enemies) {
                if (e.enemyName == enemyName) was = &e;
            }
            for (const AggroEnemy& e : after.enemies) {
                if (e.enemyName == enemyName) is = &e;
            }
            std::string line = "'" + enemyName + "': main ";
            line += (was != nullptr && !was->mainTargetName.empty()) ? "'" + was->mainTargetName + "'" : "NOTHING";
            line += " -> ";
            line += (is != nullptr && !is->mainTargetName.empty()) ? "'" + is->mainTargetName + "'" : "NOTHING";
            if (is != nullptr) {
                line += "; now: ";
                for (const AggroMachine* p : puppets) {
                    bool found = false;
                    for (const AggroCandidate& c : is->candidates) {
                        if (c.actor != p->actor) continue;
                        char buf[160];
                        std::snprintf(buf, sizeof(buf), "%s %.2f%s", p->name.c_str(),
                                      static_cast<double>(c.score), c.filtered ? " FILTERED" : "");
                        line += buf;
                        found = true;
                        break;
                    }
                    if (!found) line += p->name + " absent";
                    line += "; ";
                }
            }
            out.lines.push_back(std::move(line));
        }
    }
    return out;
}

TargetWatchArm target_watch_arm() {
    TargetWatchArm out;
    g_watchEnemies.clear();
    g_watchDolls.clear();
    g_watchLinesPrinted = 0;
    g_watchStartedMs = GetTickCount64();

    const AggroSurvey s = survey_aggro();
    if (!s.worldFound) {
        out.note = "the live world is not known: " + s.worldNote;
        return out;
    }
    for (const AggroMachine& m : s.machines) {
        if (m.hasController) continue;
        if (g_watchDolls.size() >= kWatchMaxDolls) break;
        g_watchDolls.push_back({m.name, m.actor});
    }
    for (const AggroEnemy& e : s.enemies) {
        if (e.targeting < 0x10000) continue;
        if (g_watchEnemies.size() >= kWatchMaxEnemies) break;
        WatchEnemy w;
        w.name = e.enemyName;
        w.targeting = e.targeting;
        std::uint64_t mainTarget = 0;
        if (try_read_u64(e.targeting + kTargetingMainTarget, mainTarget) && mainTarget >= 0x10000) {
            w.lastMain = static_cast<std::uintptr_t>(mainTarget);
        }
        w.lastChangeMs = g_watchStartedMs;
        g_watchEnemies.push_back(std::move(w));
    }
    g_watchArmed = !g_watchEnemies.empty();
    out.ok = g_watchArmed;
    out.enemies = static_cast<std::uint32_t>(g_watchEnemies.size());
    out.machines = static_cast<std::uint32_t>(g_watchDolls.size());
    if (!out.ok) out.note = "no AI enemy with a targeting component was found in this world";
    return out;
}

bool target_watch_armed() { return g_watchArmed; }

std::vector<std::string> target_watch_sample(std::uint64_t nowMs) {
    std::vector<std::string> out;
    if (!g_watchArmed) return out;
    for (WatchEnemy& w : g_watchEnemies) {
        ++w.samples;
        std::string liveNote;
        if (!object_is_live(w.targeting, liveNote)) {
            if (w.gone == 0 && g_watchLinesPrinted < kWatchMaxLines) {
                ++g_watchLinesPrinted;
                out.push_back("'" + w.name + "': its targeting component is GONE (" + liveNote +
                              "); counted from here on, not followed");
            }
            ++w.gone;
            continue;
        }
        std::uint64_t mainTarget = 0;
        if (!try_read_u64(w.targeting + kTargetingMainTarget, mainTarget)) continue;
        const std::uintptr_t main = mainTarget >= 0x10000 ? static_cast<std::uintptr_t>(mainTarget) : 0;
        if (main == w.lastMain) continue;
        const std::uint64_t hold = nowMs >= w.lastChangeMs ? nowMs - w.lastChangeMs : 0;
        if (hold > w.longestHoldMs) w.longestHoldMs = hold;
        ++w.changes;
        const std::string was = friendly_actor(w.lastMain);
        w.lastChangeMs = nowMs;
        w.lastMain = main;
        if (g_watchLinesPrinted < kWatchMaxLines) {
            ++g_watchLinesPrinted;
            char buf[512];
            std::snprintf(buf, sizeof(buf), "%llu ms (held %llu ms) '%s': %s -> %s; scores: %s",
                          static_cast<unsigned long long>(nowMs), static_cast<unsigned long long>(hold),
                          w.name.c_str(), was.c_str(), friendly_actor(main).c_str(),
                          doll_scores(w.targeting).c_str());
            out.push_back(buf);
        }
    }
    return out;
}

std::vector<std::string> target_watch_disarm() {
    std::vector<std::string> out;
    g_watchArmed = false;
    const std::uint64_t now = GetTickCount64();
    char head[240];
    std::snprintf(head, sizeof(head), "watch: %zu enemy/enemies came through %llu ms of sampling",
                  g_watchEnemies.size(), static_cast<unsigned long long>(now - g_watchStartedMs));
    out.push_back(head);
    for (const WatchEnemy& w : g_watchEnemies) {
        char buf[420];
        std::snprintf(buf, sizeof(buf),
                      "watch: '%s': %u change(s) over %u sample(s), longest hold %llu ms, now %s%s",
                      w.name.c_str(), w.changes, w.samples,
                      static_cast<unsigned long long>(w.longestHoldMs),
                      friendly_actor(w.lastMain).c_str(),
                      w.gone != 0 ? " (its component went away)" : "");
        out.push_back(buf);
    }
    g_watchEnemies.clear();
    g_watchDolls.clear();
    return out;
}

std::vector<std::string> AggroSurvey::report_lines() const {
    std::vector<std::string> out;
    char buf[512];

    if (!worldFound) {
        out.push_back("aggro: " + worldNote);
        return out;
    }
    std::snprintf(buf, sizeof(buf),
                  "aggro: %u AI-driven character(s), of which %u NPC(s) were SKIPPED and never followed; "
                  "%u of the enemies PERCEIVE a player machine and %u hold one as a candidate (THOSE TWO "
                  "NUMBERS ARE WHAT THIS COMMAND EXISTS FOR) and %u hold an NPC; %u candidate(s) in total, %u "
                  "of them marked bFiltered by the game itself",
                  enemiesListed, npcSkipped, enemiesWithMachinePerceived, enemiesWithMachineCandidate,
                  enemiesWithNpcCandidate, candidatesTotal, candidatesFiltered);
    out.push_back(buf);
    if (enemiesWithMachinePerceived != 0 && enemiesWithMachineCandidate == 0) {
        out.push_back("aggro: NOTE -- at least one enemy PERCEIVES a player machine and NOT ONE holds a "
                      "candidate: the doll gets as far as the enemy's senses and is dropped between there and "
                      "the targeting system, which is a different place to look than 'it never saw it'");
    }

    if (!npcNames.empty()) {
        std::string line = "aggro: the NPC(s) skipped:";
        for (const std::string& n : npcNames) line += " [" + n + "]";
        out.push_back(line);
    }
    if (candidatesFiltered == 0 && candidatesTotal != 0) {
        out.push_back("aggro: NOTE -- not one candidate carries bFiltered, so nothing was thrown away by the "
                      "game's own filter on this machine: a doll missing from a list is missing because it "
                      "never became a candidate, not because it was filtered out");
    }
    out.push_back("aggro: the out-array of GetKnownPerceivedActors is filled with the ENGINE's allocator and "
                  "this project has no free path for it, so this command leaves one small allocation per "
                  "enemy behind. It is a diagnostic and is not run in any loop.");

    if (machines.empty()) {
        out.push_back("aggro: NO player machine (DAMachine descendant) is in the loaded levels at all, so "
                      "there is nothing for an enemy to consider -- the measurement below cannot answer the "
                      "question");
    } else {
        std::snprintf(buf, sizeof(buf), "aggro: %zu player machine(s) in the loaded levels:", machines.size());
        out.push_back(buf);
        for (const AggroMachine& m : machines) {
            std::string line = " MACHINE '" + m.name + "' (" + m.cls + ") controller=";
            line += m.hasController ? ("'" + m.controllerCls + "'") : std::string("NONE (a puppet this side built)");
            if (m.hasLocation) {
                std::snprintf(buf, sizeof(buf), " at (%.0f, %.0f, %.0f) cm", static_cast<double>(m.x),
                              static_cast<double>(m.y), static_cast<double>(m.z));
                line += buf;
            } else {
                line += " at a location that was NOT MEASURED (" + m.locationNote + ")";
            }
            out.push_back(line);
        }
    }

    for (std::size_t i = 0; i < enemies.size(); ++i) {
        const AggroEnemy& a = enemies[i];
        std::snprintf(buf, sizeof(buf), " [%zu] '%s' (%s, controller %s)", i + 1, a.enemyName.c_str(),
                      a.enemyClass.c_str(), a.controllerClass.c_str());
        std::string line = buf;
        if (!a.mainTargetNote.empty()) {
            line += " -- main target NOT READ (" + a.mainTargetNote + ")";
        } else if (a.mainTargetName.empty()) {
            line += " -- main target: NOTHING";
        } else {
            line += " -- main target: '" + a.mainTargetName + "' (" + a.mainTargetClass + ")";
        }
        out.push_back(line);

        if (a.listRead) {
            std::snprintf(buf, sizeof(buf), " candidate list (%zu shown%s, %u unreadable):",
                          a.candidates.size(),
                          a.candidates.size() == kMaxCandidatesListed ? ", BOUNDED here" : "",
                          a.candidatesUnreadable);
            out.push_back(buf);
            for (const AggroCandidate& c : a.candidates) {
                std::snprintf(buf, sizeof(buf),
                              " %s '%s' (%s) score=%.2f targeted=%.2fs bFiltered=%s",
                              c.isMachine ? "[MACHINE]" : (c.isNpc ? "[NPC]" : "[other]"), c.name.c_str(),
                              c.cls.c_str(), static_cast<double>(c.score),
                              static_cast<double>(c.targetedTime), c.filtered ? "YES" : "no");
                out.push_back(buf);
            }
            if (a.candidates.empty()) out.push_back(" (no candidates at all)");
        } else {
            out.push_back(" candidate list NOT read (" + a.listNote + ")");
        }

        if (a.perceptionRead) {
            std::snprintf(buf, sizeof(buf), " perception knows %u actor(s)", a.perceptionKnown);
            std::string pl = buf;
            for (const std::string& n : a.perceptionNames) pl += " [" + n + "]";
            out.push_back(pl);
        } else {
            out.push_back(" perception NOT read (" + a.perceptionNote + ")");
        }

        if (a.mainTargetName.empty() && !a.mainTargetNote.empty()) {

        } else if (a.mainTargetName.empty()) {
            out.push_back(" THIS ENEMY IS IDLE (no main target): its candidate list is EXPECTED to be "
                          "empty and that emptiness is NOT evidence about any machine's eligibility. Compare "
                          "it with the same enemy on the OTHER machine's log, where the real player it is "
                          "fighting may be listed.");
        }

        if (a.hasLocation && !machines.empty()) {
            std::size_t closest = a.machines.size();
            for (std::size_t i = 0; i < a.machines.size(); ++i) {
                if (!a.machines[i].distanceKnown) continue;
                if (closest == a.machines.size() ||
                    a.machines[i].distanceCm < a.machines[closest].distanceCm) {
                    closest = i;
                }
            }
            if (closest != a.machines.size()) {
                const AggroEnemy::MachineVerdict& v = a.machines[closest];
                std::snprintf(buf, sizeof(buf),
                              " CLOSEST MACHINE: '%s' at %.0f cm -- in perception: %s, in candidate "
                              "list: %s",
                              v.name.c_str(), static_cast<double>(v.distanceCm),
                              v.inPerception ? "YES" : "no", v.inCandidates ? "YES" : "no");
                out.push_back(buf);
            }
        }

        if (!a.machines.empty()) {
            std::string line = " against the machine(s) in this world:";
            out.push_back(line);
            for (const AggroEnemy::MachineVerdict& v : a.machines) {
                if (v.distanceKnown) {
                    std::snprintf(buf, sizeof(buf),
                                  " '%s': %.0f cm away, in perception: %s, in candidate list: %s",
                                  v.name.c_str(), static_cast<double>(v.distanceCm),
                                  v.inPerception ? "YES" : "no", v.inCandidates ? "YES" : "no");
                } else {
                    std::snprintf(buf, sizeof(buf),
                                  " '%s': distance NOT MEASURED, in perception: %s, in candidate "
                                  "list: %s",
                                  v.name.c_str(), v.inPerception ? "YES" : "no",
                                  v.inCandidates ? "YES" : "no");
                }
                out.push_back(buf);
            }
        }
    }
    return out;
}

namespace {

std::vector<std::string> impl_survey_owned_objects(std::uintptr_t actor, const std::string& label) {
    std::vector<std::string> out;
    char buf[320];
    if (actor < 0x10000) {
        out.push_back(label + ": no actor");
        return out;
    }
    std::snprintf(buf, sizeof(buf), "%s '%s' (%s):", label.c_str(), name_of_object(actor).c_str(),
                  object_class_name(actor).c_str());
    out.push_back(buf);
    std::uint32_t found = 0;
    for (std::uintptr_t offset = 0x0020; offset < 0x1800; offset += 8) {
        const std::uint64_t p = read_u64(actor + offset);
        if (p < 0x10000) continue;
        const std::string cls = object_class_name(static_cast<std::uintptr_t>(p));
        if (cls.empty()) continue;

        if (offset < 0x1600 &&
            cls.find("Fire") == std::string::npos && cls.find("Projectile") == std::string::npos &&
            cls.find("Item") == std::string::npos && cls.find("Attack") == std::string::npos) {
            continue;
        }

        std::string outerNote;
        {
            const std::uint64_t outer1 = read_u64(static_cast<std::uintptr_t>(p) + 0x20);
            if (outer1 >= 0x10000) {
                outerNote = " [outer=" + object_class_name(static_cast<std::uintptr_t>(outer1)) + " '" +
                            name_of_object(static_cast<std::uintptr_t>(outer1)) + "'";
                const std::uint64_t outer2 = read_u64(static_cast<std::uintptr_t>(outer1) + 0x20);
                if (outer2 >= 0x10000) {
                    outerNote += " < " + object_class_name(static_cast<std::uintptr_t>(outer2));
                }
                outerNote += "]";
            }
        }
        std::snprintf(buf, sizeof(buf), " %04llX -> %s '%s'%s",
                      static_cast<unsigned long long>(offset), cls.c_str(),
                      name_of_object(static_cast<std::uintptr_t>(p)).c_str(), outerNote.c_str());
        out.push_back(buf);
        if (++found >= 24) {
            out.push_back(" ... (stopped at 24)");
            break;
        }
    }
    if (found == 0) out.push_back(" (no weapon-like pointer found in..)");
    return out;
}
std::uintptr_t asc_offset_cached = 0x0860;
char g_ascNote[320] = {0};

std::uintptr_t ability_system_of_actor(std::uintptr_t actor, bool* usedBoundedScan, std::string* note) {
    if (usedBoundedScan != nullptr) *usedBoundedScan = false;
    if (actor < 0x10000) {
        if (note != nullptr) *note = "no actor";
        return 0;
    }
    const std::uint64_t candidate = read_u64(actor + asc_offset_cached);
    if (candidate >= 0x10000) {
        const std::string cls = object_class_name(static_cast<std::uintptr_t>(candidate));
        if (cls.find("AbilitySystemComponent") != std::string::npos) {
            return static_cast<std::uintptr_t>(candidate);
        }
    }
    for (std::uintptr_t offset = 0x0200; offset < 0x1400; offset += 8) {
        const std::uint64_t p = read_u64(actor + offset);
        if (p < 0x10000) continue;
        const std::string cls = object_class_name(static_cast<std::uintptr_t>(p));
        if (cls.find("AbilitySystemComponent") == std::string::npos) continue;
        if (offset != asc_offset_cached) asc_offset_cached = offset;
        if (usedBoundedScan != nullptr) *usedBoundedScan = true;
        if (note != nullptr) {
            *note = "at 0x" + std::to_string(offset) + " (not the player-measured; now cached)";
        }
        return static_cast<std::uintptr_t>(p);
    }
    if (note != nullptr) *note = "nothing in.. holds an ability system";
    return 0;
}
void collect_ability_systems(const EnemySurvey& survey,
                             std::unordered_map<std::uintptr_t, std::uintptr_t>& byOuter,
                             std::string& note) {

    note.clear();
    std::uint32_t resolved = 0;
    std::uint32_t bounded = 0;
    std::string boundedAt;
    const LiveObjects live = find_live_objects();
    if (live.pawn >= 0x10000) {
        bool usedScan = false;
        std::string one;
        const std::uintptr_t asc = ability_system_of_actor(live.pawn, &usedScan, &one);
        if (asc != 0) { byOuter[live.pawn] = asc; ++resolved; }
        if (usedScan) { ++bounded; if (boundedAt.empty()) boundedAt = "local pawn: " + one; }
    }
    for (const EnemyRecord& e : survey.enemies) {
        if (e.actor < 0x10000) continue;
        bool usedScan = false;
        std::string one;
        const std::uintptr_t asc = ability_system_of_actor(e.actor, &usedScan, &one);
        if (asc != 0) { byOuter[e.actor] = asc; ++resolved; }
        if (usedScan) { ++bounded; if (boundedAt.empty()) boundedAt = "'" + e.actorName + "': " + one; }
    }
    std::snprintf(g_ascNote, sizeof(g_ascNote),
                  "resolved %u ability system(s) WITHOUT the object-table walk (%u needed the bounded "
                  "per-actor scan%s%s)", resolved, bounded,
                  boundedAt.empty() ? "" : "; first one was ", boundedAt.c_str());
    note = g_ascNote;
}

std::unordered_map<std::uintptr_t, std::vector<float>>& hp_snapshots() {
    static std::unordered_map<std::uintptr_t, std::vector<float>> snapshots;
    return snapshots;
}

std::string hp_change_note(std::uintptr_t setAddress, const float* now, int count) {
    auto& snapshots = hp_snapshots();
    std::string note;
    char buf[96];
    const auto it = snapshots.find(setAddress);
    if (it == snapshots.end()) {
        note = "FIRST reading of this set -- there is no previous snapshot, so nothing can be said about "
               "what moved. Run the command again after taking a hit: the offsets that changed name "
               "themselves, which is how the SHIELD will be found without reading it off the screen.";
    } else {
        for (int record = 0; record * 4 + 3 < count; ++record) {
            const int valueIndex = record * 4 + 2;
            const float before = it->second[static_cast<std::size_t>(valueIndex)];
            if (before == now[valueIndex]) continue;
            std::snprintf(buf, sizeof(buf), "%02X: %.3f -> %.3f; ", 0x30 + record * 4 + 8,
                          static_cast<double>(before), static_cast<double>(now[valueIndex]));
            note += buf;
        }
        if (note.empty()) note = "NOTHING in.. moved since the previous reading";
    }
    snapshots[setAddress] = std::vector<float>(now, now + count);
    return note;
}

}

HpSurvey survey_hp(bool localPlayerToo) {
    HpSurvey s;
    const EnemySurvey survey = survey_enemies();
    if (!survey.worldFound) {
        s.worldNote = "the live world is not known, so no level was walked: " + survey.worldNote;
        return s;
    }
    s.worldFound = true;
    s.enemiesListed = static_cast<std::uint32_t>(survey.enemies.size());

    std::unordered_map<std::uintptr_t, std::uintptr_t> ascByOuter;
    std::string walkNote;
    collect_ability_systems(survey, ascByOuter, walkNote);

    if (localPlayerToo) {
        const LiveObjects live = find_live_objects();
        if (live.pawn < 0x10000) {
            s.localNote = "there is no local pawn in this process, so the calibration half cannot be read";
        } else {
            s.localClass = object_class_name(live.pawn) + " '" + name_of_object(live.pawn) + "'";
            const auto it = ascByOuter.find(live.pawn);
            if (it == ascByOuter.end()) {
                s.localNote = "the local pawn has no object whose class name contains "
                              "'AbilitySystemComponent' as its Outer";
            } else {
                s.localFound = true;
                s.localAsc = it->second;
                ArrayHeader sets;
                if (!read_tarray(s.localAsc + 0x0140, 4096, sets)) {
                    s.localNote = "its SpawnedAttributes could not be read as a TArray";
                } else {
                    for (std::uint32_t i = 0; i < sets.num && i < 8; ++i) {
                        std::uint64_t setPtr = 0;
                        if (!try_read_u64(sets.data + static_cast<std::uintptr_t>(i) * 8, setPtr) ||
                            setPtr < 0x10000) {
                            continue;
                        }
                        HpAttributeSetRead set;
                        set.address = static_cast<std::uintptr_t>(setPtr);
                        set.cls = object_class_name(set.address);
                        set.name = name_of_object(set.address);
                        set.narrowRead = try_read_bytes(set.address + 0x0030, set.narrow,
                                                        sizeof(set.narrow));
                        set.wideRead = try_read_bytes(set.address, set.wide, sizeof(set.wide));
                        if (set.narrowRead) {
                            set.changeNote = hp_change_note(set.address, set.narrow,
                                                            HpAttributeSetRead::kEnemyFloats);
                        }
                        s.localSets.push_back(std::move(set));
                    }
                    if (s.localSets.empty()) s.localNote = "it owns no attribute set at all";
                }
            }
        }
    }

    for (const EnemyRecord& e : survey.enemies) {
        if (e.actorName.empty()) continue;

        if (!npc_exclusion_reason(e.actorClass, e.controllerClass).empty()) {
            ++s.npcSkipped;
            continue;
        }
        HpEnemyRead out;
        out.enemyName = e.actorName;
        out.enemyClass = e.actorClass;
        const auto it = ascByOuter.find(e.actor);
        if (it == ascByOuter.end()) {
            out.note = "no object whose class name contains 'AbilitySystemComponent' has this character as "
                       "its Outer, so there is no attribute set to read";
            s.enemies.push_back(std::move(out));
            continue;
        }
        out.asc = it->second;
        out.ascClass = object_class_name(out.asc);
        ++s.withAbilitySystem;

        ArrayHeader sets;
        if (!read_tarray(out.asc + 0x0140, 4096, sets)) {
            out.note = "the ability system's SpawnedAttributes could not be read as a TArray";
            s.enemies.push_back(std::move(out));
            continue;
        }
        for (std::uint32_t i = 0; i < sets.num && i < 8; ++i) {
            std::uint64_t setPtr = 0;
            if (!try_read_u64(sets.data + static_cast<std::uintptr_t>(i) * 8, setPtr) || setPtr < 0x10000) {
                continue;
            }
            HpAttributeSetRead set;
            set.address = static_cast<std::uintptr_t>(setPtr);
            set.cls = object_class_name(set.address);
            set.name = name_of_object(set.address);
            set.narrowRead = try_read_bytes(set.address + 0x0030, set.narrow, sizeof(set.narrow));
            out.sets.push_back(std::move(set));
        }
        if (out.sets.empty()) out.note = "the ability system owns no attribute set at all";
        s.enemies.push_back(std::move(out));
    }
    if (!walkNote.empty()) s.worldNote = walkNote;
    return s;
}

std::vector<std::string> HpSurvey::report_lines() const {
    std::vector<std::string> out;
    char buf[1024];
    if (!worldFound) {
        out.push_back("hp: " + worldNote);
        return out;
    }

    if (localFound || !localNote.empty() || !localClass.empty()) {
        out.push_back("hp: THE LOCAL PLAYER's own attribute sets, printed so the window can be CALIBRATED: "
                      "your health is on screen, so the number that matches it (or that drops when you are "
                      "hit) is the live one. Nothing here is guessed.");
        if (!localClass.empty()) out.push_back(" local pawn: " + localClass);
        if (!localNote.empty()) out.push_back(" local: " + localNote);
        for (const HpAttributeSetRead& set : localSets) {
            std::snprintf(buf, sizeof(buf), " attribute set '%s' (%s) @0x%llX:", set.name.c_str(),
                          set.cls.c_str(), static_cast<unsigned long long>(set.address));
            out.push_back(buf);
            if (!set.wideRead) {
                out.push_back(" the wide window (..) could NOT be read");
            } else {
                for (int row = 0; row < HpAttributeSetRead::kWideFloats; row += 4) {
                    std::snprintf(buf, sizeof(buf),
                                  " %02X: %12.3f %12.3f %12.3f %12.3f", 4 * row,
                                  static_cast<double>(set.wide[row]),
                                  static_cast<double>(set.wide[row + 1]),
                                  static_cast<double>(set.wide[row + 2]),
                                  static_cast<double>(set.wide[row + 3]));
                    out.push_back(buf);
                }
            }
            if (!set.changeNote.empty()) {
                out.push_back(" WHAT MOVED SINCE THE PREVIOUS RUN: " + set.changeNote);
            }
        }
    }

    std::snprintf(buf, sizeof(buf),
                  "hp: %u AI-driven character(s), %u NPC(s) skipped and never read; an ability system was "
                  "found on %u of them. The eight floats per set are.. -- the two candidate "
                  "fields (Health, MaxHealth) and their neighbours. SHOOT ONE AND SEE WHICH NUMBER MOVES: "
                  "the game's offsets printed byte-identical blocks, so the window is what identifies it.",
                  enemiesListed, npcSkipped, withAbilitySystem);
    out.push_back(buf);
    if (!worldNote.empty()) out.push_back("hp: " + worldNote);

    for (const HpEnemyRead& e : enemies) {
        std::string line = " '" + e.enemyName + "' (" + e.enemyClass + ")";
        if (e.asc == 0) {
            line += " -- " + (e.note.empty() ? std::string("no ability system found") : e.note);
            out.push_back(line);
            continue;
        }
        std::snprintf(buf, sizeof(buf), " -- ability system %s @0x%llX", e.ascClass.c_str(),
                      static_cast<unsigned long long>(e.asc));
        line += buf;
        if (!e.note.empty()) line += " (" + e.note + ")";
        out.push_back(line);

        for (const HpAttributeSetRead& set : e.sets) {
            if (!set.narrowRead) {
                std::snprintf(buf, sizeof(buf), " attribute set '%s' (%s):.. NOT READ",
                              set.name.c_str(), set.cls.c_str());
                out.push_back(buf);
                continue;
            }

            std::string line = " attribute set '" + set.name + "' (" + set.cls + "): ";
            for (int record = 0; record * 4 < HpAttributeSetRead::kEnemyFloats; ++record) {
                const int base = record * 4;
                if (base + 3 >= HpAttributeSetRead::kEnemyFloats) break;
                char one[64];
                std::snprintf(one, sizeof(one), "%02X:%12.3f ", 0x30 + base,
                              static_cast<double>(set.narrow[base + 2]));
                line += one;
            }
            out.push_back(line);
        }
    }
    return out;
}

}

namespace dsh::reflect {

namespace {
std::vector<HpWatchTarget> g_hpWatchTargets;

float hp_read_field(std::uintptr_t set, std::uintptr_t offset, bool& readOk) {
    float value = 0.0f;
    readOk = try_read_bytes(set + offset, &value, sizeof(value));
    return value;
}
}

const std::vector<HpWatchTarget>& hp_watch_targets() { return g_hpWatchTargets; }

void hp_watch_forget_targets() {
    g_hpWatchTargets.clear();
}

void hp_watch_refresh_targets(bool includeNpcClassified) {

    DSH_TIME_WALK("health:refresh-targets");
    g_hpWatchTargets.clear();

    const EnemySurvey survey = survey_enemies();
    const LiveObjects live = find_live_objects();
    std::unordered_map<std::uintptr_t, std::uintptr_t> ascByOuter;
    std::string walkNote;
    collect_ability_systems(survey, ascByOuter, walkNote);

    if (live.pawn >= 0x10000) {
        const auto it = ascByOuter.find(live.pawn);
        if (it != ascByOuter.end()) {
            ArrayHeader sets;
            if (read_tarray(it->second + 0x0140, 4096, sets) && sets.num > 0) {
                std::uint64_t setPtr = 0;
                if (try_read_u64(sets.data, setPtr) && setPtr >= 0x10000) {
                    HpWatchTarget t;
                    t.actor = live.pawn;
                    t.set = static_cast<std::uintptr_t>(setPtr);
                    t.name = name_of_object(live.pawn);
                    t.cls = object_class_name(live.pawn);
                    t.isLocal = true;
                    g_hpWatchTargets.push_back(std::move(t));
                }
            }
        }
    }

    for (const EnemyRecord& e : survey.enemies) {
        if (e.actorName.empty()) continue;
        const bool npc = !npc_exclusion_reason(e.actorClass, e.controllerClass).empty();
        if (npc && !includeNpcClassified) continue;
        const auto it = ascByOuter.find(e.actor);
        if (it == ascByOuter.end()) continue;
        ArrayHeader sets;
        if (!read_tarray(it->second + 0x0140, 4096, sets) || sets.num == 0) continue;
        std::uint64_t setPtr = 0;
        if (!try_read_u64(sets.data, setPtr) || setPtr < 0x10000) continue;
        HpWatchTarget t;
        t.actor = e.actor;
        t.set = static_cast<std::uintptr_t>(setPtr);
        t.name = e.actorName;
        t.cls = e.actorClass;
        t.npcClassified = npc;
        g_hpWatchTargets.push_back(std::move(t));
    }
}

HpWatchFrame hp_watch_sample() {
    HpWatchFrame frame;
    if (g_hpWatchTargets.empty()) {
        frame.note = "the target list is empty -- nothing has been resolved yet";
        return frame;
    }
    frame.ok = true;
    for (const HpWatchTarget& t : g_hpWatchTargets) {
        HpWatchSample s;
        s.target = t;
        s.health = hp_read_field(t.set, 0x0048, s.healthRead);
        s.shield = hp_read_field(t.set, 0x00D8, s.shieldRead);
        bool ignoredRead = false;
        s.maxHealth = hp_read_field(t.set, 0x0038, ignoredRead);
        s.maxShield = hp_read_field(t.set, 0x00C8, ignoredRead);
        frame.samples.push_back(std::move(s));
    }
    return frame;
}

}

namespace dsh::reflect {
bool hp_notify_health_changed(std::uintptr_t set, float oldValue, std::string& note);

bool hp_write_health(std::uintptr_t set, float value, float* readBackOut, std::string& note) {
    note.clear();
    if (readBackOut != nullptr) *readBackOut = 0.0f;
    if (set < 0x10000) {
        note = "no attribute set to write into";
        return false;
    }
    if (!object_is_live(set, note)) {
        note = "the attribute set is not live (" + note + ") -- nothing was written";
        return false;
    }
    if (!std::isfinite(value)) {
        note = "refusing to write a non-finite health";
        return false;
    }

    if (!write_process_bytes(set + 0x0048, &value, sizeof(value))) {
        note = "write_process_bytes refused the range at";
        return false;
    }
    if (!write_process_bytes(set + 0x004C, &value, sizeof(value))) {
        note = "the value at was written but the duplicate at was refused";
        return false;
    }
    float back = 0.0f;
    if (!try_read_bytes(set + 0x0048, &back, sizeof(back))) {
        note = "the write was issued but the read-back failed";
        return false;
    }
    if (readBackOut != nullptr) *readBackOut = back;
    if (back != value) {
        note = "the write was issued but the read-back returned " + std::to_string(back) + " instead of " +
               std::to_string(value) + " -- something else wrote it";
        return false;
    }

    return true;
}}

namespace dsh::reflect {

namespace {
constexpr std::uintptr_t kCharacterAttributeSetOffset = 0x0948;

constexpr std::uintptr_t kAttributeHealthOffset      = 0x0048;
constexpr std::uintptr_t kAttributeMaxHealthOffset   = 0x0038;
constexpr std::uintptr_t kAttributeShieldOffset      = 0x00D8;
constexpr std::uintptr_t kAttributeMaxShieldOffset   = 0x00C8;
constexpr std::uintptr_t kAttributeEnergyOffset      = 0x0088;
constexpr std::uintptr_t kAttributeMaxEnergyOffset   = 0x0078;
}

bool character_attribute_set(std::uintptr_t actor, std::uintptr_t& setOut, std::string& note) {
    setOut = 0;
    note.clear();
    if (actor < 0x10000) {
        note = "no character was given";
        return false;
    }
    std::uint64_t raw = 0;
    if (!try_read_u64(actor + kCharacterAttributeSetOffset, raw)) {
        note = "the character's CharacterAttributeSet could not be read";
        return false;
    }
    if (raw < 0x10000) {
        note = "the character's CharacterAttributeSet is NULL -- the class has not built it, so "
               "there is no health to read or write on this actor";
        return false;
    }
    const auto set = static_cast<std::uintptr_t>(raw);
    const std::string cls = object_class_name(set);
    if (cls != "DACharacterAttributeSet") {
        note = "the pointer at points at '" + (cls.empty() ? std::string("<unnamed>") : cls) +
               "', not a DACharacterAttributeSet -- is not that field on this build, so nothing is "
               "read from it";
        return false;
    }
    setOut = set;
    return true;
}

CharacterVitals character_vitals(std::uintptr_t actor) {
    CharacterVitals out;
    if (!character_attribute_set(actor, out.set, out.note)) return out;
    out.classNote = object_class_name(out.set);
    float health = 0.0f, maxHealth = 0.0f, shield = 0.0f, maxShield = 0.0f;
    if (!try_read_bytes(out.set + kAttributeHealthOffset, &health, sizeof(health)) ||
        !try_read_bytes(out.set + kAttributeMaxHealthOffset, &maxHealth, sizeof(maxHealth)) ||
        !try_read_bytes(out.set + kAttributeShieldOffset, &shield, sizeof(shield)) ||
        !try_read_bytes(out.set + kAttributeMaxShieldOffset, &maxShield, sizeof(maxShield))) {
        out.note = "the attribute set is live but one of/// refused to read";
        return out;
    }
    out.health = health;
    out.maxHealth = maxHealth;
    out.shield = shield;
    out.maxShield = maxShield;
    out.read = true;
    return out;
}

CharacterEnergy character_energy(std::uintptr_t actor) {
    CharacterEnergy out;
    if (!character_attribute_set(actor, out.set, out.note)) return out;
    float energy = 0.0f, maxEnergy = 0.0f;
    if (!try_read_bytes(out.set + kAttributeEnergyOffset, &energy, sizeof(energy)) ||
        !try_read_bytes(out.set + kAttributeMaxEnergyOffset, &maxEnergy, sizeof(maxEnergy))) {
        out.note = "the attribute set is live but/ refused to read";
        return out;
    }
    out.energy = energy;
    out.maxEnergy = maxEnergy;
    out.read = true;
    return out;
}
}

namespace dsh::reflect {

bool hp_notify_health_changed(std::uintptr_t set, float oldValue, std::string& note) {
    note.clear();
    if (set < 0x10000) {
        note = "no attribute set";
        return false;
    }
    const std::uintptr_t setClass = read_u64(set + kUObjectClassOffset);
    if (setClass < 0x10000) {
        note = "the attribute set's class could not be read";
        return false;
    }
    const FunctionRef fn = reflect_resolve_function(setClass, "OnRep_Health");
    if (!fn.usable) {
        note = "the set's class does not declare a usable OnRep_Health";
        return false;
    }

    unsigned char frame[0x10] = {0};
    std::memcpy(frame + 0x00, &oldValue, sizeof(oldValue));
    std::memcpy(frame + 0x08, &oldValue, sizeof(oldValue));
    if (!reflect_invoke_function(set, fn, frame, note)) {
        note = "OnRep_Health could not be invoked: " + note;
        return false;
    }
    return true;
}
}

namespace dsh::reflect {

std::vector<std::string> survey_owned_objects(std::uintptr_t actor, const std::string& label) {
    return impl_survey_owned_objects(actor, label);
}
}

namespace dsh::reflect {

std::vector<std::string> impl_survey_weapon_inner(std::uintptr_t weapon, const std::string& label) {
    std::vector<std::string> out;
    char buf[320];
    if (weapon < 0x10000) {
        out.push_back(label + ": no weapon");
        return out;
    }
    std::snprintf(buf, sizeof(buf), "%s '%s' (%s): what it points at", label.c_str(),
                  name_of_object(weapon).c_str(), object_class_name(weapon).c_str());
    out.push_back(buf);
    std::uint32_t shown = 0;
    for (std::uintptr_t offset = 0x0000; offset < 0x0800; offset += 8) {
        const std::uint64_t p = read_u64(weapon + offset);
        if (p < 0x10000) continue;
        const std::string cls = object_class_name(static_cast<std::uintptr_t>(p));
        if (cls.empty()) continue;
        std::string outer = "none";
        const std::uint64_t o1 = read_u64(static_cast<std::uintptr_t>(p) + 0x20);
        if (o1 >= 0x10000) {
            outer = object_class_name(static_cast<std::uintptr_t>(o1)) + " '" +
                    name_of_object(static_cast<std::uintptr_t>(o1)) + "'";
        }
        std::snprintf(buf, sizeof(buf), " ^%04llX -> %s '%s' [outer=%s]",
                      static_cast<unsigned long long>(offset), cls.c_str(),
                      name_of_object(static_cast<std::uintptr_t>(p)).c_str(), outer.c_str());
        out.push_back(buf);
        if (++shown >= 18) {
            out.push_back(" ... (stopped at 18)");
            break;
        }
    }
    if (shown == 0) out.push_back(" (nothing readable in..)");
    return out;
}
namespace dsh::reflect {
std::vector<std::string> survey_weapon_inner(std::uintptr_t weapon, const std::string& label) {
    return impl_survey_weapon_inner(weapon, label);
}
}
}
namespace dsh::reflect {

std::vector<std::string> survey_weapon_slot(std::uintptr_t actor, std::uintptr_t slotOffset,
                                            const std::string& label) {
    char head[96];
    std::snprintf(head, sizeof(head), "%s %04llX", label.c_str(),
                  static_cast<unsigned long long>(slotOffset));
    if (actor < 0x10000) return {std::string(head) + ": no actor"};
    const std::uint64_t bits = read_u64(actor + slotOffset);
    if (bits < 0x10000) return {std::string(head) + ": the slot holds nothing readable"};
    return impl_survey_weapon_inner(static_cast<std::uintptr_t>(bits), head);
}
}

namespace dsh::reflect {
namespace {

std::string join_names(const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) out += ", ";
        out += "'" + names[i] + "'";
    }
    return out;
}

const HpWatchTarget* resolve_probe_target(const std::string& wanted, std::vector<std::string>& matches,
                                          std::size_t& matched) {
    matches.clear();
    matched = 0;
    if (wanted.empty()) return nullptr;

    for (const HpWatchTarget& t : hp_watch_targets()) {
        if (t.set < 0x10000 || t.isLocal || t.npcClassified) continue;
        if (t.name == wanted) {
            matched = 1;
            return &t;
        }
    }
    const HpWatchTarget* only = nullptr;
    for (const HpWatchTarget& t : hp_watch_targets()) {
        if (t.set < 0x10000 || t.isLocal || t.npcClassified) continue;
        if (t.name.find(wanted) == std::string::npos) continue;
        ++matched;
        if (matches.size() < 8) matches.push_back(t.name);
        only = &t;
    }
    return matched == 1 ? only : nullptr;
}

const HpWatchTarget* resolve_nearest_target(std::string& why) {
    const std::uintptr_t actorClass = resolve_uclass_address("Actor");
    if (actorClass == 0) {
        why = "the Actor UClass is not in the object array";
        return nullptr;
    }
    const FunctionRef fnActor = reflect_resolve_function(actorClass, "K2_GetActorLocation");
    if (!fnActor.usable) {
        why = fnActor.note;
        return nullptr;
    }
    const LiveObjects live = find_live_objects();
    if (live.pawn < 0x10000) {
        why = "the local pawn is not known";
        return nullptr;
    }
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    std::string note;
    if (!read_actor_location(live.pawn, fnActor, px, py, pz, note)) {
        why = "the pawn's own position could not be read: " + note;
        return nullptr;
    }
    const HpWatchTarget* best = nullptr;
    float bestDistance = 0.0f;
    for (const HpWatchTarget& t : hp_watch_targets()) {
        if (t.set < 0x10000 || t.isLocal || t.npcClassified) continue;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (!read_actor_location(t.actor, fnActor, x, y, z, note)) continue;
        const float dx = x - px;
        const float dy = y - py;
        const float dz = z - pz;
        const float d = dx * dx + dy * dy + dz * dz;
        if (best == nullptr || d < bestDistance) {
            best = &t;
            bestDistance = d;
        }
    }
    if (best == nullptr) why = "no candidate's position could be read";
    return best;
}

const HpWatchTarget* resolve_any_target(const std::string& namePart, std::vector<std::string>& matches,
                                        std::size_t& matched, std::string& why) {
    if (namePart == "near" || namePart == "nearest" || namePart == "nearestenemy") {
        matched = 0;
        const HpWatchTarget* t = resolve_nearest_target(why);
        if (t != nullptr) matched = 1;
        return t;
    }
    return resolve_probe_target(namePart, matches, matched);
}

}

HpDamageProbe hp_probe_apply_damage(const std::string& namePart, float amount) {
    HpDamageProbe out;
    if (!std::isfinite(amount) || amount <= 0.0f) {
        out.note = "the amount must be a positive, finite number";
        return out;
    }
    if (!game_thread_hook_installed()) {
        out.note = "the game-thread hook is not installed -- run 'gthook' first, because ProcessEvent "
                   "belongs to the game thread and an off-thread call is how the client died on "
                   "2026-10-17";
        return out;
    }
    if (hp_watch_targets().empty()) hp_watch_refresh_targets(false);
    std::vector<std::string> matches;
    std::size_t matched = 0;
    std::string why;
    const HpWatchTarget* target = resolve_any_target(namePart, matches, matched, why);
    if (target == nullptr) {
        out.note = matched == 0
                       ? ("nothing matched '" + namePart + "'" + (why.empty() ? "" : " -- " + why))
                       : ("'" + namePart + "' matches " + std::to_string(matched) +
                          " enemies, so it names none of them; narrow it: " + join_names(matches));
        return out;
    }
    out.resolved = true;
    out.name = target->name;
    out.actorClass = object_class_name(target->actor);
    float before = 0.0f;
    out.readBefore = try_read_bytes(target->set + 0x0048, &before, sizeof(before));
    out.before = before;

    const StaticFunctionRef fn = reflect_resolve_static("DAGameplayStatics", "ApplyDADamage");
    if (!fn.usable) {
        out.note = fn.note;
        return out;
    }

    alignas(16) unsigned char frame[0x40] = {};
    const std::uintptr_t actor = target->actor;
    std::memcpy(frame + 0x00, &actor, sizeof(actor));
    const float params[5] = {amount, 0.0f, amount, 0.0f, 0.0f};
    std::memcpy(frame + 0x08, params, sizeof(params));

    bool called = false;
    std::string callNote;
    const bool ran = run_on_game_thread(
        [&]() { called = reflect_invoke_function(fn.cdo, fn.fn, frame, callNote); }, 500);
    out.callMade = called;
    if (!ran) {
        out.note = "the game thread did not run the call within 500 ms";
        return out;
    }
    if (!called) {
        out.note = "the function was NOT called: " + callNote;
        return out;
    }
    std::uint8_t effective = 0;
    std::memcpy(&effective, frame + 0x38, sizeof(effective));
    out.effective = effective != 0;
    std::memcpy(&out.returned, frame + 0x3C, sizeof(out.returned));
    float after = 0.0f;
    out.readAfter = try_read_bytes(target->set + 0x0048, &after, sizeof(after));
    out.after = after;
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "%s; the game answered bEffective=%s and ReturnValue=%.3f; health %.1f -> %.1f",
                  fn.note.c_str(), out.effective ? "true" : "false",
                  static_cast<double>(out.returned), static_cast<double>(out.before),
                  static_cast<double>(out.after));
    out.note = buf;
    return out;
}

HpNotifyProbe hp_probe_set_and_notify(const std::string& namePart, float value) {    HpNotifyProbe out;
    if (!game_thread_hook_installed()) {
        out.note = "the game-thread hook is not installed -- run 'gthook' first: the write itself is "
                   "safe anywhere, but the notify is a ProcessEvent and belongs to the game thread";
        return out;
    }
    if (hp_watch_targets().empty()) hp_watch_refresh_targets(false);
    std::vector<std::string> matches;
    std::size_t matched = 0;
    std::string why;
    const HpWatchTarget* target = resolve_any_target(namePart, matches, matched, why);
    if (target == nullptr) {
        out.note = matched == 0
                       ? ("nothing matched '" + namePart + "'" + (why.empty() ? "" : " -- " + why))
                       : ("'" + namePart + "' matches " + std::to_string(matched) +
                          " enemies, so it names none of them; narrow it: " + join_names(matches));
        return out;
    }
    out.resolved = true;
    out.name = target->name;
    float before = 0.0f;
    if (!try_read_bytes(target->set + 0x0048, &before, sizeof(before))) {
        out.note = "the health field at could not be read";
        return out;
    }
    out.before = before;
    const float wanted = value > 0.0f ? value : before * 0.7f;
    float back = 0.0f;
    std::string writeNote;
    out.writeLanded = hp_write_health(target->set, wanted, &back, writeNote);
    out.wrote = back;
    if (!out.writeLanded) {
        out.note = "the write itself did not land: " + writeNote;
        return out;
    }

    std::string notifyNote;
    bool notified = false;
    const std::uintptr_t set = target->set;
    const float oldValue = before;
    const bool ran = run_on_game_thread(
        [&]() { notified = hp_notify_health_changed(set, oldValue, notifyNote); }, 500);
    out.notified = notified;
    if (!ran) {
        out.note = "the game thread did not run the notify within 500 ms";
        return out;
    }
    float after = 0.0f;
    out.readAfter = try_read_bytes(target->set + 0x0048, &after, sizeof(after));
    out.after = after;
    char buf[288];
    std::snprintf(buf, sizeof(buf), "wrote %.1f over %.1f (read back %.1f); OnRep_Health %s%s",
                  static_cast<double>(wanted), static_cast<double>(before),
                  static_cast<double>(back), notified ? "was CALLED" : "was NOT called",
                  notified ? "" : (" -- " + notifyNote).c_str());
    out.note = buf;
    return out;
}

std::vector<std::string> hp_probe_near_list(std::size_t maxLines) {    std::vector<std::string> out;
    if (maxLines == 0) maxLines = 8;

    hp_watch_refresh_targets(false);

    FunctionRef fnActor;
    {
        const std::uintptr_t actorClass = resolve_uclass_address("Actor");
        if (actorClass != 0) {
            fnActor = reflect_resolve_function(actorClass, "K2_GetActorLocation");
        } else {
            fnActor.note = "the Actor UClass is not in the object array";
        }
    }

    const LiveObjects live = find_live_objects();
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    bool pawnOk = false;
    std::string pawnNote = "the local pawn is not known";
    if (live.pawn >= 0x10000) {
        pawnOk = read_actor_location(live.pawn, fnActor, px, py, pz, pawnNote);
    }

    struct Row {
        bool        distOk = false;
        float       distM = 0.0f;
        std::string name;
        std::string cls;
        float       health = 0.0f;
        float       maxHealth = 0.0f;
        bool        healthOk = false;
    };
    std::vector<Row> rows;
    for (const HpWatchTarget& t : hp_watch_targets()) {
        if (t.set < 0x10000 || t.isLocal || t.npcClassified) continue;
        Row r;
        r.name = t.name;
        r.cls = t.cls;
        r.health = hp_read_field(t.set, 0x0048, r.healthOk);
        bool maxOk = false;
        r.maxHealth = hp_read_field(t.set, 0x0038, maxOk);
        if (pawnOk && fnActor.usable) {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            std::string note;
            if (read_actor_location(t.actor, fnActor, x, y, z, note)) {
                const float dx = x - px;
                const float dy = y - py;
                const float dz = z - pz;
                r.distM = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.01f;
                r.distOk = true;
            }
        }
        rows.push_back(std::move(r));
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.distOk != b.distOk) return a.distOk;
        if (a.distOk && b.distOk) return a.distM < b.distM;
        return a.name < b.name;
    });

    const EnemySurvey walk = survey_enemies();
    char buf[768];
    std::snprintf(buf, sizeof(buf),
                  "near: %zu candidate(s) from a cache of %zu target(s); the level walk saw %u level(s), "
                  "%llu actor(s) and %llu pawn(s) and accepted %zu as AI-driven (baseClass=0x%llX). The local "
                  "player and NPC-classified characters are never candidates%s",
                  rows.size(), hp_watch_targets().size(), walk.levelsWalked,
                  static_cast<unsigned long long>(walk.actorsWalked),
                  static_cast<unsigned long long>(walk.pawnsWalked), walk.enemies.size(),
                  static_cast<unsigned long long>(walk.baseClass),
                  pawnOk ? "." : "; the pawn's position is unavailable, so the order is by name.");
    out.push_back(buf);
    if (!walk.worldNote.empty()) out.push_back("near: the walk's own note: " + walk.worldNote);
    if (!pawnOk) out.push_back("near: why the distance is missing: " + pawnNote);
    if (rows.empty() && !walk.candidateClasses.empty()) {
        std::string listed;
        for (std::size_t i = 0; i < walk.candidateClasses.size() && i < 10; ++i) {
            if (!listed.empty()) listed += ", ";
            listed += walk.candidateClasses[i].first + " x" + std::to_string(walk.candidateClasses[i].second);
        }
        out.push_back("near: enemy-looking classes the walk SAW but did NOT accept: " + listed);
    }
    for (std::size_t i = 0; i < rows.size() && i < maxLines; ++i) {
        const Row& r = rows[i];
        char line[640];
        if (r.distOk) {
            std::snprintf(line, sizeof(line), "near: %2zu. %7.1f m '%s' [%s] health %.1f / %.1f", i + 1,
                          static_cast<double>(r.distM), r.name.c_str(), r.cls.c_str(),
                          static_cast<double>(r.health), static_cast<double>(r.maxHealth));
        } else {
            std::snprintf(line, sizeof(line), "near: %2zu. ? m '%s' [%s] health %.1f / %.1f", i + 1,
                          r.name.c_str(), r.cls.c_str(), static_cast<double>(r.health),
                          static_cast<double>(r.maxHealth));
        }
        out.push_back(line);
    }
    if (rows.size() > maxLines) {
        out.push_back("near: ... and " + std::to_string(rows.size() - maxLines) + " more, further away.");
    }
    return out;
}

void hp_apply_lethal_damage_on_game_thread(std::uintptr_t set, std::uintptr_t actor, const std::string& name) {
    std::string liveNote;
    if (actor < 0x10000 || set < 0x10000) {
        DSH_WARN("health: '%s' has no actor/set pair to kill, so no death was applied", name.c_str());
        return;
    }
    if (!object_is_live(actor, liveNote) || !object_is_live(set, liveNote)) {
        DSH_WARN("health: '%s' is not live (%s), so no death was applied -- it was already gone",
                 name.c_str(), liveNote.c_str());
        return;
    }
    float before = 0.0f;
    const bool beforeRead = try_read_bytes(set + 0x0048, &before, sizeof(before));
    const float request = (beforeRead && before > 1.0f ? before : 1.0f) * 4.0f + 1000000.0f;

    const StaticFunctionRef fn = reflect_resolve_static("DAGameplayStatics", "ApplyDADamage");
    if (!fn.usable) {
        DSH_WARN("health: '%s' could NOT be killed: %s", name.c_str(), fn.note.c_str());
        return;
    }
    alignas(16) unsigned char frame[0x40] = {};
    std::memcpy(frame + 0x00, &actor, sizeof(actor));
    const float params[5] = {request, 0.0f, request, 0.0f, 0.0f};
    std::memcpy(frame + 0x08, params, sizeof(params));
    std::string callNote;
    if (!reflect_invoke_function(fn.cdo, fn.fn, frame, callNote)) {
        DSH_WARN("health: '%s' was NOT killed -- the call was not made: %s", name.c_str(), callNote.c_str());
        return;
    }
    float after = 0.0f;
    const bool afterRead = try_read_bytes(set + 0x0048, &after, sizeof(after));
    DSH_INFO("health: '%s' was killed through the game's own damage call: health %.1f -> %.1f (asked %.0f)%s. "
             "The death, the animation and the drops are the game's own",
             name.c_str(), static_cast<double>(before), static_cast<double>(after),
             static_cast<double>(request),
             afterRead ? "" : " -- the read-back failed, so that number is not a fact");
}
}
namespace dsh::reflect {

BossMoveResult move_ai_character_near_player(const std::string& namePart, float distanceCm) {
    BossMoveResult out;
    if (!std::isfinite(distanceCm) || distanceCm < 100.0f || distanceCm > 200000.0f) {
        out.note = "the distance must be between 100 and 200000 centimetres";
        return out;
    }
    if (hp_watch_targets().empty()) hp_watch_refresh_targets(false);
    std::vector<std::string> matches;
    std::size_t matched = 0;
    std::string why;
    const HpWatchTarget* target = resolve_any_target(namePart, matches, matched, why);
    if (target == nullptr) {
        out.note = matched == 0 ? ("nothing matched '" + namePart + "'" + (why.empty() ? "" : " -- " + why))
                                : ("'" + namePart + "' matches " + std::to_string(matched) + " characters");
        return out;
    }
    if (target->name.find("NPC") != std::string::npos) {
        out.note = "'" + target->name + "' is an NPC, and NPCs are not touched";
        return out;
    }
    const LiveObjects live = find_live_objects();
    if (live.pawn < 0x10000) { out.note = "the local pawn is not known"; return out; }
    const std::uintptr_t actorClass = resolve_uclass_address("Actor");
    const FunctionRef fn = actorClass != 0 ? reflect_resolve_function(actorClass, "K2_GetActorLocation")
                                           : FunctionRef{};
    if (!fn.usable) { out.note = "K2_GetActorLocation is not usable: " + fn.note; return out; }
    float px = 0.0f, py = 0.0f, pz = 0.0f, bx = 0.0f, by = 0.0f, bz = 0.0f;
    std::string note;
    if (!read_actor_location(live.pawn, fn, px, py, pz, note)) { out.note = "the pawn's position: " + note; return out; }
    out.hadBefore = read_actor_location(target->actor, fn, bx, by, bz, note);
    out.beforeX = bx; out.beforeY = by; out.beforeZ = bz;

    const float posM[3] = {(px + distanceCm) * 0.01f, py * 0.01f, (pz + 30.0f) * 0.01f};
    out.name = target->name;
    out.applyOk = puppet_apply_actor_pose(target->actor, posM, 0.0f, 0.0f);
    out.hadAfter = read_actor_location(target->actor, fn, bx, by, bz, note);
    out.afterX = bx; out.afterY = by; out.afterZ = bz;
    const float dx = out.afterX - px, dy = out.afterY - py, dz = out.afterZ - pz;
    out.distanceToPawnCm = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!out.applyOk) out.note = "the pose writer refused; the character did not move";
    return out;
}

AiBootResult boot_one_ai_character(const std::string& namePart) {
    AiBootResult out;
    if (hp_watch_targets().empty()) hp_watch_refresh_targets(false);
    std::vector<std::string> matches;
    std::size_t matched = 0;
    std::string why;
    const HpWatchTarget* target = resolve_any_target(namePart, matches, matched, why);
    if (target == nullptr) {
        out.note = matched == 0 ? ("nothing matched '" + namePart + "'" + (why.empty() ? "" : " -- " + why))
                                : ("'" + namePart + "' matches " + std::to_string(matched) + " characters");
        return out;
    }
    if (target->name.find("NPC") != std::string::npos) {
        out.note = "'" + target->name + "' is an NPC, and NPCs are not touched";
        return out;
    }
    out.attempted = true;
    std::uint64_t controller = 0;
    if (!try_read_u64(target->actor + 0x0258, controller) || controller < 0x10000) {
        out.note = "'" + target->name + "' has NO controller at, so BootAI has nothing to be called on";
        return out;
    }
    const std::uintptr_t aiClass = resolve_uclass_address("DAAIController");
    if (aiClass == 0) { out.note = "the DAAIController UClass is not in the object array"; return out; }
    const FunctionRef fn = reflect_resolve_function(aiClass, "BootAI");
    if (!fn.usable) { out.note = "BootAI is not usable: " + fn.note; return out; }
    bool made = false;
    std::string callNote;
    const std::uintptr_t c = static_cast<std::uintptr_t>(controller);
    const bool ran = run_on_game_thread(
        [&]() { made = reflect_invoke_function(c, fn, nullptr, callNote); }, 500);
    out.made = made;
    out.note = ran ? ("BootAI " + std::string(made ? "was CALLED" : ("was NOT called: " + callNote)))
                   : std::string("the game thread did not run the call within 500 ms");
    return out;
}

namespace {

constexpr std::uint32_t kAiTeamFrame          = 0x0010;
constexpr std::uint32_t kAiAttitudeFrame      = 0x0018;
constexpr std::uint32_t kAiSensesConfig       = 0x00B0;
constexpr std::uint32_t kAiSightRadius        = 0x0050;
constexpr std::uint32_t kAiSightLoseRadius    = 0x0054;
constexpr std::uint32_t kAiSightHalfAngle     = 0x0058;
constexpr std::uint32_t kAiSightAffiliation   = 0x005C;
constexpr std::uint32_t kAiControllerPerception = 0x0350;
constexpr std::uint32_t kAiControllerState    = 0x0360;
constexpr std::uint32_t kAiCurrentMainTarget  = 0x00D8;
constexpr std::uint32_t kAiCalculationClasses = 0x00C0;
constexpr std::uint32_t kAiCandidateFrame     = 0x0048;
constexpr std::uint32_t kAiFilterFrame        = 0x0048;
constexpr std::uint32_t kAiRuleFrame          = 0x0028;
constexpr std::uint32_t kAiScoreRuleFrame     = 0x0028;
constexpr std::uint32_t kAiSenseConfigsListed = 6;
constexpr std::uint32_t kAiRulesListed        = 8;
constexpr std::uint32_t kAiMaxDetail           = 8;
constexpr std::uint32_t kAiDefaultDetail       = 3;

const char* dat_team_name(int v) {
    switch (v) {
        case 0:   return "Player(0)";
        case 5:   return "Arvud(5)";
        case 10:  return "Metametria(10)";
        case 15:  return "Gardina(15)";
        case 20:  return "Herros(20)";
        case 25:  return "Alqand(25)";
        case 30:  return "GyproVerde(30)";
        case 35:  return "Porokka(35)";
        case 40:  return "BauKhan(40)";
        case 45:  return "Favlar(45)";
        case 50:  return "Nagara(50)";
        case 100: return "Bugs(100)";
        case 110: return "Alliance(110)";
        case 120: return "Berserker(120)";
        case 255: return "NoTeam(255)";
        default:  return "an unlisted EDATeam";
    }
}

const char* ai_attitude_name(int v) {
    switch (v) {
        case 0:   return "Friendly(0)";
        case 1:   return "Neutral(1)";
        case 2:   return "Hostile(2)";
        default:  return "an unlisted ETeamAttitude";
    }
}

const char* ai_behavior_name(int v) {
    switch (v) {
        case 0:   return "Idle(0)";
        case 1:   return "Battle(1)";
        case 2:   return "Return(2)";
        default:  return "an unlisted EDAAIBehaviorState";
    }
}

struct AiProbeCtx {
    std::uintptr_t puppet = 0;
    std::uintptr_t own = 0;
    std::string    puppetName;
    std::string    ownName;
    bool           puppetLoc = false;
    float          px = 0.0f, py = 0.0f, pz = 0.0f;
    bool           ownLoc = false;
    float          ox = 0.0f, oy = 0.0f, oz = 0.0f;
    StaticFunctionRef team;
    StaticFunctionRef attitude;
    StaticFunctionRef filterLib;
    FunctionRef    candidateCheck;
    FunctionRef    checkFilter;
    FunctionRef    calcScore;
    FunctionRef    mainTarget;
    FunctionRef    knownPerceived;
    std::unordered_map<std::uintptr_t, bool> cache;
};

struct AiProbeEnemy {
    std::uintptr_t actor = 0;
    std::uintptr_t controller = 0;
    std::uintptr_t targeting = 0;
    std::string    name;
    std::string    cls;
    std::string    ctrlCls;
    bool           hasLoc = false;
    float          x = 0.0f, y = 0.0f, z = 0.0f;
    bool           distanceKnown = false;
    float          distanceCm = 0.0f;
    std::string    note;
};

float ai_distance(float ax, float ay, float az, float bx, float by, float bz) {
    const float dx = ax - bx;
    const float dy = ay - by;
    const float dz = az - bz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool ai_read_team(const AiProbeCtx& ctx, std::uintptr_t actor, int& out) {
    if (!ctx.team.usable || actor < 0x10000) return false;
    alignas(16) unsigned char frame[kAiTeamFrame] = {};
    const std::uint64_t argument = actor;
    std::memcpy(frame + 0x00, &argument, sizeof(argument));
    std::string note;
    if (!reflect_invoke_function(ctx.team.cdo, ctx.team.fn, frame, note)) return false;
    std::uint8_t value = 0;
    if (!try_read_bytes(reinterpret_cast<std::uintptr_t>(frame) + 0x08, &value, sizeof(value))) return false;
    out = static_cast<int>(value);
    return true;
}

bool ai_read_attitude(const AiProbeCtx& ctx, std::uintptr_t actor, std::uintptr_t other, int& out) {
    if (!ctx.attitude.usable || actor < 0x10000 || other < 0x10000) return false;
    alignas(16) unsigned char frame[kAiAttitudeFrame] = {};
    const std::uint64_t a = actor;
    const std::uint64_t b = other;
    std::memcpy(frame + 0x00, &a, sizeof(a));
    std::memcpy(frame + 0x08, &b, sizeof(b));
    std::string note;
    if (!reflect_invoke_function(ctx.attitude.cdo, ctx.attitude.fn, frame, note)) return false;
    std::uint8_t value = 0;
    if (!try_read_bytes(reinterpret_cast<std::uintptr_t>(frame) + 0x10, &value, sizeof(value))) return false;
    out = static_cast<int>(value);
    return true;
}

void ai_read_senses(std::uintptr_t perception, AiEnemyFacts& f,
                    std::unordered_map<std::uintptr_t, bool>& cache) {
    ArrayHeader configs;
    if (!read_tarray(perception + kAiSensesConfig, 64, configs)) {
        f.sensesNote = "SensesConfig could not be read as a TArray";
        return;
    }
    if (configs.num == 0) {
        f.sensesNote = "the perception component has NO sense configured";
        return;
    }
    char buf[384];
    for (std::uint32_t i = 0; i < configs.num && i < kAiSenseConfigsListed; ++i) {
        std::uint64_t raw = 0;
        if (!try_read_u64(configs.data + static_cast<std::uintptr_t>(i) * 8, raw) || raw < 0x10000) continue;
        const auto config = static_cast<std::uintptr_t>(raw);
        const std::string cls = object_class_name(config);

        std::uint64_t classPtr = 0;
        const bool haveClass = try_read_u64(config + offsets::kUObjectClassOffset, classPtr) &&
                               classPtr >= 0x10000;
        const bool isDaSight =
            haveClass && class_derives_from_named(static_cast<std::uintptr_t>(classPtr),
                                                 "AISenseConfig_DASight", cache);
        const bool isEngineSight =
            !isDaSight && haveClass && class_derives_from_named(static_cast<std::uintptr_t>(classPtr),
                                                                "AISenseConfig_Sight", cache);
        if (isDaSight || isEngineSight) {
            float radius = 0.0f, lose = 0.0f, angle = 0.0f, loseAngle = 0.0f;
            const bool haveNumbers =
                try_read_bytes(config + kAiSightRadius, &radius, 4) &&
                try_read_bytes(config + kAiSightLoseRadius, &lose, 4) &&
                try_read_bytes(config + kAiSightHalfAngle, &angle, 4);
            if (isDaSight) try_read_bytes(config + 0x005C, &loseAngle, 4);
            const std::uint32_t affiliationAt = isDaSight ? 0x0060u : kAiSightAffiliation;
            std::uint8_t affiliation = 0;
            const bool haveFilter = try_read_bytes(config + affiliationAt, &affiliation, 1);
            if (haveNumbers && haveFilter) {
                f.sightKnown = true;
                f.sightDetectsEnemies = (affiliation & 1u) != 0;
                f.sightDetectsNeutrals = (affiliation & 2u) != 0;
                f.sightDetectsFriendlies = (affiliation & 4u) != 0;
                std::snprintf(buf, sizeof(buf),
                              "sight '%s' (%s, affiliation at %02X) radius=%.0f lose=%.0f halfAngle=%.1f"
                              "%s detect(enemies=%d neutrals=%d friendlies=%d)",
                              cls.c_str(), isDaSight ? "the GAME's own sense" : "the engine's sense",
                              affiliationAt, static_cast<double>(radius), static_cast<double>(lose),
                              static_cast<double>(angle),
                              isDaSight ? (", loseAngle=" + std::to_string(static_cast<double>(loseAngle))).c_str()
                                        : "",
                              f.sightDetectsEnemies ? 1 : 0, f.sightDetectsNeutrals ? 1 : 0,
                              f.sightDetectsFriendlies ? 1 : 0);
            } else {
                std::snprintf(buf, sizeof(buf), "sight '%s' (its fields could not be read)", cls.c_str());
            }
            f.senses.push_back(buf);
        } else {
            f.senses.push_back("'" + cls + "'");
        }
    }
    if (configs.num > kAiSenseConfigsListed) {
        f.sensesNote = "+" + std::to_string(configs.num - kAiSenseConfigsListed) +
                       " more sense config(s) were NOT printed";
    }
}

void ai_read_candidates(std::uintptr_t targeting, AiEnemyFacts& f, const std::string& puppetName) {
    ArrayHeader list;
    if (!read_tarray(targeting + kTargetingTargetDataList, 4096, list)) {
        f.listNote = "TargetDataList could not be read as a TArray";
        return;
    }
    f.listRead = true;
    f.candidates = list.num;
    for (std::uint32_t i = 0; i < list.num; ++i) {
        const std::uintptr_t entry = list.data + static_cast<std::uintptr_t>(i) * kScoringStride;
        std::uint64_t raw = 0;
        if (!try_read_u64(entry, raw) || raw < 0x10000) continue;
        const std::string name = name_of_object(static_cast<std::uintptr_t>(raw));
        float score = 0.0f;
        const bool scoreRead = read_f32(entry + kScoringScore, score);
        std::uint8_t filtered = 0;
        const bool filteredRead = try_read_bytes(entry + kScoringFiltered, &filtered, 1);
        if (filteredRead && filtered != 0) ++f.candidatesFiltered;
        if (!puppetName.empty() && name == puppetName) {
            f.puppetIsCandidate = true;
            f.puppetCandidateFiltered = filteredRead && filtered != 0;
            f.puppetCandidateScoreKnown = scoreRead;
            f.puppetCandidateScore = score;
        }
    }
}

void ai_read_rules(AiProbeCtx& ctx, std::uintptr_t actor, std::uintptr_t targeting, AiEnemyFacts& f) {
    if (ctx.puppet < 0x10000) return;
    ArrayHeader list;
    if (!read_tarray(targeting + kAiCalculationClasses, 256, list)) {
        AiRuleVerdict r;
        r.ruleClass = "<unreadable>";
        r.note = "CalculationClasses could not be read as a TArray";
        f.rules.push_back(std::move(r));
        return;
    }
    if (list.num == 0) {
        AiRuleVerdict r;
        r.ruleClass = "<none>";
        r.note = "the targeting component carries NO scoring rule, so nothing here can reject the puppet "
                 "and nothing here can score it either";
        f.rules.push_back(std::move(r));
        return;
    }
    for (std::uint32_t i = 0; i < list.num && f.rules.size() < kAiRulesListed; ++i) {
        std::uint64_t raw = 0;
        if (!try_read_u64(list.data + static_cast<std::uintptr_t>(i) * 8, raw) || raw < 0x10000) continue;
        const auto instance = static_cast<std::uintptr_t>(raw);
        AiRuleVerdict r;
        r.ruleClass = object_class_name(instance);
        std::uint64_t cls = 0;
        const bool isBlueprintRule =
            try_read_u64(instance + offsets::kUObjectClassOffset, cls) && cls >= 0x10000 &&
            class_derives_from_named(static_cast<std::uintptr_t>(cls), "NPTargetingSC_Blueprint", ctx.cache);
        if (!isBlueprintRule) {
            r.nativeRule = true;
            if (r.ruleClass.find("Distance") != std::string::npos) {
                float maxRange = 0.0f;
                std::uint8_t inverse = 0;
                if (try_read_bytes(instance + 0x30, &maxRange, 4)) {
                    r.hasRange = true;
                    r.maxRange = maxRange;
                }
                if (try_read_bytes(instance + 0x34, &inverse, 1)) r.inverse = inverse != 0;
            }
            f.rules.push_back(std::move(r));
            continue;
        }
        if (!ctx.checkFilter.usable) {
            r.note = "K2_CheckFilter could not be resolved: " + ctx.checkFilter.note;
            f.rules.push_back(std::move(r));
            continue;
        }
        {
            alignas(16) unsigned char frame[kAiRuleFrame] = {};
            const std::uint64_t owner = actor;
            const std::uint64_t candidate = ctx.puppet;
            std::memcpy(frame + 0x00, &owner, sizeof(owner));
            std::memcpy(frame + 0x08, &candidate, sizeof(candidate));
            std::string note;
            if (reflect_invoke_function(instance, ctx.checkFilter, frame, note)) {
                std::uint8_t filtered = 0;
                if (try_read_bytes(reinterpret_cast<std::uintptr_t>(frame) + 0x18, &filtered, 1)) {
                    r.asked = true;
                    r.filtered = filtered != 0;
                } else {
                    r.note = "K2_CheckFilter ran but its bFiltered could not be read back";
                }
            } else {
                r.note = "K2_CheckFilter was NOT called: " + note;
            }
        }
        if (ctx.calcScore.usable) {
            alignas(16) unsigned char frame[kAiScoreRuleFrame] = {};
            const std::uint64_t owner = actor;
            const std::uint64_t candidate = ctx.puppet;
            std::memcpy(frame + 0x00, &owner, sizeof(owner));
            std::memcpy(frame + 0x08, &candidate, sizeof(candidate));
            std::string note;
            if (reflect_invoke_function(instance, ctx.calcScore, frame, note)) {
                float score = 0.0f;
                if (try_read_bytes(reinterpret_cast<std::uintptr_t>(frame) + 0x20, &score, 4)) {
                    r.scoreKnown = true;
                    r.score = score;
                }
            } else if (r.note.empty()) {
                r.note = "K2_CalcScore was NOT called: " + note;
            }
        }
        f.rules.push_back(std::move(r));
    }
    if (list.num > kAiRulesListed) {
        AiRuleVerdict more;
        more.ruleClass = "<more>";
        more.note = "+" + std::to_string(list.num - kAiRulesListed) + " more scoring rule(s) were NOT printed";
        f.rules.push_back(std::move(more));
    }
}

void ai_ask_the_game(AiProbeCtx& ctx, const AiProbeEnemy& src, AiEnemyFacts& f) {
    const auto snapshot = [&](int& state, std::string& mainTarget, std::uint32_t& candidates) {
        state = -1;
        if (src.controller >= 0x10000) {
            std::uint8_t value = 0;
            if (try_read_bytes(src.controller + kAiControllerState, &value, 1)) state = value;
        }
        mainTarget.clear();
        if (ctx.mainTarget.usable && src.targeting >= 0x10000) {
            unsigned char ret[0x10] = {};
            std::string note;
            if (reflect_invoke_function(src.targeting, ctx.mainTarget, ret, note)) {
                std::uint64_t held = 0;
                if (try_read_u64(reinterpret_cast<std::uintptr_t>(ret), held) && held >= 0x10000) {
                    mainTarget = name_of_object(static_cast<std::uintptr_t>(held));
                }
            }
        }
        candidates = 0;
        ArrayHeader list;
        if (src.targeting >= 0x10000 &&
            read_tarray(src.targeting + kTargetingTargetDataList, 4096, list)) {
            candidates = list.num;
        }
    };

    int stateBefore = -1;
    std::string targetBefore;
    std::uint32_t candidatesBefore = 0;
    snapshot(stateBefore, targetBefore, candidatesBefore);

    if (ctx.puppet >= 0x10000 && src.hasLoc && ctx.puppetLoc) {
        if (!ctx.candidateCheck.usable) {
            f.candidateCheckNote = "CheckCanditateTarget could not be resolved: " + ctx.candidateCheck.note;
        } else {
            alignas(16) unsigned char frame[kAiCandidateFrame] = {};
            const std::uint64_t target = ctx.puppet;
            std::memcpy(frame + 0x00, &target, sizeof(target));
            const float age = 0.0f, expire = 5.0f, strength = 1.0f;
            std::memcpy(frame + 0x08, &age, 4);
            std::memcpy(frame + 0x0C, &expire, 4);
            std::memcpy(frame + 0x10, &strength, 4);
            std::memcpy(frame + 0x14, &ctx.px, 4);
            std::memcpy(frame + 0x18, &ctx.py, 4);
            std::memcpy(frame + 0x1C, &ctx.pz, 4);
            std::memcpy(frame + 0x20, &src.x, 4);
            std::memcpy(frame + 0x24, &src.y, 4);
            std::memcpy(frame + 0x28, &src.z, 4);

            frame[0x40] = 0x02;
            std::string note;
            if (reflect_invoke_function(src.controller, ctx.candidateCheck, frame, note)) {
                std::uint8_t accepted = 0;
                if (try_read_bytes(reinterpret_cast<std::uintptr_t>(frame) + 0x44, &accepted, 1)) {
                    f.candidateCheckMade = true;
                    f.candidateAccepted = accepted != 0;
                } else {
                    f.candidateCheckNote = "CheckCanditateTarget ran but its answer could not be read back";
                }
            } else {
                f.candidateCheckNote = "CheckCanditateTarget was NOT called: " + note;
            }
        }
    } else if (ctx.puppet >= 0x10000) {
        f.candidateCheckNote = ctx.puppetLoc ? "the enemy has no location" : "the puppet has no location";
    } else {
        f.candidateCheckNote = "there is no puppet on this machine, so there is nothing to ask about";
    }

    if (ctx.puppet >= 0x10000 && src.targeting >= 0x10000) {
        if (!ctx.filterLib.usable) {
            f.filterCheckNote = "CheckFilteredTarget could not be resolved: " + ctx.filterLib.note;
        } else {
            ArrayHeader calcs;
            const bool calcsRead = read_tarray(src.targeting + kAiCalculationClasses, 256, calcs);
            alignas(16) unsigned char frame[kAiFilterFrame] = {};
            const std::uint64_t owner = src.actor;
            const std::uint64_t candidate = ctx.puppet;
            std::memcpy(frame + 0x00, &owner, sizeof(owner));
            std::memcpy(frame + 0x08, &candidate, sizeof(candidate));
            if (calcsRead && calcs.num != 0) {
                const std::uint64_t data = static_cast<std::uint64_t>(calcs.data);
                const std::uint32_t num = calcs.num;
                const std::uint32_t max = calcs.max;
                std::memcpy(frame + 0x20, &data, sizeof(data));
                std::memcpy(frame + 0x28, &num, sizeof(num));
                std::memcpy(frame + 0x2C, &max, sizeof(max));
            }
            std::string note;
            if (!reflect_invoke_function(ctx.filterLib.cdo, ctx.filterLib.fn, frame, note)) {
                f.filterCheckNote = "CheckFilteredTarget was NOT called: " + note;
            } else {
                std::uint8_t filtered = 0;
                if (try_read_bytes(reinterpret_cast<std::uintptr_t>(frame) + 0x40, &filtered, 1)) {
                    f.filterCheckMade = true;
                    f.filterSaysFiltered = filtered != 0;
                } else {
                    f.filterCheckNote = "CheckFilteredTarget ran but its bFiltered could not be read back";
                }
                float score = 0.0f;
                if (try_read_bytes(reinterpret_cast<std::uintptr_t>(frame) + 0x38, &score, 4)) {
                    f.filterScoreKnown = true;
                    f.filterScore = score;
                }
                if (calcsRead) {
                    ArrayHeader again;
                    if (!read_tarray(src.targeting + kAiCalculationClasses, 256, again) ||
                        again.data != calcs.data || again.num != calcs.num) {
                        f.sideEffectObserved = true;
                        f.sideEffectNote += (f.sideEffectNote.empty() ? "" : "; ");
                        f.sideEffectNote +=
                            "CheckFilteredTarget CHANGED CalculationClasses -- the system's own "
                            "rule list was rewritten by the call";
                    }
                }
            }
        }
    }

    int stateAfter = -1;
    std::string targetAfter;
    std::uint32_t candidatesAfter = 0;
    snapshot(stateAfter, targetAfter, candidatesAfter);
    if (stateBefore != stateAfter) {
        f.sideEffectObserved = true;
        f.sideEffectNote += (f.sideEffectNote.empty() ? "" : "; ");
        f.sideEffectNote += "AIBehaviorState " + std::string(ai_behavior_name(stateBefore)) + " -> " +
                            ai_behavior_name(stateAfter);
    }
    if (targetBefore != targetAfter) {
        f.sideEffectObserved = true;
        f.sideEffectNote += (f.sideEffectNote.empty() ? "" : "; ");
        f.sideEffectNote += "the main target moved '" + targetBefore + "' -> '" + targetAfter + "'";
    }
    if (candidatesBefore != candidatesAfter) {
        f.sideEffectObserved = true;
        f.sideEffectNote += (f.sideEffectNote.empty() ? "" : "; ");
        f.sideEffectNote += "the candidate list grew/shrank " + std::to_string(candidatesBefore) + " -> " +
                            std::to_string(candidatesAfter);
    }
}

void ai_fill_enemy(AiProbeCtx& ctx, const AiProbeEnemy& src, AiEnemyFacts& f) {
    f.enemyName = src.name;
    f.enemyClass = src.cls;
    f.controllerClass = src.ctrlCls;
    f.distanceToPuppetKnown = src.distanceKnown;
    f.distanceToPuppetCm = src.distanceCm;
    if (ctx.ownLoc && src.hasLoc) {
        f.distanceToOwnKnown = true;
        f.distanceToOwnCm = ai_distance(src.x, src.y, src.z, ctx.ox, ctx.oy, ctx.oz);
    }

    int team = -1;
    if (ai_read_team(ctx, src.actor, team)) {
        f.teamKnown = true;
        f.team = team;
    }
    if (ctx.puppet >= 0x10000) {
        int attitude = -1;
        if (ai_read_attitude(ctx, src.actor, ctx.puppet, attitude)) {
            f.attitudeToPuppetKnown = true;
            f.attitudeToPuppet = attitude;
        }
    }
    if (ctx.own >= 0x10000) {
        int attitude = -1;
        if (ai_read_attitude(ctx, src.actor, ctx.own, attitude)) {
            f.attitudeToOwnKnown = true;
            f.attitudeToOwn = attitude;
        }
    }

    if (src.controller >= 0x10000) {
        std::uint8_t state = 0;
        if (try_read_bytes(src.controller + kAiControllerState, &state, 1)) {
            f.stateKnown = true;
            f.state = state;
        }
        std::uint64_t perception = 0;
        if (try_read_u64(src.controller + kAiControllerPerception, perception) && perception >= 0x10000) {
            const auto perceptionPtr = static_cast<std::uintptr_t>(perception);
            ai_read_senses(perceptionPtr, f, ctx.cache);
            if (ctx.knownPerceived.usable) {
                unsigned char frame[0x20] = {};

                std::string note;
                if (reflect_invoke_function(perceptionPtr, ctx.knownPerceived, frame, note)) {
                    ArrayHeader known;
                    if (read_tarray(reinterpret_cast<std::uintptr_t>(frame) + 0x08, 65536, known)) {
                        f.perceptionRead = true;
                        f.perceptionKnown = known.num;
                        for (std::uint32_t i = 0; i < known.num && f.perceptionNames.size() < 8; ++i) {
                            std::uint64_t raw = 0;
                            if (!try_read_u64(known.data + static_cast<std::uintptr_t>(i) * 8, raw) ||
                                raw < 0x10000) {
                                continue;
                            }
                            const std::string name = name_of_object(static_cast<std::uintptr_t>(raw));
                            f.perceptionNames.push_back(name);
                            if (!ctx.puppetName.empty() && name == ctx.puppetName) f.seesPuppet = true;
                            if (!ctx.ownName.empty() && name == ctx.ownName) f.seesOwn = true;
                        }
                    } else {
                        f.perceptionNote = "the call returned but its out-array could not be read";
                    }
                } else {
                    f.perceptionNote = "GetKnownPerceivedActors was NOT called: " + note;
                }
            } else {
                f.perceptionNote = "GetKnownPerceivedActors could not be resolved: " + ctx.knownPerceived.note;
            }
        } else {
            f.perceptionNote = "the controller has nothing at (no perception component)";
        }
    } else {
        f.listNote = "the survey gave this character no controller to ask";
    }

    if (src.targeting < 0x10000) {
        if (f.listNote.empty()) f.listNote = "the controller has nothing at (no targeting component)";
        return;
    }
    if (ctx.mainTarget.usable) {
        unsigned char ret[0x10] = {};
        std::string note;
        std::uint64_t held = 0;
        const bool answered = reflect_invoke_function(src.targeting, ctx.mainTarget, ret, note) &&
                              try_read_u64(reinterpret_cast<std::uintptr_t>(ret), held) && held >= 0x10000;
        std::uint64_t field = 0;
        const bool fieldRead = try_read_u64(src.targeting + kAiCurrentMainTarget, field) && field >= 0x10000;
        if (answered) {
            f.mainTargetName = name_of_object(static_cast<std::uintptr_t>(held));
            if (fieldRead && field != held) {
                f.mainTargetName += " (the field names a DIFFERENT actor: '" +
                                    name_of_object(static_cast<std::uintptr_t>(field)) + "')";
            }
        } else if (fieldRead) {
            f.mainTargetName = name_of_object(static_cast<std::uintptr_t>(field)) +
                               " (from; GetMainTarget did not answer: " + note + ")";
        }
    }
    ai_read_candidates(src.targeting, f, ctx.puppetName);
    ai_read_rules(ctx, src.actor, src.targeting, f);
    ai_ask_the_game(ctx, src, f);
}

}

AiTargetingSurvey survey_ai_targeting(std::uint32_t detailCount) {
    AiTargetingSurvey s;
    if (detailCount == 0) detailCount = kAiDefaultDetail;
    if (detailCount > kAiMaxDetail) detailCount = kAiMaxDetail;

    const EnemySurvey survey = survey_enemies();
    if (!survey.worldFound) {
        s.worldNote = "the live world is not known, so no level was walked: " + survey.worldNote;
        return s;
    }
    s.worldFound = true;
    s.enemiesListed = static_cast<std::uint32_t>(survey.enemies.size());

    AiProbeCtx ctx;
    {
        const std::uintptr_t actorClass = resolve_uclass_address("Actor");
        FunctionRef location;
        if (actorClass != 0) location = reflect_resolve_function(actorClass, "K2_GetActorLocation");
        const std::uintptr_t targetingClass = resolve_uclass_address("NPTargetingSystemComponent");
        if (targetingClass != 0) {
            ctx.mainTarget = reflect_resolve_function(targetingClass, "GetMainTarget");
        }
        const std::uintptr_t perceptionClass = resolve_uclass_address("AIPerceptionComponent");
        if (perceptionClass != 0) {
            ctx.knownPerceived = reflect_resolve_function(perceptionClass, "GetKnownPerceivedActors");
        }
        const std::uintptr_t aiClass = resolve_uclass_address("DAAIController");
        if (aiClass != 0) {
            ctx.candidateCheck = reflect_resolve_function(aiClass, "CheckCanditateTarget");
        }
        const std::uintptr_t blueprintRule = resolve_uclass_address("NPTargetingSC_Blueprint");
        if (blueprintRule != 0) {
            ctx.checkFilter = reflect_resolve_function(blueprintRule, "K2_CheckFilter");
            ctx.calcScore = reflect_resolve_function(blueprintRule, "K2_CalcScore");
        }
        ctx.team = reflect_resolve_static("DAGameplayStatics", "GetDATeam");
        ctx.attitude = reflect_resolve_static("DAGameplayStatics", "GetDATeamAttitudeTowards");
        ctx.filterLib = reflect_resolve_static("NPTargetScoringLibrary", "CheckFilteredTarget");
        s.teamFunctionUsable = ctx.team.usable;
        if (!ctx.team.usable) s.teamFunctionNote = ctx.team.note;

        std::string machineNote;
        const std::vector<AggroMachine> machines = collect_machines(location, ctx.cache, machineNote);
        if (!machineNote.empty()) s.worldNote = machineNote;
        for (const AggroMachine& m : machines) {
            if (!m.hasController && !s.hasPuppet) {
                s.hasPuppet = true;
                ctx.puppet = m.actor;
                ctx.puppetName = m.name;
                s.puppetName = m.name;
                s.puppetClass = m.cls;
                ctx.puppetLoc = m.hasLocation;
                ctx.px = m.x;
                ctx.py = m.y;
                ctx.pz = m.z;
            } else if (m.hasController && !s.hasOwn) {
                s.hasOwn = true;
                ctx.own = m.actor;
                ctx.ownName = m.name;
                s.ownName = m.name;
                s.ownClass = m.cls;
                ctx.ownLoc = m.hasLocation;
                ctx.ox = m.x;
                ctx.oy = m.y;
                ctx.oz = m.z;
            }
        }
    }

    if (s.hasPuppet) {
        int team = -1;
        if (ai_read_team(ctx, ctx.puppet, team)) {
            s.puppetTeamKnown = true;
            s.puppetTeam = team;
            ctx.puppetName = s.puppetName;
        }
    }
    if (s.hasOwn) {
        int team = -1;
        if (ai_read_team(ctx, ctx.own, team)) {
            s.ownTeamKnown = true;
            s.ownTeam = team;
        }
    }

    if (s.hasPuppet || s.hasOwn) {
        const std::uint32_t count = reflection_state().objectCount;
        for_each_object(count, [&](std::uintptr_t object) {
            std::uint64_t outer = 0;
            if (!try_read_u64(object + 0x20, outer) || outer < 0x10000) return;
            const auto outerPtr = static_cast<std::uintptr_t>(outer);
            const bool isPuppet = s.hasPuppet && outerPtr == ctx.puppet;
            const bool isOwn = s.hasOwn && outerPtr == ctx.own;
            if (!isPuppet && !isOwn) return;
            const std::string cls = object_class_name(object);
            if (cls.empty()) return;
            const bool stimuli = cls.find("StimuliSource") != std::string::npos;
            if (isPuppet) {
                if (stimuli) s.puppetHasStimuliSource = true;
                if (s.puppetComponents.size() < 12) s.puppetComponents.push_back(cls);
            } else {
                if (stimuli) s.ownHasStimuliSource = true;
                if (s.ownComponents.size() < 12) s.ownComponents.push_back(cls);
            }
        });
        s.stimuliScanned = true;
    }

    std::vector<AiProbeEnemy> briefs;
    const std::uintptr_t actorClass = resolve_uclass_address("Actor");
    const FunctionRef location =
        actorClass != 0 ? reflect_resolve_function(actorClass, "K2_GetActorLocation") : FunctionRef{};
    for (const EnemyRecord& e : survey.enemies) {
        if (e.actorName.empty()) continue;
        const std::string npcWhy = npc_exclusion_reason(e.actorClass, e.controllerClass);
        if (!npcWhy.empty()) {
            ++s.npcSkipped;
            if (s.npcNames.size() < 8) s.npcNames.push_back(e.actorName + " (" + npcWhy + ")");
            continue;
        }
        AiProbeEnemy b;
        b.actor = e.actor;
        b.controller = e.controller;
        b.name = e.actorName;
        b.cls = e.actorClass;
        b.ctrlCls = e.controllerClass;
        b.hasLoc = read_actor_location(e.actor, location, b.x, b.y, b.z, b.note);
        if (b.hasLoc && ctx.puppetLoc) {
            b.distanceKnown = true;
            b.distanceCm = ai_distance(b.x, b.y, b.z, ctx.px, ctx.py, ctx.pz);
        }
        if (e.controller >= 0x10000) {
            std::uint64_t targeting = 0;
            if (try_read_u64(e.controller + kControllerTargeting, targeting) && targeting >= 0x10000) {
                b.targeting = static_cast<std::uintptr_t>(targeting);
            }
        }
        briefs.push_back(std::move(b));
    }

    std::sort(briefs.begin(), briefs.end(), [](const AiProbeEnemy& a, const AiProbeEnemy& b) {
        const float da = a.distanceKnown ? a.distanceCm : 1.0e30f;
        const float db = b.distanceKnown ? b.distanceCm : 1.0e30f;
        return da < db;
    });

    const std::uint32_t detailed = static_cast<std::uint32_t>(std::min<std::size_t>(detailCount, briefs.size()));
    for (std::uint32_t i = 0; i < detailed; ++i) {
        AiEnemyFacts f;
        ai_fill_enemy(ctx, briefs[i], f);
        s.enemies.push_back(std::move(f));
    }
    s.enemiesNotDetailed = static_cast<std::uint32_t>(briefs.size()) - detailed;

    std::uint32_t sightKnown = 0, sightNeutralOff = 0;
    std::uint32_t sawPuppet = 0, perceptionRead = 0;
    std::uint32_t candPuppet = 0, candPuppetFiltered = 0;
    std::uint32_t filterAsked = 0, filterSaidFiltered = 0, candAsked = 0, candAccepted = 0;
    for (const AiEnemyFacts& f : s.enemies) {
        if (f.sightKnown) {
            ++sightKnown;
            if (!f.sightDetectsNeutrals) ++sightNeutralOff;
        }
        if (f.perceptionRead) {
            ++perceptionRead;
            if (f.seesPuppet) ++sawPuppet;
        }
        if (f.puppetIsCandidate) {
            ++candPuppet;
            if (f.puppetCandidateFiltered) ++candPuppetFiltered;
        }
        if (f.filterCheckMade) {
            ++filterAsked;
            if (f.filterSaysFiltered) ++filterSaidFiltered;
        }
        if (f.candidateCheckMade) {
            ++candAsked;
            if (f.candidateAccepted) ++candAccepted;
        }
    }
    const std::string sightsEnemiesOnly =
        (sightKnown != 0 && sightNeutralOff == sightKnown)
            ? ("all " + std::to_string(sightKnown) + " sight config(s) read here detect ENEMIES ONLY, so a "
               "NEUTRAL actor cannot be reported by sight at all")
            : (sightKnown != 0 ? (std::to_string(sightKnown - sightNeutralOff) + " of " +
                                  std::to_string(sightKnown) + " sight config(s) DO detect neutrals")
                               : std::string("no sight config could be read"));
    if (!s.hasPuppet) {
        s.verdict =
            "there is NO machine without a controller in the levels, so the puppet is not built on this "
            "machine right now -- run this on the HOST while a session is running";
    } else if (s.puppetTeamKnown && s.puppetTeam == 255) {
        s.verdict = "TEAM -- the puppet is " + std::string(dat_team_name(s.puppetTeam)) + " and " +
                    sightsEnemiesOnly + ". Branch 1: SetDATeam(puppet, Player) is the first thing to try.";
    } else if (perceptionRead != 0 && sawPuppet == 0) {
        s.verdict = "REGISTRATION -- the puppet's team reads " +
                    std::string(s.puppetTeamKnown ? dat_team_name(s.puppetTeam) : "NOT READ") +
                    ", but NOT ONE of the " + std::to_string(perceptionRead) +
                    " enemies read here knows it. " + sightsEnemiesOnly +
                    ". Branch 2: register it as a stimuli source.";
    } else if (candPuppet == 0) {
        s.verdict = "FILTER -- the puppet IS perceived (" + std::to_string(sawPuppet) + " of " +
                    std::to_string(perceptionRead) + " enemies) but is in NO candidate list; " +
                    std::to_string(filterAsked) + " filter answer(s) read, " +
                    std::to_string(filterSaidFiltered) + " of them rejected it. Branch 3.";
    } else {
        s.verdict = "SCORE -- the puppet is a candidate on " + std::to_string(candPuppet) +
                    " enemy/enemies and " + std::to_string(candPuppetFiltered) +
                    " of those entries are bFiltered. Branch 4: it is a scoring question, not a visibility "
                    "one.";
    }
    if (candAsked != 0) {
        s.verdict += " (CheckCanditateTarget answered YES on " + std::to_string(candAccepted) + " of " +
                     std::to_string(candAsked) + ".)";
    }
    return s;
}

std::vector<std::string> AiTargetingSurvey::report_lines() const {
    std::vector<std::string> out;
    char buf[1024];

    if (!worldFound) {
        out.push_back("ai: " + worldNote);
        return out;
    }
    out.push_back("ai: FOUR PLACES THE PUPPET CAN BE LOST -- team, stimuli source, the game's own filter, the "
                  "score. Every number below is either a read or the GAME's own answer. Nothing was written.");

    if (hasPuppet) {
        std::snprintf(buf, sizeof(buf), "ai: puppet '%s' [%s] GetDATeam=%s | this machine's own '%s' GetDATeam=%s",
                      puppetName.c_str(), puppetClass.c_str(),
                      puppetTeamKnown ? dat_team_name(puppetTeam) : "NOT READ", ownName.c_str(),
                      ownTeamKnown ? dat_team_name(ownTeam) : "NOT READ");
    } else if (hasOwn) {
        std::snprintf(buf, sizeof(buf),
                      "ai: NO machine without a controller in the levels -- only this machine's own '%s' "
                      "GetDATeam=%s. The puppet exists only while a session builds it.",
                      ownName.c_str(), ownTeamKnown ? dat_team_name(ownTeam) : "NOT READ");
    } else {
        std::snprintf(buf, sizeof(buf), "ai: NO player machine (DAMachine) is in the loaded levels at all");
    }
    out.push_back(buf);
    if (!teamFunctionUsable) out.push_back("ai: GetDATeam is NOT usable: " + teamFunctionNote);

    if (stimuliScanned) {
        std::snprintf(buf, sizeof(buf),
                      "ai: AIPerceptionStimuliSourceComponent: puppet %s | this machine's own %s",
                      puppetHasStimuliSource ? "PRESENT" : "ABSENT",
                      ownHasStimuliSource ? "PRESENT" : "ABSENT");
        out.push_back(buf);
        std::string puppetParts;
        for (const std::string& one : puppetComponents) {
            if (!puppetParts.empty()) puppetParts += ", ";
            puppetParts += one;
        }
        if (!puppetParts.empty()) out.push_back("ai: the puppet's own objects (Outer = the puppet): " + puppetParts);
    }

    std::snprintf(buf, sizeof(buf),
                  "ai: %u AI character(s) examined, %u NPC(s) SKIPPED and never called; %zu detailed, %u more "
                  "only counted",
                  enemiesListed, npcSkipped, enemies.size(), enemiesNotDetailed);
    out.push_back(buf);
    if (!npcNames.empty()) {
        std::string names;
        for (const std::string& one : npcNames) {
            if (!names.empty()) names += "; ";
            names += one;
        }
        out.push_back("ai: NPC(s) skipped: " + names);
    }

    for (const AiEnemyFacts& f : enemies) {
        std::snprintf(buf, sizeof(buf), "ai: enemy '%s' [%s] via [%s] team=%s state=%s",
                      f.enemyName.c_str(), f.enemyClass.c_str(), f.controllerClass.c_str(),
                      f.teamKnown ? dat_team_name(f.team) : "NOT READ",
                      f.stateKnown ? ai_behavior_name(f.state) : "NOT READ");
        out.push_back(buf);
        std::snprintf(buf, sizeof(buf),
                      "ai: attitude towards the puppet = %s ; towards this machine's own = %s ; "
                      "distance: puppet %s / own %s",
                      f.attitudeToPuppetKnown ? ai_attitude_name(f.attitudeToPuppet) : "NOT ANSWERED",
                      f.attitudeToOwnKnown ? ai_attitude_name(f.attitudeToOwn) : "NOT ANSWERED",
                      f.distanceToPuppetKnown
                          ? (std::to_string(static_cast<int>(f.distanceToPuppetCm)) + " cm").c_str()
                          : "NOT MEASURED",
                      f.distanceToOwnKnown
                          ? (std::to_string(static_cast<int>(f.distanceToOwnCm)) + " cm").c_str()
                          : "NOT MEASURED");
        out.push_back(buf);

        std::string senses;
        for (const std::string& one : f.senses) {
            if (!senses.empty()) senses += " ; ";
            senses += one;
        }
        if (senses.empty()) senses = f.sensesNote.empty() ? "NONE reported" : f.sensesNote;
        else if (!f.sensesNote.empty()) senses += " (" + f.sensesNote + ")";
        out.push_back("ai: senses: " + senses);

        if (f.perceptionRead) {
            std::string names;
            for (const std::string& one : f.perceptionNames) {
                if (!names.empty()) names += ", ";
                names += "'" + one + "'";
            }
            if (names.empty()) names = "(no name resolved)";
            std::snprintf(buf, sizeof(buf), "ai: perception knows %u: %s%s", f.perceptionKnown,
                          names.c_str(), f.seesPuppet ? " <-- IT KNOWS THE PUPPET" : "");
            out.push_back(buf);
        } else {
            out.push_back("ai: perception: " + (f.perceptionNote.empty() ? "NOT READ" : f.perceptionNote));
        }

        out.push_back("ai: main target: " +
                      (f.mainTargetName.empty() ? std::string("NOTHING")
                                                : ("'" + f.mainTargetName + "'")));
        if (f.listRead) {
            std::snprintf(buf, sizeof(buf),
                          "ai: candidates %u (%u bFiltered); the puppet is %s",
                          f.candidates, f.candidatesFiltered,
                          f.puppetIsCandidate
                              ? (f.puppetCandidateFiltered ? "present but FILTERED" : "present and NOT filtered")
                              : "NOT in the list");
            std::string line = buf;
            if (f.puppetIsCandidate && f.puppetCandidateScoreKnown) {
                line += ", score " + std::to_string(static_cast<double>(f.puppetCandidateScore));
            }
            out.push_back(line);
        } else {
            out.push_back("ai: candidates: " + (f.listNote.empty() ? "NOT READ" : f.listNote));
        }

        std::snprintf(buf, sizeof(buf),
                      "ai: the GAME's own answer: CheckCanditateTarget=%s | CheckFilteredTarget=%s",
                      f.candidateCheckMade ? (f.candidateAccepted ? "TRUE (it would take the puppet)" : "FALSE")
                                           : "NOT ASKED",
                      f.filterCheckMade ? (f.filterSaysFiltered ? "filtered=TRUE" : "filtered=FALSE")
                                        : "NOT ASKED");
        std::string answer = buf;
        if (f.filterScoreKnown) answer += ", score " + std::to_string(static_cast<double>(f.filterScore));
        if (!f.candidateCheckNote.empty()) answer += " [" + f.candidateCheckNote + "]";
        if (!f.filterCheckNote.empty()) answer += " [" + f.filterCheckNote + "]";
        out.push_back(answer);

        if (!f.rules.empty()) {
            std::string rules;
            for (const AiRuleVerdict& r : f.rules) {
                if (!rules.empty()) rules += " ; ";
                rules += "'" + r.ruleClass + "'";
                if (r.asked) {
                    rules += r.filtered ? " filtered=TRUE" : " filtered=FALSE";
                    if (r.scoreKnown) rules += " score=" + std::to_string(static_cast<double>(r.score));
                } else if (r.nativeRule) {
                    rules += " (native, not asked";
                    if (r.hasRange) {
                        rules += ", MaxRange=" + std::to_string(static_cast<int>(r.maxRange));
                        rules += r.inverse ? " bInverse=1" : " bInverse=0";
                    }
                    rules += ")";
                } else {
                    rules += " NOT ASKED";
                }
                if (!r.note.empty()) rules += " [" + r.note + "]";
            }
            out.push_back("ai: rules: " + rules);
        }
        if (f.sideEffectObserved) out.push_back("ai: ASKING CHANGED SOMETHING -- " + f.sideEffectNote);
    }

    out.push_back("ai: VERDICT (derived from the numbers above, NOT a measurement): " + verdict);
    return out;
}

AiTeamWriteResult ai_set_puppet_team(int team) {
    AiTeamWriteResult out;
    out.askedFor = team;
    if (team < 0 || team > 255) {
        out.note = "the team must be a value of the game's own EDATeam (0 = Player .. 255 = NoTeam)";
        return out;
    }

    AiProbeCtx ctx;
    {
        const std::uintptr_t actorClass = resolve_uclass_address("Actor");
        FunctionRef location;
        if (actorClass != 0) location = reflect_resolve_function(actorClass, "K2_GetActorLocation");
        std::string machineNote;
        const std::vector<AggroMachine> machines = collect_machines(location, ctx.cache, machineNote);
        for (const AggroMachine& m : machines) {
            if (!m.hasController) {
                ctx.puppet = m.actor;
                ctx.puppetName = m.name;
                break;
            }
        }
        if (ctx.puppet < 0x10000) {
            out.note = "there is NO machine without a controller in the levels, so there is no puppet to put on "
                       "a team -- run this on the HOST while a session is running";
            return out;
        }
    }
    out.puppetFound = true;
    out.puppetName = ctx.puppetName;

    ctx.team = reflect_resolve_static("DAGameplayStatics", "GetDATeam");
    const StaticFunctionRef set = reflect_resolve_static("DAGameplayStatics", "SetDATeam");
    if (!set.usable) {
        out.note = "SetDATeam could not be resolved: " + set.note;
        return out;
    }

    int before = -1;
    if (ai_read_team(ctx, ctx.puppet, before)) {
        out.beforeKnown = true;
        out.before = before;
    }

    alignas(16) unsigned char frame[0x10] = {};
    const std::uint64_t argument = ctx.puppet;
    std::memcpy(frame + 0x00, &argument, sizeof(argument));
    std::uint8_t teamByte = static_cast<std::uint8_t>(team & 0xFF);
    std::memcpy(frame + 0x08, &teamByte, sizeof(teamByte));
    std::string note;
    if (!reflect_invoke_function(set.cdo, set.fn, frame, note)) {
        out.note = "SetDATeam was NOT called: " + note;
        return out;
    }
    out.callMade = true;
    std::uint8_t returned = 0;
    if (try_read_bytes(reinterpret_cast<std::uintptr_t>(frame) + 0x09, &returned, 1)) {
        out.written = returned != 0;
    } else {
        out.note = "SetDATeam ran but its return value could not be read back";
    }

    int after = -1;
    if (ai_read_team(ctx, ctx.puppet, after)) {
        out.afterKnown = true;
        out.after = after;
    }
    return out;
}

std::vector<std::string> AiTeamWriteResult::report_lines() const {
    std::vector<std::string> out;
    char buf[512];
    if (!puppetFound) {
        out.push_back("ai team: " + note);
        return out;
    }
    std::snprintf(buf, sizeof(buf),
                  "ai team: puppet '%s' GetDATeam %s -> asked for %s (%d)",
                  puppetName.c_str(), beforeKnown ? dat_team_name(before) : "NOT READ",
                  dat_team_name(askedFor), askedFor);
    out.push_back(buf);
    std::snprintf(buf, sizeof(buf), "ai team: SetDATeam %s, and it returned %s; GetDATeam now reads %s",
                  callMade ? "was CALLED" : "was NOT called", written ? "TRUE" : "FALSE",
                  afterKnown ? dat_team_name(after) : "NOT READ");
    std::string line = buf;
    if (!note.empty()) line += " [" + note + "]";
    out.push_back(line);
    if (callMade && !written) {
        out.push_back("ai team: THE GAME REFUSED IT -- it has nowhere to keep a team on a machine that has no "
                      "controller. That is the answer, not a failure: the team lives on the controller, and "
                      "this branch of the design is closed.");
    } else if (written && afterKnown && after != askedFor) {
        out.push_back("ai team: SetDATeam returned TRUE and GetDATeam does NOT agree -- the write landed "
                      "somewhere other than the field the read uses; do not build on it until that is explained");
    } else if (written) {
        out.push_back("ai team: THE WRITE LANDED. Watch the enemies: if they now walk to and shoot the puppet, "
                      "the team was the whole gate. `ai` immediately below is the numbers for it; "
                      "`ai team 255` puts it back.");
    }
    return out;
}

namespace {

constexpr std::uint32_t kAiModifyTargetFrame   = 0x0048;
constexpr std::uint32_t kAiWeakIndex           = 0x0004;
constexpr std::uint32_t kAiWeakSerial          = 0x0008;
constexpr std::uint32_t kAiStimulusBase        = 0x000C;
constexpr std::uint32_t kAiSenseFriendliesBit  = 0x04;

struct AiNudgeCtx {
    std::uintptr_t puppet = 0;
    std::string    puppetName;
    bool           puppetLoc = false;
    float          px = 0.0f, py = 0.0f, pz = 0.0f;
    std::uintptr_t enemy = 0;
    std::uintptr_t controller = 0;
    std::uintptr_t targeting = 0;
    std::uintptr_t perception = 0;
    std::string    enemyName, enemyClass;
    bool           enemyLoc = false;
    float          ex = 0.0f, ey = 0.0f, ez = 0.0f;
    FunctionRef    mainTarget;
    FunctionRef    knownPerceived;
    FunctionRef    modifyTarget;
    std::unordered_map<std::uintptr_t, bool> cache;
};

bool ai_weak_pointer_for(std::uintptr_t object, std::int32_t& indexOut, std::int32_t& serialOut,
                         std::string& why) {
    const ReflectionState& state = reflection_state();
    std::uint32_t index = 0;
    if (!try_read_u32(object + offsets::kUObjectIndexOffset, index)) {
        why = "its own index (UObject::Index,) could not be read";
        return false;
    }

    const std::uintptr_t base = main_module_base();
    if (base == 0) {
        why = "the main executable's base could not be resolved";
        return false;
    }
    const std::uintptr_t array = base + static_cast<std::uintptr_t>(offsets::kGObjects);
    std::uint64_t chunkTable = 0;
    std::uint32_t liveElements = 0;
    if (!try_read_u64(array + offsets::kObjectsOffset, chunkTable) || chunkTable < 0x10000) {
        why = "the object table's chunk table (TUObjectArray::Objects) could not be read";
        return false;
    }
    if (!try_read_u32(array + offsets::kNumElementsOffset, liveElements)) {
        why = "the object table's live element count could not be read";
        return false;
    }
    if (liveElements == 0 || index >= liveElements) {
        why = "its index " + std::to_string(index) + " is outside the table's " +
              std::to_string(liveElements) + " live element(s)";
        return false;
    }
    const std::uint32_t chunkIndex = index / offsets::kElementsPerChunk;
    const std::uint32_t within = index % offsets::kElementsPerChunk;
    std::uint64_t chunk = 0;
    if (!try_read_u64(static_cast<std::uintptr_t>(chunkTable) +
                          static_cast<std::uintptr_t>(chunkIndex) * 8, chunk) || chunk < 0x10000) {
        why = "chunk " + std::to_string(chunkIndex) + " of the object table is not a pointer";
        return false;
    }
    const std::uintptr_t item = static_cast<std::uintptr_t>(chunk) +
                                static_cast<std::uintptr_t>(within) * offsets::kObjectItemStride;
    std::uint32_t serial = 0;
    if (!try_read_u32(item + 0x10, serial)) {
        why = "the serial number at of its object-table item could not be read (item 0x" +
              std::to_string(static_cast<unsigned>(item)) + ")";
        return false;
    }
    indexOut = static_cast<std::int32_t>(index);
    serialOut = static_cast<std::int32_t>(serial);
    return true;
}

bool ai_find_puppet(AiNudgeCtx& ctx, std::string& note) {
    const std::uintptr_t actorClass = resolve_uclass_address("Actor");
    FunctionRef location;
    if (actorClass != 0) location = reflect_resolve_function(actorClass, "K2_GetActorLocation");
    std::string machineNote;
    const std::vector<AggroMachine> machines = collect_machines(location, ctx.cache, machineNote);
    for (const AggroMachine& m : machines) {
        if (m.hasController) continue;
        ctx.puppet = m.actor;
        ctx.puppetName = m.name;
        ctx.puppetLoc = m.hasLocation;
        ctx.px = m.x;
        ctx.py = m.y;
        ctx.pz = m.z;
        break;
    }
    if (ctx.puppet < 0x10000) {
        note = "there is NO machine without a controller in the levels -- the puppet is built per session, so "
               "run this on the HOST while a session is running";
        return false;
    }
    return true;
}

bool ai_find_enemy(AiNudgeCtx& ctx, const std::string& part, std::string& note) {
    const EnemySurvey survey = survey_enemies();
    if (!survey.worldFound) {
        note = "the live world is not known: " + survey.worldNote;
        return false;
    }
    std::vector<const EnemyRecord*> matches;
    for (const EnemyRecord& e : survey.enemies) {
        if (e.actorName.empty()) continue;
        if (!npc_exclusion_reason(e.actorClass, e.controllerClass).empty()) continue;
        if (part.empty() || e.actorName.find(part) != std::string::npos) matches.push_back(&e);
    }
    if (matches.empty()) {
        note = part.empty() ? "there is no AI-driven character in the levels"
                            : ("nothing matched '" + part + "' among the AI-driven characters");
        return false;
    }
    if (matches.size() > 1) {
        std::string names;
        for (const EnemyRecord* e : matches) {
            if (!names.empty()) names += ", ";
            names += "'" + e->actorName + "'";
        }
        note = "'" + part + "' matches " + std::to_string(matches.size()) + " characters: " + names +
               " -- name one of them";
        return false;
    }
    const EnemyRecord& e = *matches.front();
    ctx.enemy = e.actor;
    ctx.enemyName = e.actorName;
    ctx.enemyClass = e.actorClass;
    ctx.controller = e.controller;
    if (ctx.controller < 0x10000) {
        note = "'" + ctx.enemyName + "' has no controller";
        return false;
    }
    std::uint64_t targeting = 0;
    if (try_read_u64(ctx.controller + kControllerTargeting, targeting) && targeting >= 0x10000) {
        ctx.targeting = static_cast<std::uintptr_t>(targeting);
    }
    std::uint64_t perception = 0;
    if (try_read_u64(ctx.controller + kAiControllerPerception, perception) && perception >= 0x10000) {
        ctx.perception = static_cast<std::uintptr_t>(perception);
    }
    const std::uintptr_t actorClass = resolve_uclass_address("Actor");
    const FunctionRef location =
        actorClass != 0 ? reflect_resolve_function(actorClass, "K2_GetActorLocation") : FunctionRef{};
    std::string locationNote;
    ctx.enemyLoc = read_actor_location(ctx.enemy, location, ctx.ex, ctx.ey, ctx.ez, locationNote);
    if (!ctx.enemyLoc) note = locationNote;
    return true;
}

void ai_take_snapshot(const AiNudgeCtx& ctx, AiTargetSnapshot& s) {
    if (ctx.perception >= 0x10000 && ctx.knownPerceived.usable) {
        unsigned char frame[0x20] = {};
        std::string note;
        if (reflect_invoke_function(ctx.perception, ctx.knownPerceived, frame, note)) {
            ArrayHeader known;
            if (read_tarray(reinterpret_cast<std::uintptr_t>(frame) + 0x08, 65536, known)) {
                s.perceptionRead = true;
                s.perceived = known.num;
                for (std::uint32_t i = 0; i < known.num; ++i) {
                    std::uint64_t raw = 0;
                    if (!try_read_u64(known.data + static_cast<std::uintptr_t>(i) * 8, raw)) continue;
                    if (raw >= 0x10000 &&
                        name_of_object(static_cast<std::uintptr_t>(raw)) == ctx.puppetName) {
                        s.seesPuppet = true;
                    }
                }
            }
        }
    }
    if (ctx.targeting >= 0x10000) {
        ArrayHeader list;
        if (read_tarray(ctx.targeting + kTargetingTargetDataList, 4096, list)) {
            s.listRead = true;
            for (std::uint32_t i = 0; i < list.num; ++i) {
                const std::uintptr_t entry = list.data + static_cast<std::uintptr_t>(i) * kScoringStride;
                std::uint64_t raw = 0;
                if (!try_read_u64(entry, raw) || raw < 0x10000) continue;
                if (name_of_object(static_cast<std::uintptr_t>(raw)) != ctx.puppetName) continue;
                s.puppetCandidate = true;
                std::uint8_t filtered = 0;
                s.puppetFiltered = try_read_bytes(entry + kScoringFiltered, &filtered, 1) && filtered != 0;
                float score = 0.0f;
                s.puppetScoreKnown = read_f32(entry + kScoringScore, score);
                s.puppetScore = score;
            }
        }
        if (ctx.mainTarget.usable) {
            unsigned char ret[0x10] = {};
            std::string note;
            if (reflect_invoke_function(ctx.targeting, ctx.mainTarget, ret, note)) {
                std::uint64_t held = 0;
                if (try_read_u64(reinterpret_cast<std::uintptr_t>(ret), held) && held >= 0x10000) {
                    s.mainTarget = name_of_object(static_cast<std::uintptr_t>(held));
                }
            }
        }
    }
    if (ctx.controller >= 0x10000) {
        std::uint8_t state = 0;
        if (try_read_bytes(ctx.controller + kAiControllerState, &state, 1)) s.state = state;
    }
}

void ai_nudge_resolve(AiNudgeCtx& ctx) {
    const std::uintptr_t targetingClass = resolve_uclass_address("NPTargetingSystemComponent");
    if (targetingClass != 0) ctx.mainTarget = reflect_resolve_function(targetingClass, "GetMainTarget");
    const std::uintptr_t perceptionClass = resolve_uclass_address("AIPerceptionComponent");
    if (perceptionClass != 0) {
        ctx.knownPerceived = reflect_resolve_function(perceptionClass, "GetKnownPerceivedActors");
    }
    const std::uintptr_t aiClass = resolve_uclass_address("DAAIController");
    if (aiClass != 0) ctx.modifyTarget = reflect_resolve_function(aiClass, "ModifyTargetingSystemTarget");
}

}

AiNudgeResult ai_push_perception(const std::string& enemyPart) {
    AiNudgeResult out;
    AiNudgeCtx ctx;
    ai_nudge_resolve(ctx);
    if (!ai_find_puppet(ctx, out.note)) return out;
    if (!ai_find_enemy(ctx, enemyPart, out.note)) return out;
    out.found = true;
    out.enemyName = ctx.enemyName;
    ai_take_snapshot(ctx, out.before);
    if (!ctx.modifyTarget.usable) {
        out.note = "ModifyTargetingSystemTarget could not be resolved: " + ctx.modifyTarget.note;
        return out;
    }
    if (!ctx.puppetLoc || !ctx.enemyLoc) {
        out.note = std::string("the stimulus needs both positions; ") +
                   (ctx.puppetLoc ? "the enemy's is missing" : "the puppet's is missing");
        return out;
    }
    std::int32_t weakIndex = 0;
    std::int32_t weakSerial = 0;
    std::string weakWhy;
    if (!ai_weak_pointer_for(ctx.puppet, weakIndex, weakSerial, weakWhy)) {
        out.note = "the puppet's weak pointer could not be built: " + weakWhy;
        return out;
    }
    alignas(16) unsigned char frame[kAiModifyTargetFrame] = {};
    const std::int32_t targetId = 0;
    std::memcpy(frame + 0x00, &targetId, sizeof(targetId));
    std::memcpy(frame + kAiWeakIndex, &weakIndex, sizeof(weakIndex));
    std::memcpy(frame + kAiWeakSerial, &weakSerial, sizeof(weakSerial));
    const float age = 0.0f, expire = 5.0f, strength = 1.0f;
    std::memcpy(frame + kAiStimulusBase + 0x00, &age, 4);
    std::memcpy(frame + kAiStimulusBase + 0x04, &expire, 4);
    std::memcpy(frame + kAiStimulusBase + 0x08, &strength, 4);
    std::memcpy(frame + kAiStimulusBase + 0x0C, &ctx.px, 4);
    std::memcpy(frame + kAiStimulusBase + 0x10, &ctx.py, 4);
    std::memcpy(frame + kAiStimulusBase + 0x14, &ctx.pz, 4);
    std::memcpy(frame + kAiStimulusBase + 0x18, &ctx.ex, 4);
    std::memcpy(frame + kAiStimulusBase + 0x1C, &ctx.ey, 4);
    std::memcpy(frame + kAiStimulusBase + 0x20, &ctx.ez, 4);
    frame[kAiStimulusBase + 0x38] = 0x02;
    std::string callNote;
    if (!reflect_invoke_function(ctx.controller, ctx.modifyTarget, frame, callNote)) {
        out.note = "ModifyTargetingSystemTarget was NOT called: " + callNote;
        return out;
    }
    out.callMade = true;
    ai_take_snapshot(ctx, out.after);
    out.afterTaken = true;
    return out;
}

AiNudgeResult ai_set_enemy_friendlies(const std::string& enemyPart, bool enable) {
    AiNudgeResult out;
    AiNudgeCtx ctx;
    ai_nudge_resolve(ctx);
    if (!ai_find_puppet(ctx, out.note)) return out;
    if (!ai_find_enemy(ctx, enemyPart, out.note)) return out;
    out.found = true;
    out.enemyName = ctx.enemyName;
    ai_take_snapshot(ctx, out.before);
    if (ctx.perception < 0x10000) {
        out.note = "'" + ctx.enemyName + "' has no perception component at";
        return out;
    }
    ArrayHeader configs;
    if (!read_tarray(ctx.perception + kAiSensesConfig, 64, configs)) {
        out.note = "SensesConfig could not be read as a TArray";
        return out;
    }
    std::uint32_t flipped = 0;
    std::string perConfig;
    for (std::uint32_t i = 0; i < configs.num; ++i) {
        std::uint64_t raw = 0;
        if (!try_read_u64(configs.data + static_cast<std::uintptr_t>(i) * 8, raw) || raw < 0x10000) continue;
        const auto config = static_cast<std::uintptr_t>(raw);
        std::uint64_t cls = 0;
        if (!try_read_u64(config + offsets::kUObjectClassOffset, cls) || cls < 0x10000) continue;
        const auto clsPtr = static_cast<std::uintptr_t>(cls);

        std::uint32_t affiliationAt = 0;
        const char* which = nullptr;
        if (class_derives_from_named(clsPtr, "AISenseConfig_DASight", ctx.cache)) {
            affiliationAt = 0x0060;
            which = "DASight";
        } else if (class_derives_from_named(clsPtr, "AISenseConfig_Approach", ctx.cache)) {
            affiliationAt = 0x0058;
            which = "Approach";
        }
        if (which == nullptr) {
            perConfig += " " + object_class_name(config) + "(class 0x" + object_class_name(config) +
                         ", derives DASight=" + (class_derives_from_named(clsPtr, "AISenseConfig_DASight", ctx.cache) ? "1" : "0") +
                         " Approach=" + (class_derives_from_named(clsPtr, "AISenseConfig_Approach", ctx.cache) ? "1" : "0") +
                         ", untouched)";
            continue;
        }
        std::uint8_t before = 0;
        if (!try_read_bytes(config + affiliationAt, &before, 1)) continue;
        const std::uint8_t after = static_cast<std::uint8_t>(
            enable ? (before | kAiSenseFriendliesBit) : (before & ~kAiSenseFriendliesBit));
        if (!out.affiliationKnown) {
            out.affiliationKnown = true;
            out.affiliationBefore = before;
        }
        if (after != before) {
            if (!write_process_bytes(config + affiliationAt, &after, 1)) {
                out.note = "the affiliation byte could not be written";
                return out;
            }
            ++flipped;
        }
        std::uint8_t check = 0;
        if (try_read_bytes(config + affiliationAt, &check, 1)) out.affiliationAfter = check;
        char buf[128];
        std::snprintf(buf, sizeof(buf), " %s@%02X 0x%02X->0x%02X", which,
                      static_cast<unsigned>(affiliationAt), static_cast<unsigned>(before),
                      static_cast<unsigned>(check));
        perConfig += buf;
        out.callMade = true;
    }
    if (!out.affiliationKnown) {
        out.note = "'" + ctx.enemyName + "' has no sense with an affiliation filter (only: " + perConfig + " )";
        return out;
    }
    out.changed = flipped != 0;
    out.note = std::to_string(flipped) + " affiliation byte(s) written:" + perConfig;
    ai_take_snapshot(ctx, out.after);
    out.afterTaken = true;
    return out;
}

AiNudgeResult ai_register_puppet(const std::string& enemyPart, bool enable) {
    AiNudgeResult out;
    AiNudgeCtx ctx;
    ai_nudge_resolve(ctx);
    if (!ai_find_puppet(ctx, out.note)) return out;
    if (!ai_find_enemy(ctx, enemyPart, out.note)) return out;
    out.found = true;
    out.enemyName = ctx.enemyName;
    ai_take_snapshot(ctx, out.before);
    const StaticFunctionRef fn = reflect_resolve_static("DAGameplayStatics",
        enable ? "RegisterSourceToAIPerceptionSystem" : "UnregisterSourceToAIPerceptionSystem");
    if (!fn.usable) {
        out.note = std::string(enable ? "RegisterSourceToAIPerceptionSystem" : "UnregisterSourceToAIPerceptionSystem") +
                   " could not be resolved: " + fn.note;
        return out;
    }
    ArrayHeader configs;
    if (ctx.perception < 0x10000 || !read_tarray(ctx.perception + kAiSensesConfig, 64, configs)) {
        out.note = "the enemy's SensesConfig could not be read, so the sense classes are unknown";
        return out;
    }
    std::vector<std::uintptr_t> senses;
    std::string seen;
    for (std::uint32_t i = 0; i < configs.num; ++i) {
        std::uint64_t raw = 0;
        if (!try_read_u64(configs.data + static_cast<std::uintptr_t>(i) * 8, raw) || raw < 0x10000) continue;
        const auto config = static_cast<std::uintptr_t>(raw);
        std::uint64_t impl = 0;
        if (!try_read_u64(config + 0x0048, impl) || impl < 0x10000) continue;
        const std::string senseClass = name_of_object(static_cast<std::uintptr_t>(impl));
        seen += (seen.empty() ? "" : ", ") + object_class_name(config) + " -> " + senseClass;
        if (senseClass.find("AISense") == std::string::npos) continue;
        bool already = false;
        for (const std::uintptr_t s : senses) {
            if (s == static_cast<std::uintptr_t>(impl)) already = true;
        }
        if (!already) senses.push_back(static_cast<std::uintptr_t>(impl));
    }
    if (senses.empty()) {
        out.note = "the enemy's senses name no AISense_* class (" + seen + "), so there is nothing to register";
        return out;
    }
    std::string calls;
    for (const std::uintptr_t sense : senses) {
        alignas(16) unsigned char frame[0x10] = {};
        const std::uint64_t actor = ctx.puppet;
        const std::uint64_t senseArg = sense;
        std::memcpy(frame + 0x00, &actor, sizeof(actor));
        std::memcpy(frame + 0x08, &senseArg, sizeof(senseArg));
        std::string note;
        const bool made = reflect_invoke_function(fn.cdo, fn.fn, frame, note);
        calls += (calls.empty() ? "" : ", ") + name_of_object(sense) + (made ? " CALLED" : (" NOT called: " + note));
        if (made) out.callMade = true;
    }
    out.note = std::string(enable ? "registered" : "unregistered") + " the puppet for: " + calls +
               " (the enemy listens with: " + seen + ")";
    ai_take_snapshot(ctx, out.after);
    out.afterTaken = true;
    return out;
}

namespace {

constexpr int kAiTeamsToProbe[] = {0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 100, 110, 120, 255};

}

AiAttitudeTable ai_team_attitudes_towards_no_team() {
    AiAttitudeTable out;
    const StaticFunctionRef fn = reflect_resolve_static("DAGameplayStatics", "GetDATeamAttitude");
    if (!fn.usable) {
        out.note = "GetDATeamAttitude could not be resolved: " + fn.note;
        return out;
    }
    out.usable = true;
    for (const int team : kAiTeamsToProbe) {
        alignas(16) unsigned char frame[0x10] = {};
        std::uint8_t a = static_cast<std::uint8_t>(team);
        std::uint8_t b = static_cast<std::uint8_t>(255);
        std::memcpy(frame + 0x00, &a, 1);
        std::memcpy(frame + 0x01, &b, 1);
        std::string note;
        if (!reflect_invoke_function(fn.cdo, fn.fn, frame, note)) continue;
        std::uint8_t attitude = 0xFF;
        if (!try_read_bytes(reinterpret_cast<std::uintptr_t>(frame) + 0x02, &attitude, 1)) continue;
        ++out.readCount;
        if (attitude == 2) {
            ++out.hostileCount;
            AiAttitudeRow row;
            row.team = team;
            row.teamName = dat_team_name(team);
            row.attitude = attitude;
            out.rows.push_back(std::move(row));
        }
    }
    if (out.readCount == 0) out.note = "not one call answered";
    return out;
}

std::vector<std::string> AiAttitudeTable::report_lines() const {
    std::vector<std::string> out;
    if (!usable) {
        out.push_back("ai: " + note);
        return out;
    }
    out.push_back("ai: the game's own team table, asked " + std::to_string(readCount) +
                  " time(s): 'attitude towards NoTeam(255)' is HOSTILE for " + std::to_string(hostileCount) +
                  " team(s)" + (note.empty() ? "" : (" [" + note + "]")));
    for (const AiAttitudeRow& row : rows) {
        out.push_back("ai: " + row.teamName + " -> Hostile(2)" +
                      (row.team == 0 ? " <-- Player(0) is hostile to a controller-less machine"
                                     : ""));
    }
    if (hostileCount == 0) {
        out.push_back("ai: NOT ONE of the 15 teams treats NoTeam as hostile -- so no team-and-attitude "
                      "trick can make the puppet a target, and forcing is the only path left");
    }
    return out;
}

AiNudgeResult ai_set_enemy_team(const std::string& enemyPart, int team) {
    AiNudgeResult out;
    if (team < 0 || team > 255) {
        out.note = "the team must be a value of the game's own EDATeam";
        return out;
    }
    AiNudgeCtx ctx;
    ai_nudge_resolve(ctx);
    if (!ai_find_puppet(ctx, out.note)) return out;
    if (!ai_find_enemy(ctx, enemyPart, out.note)) return out;
    out.found = true;
    out.enemyName = ctx.enemyName;
    ai_take_snapshot(ctx, out.before);
    const StaticFunctionRef get = reflect_resolve_static("DAGameplayStatics", "GetDATeam");
    const StaticFunctionRef set = reflect_resolve_static("DAGameplayStatics", "SetDATeam");
    if (!set.usable) {
        out.note = "SetDATeam could not be resolved: " + set.note;
        return out;
    }
    auto read_team = [&](std::uintptr_t actor, int& value) -> bool {
        if (!get.usable) return false;
        alignas(16) unsigned char frame[0x10] = {};
        const std::uint64_t argument = actor;
        std::memcpy(frame + 0x00, &argument, sizeof(argument));
        std::string note;
        if (!reflect_invoke_function(get.cdo, get.fn, frame, note)) return false;
        std::uint8_t raw = 0;
        if (!try_read_bytes(reinterpret_cast<std::uintptr_t>(frame) + 0x08, &raw, 1)) return false;
        value = static_cast<int>(raw);
        return true;
    };
    int before = -1;
    const bool beforeKnown = read_team(ctx.enemy, before);
    alignas(16) unsigned char frame[0x10] = {};
    const std::uint64_t argument = ctx.enemy;
    std::memcpy(frame + 0x00, &argument, sizeof(argument));
    std::uint8_t teamByte = static_cast<std::uint8_t>(team & 0xFF);
    std::memcpy(frame + 0x08, &teamByte, 1);
    std::string note;
    if (!reflect_invoke_function(set.cdo, set.fn, frame, note)) {
        out.note = "SetDATeam was NOT called: " + note;
        return out;
    }
    out.callMade = true;
    std::uint8_t returned = 0;
    if (try_read_bytes(reinterpret_cast<std::uintptr_t>(frame) + 0x09, &returned, 1)) {
        out.changed = returned != 0;
    }
    int after = -1;
    const bool afterKnown = read_team(ctx.enemy, after);
    std::string detail = "enemy team " + std::string(beforeKnown ? dat_team_name(before) : "NOT READ") +
                         " -> asked " + dat_team_name(team) + ", returned " +
                         (out.changed ? "TRUE" : "FALSE") + ", now reads " +
                         (afterKnown ? dat_team_name(after) : "NOT READ");
    ai_take_snapshot(ctx, out.after);
    out.afterTaken = true;
    out.note = detail;
    return out;
}

AiNudgeResult ai_report_damage_to_enemy(const std::string& enemyPart, float amount) {
    AiNudgeResult out;
    AiNudgeCtx ctx;
    ai_nudge_resolve(ctx);
    if (!ai_find_puppet(ctx, out.note)) return out;
    if (!ai_find_enemy(ctx, enemyPart, out.note)) return out;
    out.found = true;
    out.enemyName = ctx.enemyName;
    ai_take_snapshot(ctx, out.before);
    const StaticFunctionRef fn = reflect_resolve_static("AISense_Damage", "ReportDamageEvent");
    if (!fn.usable) {
        out.note = "UAISense_Damage::ReportDamageEvent could not be resolved: " + fn.note;
        return out;
    }
    if (!ctx.puppetLoc || !ctx.enemyLoc) {
        out.note = std::string("the event needs both positions; ") +
                   (ctx.puppetLoc ? "the enemy's is missing" : "the puppet's is missing");
        return out;
    }
    const std::uintptr_t world = live_world_address();
    if (world < 0x10000) {
        out.note = "the live world is not known, and the event needs it as its context";
        return out;
    }
    alignas(16) unsigned char frame[0x40] = {};
    const std::uint64_t context = world;
    const std::uint64_t damaged = ctx.enemy;
    const std::uint64_t instigator = ctx.puppet;
    const float damage = (amount > 0.0f ? amount : 1.0f);
    std::memcpy(frame + 0x00, &context, sizeof(context));
    std::memcpy(frame + 0x08, &damaged, sizeof(damaged));
    std::memcpy(frame + 0x10, &instigator, sizeof(instigator));
    std::memcpy(frame + 0x18, &damage, 4);
    std::memcpy(frame + 0x1C, &ctx.px, 4);
    std::memcpy(frame + 0x20, &ctx.py, 4);
    std::memcpy(frame + 0x24, &ctx.pz, 4);
    std::memcpy(frame + 0x28, &ctx.ex, 4);
    std::memcpy(frame + 0x2C, &ctx.ey, 4);
    std::memcpy(frame + 0x30, &ctx.ez, 4);
    std::string note;
    if (!reflect_invoke_function(fn.cdo, fn.fn, frame, note)) {
        out.note = "ReportDamageEvent was NOT called: " + note;
        return out;
    }
    out.callMade = true;
    out.note = "filed a damage stimulus: '" + ctx.enemyName + "' was told it was damaged by '" +
               ctx.puppetName + "' (amount " + std::to_string(static_cast<int>(damage)) +
               ", NO health was changed)";
    ai_take_snapshot(ctx, out.after);
    out.afterTaken = true;
    return out;
}

std::vector<std::string> AiNudgeResult::report_lines() const {
    std::vector<std::string> out;
    if (!found) {
        out.push_back("ai: " + note);
        return out;
    }
    char buf[640];
    std::snprintf(buf, sizeof(buf), "ai: enemy '%s'%s", enemyName.c_str(),
                  note.empty() ? "" : (" -- " + note).c_str());
    out.push_back(buf);
    const auto row = [&](const char* what, const AiTargetSnapshot& s) {
        std::string mainText = s.mainTarget.empty() ? std::string("NOTHING") : ("'" + s.mainTarget + "'");
        std::snprintf(buf, sizeof(buf),
                      "ai: %-6s perception %s %u %s | candidate %s | main target %s | state %s", what,
                      s.perceptionRead ? "read" : "NOT READ", s.perceived,
                      s.seesPuppet ? "and it KNOWS THE PUPPET" : "and it does not know the puppet",
                      s.listRead ? (s.puppetCandidate
                                        ? (s.puppetFiltered ? "PRESENT but bFiltered" : "PRESENT, not filtered")
                                        : "absent")
                                 : "NOT READ",
                      mainText.c_str(), s.state < 0 ? "NOT READ" : ai_behavior_name(s.state));
        out.push_back(buf);
    };
    row("before", before);
    if (afterTaken) {
        row("after", after);
    } else {
        out.push_back("ai: after NOT TAKEN -- the experiment never ran, so nothing was changed");
    }
    if (after.seesPuppet && !before.seesPuppet) {
        out.push_back("ai: THE PERCEPTION LIST GAINED THE PUPPET");
    } else if (before.seesPuppet && !after.seesPuppet) {
        out.push_back("ai: the perception list LOST the puppet");
    }
    if (after.puppetCandidate && !before.puppetCandidate) {
        out.push_back("ai: THE CANDIDATE LIST GAINED THE PUPPET" +
                      (after.puppetScoreKnown
                           ? (", score " + std::to_string(static_cast<double>(after.puppetScore)))
                           : std::string()));
    }
    if (after.mainTarget != before.mainTarget) out.push_back("ai: THE MAIN TARGET MOVED");
    return out;
}

}
