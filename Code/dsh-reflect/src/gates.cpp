#include "dsh/reflect/gates.h"

#include "dsh/reflect/scanner.h"
#include "dsh/reflect/offsets.h"
#include "dsh/reflect/spawn.h"
#include "dsh/reflect/world.h"

#include <algorithm>
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
constexpr std::uint32_t kMaxLevels = 4096;
constexpr std::uint32_t kMaxActorsPerLevel = 200000;
constexpr std::uint32_t kLocationFrameBytes = 0x10;

std::uint64_t read_u64(std::uintptr_t addr) {
    std::uint64_t v = 0;
    if (!try_read_u64(addr, v)) return 0;
    return v;
}

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
    if (num > limit || max < num || data < 0x10000) return false;
    out.data = static_cast<std::uintptr_t>(data);
    out.num = num;
    out.max = max;
    return true;
}

bool name_is_gate_ish(const std::string& cls) {
    static const char* const kWords[] = {"DigitalWall", "Gate",   "Door",   "Portal",
                                        "Elevator",    "Wall",   "Shutter"};
    for (const char* w : kWords) {
        if (cls.find(w) != std::string::npos) return true;
    }
    return false;
}

}

GateSurvey survey_gates() {
    GateSurvey s;

    const LiveObjects live = find_live_objects();
    if (live.world == 0 || live.level == 0) {
        s.note = "the live world or its persistent level is not known, so no level was walked";
        return s;
    }
    s.worldFound = true;

    ArrayHeader levels;
    if (!read_tarray(live.world + kWorldLevels, kMaxLevels, levels)) {
        s.note = "UWorld::Levels could not be read, so no level was walked";
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

    std::unordered_map<std::string, std::uint32_t> candidates;
    std::uint64_t actorsSeen = 0;

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
            if (cls.empty()) continue;
            if (!name_is_gate_ish(cls)) continue;
            ++candidates[cls];

            GateActor g;
            g.actor = actor;
            g.actorClass = cls;
            g.actorName = name_of_object(actor);

            if (getLocation.usable) {
                unsigned char frame[kLocationFrameBytes] = {0};
                std::string note;
                if (!reflect_invoke_function(actor, getLocation, frame, note)) {
                    g.locationNote = "the call was NOT made: " + note;
                } else {
                    float xyz[3] = {0.0f, 0.0f, 0.0f};
                    if (try_read_bytes(reinterpret_cast<std::uintptr_t>(frame), xyz, sizeof(xyz))) {
                        g.x = xyz[0];
                        g.y = xyz[1];
                        g.z = xyz[2];
                        g.locationRead = true;
                    } else {
                        g.locationNote = "K2_GetActorLocation WAS called but its answer could not be read";
                    }
                }
            } else {
                g.locationNote = "no location was read: K2_GetActorLocation is not resolvable (" +
                                 getLocation.note + ")";
            }
            s.actors.push_back(std::move(g));
        }
    }

    s.actorsWalked = actorsSeen;
    s.classHistogram.assign(candidates.begin(), candidates.end());
    std::sort(s.classHistogram.begin(), s.classHistogram.end(),
              [](const std::pair<std::string, std::uint32_t>& a,
                 const std::pair<std::string, std::uint32_t>& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });
    return s;
}

std::vector<std::string> GateSurvey::report_lines() const {
    std::vector<std::string> out;
    char buf[512];

    std::snprintf(buf, sizeof(buf), "gates: %u level(s) walked, %llu actor(s) seen; %zu door/wall/gate "
                                    "actor(s) found",
                  levelsWalked, static_cast<unsigned long long>(actorsWalked), actors.size());
    out.push_back(buf);
    if (!note.empty()) out.push_back("gates: " + note);

    if (!classHistogram.empty()) {
        std::string line = "gates: classes matched (name contains a door/wall/gate word):";
        for (std::size_t i = 0; i < classHistogram.size() && i < 14; ++i) {
            line += " " + classHistogram[i].first + "=" + std::to_string(classHistogram[i].second);
        }
        out.push_back(line);
    } else if (worldFound) {
        out.push_back("gates: NO class name matched -- so either this area holds no door actor, or the "
                      "actors are named with a word this build does not know. The class of the thing the "
                      "operator is looking at is what has to be found next.");
    }

    for (std::size_t i = 0; i < actors.size(); ++i) {
        const GateActor& g = actors[i];
        std::string line = " [" + std::to_string(i + 1) + "] " + g.actorClass + " '" + g.actorName + "'";
        if (g.locationRead) {
            std::snprintf(buf, sizeof(buf), " at (%.1f, %.1f, %.1f) cm", static_cast<double>(g.x),
                          static_cast<double>(g.y), static_cast<double>(g.z));
            line += buf;
        } else {
            line += " at a location that was NOT MEASURED (" + g.locationNote + ")";
        }
        std::snprintf(buf, sizeof(buf), " @0x%llX", static_cast<unsigned long long>(g.actor));
        line += buf;
        out.push_back(line);
    }
    out.push_back("gates: the class NAME above is the thing to look up next -- this command deliberately "
                  "does not guess at an open/close verb, because a wrong function name is a crash rather "
                  "than a wrong number.");
    return out;
}

}
