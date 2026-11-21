#include "dsh/reflect/mission.h"
#include "dsh/reflect/enemies.h"

#include "dsh/reflect/gamethread.h"
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

#include <windows.h>

namespace dsh::reflect {
namespace {

using namespace dsh::reflect::offsets;

constexpr std::uint32_t kWorldLevels   = 0x0138;
constexpr std::uint32_t kLevelActors   = 0x0098;
constexpr std::uint32_t kMissionType   = 0x0268;
constexpr std::uint32_t kMissionId     = 0x026C;
constexpr std::uint32_t kEmsSlotName   = 0x0060;
constexpr std::uint32_t kCustomSaveName = 0x0028;

constexpr std::uint32_t kBossStateFrameBytes = 0x10;
constexpr std::uint32_t kBossStateReturnAt   = 0x08;
constexpr std::uint32_t kDefeatedFrameBytes  = 0x10;
constexpr std::uint32_t kDefeatedReturnAt    = 0x00;

constexpr std::uint32_t kMaxLevels         = 4096;
constexpr std::uint32_t kMaxActorsPerLevel = 200000;
constexpr std::uint32_t kMaxObjectElements = 5000000;
constexpr std::uint32_t kMaxFStringChars   = 512;
constexpr int           kMaxSuperChain     = 32;

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

bool read_tarray(std::uintptr_t addr, std::uint32_t limit, ArrayHeader& out, std::string& note) {
    const std::uint64_t data = read_u64(addr);
    std::uint32_t num = 0;
    std::uint32_t max = 0;
    if (!try_read_u32(addr + 8, num) || !try_read_u32(addr + 12, max)) {
        note = "the array header is not readable";
        return false;
    }
    if (num == 0) {
        out = ArrayHeader{};
        return true;
    }
    if (num > limit) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "the array's element count %u is above the sane bound %u, so its elements were "
                      "NOT read (the layout is not what this file thinks it is)", num, limit);
        note = buf;
        return false;
    }
    if (max < num || data < 0x10000) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "the array header is inconsistent (data=0x%llX num=%u max=%u), so its elements "
                      "were NOT read", static_cast<unsigned long long>(data), num, max);
        note = buf;
        return false;
    }
    out.data = static_cast<std::uintptr_t>(data);
    out.num = num;
    out.max = max;
    return true;
}

bool read_fstring(std::uintptr_t addr, std::string& out, std::string& note) {
    const std::uint64_t data = read_u64(addr);
    std::uint32_t num = 0;
    std::uint32_t max = 0;
    if (!try_read_u32(addr + 8, num) || !try_read_u32(addr + 12, max)) {
        note = "the FString header is not readable";
        return false;
    }
    if (num == 0) {
        out.clear();
        return true;
    }
    if (num > kMaxFStringChars || max < num || data < 0x10000) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "the FString header is implausible (data=0x%llX num=%u max=%u), so its characters "
                      "were NOT read", static_cast<unsigned long long>(data), num, max);
        note = buf;
        return false;
    }
    std::vector<wchar_t> chars(num, L'\0');
    if (!try_read_bytes(static_cast<std::uintptr_t>(data), chars.data(),
                        chars.size() * sizeof(wchar_t))) {
        note = "the FString's characters are not readable";
        return false;
    }

    for (const wchar_t c : chars) {
        if (c == 0) break;
        out.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
    }
    return true;
}

bool contains_ci(const std::string& haystack, const char* needle) {
    const std::string n = needle != nullptr ? std::string(needle) : std::string();
    if (n.empty() || haystack.size() < n.size()) return false;
    for (std::size_t i = 0; i + n.size() <= haystack.size(); ++i) {
        bool same = true;
        for (std::size_t j = 0; j < n.size(); ++j) {
            const char a = haystack[i + j];
            const char b = n[j];
            const char la = (a >= 'a' && a <= 'z') ? static_cast<char>(a - 'a' + 'A') : a;
            const char lb = (b >= 'a' && b <= 'z') ? static_cast<char>(b - 'a' + 'A') : b;
            if (la != lb) {
                same = false;
                break;
            }
        }
        if (same) return true;
    }
    return false;
}

template <typename Fn>
std::uint32_t for_each_object_address(Fn&& fn) {
    const std::uintptr_t base = main_module_base();
    if (base == 0) return 0;
    const std::uintptr_t array = base + static_cast<std::uintptr_t>(kGObjects);

    const std::uint64_t chunkTable = read_u64(array + kObjectsOffset);
    std::uint32_t numElements = 0;
    if (chunkTable < 0x10000) return 0;
    if (!try_read_u32(array + kNumElementsOffset, numElements)) return 0;
    if (numElements > kMaxObjectElements) return 0;

    for (std::uint32_t i = 0; i < numElements; ++i) {
        const std::uint32_t chunkIndex = i / kElementsPerChunk;
        const std::uint32_t within = i % kElementsPerChunk;
        const std::uint64_t chunk =
            read_u64(static_cast<std::uintptr_t>(chunkTable) +
                     static_cast<std::uintptr_t>(chunkIndex) * 8);
        if (chunk < 0x10000) continue;
        const std::uint64_t obj = read_u64(static_cast<std::uintptr_t>(chunk) +
                                           static_cast<std::uintptr_t>(within) * kObjectItemStride);
        if (obj < 0x10000) continue;
        fn(static_cast<std::uintptr_t>(obj));
    }
    return numElements;
}

bool class_reaches_mission_volume(std::uintptr_t cls,
                                  std::unordered_map<std::uintptr_t, bool>& cache) {
    if (cls < 0x10000) return false;
    const auto it = cache.find(cls);
    if (it != cache.end()) return it->second;

    bool found = false;
    std::uintptr_t cur = cls;
    for (int depth = 0; depth < kMaxSuperChain; ++depth) {
        if (cur < 0x10000) break;
        if (name_of_object(cur) == "DAMissionVolume") {
            found = true;
            break;
        }
        const std::uint64_t super = read_u64(cur + kUStructSuperOffset);
        if (super == 0) break;
        cur = static_cast<std::uintptr_t>(super);
    }

    cache.emplace(cls, found);
    return found;
}

bool class_reaches_named(std::uintptr_t cls, const char* baseName,
                         std::unordered_map<std::uintptr_t, bool>& cache) {
    if (cls < 0x10000) return false;
    const auto it = cache.find(cls);
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

    cache.emplace(cls, found);
    return found;
}

const char* mission_type_name(std::uint8_t type) {
    switch (type) {
        case 0:  return "Boss(0)";
        case 1:  return "Arena(1)";
        default: return "?";
    }
}

const char* mission_state_name(std::uint8_t state) {
    switch (state) {
        case 0:  return "Unattempted(0)";
        case 1:  return "Attempted(1)";
        case 2:  return "Cleared(2)";
        default: return "?";
    }
}

}
MissionTable read_mission_table() {
    MissionTable t;

    std::vector<std::uintptr_t> emsObjects;
    std::vector<std::uintptr_t> saveObjects;
    const std::uint32_t elements = for_each_object_address([&](std::uintptr_t obj) {
        const std::string cls = object_class_name(obj);
        if (cls.empty()) return;
        const bool isEms = (cls == "EMSObject");
        const bool isSave = (cls == "DAPersistentSaveGame");
        if (!isEms && !isSave) return;
        const std::string name = name_of_object(obj);
        if (name.rfind("Default__", 0) == 0) return;
        if (isEms) emsObjects.push_back(obj);
        if (isSave) saveObjects.push_back(obj);
    });
    t.emsInstances = static_cast<std::uint32_t>(emsObjects.size());
    t.saveInstances = static_cast<std::uint32_t>(saveObjects.size());

    if (elements == 0) {
        t.saveNote = (t.saveNote.empty() ? "" : t.saveNote + " ");
        t.saveNote += "the object array could not be walked at all (no module base, no element count, "
                      "or an implausible one), so NEITHER the save slot NOR the save object is known";
    }

    for (const std::uintptr_t ems : emsObjects) {
        std::string name;
        std::string note;
        if (!read_fstring(ems + kEmsSlotName, name, note)) {
            if (t.slotNote.empty()) {
                t.slotNote = "the EMSObject at 0x" + std::to_string(ems) +
                             " has no readable CurrentSaveGameName: " + note;
            }
            continue;
        }
        if (name.empty()) continue;
        t.slotName = name;
        t.slotNameMeasured = true;
        break;
    }
    if (!t.slotNameMeasured && t.slotNote.empty()) {
        t.slotNote = t.emsInstances == 0
                         ? "no EMSObject was found in the object array, so WHICH SAVE IS LOADED is not "
                           "known -- and the mission state is not read without knowing it"
                         : "the EMSObject(s) were found but none carried a readable, non-empty save "
                           "slot name";
    }

    t.stateReadAllowed = true;
    if (!t.slotNameMeasured) {
        t.gateNote = "READ (slot NOT IDENTIFIED): " + t.slotNote +
                     " -- the state below is from whichever slot is loaded, and this build could not "
                     "measure its name";
    } else if (contains_ci(t.slotName, kMainSaveSlot)) {
        t.gateNote = "READ: the loaded slot is the MAIN save ('" + t.slotName +
                     "'). Allowed by the operator's ruling; no save FILE is opened by this build";
    } else if (contains_ci(t.slotName, kTestSaveSlot)) {
        t.gateNote = "READ: the loaded slot is the TEST save ('" + t.slotName + "')";
    } else {
        t.gateNote = "READ: the loaded slot is '" + t.slotName +
                     "', which is neither of the two names this build knows -- the state below is still "
                     "read, from memory only";
    }

    if (!saveObjects.empty()) {
        t.saveObject = saveObjects.front();
        t.saveObjectName = name_of_object(t.saveObject);
        t.saveClassName = object_class_name(t.saveObject);
        std::string note;
        if (read_fstring(t.saveObject + kCustomSaveName, t.saveCustomName, note)) {
            t.saveCustomNameMeasured = true;
        } else {
            t.saveCustomNameNote = "UEMSCustomSaveGame::SaveGameName is not readable: " + note;
        }
    } else {
        t.saveNote = "no `DAPersistentSaveGame` INSTANCE was found in the object array (its class "
                     "default object does not count), so there is no object to ask";
    }
    if (saveObjects.size() > 1) {
        t.saveNote += (t.saveNote.empty() ? "" : " ");
        t.saveNote += std::to_string(saveObjects.size()) +
                      " instances were found; the state is read from the first one and the others are "
                      "named here so that a wrong choice is visible: ";
        for (const std::uintptr_t s : saveObjects) {
            t.saveNote += "'" + name_of_object(s) + "' ";
        }
    }

    const LiveObjects live = find_live_objects();
    if (live.world == 0 || live.level == 0) {
        t.worldNote = (t.worldNote.empty() ? "" : t.worldNote + "; ");
        t.worldNote += "the live world or its persistent level is not known, so no level was walked";
    } else {
        t.worldFound = true;
        ArrayHeader levels;
        std::string levelNote;
        if (!read_tarray(live.world + kWorldLevels, kMaxLevels, levels, levelNote)) {
            t.worldNote = "UWorld::Levels could not be read: " + levelNote;
        } else {
            std::unordered_map<std::uintptr_t, bool> volumeClassCache;

            std::unordered_map<std::string, std::uint32_t> volumeishClasses;
            std::uint64_t actorsSeen = 0;
            for (std::uint32_t li = 0; li < levels.num; ++li) {
                const std::uint64_t levelPtr = read_u64(levels.data + li * 8);
                if (levelPtr < 0x10000) continue;
                ++t.levelsWalked;

                ArrayHeader actors;
                std::string actorNote;
                if (!read_tarray(static_cast<std::uintptr_t>(levelPtr) + kLevelActors,
                                 kMaxActorsPerLevel, actors, actorNote)) {
                    continue;
                }
                for (std::uint32_t ai = 0; ai < actors.num; ++ai) {
                    const std::uint64_t actorPtr = read_u64(actors.data + ai * 8);
                    if (actorPtr < 0x10000) continue;
                    ++actorsSeen;

                    const auto actor = static_cast<std::uintptr_t>(actorPtr);
                    const std::uint64_t classPtr = read_u64(actor + kUObjectClassOffset);
                    if (classPtr < 0x10000) continue;
                    const std::string cls = object_class_name(actor);
                    if (contains_ci(cls, "Volume")) ++volumeishClasses[cls];
                    if (!class_reaches_mission_volume(static_cast<std::uintptr_t>(classPtr),
                                                      volumeClassCache)) {
                        continue;
                    }

                    MissionVolume v;
                    v.actor = actor;
                    v.actorClass = cls;
                    v.actorName = name_of_object(actor);

                    std::uint8_t type = 0;
                    if (try_read_bytes(actor + kMissionType, &type, 1) && type <= 1) {
                        v.missionType = type;
                        v.typeMeasured = true;
                    } else {
                        v.typeNote = "the MissionType byte did not read as one of the two "
                                     "EDAMissionType values, so the type is NOT reported";
                    }

                    std::uint32_t index = 0;
                    std::uint32_t number = 0;
                    const bool headerRead =
                        try_read_bytes(actor + kMissionId, &index, 4) &&
                        try_read_bytes(actor + kMissionId + 4, &number, 4);
                    if (!headerRead || index == 0) {
                        v.missionIdNote = "the FName at did not read as a plausible name "
                                          "index, so the mission id is NOT reported";
                    } else {
                        v.missionIdIndex = index;
                        v.missionIdNumber = number;
                        std::string note;
                        if (!resolve_fname(index, v.missionId, note)) {
                            v.missionIdNote = "the FName index " + std::to_string(index) +
                                              " did not resolve to a string: " + note;
                        } else if (number != 0) {

                            v.missionIdNote = "resolved, but the FName carries number " +
                                              std::to_string(number) +
                                              " (not 0), which names a DIFFERENT FName than the same "
                                              "string with number 0";
                        }
                    }
                    t.volumes.push_back(std::move(v));
                }
            }
            t.actorsWalked = actorsSeen;
            t.volumeishClasses.assign(volumeishClasses.begin(), volumeishClasses.end());
            std::sort(t.volumeishClasses.begin(), t.volumeishClasses.end(),
                      [](const std::pair<std::string, std::uint32_t>& a,
                         const std::pair<std::string, std::uint32_t>& b) {
                          if (a.second != b.second) return a.second > b.second;
                          return a.first < b.first;
                      });
            if (levels.num == 0) {
                t.worldNote = (t.worldNote.empty() ? "" : t.worldNote + "; ");
                t.worldNote += "UWorld::Levels is present but EMPTY, so no actor was walked";
            }
        }
    }

    if (t.stateReadAllowed && t.saveObject != 0 && t.saveClassName == "DAPersistentSaveGame") {
        const std::uintptr_t declaringClass = static_cast<std::uintptr_t>(
            read_u64(t.saveObject + kUObjectClassOffset));

        const FunctionRef getBossState = reflect_resolve_function(declaringClass, "GetBossState");
        const FunctionRef getDefeated = reflect_resolve_function(declaringClass, "GetDefeatedBossNum");

        if (getBossState.usable) {
            for (MissionVolume& v : t.volumes) {
                if (!v.typeMeasured) {
                    v.stateNote = "the mission TYPE could not be read, so which accessor applies is "
                                  "unknown and no state was asked for";
                    continue;
                }
                if (v.missionId.empty()) {
                    v.stateNote = "the mission ID could not be read" +
                                  (v.missionIdNote.empty() ? std::string()
                                                           : (" (" + v.missionIdNote + ")")) +
                                  ", and the accessor is keyed by it, so no state was asked for";
                    continue;
                }
                if (v.missionType != 0) {

                    v.stateNote = "NOT MEASURED: an arena's progress lives in "
                                  "DAPersistentSaveGame::ArenaDataList ( "
                                  "TMap<FName,FDAArenaData>) and this build exposes NO accessor for it "
                                  "(DAPersistentSaveGame declares GetBossState and GetDefeatedBossNum "
                                  "and nothing else), so calling GetBossState here would return a "
                                  "default-constructed Unattempted and print a guess as a measurement";
                    continue;
                }

                unsigned char frame[kBossStateFrameBytes] = {0};
                std::uint32_t index = v.missionIdIndex;
                std::uint32_t number = v.missionIdNumber;
                std::memcpy(frame + 0x00, &index, 4);
                std::memcpy(frame + 0x04, &number, 4);

                std::string note;
                if (!reflect_invoke_function(t.saveObject, getBossState, frame, note)) {
                    v.stateNote = "the call was NOT made: " + note;
                    continue;
                }
                std::uint8_t state = 0;
                if (!try_read_bytes(reinterpret_cast<std::uintptr_t>(frame) + kBossStateReturnAt,
                                    &state, 1) ||
                    state > 2) {
                    v.stateNote = "GetBossState WAS called but the byte it left at frame is not "
                                  "one of the three EDAMissionState values, so it is NOT reported";
                    continue;
                }

                v.state = state;
                v.stateMeasured = true;
            }
        } else {
            for (MissionVolume& v : t.volumes) {
                v.stateNote = "GetBossState could not be resolved, so NO mission state was read: " +
                              getBossState.note;
            }
        }

        if (getDefeated.usable) {
            unsigned char frame[kDefeatedFrameBytes] = {0};
            std::string note;
            if (!reflect_invoke_function(t.saveObject, getDefeated, frame, note)) {
                t.defeatedBossNumNote = "the call was NOT made: " + note;
            } else {
                std::int32_t value = 0;
                if (try_read_bytes(reinterpret_cast<std::uintptr_t>(frame) + kDefeatedReturnAt,
                                   &value, 4)) {
                    t.defeatedBossNum = value;
                    t.defeatedBossNumMeasured = true;
                } else {
                    t.defeatedBossNumNote = "GetDefeatedBossNum WAS called but its return value could "
                                            "not be read back out of the frame";
                }
            }
        } else {
            t.defeatedBossNumNote = "GetDefeatedBossNum could not be resolved: " + getDefeated.note;
        }
    } else if (!t.stateReadAllowed) {
        for (MissionVolume& v : t.volumes) {
            v.stateNote = "NOT MEASURED: the save gate refused this read (" + t.gateNote + ")";
        }
    } else if (t.saveObject == 0) {
        for (MissionVolume& v : t.volumes) {
            v.stateNote = "NOT MEASURED: " + t.saveNote;
        }
    } else {

        for (MissionVolume& v : t.volumes) {
            v.stateNote = "NOT MEASURED: the object found at 0x" + std::to_string(t.saveObject) +
                          " has class '" + t.saveClassName +
                          "', not 'DAPersistentSaveGame', so no accessor was resolved on it";
        }
    }

    return t;
}

std::vector<std::string> MissionTable::report_lines() const {
    std::vector<std::string> out;
    char buf[512];

    if (slotNameMeasured) {
        out.push_back(std::string("slot: WHICH SAVE IS LOADED = '") + slotName +
                      "' (UEMSObject::CurrentSaveGameName); " + std::to_string(emsInstances) +
                      " EMSObject instance(s) found");
    } else {
        out.push_back("slot: NOT MEASURED -- " + slotNote);
    }
    out.push_back("gate: " + gateNote);

    if (saveObject != 0) {
        const std::string extra = saveNote.empty() ? std::string() : (" -- " + saveNote);
        std::snprintf(buf, sizeof(buf), "save: object '%s' (%s) at 0x%llX; %u instance(s) found%s",
                      saveObjectName.c_str(), saveClassName.c_str(),
                      static_cast<unsigned long long>(saveObject), saveInstances, extra.c_str());
        out.push_back(buf);

        if (saveCustomNameMeasured) {
            out.push_back(std::string("save: UEMSCustomSaveGame::SaveGameName = '") +
                          saveCustomName + "' (this is the custom save's name, not the slot above)");
        } else {
            out.push_back("save: UEMSCustomSaveGame::SaveGameName NOT MEASURED -- " +
                          (saveCustomNameNote.empty() ? std::string("not read")
                                                      : saveCustomNameNote));
        }
    } else {
        out.push_back("save: NO OBJECT -- " + (saveNote.empty() ? std::string("not found") : saveNote));
    }

    const std::string worldExtra = worldNote.empty() ? std::string() : (" -- " + worldNote);
    std::snprintf(buf, sizeof(buf), "world: %u level(s) walked, %llu actor(s) seen, %zu mission "
                                    "volume(s) found%s",
                  levelsWalked, static_cast<unsigned long long>(actorsWalked), volumes.size(),
                  worldExtra.c_str());
    out.push_back(buf);

    if (!volumeishClasses.empty()) {
        std::string line = "volume-ish actor classes in these levels (name contains 'Volume'):";
        for (std::size_t i = 0; i < volumeishClasses.size() && i < 10; ++i) {
            const std::string& cls = volumeishClasses[i].first;
            const bool accepted = std::any_of(volumes.begin(), volumes.end(),
                                              [&](const MissionVolume& v) {
                                                  return v.actorClass == cls;
                                              });
            line += " " + cls + "=" + std::to_string(volumeishClasses[i].second) +
                    (accepted ? " [accepted]" : " [NOT accepted]");
        }
        if (volumeishClasses.size() > 10) {
            line += " (+" + std::to_string(volumeishClasses.size() - 10) + " more)";
        }
        out.push_back(line);
    } else if (worldFound) {
        out.push_back("volume-ish actor classes in these levels: NONE -- no actor's class name "
                      "contains 'Volume' at all, so the level declares its missions some other way and "
                      "this reader has found nothing to read");
    }

    std::uint32_t statesMeasured = 0;
    std::uint32_t idsResolved = 0;
    std::uint32_t cleared = 0;
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        const MissionVolume& v = volumes[i];
        if (v.stateMeasured) ++statesMeasured;
        if (!v.missionId.empty()) ++idsResolved;
        if (v.stateMeasured && v.state == 2) ++cleared;

        std::string line = " [" + std::to_string(i + 1) + "] " + v.actorClass + " '" + v.actorName +
                           "' ";
        line += v.missionId.empty() ? std::string("MissionId=NOT MEASURED")
                                    : ("MissionId='" + v.missionId + "'");
        if (!v.missionId.empty()) {
            line += " (index=" + std::to_string(v.missionIdIndex) +
                    " number=" + std::to_string(v.missionIdNumber) + ")";
        }
        line += v.typeMeasured ? (" type=" + std::string(mission_type_name(v.missionType)))
                               : std::string(" type=NOT MEASURED");
        if (v.stateMeasured) {
            line += " state=" + std::string(mission_state_name(v.state));
            line += " [the game's own GetBossState]";
        } else {
            line += " state=NOT MEASURED";
        }

        std::string why = v.missionIdNote;
        if (!v.typeNote.empty()) why += (why.empty() ? "" : "; ") + v.typeNote;
        if (!v.stateNote.empty()) why += (why.empty() ? "" : "; ") + v.stateNote;
        if (!why.empty()) line += " -- " + why;

        std::snprintf(buf, sizeof(buf), " @0x%llX", static_cast<unsigned long long>(v.actor));
        line += buf;
        out.push_back(line);
    }

    if (defeatedBossNumMeasured) {
        std::snprintf(buf, sizeof(buf),
                      "cross-check: GetDefeatedBossNum = %d, and %u volume(s) in this level printed "
                      "Cleared(2). THE TWO ARE NOT EXPECTED TO BE EQUAL: the accessor counts every "
                      "cleared boss in the SAVE, and this list only holds what the loaded levels "
                      "declare. The accessor is the check on the accessor -- if it is 0 while a volume "
                      "here is Cleared, one of the two reads is wrong.",
                      static_cast<int>(defeatedBossNum), cleared);
        out.push_back(buf);
    } else {
        out.push_back("cross-check: GetDefeatedBossNum NOT MEASURED -- " +
                      (defeatedBossNumNote.empty() ? std::string("not called") : defeatedBossNumNote));
    }

    std::snprintf(buf, sizeof(buf),
                  "summary: volumes=%zu ids_resolved=%u types_measured=%u states_measured=%u "
                  "cleared=%u",
                  volumes.size(), idsResolved,
                  static_cast<std::uint32_t>(std::count_if(
                      volumes.begin(), volumes.end(),
                      [](const MissionVolume& v) { return v.typeMeasured; })),
                  statesMeasured, cleared);
    out.push_back(buf);

    return out;
}

namespace {

constexpr std::uint32_t kSparseFirstFree = 0x28;
constexpr std::uint32_t kSparseNumFree   = 0x2C;

constexpr std::uint32_t kSaveDataVersion = 0x0040;
constexpr std::uint32_t kSaveMoney       = 0x0048;
constexpr std::uint32_t kSaveItemSlots   = 0x00A0;
struct ContainerSpec {
    const char*   name;
    std::uint32_t offset;
    const char*   type;
    std::uint32_t elementBytes;
    int           sampleKind;
};

const ContainerSpec kContainers[] = {

    {"ObtainedItems",      0x0050, "TSet<FPrimaryAssetId>",            0x18, 2},
    {"ReachedDistricts",   0x06C0, "TSet<FName>",                      0x10, 1},
    {"AcquiredItemBoxIds", 0x0760, "TSet<FGuid>",                      0x18, 3},
    {"OpenedStrongBoxIds", 0x07B0, "TSet<FGuid>",                      0x18, 3},
    {"ReleasedCheckpoints",0x0818, "TSet<FName>",                      0x10, 1},
    {"NPCStates",          0x0990, "TMap<TSoftClassPtr<UClass>,EDANPCState>", 0x28, 0},

    {"EventParams",        0x0670, "TMap<FName,int32>",                0x14, 4},
};

constexpr int kSamplesPerContainer = 4;

}
SaveProbe probe_save_containers() {
    SaveProbe p;

    std::uintptr_t save = 0;
    std::uint32_t saveInstances = 0;
    std::uint32_t saveTemplates = 0;
    for_each_object_address([&](std::uintptr_t obj) {
        if (object_class_name(obj) != "DAPersistentSaveGame") return;
        const std::string name = name_of_object(obj);
        if (name.rfind("Default__", 0) == 0) {
            ++saveTemplates;
            return;
        }
        ++saveInstances;
        if (save == 0) save = obj;
    });

    if (save == 0) {
        char b[256];
        std::snprintf(b, sizeof(b),
                      "no LIVE DAPersistentSaveGame instance is in the object array (%u template(s) were "
                      "skipped), so there is nothing to probe",
                      saveTemplates);
        p.note = b;
        return p;
    }
    p.saveFound = true;
    p.saveObject = save;
    {
        char b[256];
        std::snprintf(b, sizeof(b), "%u live instance(s), %u template(s) skipped; chose '%s'",
                      saveInstances, saveTemplates, name_of_object(save).c_str());
        p.note = b;
    }

    {
        std::uint32_t version = 0;
        std::uint32_t money = 0;
        try_read_u32(save + kSaveDataVersion, version);
        try_read_u32(save + kSaveMoney, money);
        ArrayHeader slots;
        std::string slotsNote;
        const bool slotsOk = read_tarray(save + kSaveItemSlots, 4096, slots, slotsNote);
        char b[320];
        std::snprintf(b, sizeof(b),
                      "live save @0x%llX: SavedDataVersion=%u Money=%u ItemSlots=%s",
                      static_cast<unsigned long long>(save), version, money,
                      slotsOk ? (std::to_string(slots.num) + " element(s)").c_str()
                              : ("NOT READ (" + slotsNote + ")").c_str());
        p.fields = b;
    }

    char buf[512];
    for (const ContainerSpec& spec : kContainers) {
        SaveContainer c;
        c.name = spec.name;
        c.offset = spec.offset;
        c.fieldType = spec.type;
        c.elementBytes = spec.elementBytes;

        const std::uintptr_t at = save + spec.offset;
        std::uint64_t data = 0;
        std::uint32_t num = 0;
        std::uint32_t max = 0;
        if (!try_read_u64(at, data)) {
            c.note = "the Data pointer could not be read at all";
            p.containers.push_back(std::move(c));
            continue;
        }
        if (!try_read_u32(at + 8, num) || !try_read_u32(at + 12, max)) {
            c.note = "the Data pointer read, but Num/Max could not";
            p.containers.push_back(std::move(c));
            continue;
        }
        std::uint32_t firstFree = 0;
        std::uint32_t numFree = 0;
        const bool firstFreeRead = try_read_u32(at + kSparseFirstFree, firstFree);
        const bool numFreeRead = try_read_u32(at + kSparseNumFree, numFree);

        c.headerRead = true;
        c.elements = static_cast<std::uintptr_t>(data);
        c.num = num;
        c.max = max;

        c.freeListMeasured = false;
        if (firstFreeRead) c.firstFree = firstFree;
        if (numFreeRead) c.numFree = numFree;

        if (num > max) {
            c.note = "REFUSED: Num > Max, so this is not a TArray header";
            p.containers.push_back(std::move(c));
            continue;
        }
        if (num > 0 && data < 0x10000) {
            c.note = "REFUSED: Num > 0 but the element pointer is not a pointer";
            p.containers.push_back(std::move(c));
            continue;
        }

        const std::uint32_t limit = num < static_cast<std::uint32_t>(kSamplesPerContainer)
                                        ? num
                                        : static_cast<std::uint32_t>(kSamplesPerContainer);
        for (std::uint32_t i = 0; i < limit; ++i) {
            const std::uintptr_t el = c.elements + static_cast<std::uintptr_t>(i) * spec.elementBytes;

            char tail[64] = "";
            const std::size_t samplesBefore = c.samples.size();
            {
                std::uint32_t hNext = 0;
                std::uint32_t hIdx = 0;
                if (try_read_u32(el + spec.elementBytes - 8, hNext) &&
                    try_read_u32(el + spec.elementBytes - 4, hIdx)) {
                    std::snprintf(tail, sizeof(tail), " [hashNext=%d hashIndex=%d]", static_cast<int>(hNext),
                                  static_cast<int>(hIdx));
                } else {
                    std::snprintf(tail, sizeof(tail), " [hash words NOT READ]");
                }
            }
            if (spec.sampleKind == 1) {
                std::uint32_t index = 0;
                std::uint32_t number = 0;
                if (!try_read_u32(el, index) || !try_read_u32(el + 4, number)) {
                    c.samples.push_back("#" + std::to_string(i) + " NOT READ: the FName could not be read");
                    continue;
                }
                std::string text;
                std::string note;
                if (resolve_fname(index, text, note)) {
                    c.samples.push_back("#" + std::to_string(i) + " '" + text + "'");
                } else {
                    c.samples.push_back("#" + std::to_string(i) + " NOT RESOLVED: " + note);
                }
            } else if (spec.sampleKind == 2) {

                std::uint32_t ti = 0, tn = 0, ni = 0, nn = 0;
                if (!try_read_u32(el, ti) || !try_read_u32(el + 4, tn) || !try_read_u32(el + 8, ni) ||
                    !try_read_u32(el + 12, nn)) {
                    c.samples.push_back("#" + std::to_string(i) +
                                        " NOT READ: the two FNames could not be read");
                    continue;
                }
                std::string typeText, nameText, note;
                const bool typeOk = resolve_fname(ti, typeText, note);
                const bool nameOk = resolve_fname(ni, nameText, note);
                if (typeOk && nameOk) {
                    c.samples.push_back("#" + std::to_string(i) + " type='" + typeText + "' name='" +
                                        nameText + "'");
                } else if (typeOk || nameOk) {
                    c.samples.push_back("#" + std::to_string(i) + " HALF RESOLVED: type=" +
                                        (typeOk ? ("'" + typeText + "'") : std::string("NOT RESOLVED")) +
                                        " name=" +
                                        (nameOk ? ("'" + nameText + "'") : std::string("NOT RESOLVED")));
                } else {
                    c.samples.push_back("#" + std::to_string(i) + " NEITHER FName RESOLVED: " + note);
                }
            } else if (spec.sampleKind == 3) {

                std::uint32_t a = 0, b = 0, d = 0, e = 0;
                if (try_read_u32(el, a) && try_read_u32(el + 4, b) && try_read_u32(el + 8, d) &&
                    try_read_u32(el + 12, e)) {
                    std::snprintf(buf, sizeof(buf), "#%u %08X-%08X-%08X-%08X", i, a, b, d, e);
                    c.samples.push_back(buf);
                } else {
                    c.samples.push_back("#" + std::to_string(i) + " NOT READ: the four words could not");
                }
            } else if (spec.sampleKind == 4) {

                std::uint32_t ki = 0, kn = 0;
                std::int32_t value = 0;
                std::string keyText, note;
                if (!try_read_u32(el, ki) || !try_read_u32(el + 4, kn) ||
                    !try_read_bytes(el + 8, &value, sizeof(value))) {
                    c.samples.push_back("#" + std::to_string(i) + " NOT READ: the key or the value could not");
                    continue;
                }
                if (resolve_fname(ki, keyText, note)) {
                    c.samples.push_back("#" + std::to_string(i) + " '" + keyText + "' = " +
                                        std::to_string(value));
                } else {
                    c.samples.push_back("#" + std::to_string(i) + " KEY NOT RESOLVED: " + note +
                                        " (if every sample on this line fails, the element stride is not "
                                        "0x14)");
                }
            }

            if (c.samples.size() > samplesBefore && tail[0] != '\0') {
                c.samples.back() += tail;
            }
        }

        p.containers.push_back(std::move(c));
    }

    std::vector<std::string> obtainedNames;
    for (const SaveContainer& c : p.containers) {
        if (c.name != "ObtainedItems" || !c.headerRead || c.elements == 0) continue;
        const std::uint32_t cap = c.num < 4096 ? c.num : 4096u;
        for (std::uint32_t i = 0; i < cap; ++i) {

            const std::uintptr_t el =
                c.elements + static_cast<std::uintptr_t>(i) * c.elementBytes;
            std::uint32_t ti = 0, tn = 0, ni = 0, nn = 0;
            if (!try_read_u32(el, ti) || !try_read_u32(el + 4, tn) || !try_read_u32(el + 8, ni) ||
                !try_read_u32(el + 12, nn)) {
                continue;
            }
            std::string typeText, nameText, note;
            if (resolve_fname(ni, nameText, note) && !nameText.empty() && nameText != "None") {
                obtainedNames.push_back(nameText);
            }
        }
        break;
    }
    p.obtainedItemsResolved = static_cast<std::uint32_t>(obtainedNames.size());

    std::unordered_map<std::uintptr_t, bool> keyClassCache;
    std::unordered_map<std::uintptr_t, bool> classesSeen;
    for_each_object_address([&](std::uintptr_t obj) {

        std::uint64_t clsPtr = 0;
        if (!try_read_u64(obj + kUObjectClassOffset, clsPtr)) return;
        if (clsPtr < 0x10000) return;
        const auto cls = static_cast<std::uintptr_t>(clsPtr);
        if (classesSeen.count(cls) != 0) return;
        classesSeen.emplace(cls, true);
        if (!class_reaches_named(cls, "DAKeyItem", keyClassCache)) return;

        KeyItemProbe k;

        k.className = name_of_object(cls);
        if (k.className.empty()) return;
        k.assetName = k.className;
        if (k.assetName.size() > 2 && k.assetName.compare(k.assetName.size() - 2, 2, "_C") == 0) {
            k.assetName = k.assetName.substr(0, k.assetName.size() - 2);
        }
        k.inSave = std::find(obtainedNames.begin(), obtainedNames.end(), k.assetName) !=
                   obtainedNames.end();
        if (!k.inSave) {
            k.note = "NOT in ObtainedItems -- either this save never obtained it, or the name the save "
                     "stores differs from the class name";
        }
        p.keyItems.push_back(std::move(k));
    });
    std::sort(p.keyItems.begin(), p.keyItems.end(),
              [](const KeyItemProbe& a, const KeyItemProbe& b) { return a.assetName < b.assetName; });

    for (const std::string& n : obtainedNames) {
        std::string lower = n;
        for (char& ch : lower) {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        }
        if (lower.find("key") == std::string::npos) continue;
        const bool claimed = std::any_of(p.keyItems.begin(), p.keyItems.end(),
                                         [&](const KeyItemProbe& k) { return k.assetName == n; });
        if (!claimed) p.unmatchedKeyishNames.push_back(n);
    }
    std::sort(p.unmatchedKeyishNames.begin(), p.unmatchedKeyishNames.end());

    for (const SaveContainer& c : p.containers) {
        if (c.name != "EventParams" || !c.headerRead || c.elements == 0) continue;
        const std::uint32_t cap = c.num < 4096 ? c.num : 4096u;
        for (std::uint32_t i = 0; i < cap; ++i) {
            const std::uintptr_t el = c.elements + static_cast<std::uintptr_t>(i) * c.elementBytes;
            std::uint32_t ki = 0, kn = 0;
            std::int32_t value = 0;
            if (!try_read_u32(el, ki) || !try_read_u32(el + 4, kn) ||
                !try_read_bytes(el + 8, &value, sizeof(value))) {
                continue;
            }
            std::string name;
            std::string note;
            if (!resolve_fname(ki, name, note) || name.empty() || name == "None") continue;
            ++p.flagsRead;
            const std::string entry = name + " = " + std::to_string(value);
            const bool mapFlag = name.size() >= 2 && name[0] == 'M' && name[1] >= '0' && name[1] <= '9';
            if (mapFlag) {
                p.flagSyncable.push_back(entry);
            } else if (name.rfind("Event", 0) == 0) {
                p.flagExcluded.push_back(entry +
                                         " -- excluded: starts with 'Event', so it drives an NPC, and the "
                                         "operator's rule is that NPC state is not touched");
            } else {
                p.flagExcluded.push_back(entry +
                                         " -- excluded: not an M<digits> map flag (a machine choice, a "
                                         "counter, or a name shape this build has not seen)");
            }
        }
        break;
    }
    std::sort(p.flagSyncable.begin(), p.flagSyncable.end());
    std::sort(p.flagExcluded.begin(), p.flagExcluded.end());

    return p;
}

std::vector<std::string> SaveProbe::report_lines() const {    std::vector<std::string> out;
    char buf[512];

    if (!saveFound) {
        out.push_back("probe: " + (note.empty() ? std::string("no save object") : note));
        return out;
    }
    std::snprintf(buf, sizeof(buf), "probe: save object @0x%llX -- %s",
                  static_cast<unsigned long long>(saveObject), note.c_str());
    out.push_back(buf);
    if (!fields.empty()) out.push_back("probe: " + fields);
    out.push_back("probe: each line below is a CONTAINER READ AS A UE4 SPARSE ARRAY -- Data/Num/Max are "
                  "the TArray it starts with, and numFree is printed rather than used, because whether "
                  "the free list is where this build expects it is one of the things being measured.");
    for (const SaveContainer& c : containers) {
        std::snprintf(buf, sizeof(buf), " %-20s %04X %s", c.name.c_str(), c.offset,
                      c.fieldType.c_str());
        std::string line = buf;
        if (!c.headerRead) {
            line += " -- NOT READ: " + c.note;
            out.push_back(line);
            continue;
        }
        std::snprintf(buf, sizeof(buf), " -- Data=0x%llX Num=%u Max=%u",
                      static_cast<unsigned long long>(c.elements), c.num, c.max);
        line += buf;
        if (c.freeListMeasured) {
            std::snprintf(buf, sizeof(buf), " numFree=%u (live=%d)", c.numFree,
                          static_cast<int>(c.num) - static_cast<int>(c.numFree));
            line += buf;
        } else {
            std::snprintf(buf, sizeof(buf),
                          " live count=NOT MEASURED (the free list is not at/; the two words "
                          "there read %u and %u on every container regardless of size)",
                          c.firstFree, c.numFree);
            line += buf;
        }
        if (!c.note.empty()) line += " -- " + c.note;
        out.push_back(line);
        for (const std::string& s : c.samples) out.push_back(" " + s);
        if (c.num == 0) out.push_back(" (empty)");
    }

    out.push_back("keys: " + std::to_string(obtainedItemsResolved) +
                  " name(s) were resolved out of ObtainedItems (a slot whose name did not resolve is "
                  "counted here as absent, because the container's free list is not measured yet -- every "
                  "key item line below must be read with that in mind)");
    if (keyItems.empty()) {
        out.push_back("keys: NO class in the object array reaches DAKeyItem -- so either this build is "
                      "asking the wrong base name, or no key item asset is loaded right now (the asset "
                      "must be cooked into the build; only LOADED assets are in the object array)");
    }
    for (const KeyItemProbe& k : keyItems) {
        out.push_back(std::string(" ") + (k.inSave ? "[in the save] " : "[NOT in the save] ") +
                      k.className + (k.assetName != k.className ? (" (asset '" + k.assetName + "')") : "") +
                      (k.note.empty() ? std::string() : (" -- " + k.note)));
    }
    out.push_back("keys: " + std::to_string(keyItems.size()) + " class(es) in the object array reach "
                  "DAKeyItem; " + std::to_string(unmatchedKeyishNames.size()) +
                  " name(s) in the save contain 'key' and were claimed by none of them");
    for (const std::string& n : unmatchedKeyishNames) {
        out.push_back(" [unclaimed] " + n);
    }

    out.push_back("flags: " + std::to_string(flagsRead) + " event flag(s) resolved; " +
                  std::to_string(flagSyncable.size()) + " would be synced and " +
                  std::to_string(flagExcluded.size()) + " would NOT");
    for (const std::string& s : flagSyncable) out.push_back(" [sync] " + s);
    for (const std::string& s : flagExcluded) out.push_back(" [no sync] " + s);
    return out;
}

}
namespace dsh::reflect {

std::vector<std::string> survey_mission_actors() {
    std::vector<std::string> out;
    const EnemySurvey s = survey_enemies();
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "mission probe: %zu AI-driven character(s) in this level (the boss is one of them if its "
                  "controller survived the fight)", s.enemies.size());
    out.push_back(buf);
    std::uint32_t withController = 0;
    for (const EnemyRecord& e : s.enemies) {
        if (e.controller >= 0x10000) ++withController;
        std::snprintf(buf, sizeof(buf), "mission probe: '%s' [%s] controller=%s actor=0x%llX",
                      e.actorName.c_str(), e.actorClass.c_str(),
                      e.controllerClass.empty() ? "(none)" : e.controllerClass.c_str(),
                      static_cast<unsigned long long>(e.actor));
        out.push_back(buf);
    }
    std::snprintf(buf, sizeof(buf),
                  "mission probe: %u of them still have a controller; a character that still HAS one is one "
                  "BootAI away from fighting again", withController);
    out.push_back(buf);
    return out;
}
}
namespace dsh::reflect {

std::vector<std::string> survey_mission_verbs() {
    std::vector<std::string> out;
    const char* classes[] = {"DAMissionGameMode", "DAMissionVolume", "DAMissionGameState"};
    const char* verbs[] = {"StartMission",     "BeginMission", "StartArena",
                           "RestartMission",   "ResetMission", "StartBossBattle"};
    std::uint32_t found = 0;
    for (const char* cls : classes) {
        const std::uintptr_t klass = resolve_uclass_address(cls);
        char buf[384];
        if (klass == 0) {
            std::snprintf(buf, sizeof(buf), "mission verbs: the %s UClass is not in the object array", cls);
            out.push_back(buf);
            continue;
        }
        std::string present;
        for (const char* verb : verbs) {
            const FunctionRef fn = reflect_resolve_function(klass, verb);
            if (!fn.usable) continue;
            ++found;
            present += std::string(present.empty() ? "" : ", ") + verb;
        }
        std::snprintf(buf, sizeof(buf), "mission verbs: %s -- %s",
                      cls, present.empty() ? "NONE of the six known names is declared on it"
                                           : ("FOUND: " + present).c_str());
        out.push_back(buf);
    }
    char tail[256];
    std::snprintf(tail, sizeof(tail),
                  "mission verbs: %u verb(s) found across 3 class(es) and 6 candidate names. A verb found "
                  "here is the whole of variant A: start the mission and the game does the rest", found);
    out.push_back(tail);
    return out;
}

namespace {

constexpr std::uint32_t kStartFrameBytes    = 0x10;
constexpr std::uint32_t kStartTypeAt        = 0x00;
constexpr std::uint32_t kStartNameAt        = 0x04;
constexpr std::uint32_t kConvNameReturnAt   = 0x10;
constexpr std::uint32_t kConvNameFrameBytes = 0x18;
constexpr std::uint32_t kIsModeFrameBytes   = 0x10;
constexpr std::uint32_t kRecentFrameBytes   = 0x10;

std::uintptr_t chain_find_named(std::uintptr_t cls, const char* baseName, std::string& chain) {
    chain.clear();
    std::uintptr_t cur = cls;
    for (int depth = 0; depth < kMaxSuperChain && cur >= 0x10000; ++depth) {
        const std::string name = name_of_object(cur);
        chain += (chain.empty() ? "" : " -> ");
        chain += name.empty() ? std::string("(unnamed)") : name;
        if (name == baseName) return cur;
        const std::uint64_t super = read_u64(cur + kUStructSuperOffset);
        if (super == 0) break;
        cur = static_cast<std::uintptr_t>(super);
    }
    return 0;
}

std::uintptr_t find_live_save(std::uint32_t& instances, std::uint32_t& templates) {
    instances = 0;
    templates = 0;
    std::uintptr_t save = 0;
    for_each_object_address([&](std::uintptr_t obj) {
        if (object_class_name(obj) != "DAPersistentSaveGame") return;
        if (name_of_object(obj).rfind("Default__", 0) == 0) {
            ++templates;
            return;
        }
        ++instances;
        if (save == 0) save = obj;
    });
    return save;
}

bool string_to_fname(const std::string& text, std::uint32_t& index, std::uint32_t& number,
                     std::string& note) {
    const StaticFunctionRef conv = reflect_resolve_static("KismetStringLibrary", "Conv_StringToName");
    if (!conv.usable) {
        note = "Conv_StringToName could not be resolved, so no FName was made: " + conv.note;
        return false;
    }
    std::wstring wide(text.begin(), text.end());
    struct FStringMirror {
        wchar_t*     Data;
        std::int32_t Num;
        std::int32_t Max;
    };
    FStringMirror fs{};
    fs.Data = const_cast<wchar_t*>(wide.c_str());
    fs.Num = static_cast<std::int32_t>(wide.size()) + 1;
    fs.Max = fs.Num;

    unsigned char frame[kConvNameFrameBytes] = {};
    std::memcpy(frame + 0x00, &fs, sizeof(fs));
    std::string callNote;
    if (!reflect_invoke_function(conv.cdo, conv.fn, frame, callNote)) {
        note = "Conv_StringToName was NOT called: " + callNote;
        return false;
    }
    std::memcpy(&index, frame + kConvNameReturnAt, sizeof(index));
    std::memcpy(&number, frame + kConvNameReturnAt + 4, sizeof(number));
    if (index == 0 && number == 0) {
        note = "the engine's Conv_StringToName returned the EMPTY FName for '" + text + "'";
        return false;
    }
    return true;
}

bool call_is_mission_mode(std::uintptr_t gameMode, const FunctionRef& fn, bool& value,
                          std::string& note) {
    unsigned char frame[kIsModeFrameBytes] = {};
    if (!reflect_invoke_function(gameMode, fn, frame, note)) return false;
    std::uint8_t raw = 0;
    std::memcpy(&raw, frame, sizeof(raw));
    value = (raw != 0);
    return true;
}

bool call_boss_state(std::uintptr_t save, const FunctionRef& fn, std::uint32_t index,
                     std::uint32_t number, std::uint8_t& state, std::string& note) {
    unsigned char frame[0x10] = {};
    std::memcpy(frame + 0x00, &index, sizeof(index));
    std::memcpy(frame + 0x04, &number, sizeof(number));
    if (!reflect_invoke_function(save, fn, frame, note)) return false;
    std::uint8_t raw = 0;
    std::memcpy(&raw, frame + 0x08, sizeof(raw));
    if (raw > 2) {
        note = "the byte GetBossState left at frame is not one of the three EDAMissionState values";
        return false;
    }
    state = raw;
    return true;
}

bool call_recently_defeated(std::uintptr_t gameMode, const FunctionRef& fn,
                            std::vector<std::string>& out, std::string& note) {
    unsigned char frame[kRecentFrameBytes] = {};
    if (!reflect_invoke_function(gameMode, fn, frame, note)) return false;
    std::uint64_t data = 0;
    std::uint32_t num = 0;
    std::uint32_t max = 0;
    std::memcpy(&data, frame + 0x00, sizeof(data));
    std::memcpy(&num, frame + 0x08, sizeof(num));
    std::memcpy(&max, frame + 0x0C, sizeof(max));
    if (num == 0) {
        note = "the array is EMPTY: the game counts no boss as recently defeated";
        return true;
    }
    if (num > 4096 || max < num || data < 0x10000) {
        note = "the returned TArray header is inconsistent (data=" + std::to_string(data) + " num=" +
               std::to_string(num) + " max=" + std::to_string(max) +
               "), so its elements were NOT read";
        return false;
    }
    for (std::uint32_t i = 0; i < num; ++i) {
        std::uint32_t index = 0;
        std::uint32_t number = 0;
        const std::uintptr_t at = static_cast<std::uintptr_t>(data) + static_cast<std::uintptr_t>(i) * 8;
        if (!try_read_u32(at, index) || !try_read_u32(at + 4, number)) {
            out.push_back("<element " + std::to_string(i) + " is not readable>");
            continue;
        }
        std::string resolved;
        std::string why;
        if (!resolve_fname(index, resolved, why)) {
            out.push_back("<index " + std::to_string(index) + " did not resolve: " + why + ">");
            continue;
        }
        out.push_back(number == 0 ? resolved : (resolved + "#" + std::to_string(number)));
    }
    return true;
}

}
MissionStartOutcome start_mission_mode(const std::string& missionId, std::uint8_t requestedType) {
    MissionStartOutcome o;
    o.missionId = missionId;
    o.requestedType = requestedType;
    if (missionId.empty()) {
        o.note = "the mission id is empty, so there is nothing to ask the game to start";
        return o;
    }

    const LiveObjects live = find_live_objects();
    {
        char b[64];
        std::snprintf(b, sizeof(b), "0x%llX", static_cast<unsigned long long>(live.world));
        o.world = b;
    }
    if (live.world == 0) {
        o.note = "no live world is known (find_live_objects returned 0), so there is no game mode to ask";
        return o;
    }
    std::uint64_t gameMode = 0;
    if (!try_read_u64(live.world + world_offsets::kAuthorityGameMode, gameMode) || gameMode < 0x10000) {
        o.note = "UWorld::AuthorityGameMode is 0 or not a pointer, so there is no game mode to "
                 "ask -- this is what a client (rather than a standalone host) looks like";
        return o;
    }
    o.gameMode = static_cast<std::uintptr_t>(gameMode);
    o.gameModeClass = object_class_name(o.gameMode);
    const std::uintptr_t gameModeClass =
        static_cast<std::uintptr_t>(read_u64(o.gameMode + kUObjectClassOffset));
    const std::uintptr_t link = chain_find_named(gameModeClass, "DABattleGameMode", o.chain);
    if (link == 0) {
        o.note = "the game mode's class chain does not reach 'DABattleGameMode', so NO mission verb was "
                 "asked for (a level's game mode can be a plain ADAGameMode, and this project does not "
                 "guess). The chain it walked: " + o.chain;
        return o;
    }
    o.declaringClass = name_of_object(link);

    const FunctionRef startFn = reflect_resolve_function(link, "StartMissionMode");
    if (!startFn.usable) {
        o.note = "StartMissionMode could not be resolved on the declaring class: " + startFn.note;
        return o;
    }
    const FunctionRef isModeFn = reflect_resolve_function(link, "IsMissionMode");
    const FunctionRef recentFn = reflect_resolve_function(link, "GetRecentlyDefeatedBossIds");
    o.functionResolved = true;

    if (!string_to_fname(missionId, o.nameIndex, o.nameNumber, o.note)) return o;

    std::uint32_t saveInstances = 0;
    std::uint32_t saveTemplates = 0;
    const std::uintptr_t save = find_live_save(saveInstances, saveTemplates);
    FunctionRef stateFn;
    if (save != 0) {
        stateFn = reflect_resolve_function(
            static_cast<std::uintptr_t>(read_u64(save + kUObjectClassOffset)), "GetBossState");
    }

    std::string ignored;
    if (isModeFn.usable) {
        o.beforeIsMissionModeMeasured =
            call_is_mission_mode(o.gameMode, isModeFn, o.beforeIsMissionMode, ignored);
    }
    if (stateFn.usable) {
        o.beforeStateMeasured =
            call_boss_state(save, stateFn, o.nameIndex, o.nameNumber, o.beforeState, ignored);
    }
    if (recentFn.usable) {
        o.recentlyMeasured = call_recently_defeated(o.gameMode, recentFn, o.recentlyDefeated,
                                                    o.recentlyNote);
    }

    unsigned char frame[kStartFrameBytes] = {};
    std::memcpy(frame + kStartTypeAt, &requestedType, 1);
    std::memcpy(frame + kStartNameAt, &o.nameIndex, sizeof(o.nameIndex));
    std::memcpy(frame + kStartNameAt + 4, &o.nameNumber, sizeof(o.nameNumber));
    std::string callNote;
    const bool dispatched = run_on_game_thread(
        [&]() { o.called = reflect_invoke_function(o.gameMode, startFn, frame, callNote); }, 5000);
    if (!dispatched) {
        o.note = "the game-thread dispatch was REFUSED or timed out, so StartMissionMode was NOT called "
                 "(is 'gthook' installed, or F7 pressed?)";
        return o;
    }
    if (!o.called) {
        o.note = "the call was NOT made: " + callNote;
        return o;
    }

    if (isModeFn.usable) {
        o.afterIsMissionModeMeasured =
            call_is_mission_mode(o.gameMode, isModeFn, o.afterIsMissionMode, o.afterStateNote);
    }
    if (stateFn.usable) {
        o.afterStateMeasured =
            call_boss_state(save, stateFn, o.nameIndex, o.nameNumber, o.afterState, o.afterStateNote);
    }
    return o;
}

std::vector<std::string> MissionStartOutcome::report_lines() const {
    std::vector<std::string> out;
    char buf[512];

    std::snprintf(buf, sizeof(buf), "start: asked for mission '%s' as type %s; the engine made FName "
                                    "index=%u number=%u from it",
                  missionId.c_str(), mission_type_name(requestedType), nameIndex, nameNumber);
    out.push_back(buf);

    if (gameMode != 0) {
        std::snprintf(buf, sizeof(buf),
                      "start: world %s, game mode 0x%llX of class '%s'; StartMissionMode is declared on "
                      "'%s' (chain: %s)",
                      world.c_str(), static_cast<unsigned long long>(gameMode), gameModeClass.c_str(),
                      declaringClass.c_str(), chain.c_str());
        out.push_back(buf);
    }

    if (beforeIsMissionModeMeasured) {
        out.push_back(std::string("start: BEFORE -- IsMissionMode = ") +
                      (beforeIsMissionMode ? "true" : "false"));
    }
    if (beforeStateMeasured) {
        std::snprintf(buf, sizeof(buf), "start: BEFORE -- GetBossState('%s') = %s", missionId.c_str(),
                      mission_state_name(beforeState));
        out.push_back(buf);
    }

    if (!note.empty()) {
        out.push_back("start: NOTHING WAS STARTED -- " + note);
    } else {
        std::snprintf(buf, sizeof(buf),
                      "start: CALLED StartMissionMode(%s, '%s') on 0x%llX FROM THE GAME THREAD, and the "
                      "call returned",
                      mission_type_name(requestedType), missionId.c_str(),
                      static_cast<unsigned long long>(gameMode));
        out.push_back(buf);
    }

    if (afterIsMissionModeMeasured) {
        out.push_back(std::string("start: AFTER -- IsMissionMode = ") +
                      (afterIsMissionMode ? "true" : "false") +
                      (beforeIsMissionModeMeasured && afterIsMissionMode != beforeIsMissionMode
                           ? " <-- IT CHANGED: the game went into mission mode"
                           : " (unchanged; this sample is taken the instant the call returns)"));
    } else if (!afterStateNote.empty()) {
        out.push_back("start: AFTER -- IsMissionMode was not read: " + afterStateNote);
    }
    if (afterStateMeasured) {
        std::snprintf(buf, sizeof(buf), "start: AFTER -- GetBossState('%s') = %s", missionId.c_str(),
                      mission_state_name(afterState));
        out.push_back(buf);
    }

    if (recentlyMeasured) {
        std::string line = "start: the game's own list of recently defeated bosses (" +
                           std::to_string(recentlyDefeated.size()) + "):";
        for (const std::string& id : recentlyDefeated) line += " '" + id + "'";
        out.push_back(line);
    } else {
        out.push_back("start: recently defeated bosses NOT READ -- " +
                      (recentlyNote.empty() ? std::string("the accessor is not available")
                                            : recentlyNote));
    }

    out.push_back("start: THE JUDGE IS THE BOSS AREA. If staging, music or a health bar starts, this is "
                  "the whole boss-sync mechanism; nothing in memory can tell you that, and nothing here "
                  "ends, clears or resets a mission.");
    return out;
}

namespace {

constexpr std::uint32_t kAiBehaviorStateOff = 0x0360;
constexpr std::uint32_t kAiPerceptionOff    = 0x0350;
constexpr std::uint32_t kAiTargetingOff     = 0x0358;
constexpr std::uint32_t kAiFrameBytes       = 0x10;

const char* ai_state_name(std::uint8_t state) {
    switch (state) {
        case 0:  return "Idle(0)";
        case 1:  return "Battle(1)";
        case 2:  return "Return(2)";
        default: return "?";
    }
}

std::string hex_addr(std::uintptr_t value) {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%llX", static_cast<unsigned long long>(value));
    return b;
}

struct GameModeLinks {
    std::uintptr_t instance = 0;
    std::uintptr_t battle = 0;
    std::uintptr_t base = 0;
    std::string    instanceClass;
    std::string    chain;
    std::string    note;
};

GameModeLinks find_game_mode_links() {
    GameModeLinks g;
    const LiveObjects live = find_live_objects();
    if (live.world == 0) {
        g.note = "no live world is known";
        return g;
    }
    std::uint64_t gameMode = 0;
    if (!try_read_u64(live.world + world_offsets::kAuthorityGameMode, gameMode) || gameMode < 0x10000) {
        g.note = "UWorld::AuthorityGameMode is 0 or not a pointer (this is what a client looks "
                 "like)";
        return g;
    }
    g.instance = static_cast<std::uintptr_t>(gameMode);
    g.instanceClass = object_class_name(g.instance);
    const std::uintptr_t cls =
        static_cast<std::uintptr_t>(read_u64(g.instance + kUObjectClassOffset));
    g.battle = chain_find_named(cls, "DABattleGameMode", g.chain);
    std::string baseChain;
    g.base = chain_find_named(cls, "DAGameMode", baseChain);
    return g;
}

struct AiHandles {
    FunctionRef target;
    FunctionRef transition;
};

AiHandles resolve_ai_handles() {
    AiHandles h;
    const std::uintptr_t aiClass = resolve_uclass_address("DAAIController");
    if (aiClass == 0) return h;
    h.target = reflect_resolve_function(aiClass, "GetMainTarget");
    h.transition = reflect_resolve_function(aiClass, "CheckTransitionBattleState");
    return h;
}

std::string ai_measure_line(const EnemyRecord& e, const AiHandles& h) {
    std::string line = "'" + e.actorName + "' [" + e.actorClass + "]";
    if (e.controller < 0x10000) return line + ": NO controller, so nothing can be asked of it";

    line += " controller '" + e.controllerClass + "' " + hex_addr(e.controller);

    std::uint8_t state = 0xFF;
    if (try_read_bytes(e.controller + kAiBehaviorStateOff, &state, 1)) {
        line += std::string(" AIBehaviorState=") + ai_state_name(state);
    } else {
        line += " AIBehaviorState=NOT READ";
    }

    std::uint64_t perception = 0;
    std::uint64_t targeting = 0;
    line += (try_read_u64(e.controller + kAiPerceptionOff, perception) && perception >= 0x10000)
                ? (" AIPerception=" + object_class_name(static_cast<std::uintptr_t>(perception)))
                : std::string(" AIPerception=none");
    line += (try_read_u64(e.controller + kAiTargetingOff, targeting) && targeting >= 0x10000)
                ? (" TargetingSystem=" + object_class_name(static_cast<std::uintptr_t>(targeting)))
                : std::string(" TargetingSystem=none");

    if (h.transition.usable) {
        unsigned char frame[kAiFrameBytes] = {};
        std::string note;
        if (reflect_invoke_function(e.controller, h.transition, frame, note)) {
            line += std::string(" CheckTransitionBattleState=") + (frame[0] != 0 ? "true" : "false");
        } else {
            line += " CheckTransitionBattleState=NOT CALLED (" + note + ")";
        }
    } else {
        line += " CheckTransitionBattleState=UNRESOLVED";
    }

    if (h.target.usable) {
        unsigned char frame[kAiFrameBytes] = {};
        std::string note;
        if (reflect_invoke_function(e.controller, h.target, frame, note)) {
            std::uint64_t target = 0;
            std::memcpy(&target, frame, sizeof(target));
            if (target >= 0x10000) {
                const auto t = static_cast<std::uintptr_t>(target);
                line += " GetMainTarget='" + name_of_object(t) + "' [" + object_class_name(t) + "] " +
                        hex_addr(t);
            } else {
                line += " GetMainTarget=NONE (null: it has no target at all)";
            }
        } else {
            line += " GetMainTarget=NOT CALLED (" + note + ")";
        }
    } else {
        line += " GetMainTarget=UNRESOLVED";
    }
    return line;
}

}
std::vector<std::string> survey_boss_ai(const std::string& namePart) {
    std::vector<std::string> out;

    const GameModeLinks gm = find_game_mode_links();
    if (gm.instance == 0) {
        out.push_back("boss ai: game mode NOT READ -- " + gm.note);
    } else {
        std::string ctx = "boss ai: game mode '" + gm.instanceClass + "' " + hex_addr(gm.instance) +
                          " (chain: " + gm.chain + ")";
        auto ask_bool = [&](std::uintptr_t link, const char* functionName, const char* label) {
            if (link == 0) {
                ctx += std::string(" ") + label + "=CLASS NOT IN CHAIN";
                return;
            }
            const FunctionRef fn = reflect_resolve_function(link, functionName);
            if (!fn.usable) {
                ctx += std::string(" ") + label + "=UNRESOLVED";
                return;
            }
            unsigned char frame[kAiFrameBytes] = {};
            std::string note;
            if (!reflect_invoke_function(gm.instance, fn, frame, note)) {
                ctx += std::string(" ") + label + "=NOT CALLED";
                return;
            }
            ctx += std::string(" ") + label + "=" + (frame[0] != 0 ? "true" : "false");
        };
        ask_bool(gm.battle, "IsMissionMode", "IsMissionMode");
        ask_bool(gm.base, "IsDuringCutscene", "IsDuringCutscene");
        out.push_back(ctx);
    }

    const EnemySurvey s = survey_enemies();
    const AiHandles h = resolve_ai_handles();
    std::uint32_t matched = 0;
    std::uint32_t withController = 0;
    for (const EnemyRecord& e : s.enemies) {
        if (!namePart.empty() && !contains_ci(e.actorName, namePart.c_str()) &&
            !contains_ci(e.actorClass, namePart.c_str())) {
            continue;
        }
        ++matched;
        if (e.controller >= 0x10000) ++withController;
        out.push_back("boss ai: " + ai_measure_line(e, h));
    }
    {
        char tail[320];
        std::snprintf(tail, sizeof(tail),
                      "boss ai: %u character(s) matched%s, %u of them still have a controller. Read it as: "
                      "Idle + no target = it never decided to fight; Battle + a target = it decided and "
                      "something below the decision is stopped",
                      matched, namePart.empty() ? "" : (" '" + namePart + "'").c_str(), withController);
        out.push_back(tail);
    }
    return out;
}

BossWakeResult mission_wake_boss(const std::string& namePart) {
    BossWakeResult r;
    if (namePart.empty()) {
        r.note = "needs a name part, e.g. 'mission wake SEC_M01A01'";
        return r;
    }

    const EnemySurvey s = survey_enemies();
    std::vector<const EnemyRecord*> matches;
    for (const EnemyRecord& e : s.enemies) {
        if (contains_ci(e.actorName, namePart.c_str()) || contains_ci(e.actorClass, namePart.c_str())) {
            matches.push_back(&e);
        }
    }
    if (matches.empty()) {
        r.note = "nothing in this level matches '" + namePart + "'";
        return r;
    }
    if (matches.size() > 1) {
        r.note = "'" + namePart + "' matches " + std::to_string(matches.size()) +
                 " characters, so none was chosen -- use a longer part of the name";
        return r;
    }
    const EnemyRecord& e = *matches.front();
    r.character = e.actorName;
    if (e.actorName.find("NPC") != std::string::npos) {
        r.note = "'" + e.actorName + "' is an NPC, and NPCs are not touched";
        return r;
    }
    if (e.controller < 0x10000) {
        r.note = "'" + e.actorName + "' has NO controller, so there is nothing to boot";
        return r;
    }
    r.attempted = true;

    const AiHandles h = resolve_ai_handles();
    r.lines.push_back("boss wake: BEFORE " + ai_measure_line(e, h));

    const std::uintptr_t aiClass = resolve_uclass_address("DAAIController");
    if (aiClass == 0) {
        r.note = "the DAAIController UClass is not in the object array";
        return r;
    }
    const FunctionRef boot = reflect_resolve_function(aiClass, "BootAI");
    const FunctionRef state = reflect_resolve_function(aiClass, "ChangeAIBehaviorState");
    if (!boot.usable) {
        r.note = "BootAI is not usable: " + boot.note;
        return r;
    }
    if (!state.usable) {
        r.note = "ChangeAIBehaviorState is not usable: " + state.note;
        return r;
    }

    const std::uintptr_t controller = e.controller;
    std::string bootNote;
    std::string stateNote;
    const bool ran = run_on_game_thread(
        [&]() {
            r.bootCalled = reflect_invoke_function(controller, boot, nullptr, bootNote);
            unsigned char frame[kAiFrameBytes] = {};
            frame[0] = 1;
            r.stateCalled = reflect_invoke_function(controller, state, frame, stateNote);
        },
        2000);
    r.dispatched = ran;
    if (!ran) {
        r.note = "the game-thread dispatch was REFUSED or timed out, so nothing was called (is 'gthook' "
                 "installed?)";
        return r;
    }
    r.lines.push_back("boss wake: BootAI " +
                      std::string(r.bootCalled ? std::string("was CALLED")
                                               : ("was NOT called: " + bootNote)) +
                      "; ChangeAIBehaviorState(Battle(1)) " +
                      std::string(r.stateCalled ? std::string("was CALLED")
                                                : ("was NOT called: " + stateNote)));
    r.lines.push_back("boss wake: AFTER " + ai_measure_line(e, h));
    return r;
}

namespace {

constexpr std::uint32_t kBossStatesOff    = 0x08F0;
constexpr std::uint32_t kBossStatesStride = 0x14;
constexpr std::uint32_t kBossStatesValue  = 0x08;
constexpr std::uint32_t kMaxBossEntries   = 4096;

bool call_defeated_num(std::uintptr_t save, const FunctionRef& fn, std::int32_t& value,
                       std::string& note) {
    unsigned char frame[0x10] = {};
    if (!reflect_invoke_function(save, fn, frame, note)) return false;
    std::memcpy(&value, frame, sizeof(value));
    return true;
}

}
BossStateWriteResult set_boss_state(const std::string& missionId, std::uint8_t state) {
    BossStateWriteResult r;
    r.missionId = missionId;
    r.requested = state;
    if (missionId.empty()) {
        r.note = "the mission id is empty";
        return r;
    }
    if (state > 2) {
        r.note = "only Unattempted(0), Attempted(1) and Cleared(2) exist";
        return r;
    }
    if (!string_to_fname(missionId, r.nameIndex, r.nameNumber, r.note)) return r;

    std::uint32_t instances = 0;
    std::uint32_t templates = 0;
    const std::uintptr_t save = find_live_save(instances, templates);
    if (save == 0) {
        r.note = "no live DAPersistentSaveGame instance is in the object array (" +
                 std::to_string(templates) + " template(s) were skipped)";
        return r;
    }
    r.saveFound = true;
    r.save = save;

    const std::uintptr_t saveClass =
        static_cast<std::uintptr_t>(read_u64(save + kUObjectClassOffset));
    const FunctionRef getState = reflect_resolve_function(saveClass, "GetBossState");
    const FunctionRef getDefeated = reflect_resolve_function(saveClass, "GetDefeatedBossNum");
    if (!getState.usable) {
        r.note = "GetBossState could not be resolved, so a write could not be verified and this command "
                 "does not write what it cannot check: " + getState.note;
        return r;
    }

    std::string ignored;
    r.beforeMeasured = call_boss_state(save, getState, r.nameIndex, r.nameNumber, r.before, ignored);
    if (getDefeated.usable) {
        r.defeatedBeforeMeasured =
            call_defeated_num(save, getDefeated, r.defeatedBefore, ignored);
    }

    const std::uintptr_t at = save + kBossStatesOff;
    std::uint64_t data = 0;
    std::uint32_t num = 0;
    std::uint32_t max = 0;
    if (!try_read_u64(at, data) || !try_read_u32(at + 8, num) || !try_read_u32(at + 0x0C, max)) {
        r.note = "BossStates' header at save could not be read";
        return r;
    }
    r.elements = num;
    if (num > kMaxBossEntries) {
        r.note = "BossStates' slot count (" + std::to_string(num) +
                 ") is above the sane bound, so its elements were NOT walked";
        return r;
    }
    if (num > max) {
        r.note = "BossStates' header contradicts itself (Num > Max), so it is not a sparse array and "
                 "nothing was written";
        return r;
    }
    if (num > 0 && data < 0x10000) {
        r.note = "BossStates has slots but its element pointer is not a pointer, so nothing was written";
        return r;
    }
    for (std::uint32_t i = 0; i < num; ++i) {
        const std::uintptr_t el =
            static_cast<std::uintptr_t>(data) + static_cast<std::uintptr_t>(i) * kBossStatesStride;
        std::uint32_t index = 0;
        std::uint32_t number = 0;
        if (!try_read_u32(el, index) || !try_read_u32(el + 4, number)) continue;
        ++r.entriesScanned;
        if (index != r.nameIndex || number != r.nameNumber) continue;
        r.elementFound = true;
        r.elementSlot = i;
        r.element = el;
        r.valueAddress = el + kBossStatesValue;
        break;
    }
    if (!r.elementFound) {
        r.note = "no entry for '" + missionId + "' is in BossStates (" + std::to_string(r.entriesScanned) +
                 " readable slot(s) of " + std::to_string(num) + "), and this command NEVER inserts: a "
                 "missing entry means the map already answers its default Unattempted(0). Asking for "
                 "Unattempted is therefore already true and needs no write; asking for anything else "
                 "cannot be done from here";
        return r;
    }

    const std::uintptr_t valueAddress = r.valueAddress;
    const std::uint8_t requested = state;
    const bool dispatched = run_on_game_thread(
        [&]() { r.written = write_process_bytes(valueAddress, &requested, 1); }, 2000);
    if (!dispatched) {
        r.written = false;
        r.note = "the game-thread dispatch was REFUSED or timed out, so nothing was written (is 'gthook' "
                 "installed?)";
        return r;
    }
    if (!r.written) {
        r.note = "the byte at 0x" + hex_addr(valueAddress).substr(2) + " could not be written";
        return r;
    }

    r.afterMeasured = call_boss_state(save, getState, r.nameIndex, r.nameNumber, r.after, ignored);
    if (getDefeated.usable) {
        r.defeatedAfterMeasured = call_defeated_num(save, getDefeated, r.defeatedAfter, ignored);
    }
    return r;
}

std::vector<BossStateRecord> all_boss_states(std::string& note) {
    note.clear();
    std::vector<BossStateRecord> out;

    struct BossStateCache {
        std::uintptr_t save = 0;
        FunctionRef    getState;
        std::uint32_t  rebuilds = 0;
    };
    static BossStateCache cache;
    {
        std::string whyNot;
        const bool stillValid = cache.save != 0 && cache.getState.usable &&
                                object_is_still_registered(cache.save, whyNot);
        if (!stillValid) {
            cache = BossStateCache{};
            ++cache.rebuilds;
            std::uint32_t instances = 0;
            std::uint32_t templates = 0;
            cache.save = find_live_save(instances, templates);
            if (cache.save == 0) {
                note = "no live DAPersistentSaveGame instance is in the object array (" +
                       std::to_string(templates) + " template(s) were skipped)";
                return out;
            }
            const std::uintptr_t saveClass =
                static_cast<std::uintptr_t>(read_u64(cache.save + kUObjectClassOffset));
            cache.getState = reflect_resolve_function(saveClass, "GetBossState");
            if (!cache.getState.usable) {
                note = "GetBossState could not be resolved, and this function never reports a value the "
                       "game has not confirmed: " + cache.getState.note;
                return out;
            }
        }
    }
    const std::uintptr_t save = cache.save;
    const FunctionRef getState = cache.getState;

    const std::uintptr_t at = save + kBossStatesOff;
    std::uint64_t data = 0;
    std::uint32_t num = 0;
    std::uint32_t max = 0;
    if (!try_read_u64(at, data) || !try_read_u32(at + 8, num) || !try_read_u32(at + 0x0C, max)) {
        note = "BossStates' header at save could not be read";
        return out;
    }
    if (num > kMaxBossEntries || num > max) {
        note = "BossStates' header is implausible (num=" + std::to_string(num) +
               " max=" + std::to_string(max) + "), so it was NOT walked";
        return out;
    }
    if (num > 0 && data < 0x10000) {
        note = "BossStates has slots but its element pointer is not a pointer";
        return out;
    }

    std::uint32_t candidates = 0;
    std::uint32_t unconfirmed = 0;
    std::string firstRefusal;
    for (std::uint32_t i = 0; i < num; ++i) {
        const std::uintptr_t el =
            static_cast<std::uintptr_t>(data) + static_cast<std::uintptr_t>(i) * kBossStatesStride;
        std::uint32_t index = 0;
        std::uint32_t number = 0;
        if (!try_read_u32(el, index) || !try_read_u32(el + 4, number)) continue;
        if (index == 0) continue;
        std::string id;
        std::string why;
        if (!resolve_fname(index, id, why) || id.empty()) continue;
        ++candidates;

        std::uint8_t state = 0;
        if (!call_boss_state(save, getState, index, number, state, why)) {
            ++unconfirmed;
            if (firstRefusal.empty()) firstRefusal = "'" + id + "': " + why;
            continue;
        }
        BossStateRecord record;
        record.id = id;
        record.state = state;
        out.push_back(std::move(record));
    }

    note = std::to_string(out.size()) + " record(s) confirmed out of " +
           std::to_string(candidates) + " candidate slot(s) in " + std::to_string(num) + " slot(s)";
    if (unconfirmed != 0) {
        note += ", " + std::to_string(unconfirmed) + " candidate(s) the game would not confirm";
        if (!firstRefusal.empty()) note += " (first: " + firstRefusal + ")";
    }
    return out;
}

bool mission_states_of(const std::vector<std::string>& ids,
                       std::vector<std::uint8_t>& states,
                       std::vector<bool>& measured, std::string& note) {
    note.clear();
    states.assign(ids.size(), 0);
    measured.assign(ids.size(), false);
    if (ids.empty()) {
        note = "no ids were asked about";
        return true;
    }

    std::uint32_t instances = 0;
    std::uint32_t templates = 0;
    const std::uintptr_t save = find_live_save(instances, templates);
    if (save == 0) {
        note = "no live DAPersistentSaveGame instance is in the object array (" +
               std::to_string(templates) + " template(s) were skipped)";
        return false;
    }
    const std::uintptr_t saveClass =
        static_cast<std::uintptr_t>(read_u64(save + kUObjectClassOffset));
    const FunctionRef getState = reflect_resolve_function(saveClass, "GetBossState");
    if (!getState.usable) {
        note = "GetBossState could not be resolved: " + getState.note;
        return false;
    }

    std::uint32_t readCount = 0;
    std::string refusal;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        std::uint32_t index = 0;
        std::uint32_t number = 0;
        std::string why;
        if (!string_to_fname(ids[i], index, number, why)) {
            if (refusal.empty()) refusal = "'" + ids[i] + "': " + why;
            continue;
        }
        std::uint8_t state = 0;
        if (!call_boss_state(save, getState, index, number, state, why)) {
            if (refusal.empty()) refusal = "'" + ids[i] + "': " + why;
            continue;
        }
        states[i] = state;
        measured[i] = true;
        ++readCount;
    }
    note = std::to_string(readCount) + " of " + std::to_string(ids.size()) + " state(s) read";
    if (!refusal.empty()) note += " -- first refusal: " + refusal;
    return readCount != 0;
}
std::vector<std::string> BossStateWriteResult::report_lines() const {
    std::vector<std::string> out;
    char buf[512];

    std::snprintf(buf, sizeof(buf), "setstate: asked to make '%s' %s", missionId.c_str(),
                  mission_state_name(requested));
    out.push_back(buf);

    if (saveFound) {
        std::snprintf(buf, sizeof(buf),
                      "setstate: save object 0x%llX; BossStates has %u slot(s), %u readable",
                      static_cast<unsigned long long>(save), elements, entriesScanned);
        out.push_back(buf);
    }
    if (beforeMeasured) {
        out.push_back(std::string("setstate: BEFORE -- the game's own GetBossState = ") +
                      mission_state_name(before));
    }
    if (elementFound) {
        std::snprintf(buf, sizeof(buf), "setstate: the entry is slot %u; its state byte is at 0x%llX",
                      elementSlot, static_cast<unsigned long long>(valueAddress));
        out.push_back(buf);
    }

    if (!note.empty()) {
        out.push_back("setstate: NOT WRITTEN -- " + note);
    } else {
        out.push_back("setstate: WROTE the one byte, on the game thread");
        if (afterMeasured) {
            out.push_back(std::string("setstate: AFTER -- the game's own GetBossState = ") +
                          mission_state_name(after) +
                          (after == requested
                               ? " <-- the game agrees, so this is the state it reads"
                               : " <-- THE GAME DOES NOT AGREE: the byte is not where this file thinks "
                                 "it is"));
        }
    }
    if (defeatedBeforeMeasured && defeatedAfterMeasured) {
        std::snprintf(buf, sizeof(buf), "setstate: GetDefeatedBossNum %d -> %d",
                      static_cast<int>(defeatedBefore), static_cast<int>(defeatedAfter));
        out.push_back(buf);
    }

    out.push_back("setstate: THE RECORD IS LEFT AS WRITTEN, ON PURPOSE. Nothing here removes an entry or "
                  "clears progress -- one byte of an existing entry is all this command touches. The game writes "
                  "Cleared back by itself when the boss is actually beaten, so there is nothing to undo: the worst "
                  "case of leaving this at Attempted is that the boss has to be fought once more.");
    return out;
}

namespace {

constexpr std::uint32_t kVolumeMissionWalls   = 0x0340;
constexpr std::uint32_t kVolumeMissionManager = 0x0350;
constexpr std::uint32_t kVolumeUnattempted    = 0x0420;
constexpr std::uint32_t kVolumeAttempted      = 0x0430;
constexpr std::uint32_t kVolumeCleared        = 0x0440;
constexpr std::uint32_t kSoftObjectStride     = 0x20;
constexpr std::uint32_t kSoftObjectNameAt     = 0x08;

constexpr std::uint32_t kMaxSoftLevels        = 512;
constexpr std::uint32_t kInitFrameBytes       = 0x18;

void append_soft_levels(std::uintptr_t volume, std::uint32_t offset, const char* label,
                        std::vector<std::string>& out, std::string& note) {
    ArrayHeader arr;
    std::string why;
    if (!read_tarray(volume + offset, kMaxSoftLevels, arr, why)) {
        note += std::string(note.empty() ? "" : "; ") + label + " could not be read: " + why;
        return;
    }
    for (std::uint32_t i = 0; i < arr.num; ++i) {
        const std::uintptr_t el =
            arr.data + static_cast<std::uintptr_t>(i) * kSoftObjectStride;
        std::uint32_t index = 0;
        std::uint32_t number = 0;
        if (!try_read_u32(el + kSoftObjectNameAt, index) ||
            !try_read_u32(el + kSoftObjectNameAt + 4, number)) {
            out.push_back(std::string(label) + "[" + std::to_string(i) + "] = <not readable>");
            continue;
        }
        std::string name;
        std::string whyName;
        if (!resolve_fname(index, name, whyName)) {
            out.push_back(std::string(label) + "[" + std::to_string(i) + "] = <FName index " +
                          std::to_string(index) + " did not resolve>");
            continue;
        }
        out.push_back(std::string(label) + " = '" + name + "'" +
                      (number == 0 ? "" : (" (number " + std::to_string(number) + ")")));
    }
}

std::uintptr_t find_mission_volume(const std::string& missionId, std::uint32_t nameIndex,
                                   std::uint32_t nameNumber, std::string& note) {
    std::uintptr_t found = 0;
    std::uint32_t volumes = 0;
    std::unordered_map<std::uintptr_t, bool> cache;
    for_each_object_address([&](std::uintptr_t obj) {
        if (found != 0) return;
        const std::uint64_t classPtr = read_u64(obj + kUObjectClassOffset);
        if (classPtr < 0x10000) return;
        if (!class_reaches_mission_volume(static_cast<std::uintptr_t>(classPtr), cache)) return;
        ++volumes;
        std::uint32_t index = 0;
        std::uint32_t number = 0;
        if (!try_read_u32(obj + kMissionId, index) || !try_read_u32(obj + kMissionId + 4, number)) return;
        if (index == nameIndex && number == nameNumber) found = obj;
    });
    if (found == 0) {
        note = volumes == 0
                   ? "no actor in the object array is a mission volume"
                   : (std::to_string(volumes) + " mission volume(s) exist, but none carries the MissionId "
                      "FName that '" + missionId + "' resolves to");
    }
    return found;
}

std::vector<std::string> loaded_boss_levels(std::string& note) {
    std::vector<std::string> out;
    const LiveObjects live = find_live_objects();
    if (live.world == 0) {
        note = "no live world";
        return out;
    }
    ArrayHeader levels;
    std::string why;
    if (!read_tarray(live.world + kWorldLevels, kMaxLevels, levels, why)) {
        note = "UWorld::Levels could not be read: " + why;
        return out;
    }
    for (std::uint32_t i = 0; i < levels.num; ++i) {
        const std::uint64_t levelPtr = read_u64(levels.data + i * 8);
        if (levelPtr < 0x10000) continue;
        const std::string name = name_of_object(static_cast<std::uintptr_t>(levelPtr));
        if (name.find("Boss") != std::string::npos || name.find("boss") != std::string::npos ||
            name.find("M01A01") != std::string::npos) {
            out.push_back(name);
        }
    }
    if (out.empty()) note = "no loaded level's name mentions boss or M01A01";
    return out;
}

}
std::vector<std::string> survey_mission_levels(const std::string& missionId) {
    std::vector<std::string> out;
    char buf[512];
    std::uint32_t nameIndex = 0;
    std::uint32_t nameNumber = 0;
    std::string why;
    if (!string_to_fname(missionId, nameIndex, nameNumber, why)) {
        out.push_back("levels: NOTHING READ -- " +
                      (why.empty() ? std::string("the id is empty") : why));
        return out;
    }
    std::string note;
    const std::uintptr_t volume = find_mission_volume(missionId, nameIndex, nameNumber, note);
    if (volume == 0) {
        out.push_back("levels: NOTHING READ -- " + note);
        return out;
    }
    std::snprintf(buf, sizeof(buf), "levels: volume '%s' [%s] @0x%llX for MissionId '%s'",
                  name_of_object(volume).c_str(), object_class_name(volume).c_str(),
                  static_cast<unsigned long long>(volume), missionId.c_str());
    out.push_back(buf);

    std::vector<std::string> unattempted;
    std::vector<std::string> attempted;
    std::vector<std::string> cleared;
    std::string levelsNote;
    append_soft_levels(volume, kVolumeUnattempted, "levels: UnattemptedStreamLevels", unattempted, levelsNote);
    append_soft_levels(volume, kVolumeAttempted, "levels: AttemptedStreamLevels", attempted, levelsNote);
    append_soft_levels(volume, kVolumeCleared, "levels: ClearedStreamLevels", cleared, levelsNote);
    out.insert(out.end(), unattempted.begin(), unattempted.end());
    out.insert(out.end(), attempted.begin(), attempted.end());
    out.insert(out.end(), cleared.begin(), cleared.end());
    if (unattempted.empty() && attempted.empty() && cleared.empty()) {
        out.push_back("levels: all three stream-level sets are EMPTY, so this arena swaps nothing and the "
                      "state cannot be what decides what is in it");
    }

    std::uint64_t manager = 0;
    if (try_read_u64(volume + kVolumeMissionManager, manager) && manager >= 0x10000) {
        std::snprintf(buf, sizeof(buf), "levels: MissionManager '@%s' @0x%llX",
                      object_class_name(static_cast<std::uintptr_t>(manager)).c_str(),
                      static_cast<unsigned long long>(manager));
        out.push_back(buf);
    } else {
        out.push_back("levels: the volume has NO MissionManager at");
    }

    std::string levelsWhy;
    const std::vector<std::string> loaded = loaded_boss_levels(levelsWhy);
    if (!loaded.empty()) {
        std::string line = "levels: LOADED right now (a level's name mentioning boss or M01A01):";
        for (const std::string& one : loaded) line += " '" + one + "'";
        out.push_back(line);
    } else {
        out.push_back("levels: " + levelsWhy);
    }
    if (!levelsNote.empty()) out.push_back("levels: " + levelsNote);
    return out;
}

MissionRelaunchResult relaunch_mission(const std::string& missionId) {
    MissionRelaunchResult r;
    r.missionId = missionId;
    if (missionId.empty()) {
        r.note = "no mission id";
        return r;
    }
    if (!string_to_fname(missionId, r.nameIndex, r.nameNumber, r.note)) return r;

    const std::uintptr_t volume = find_mission_volume(missionId, r.nameIndex, r.nameNumber, r.note);
    if (volume == 0) return r;
    r.volume = volume;
    r.volumeClass = object_class_name(volume);
    r.volumeName = name_of_object(volume);

    std::uint8_t type = 0;
    if (!try_read_bytes(volume + kMissionType, &type, 1) || type > 1) {
        r.note = "the volume's MissionType byte did not read as Boss(0) or Arena(1)";
        return r;
    }
    r.type = type;

    append_soft_levels(volume, kVolumeUnattempted, "levels: UnattemptedStreamLevels", r.unattemptedLevels,
                       r.levelsNote);
    append_soft_levels(volume, kVolumeAttempted, "levels: AttemptedStreamLevels", r.attemptedLevels,
                       r.levelsNote);
    append_soft_levels(volume, kVolumeCleared, "levels: ClearedStreamLevels", r.clearedLevels,
                       r.levelsNote);
    {
        ArrayHeader walls;
        std::string why;
        if (read_tarray(volume + kVolumeMissionWalls, 64, walls, why)) {
            r.walls = walls.num;
            r.wallsMeasured = true;
        } else {
            r.levelsNote += std::string(r.levelsNote.empty() ? "" : "; ") + "MissionWalls: " + why;
        }
    }

    const BossStateWriteResult previously = set_boss_state(missionId, 1);
    r.stateBeforeMeasured = previously.beforeMeasured;
    r.stateBefore = previously.before;
    r.stateAfterMeasured = previously.afterMeasured;
    r.stateAfter = previously.after;
    r.stateWritten = previously.written;
    if (!previously.written && !(previously.beforeMeasured && previously.before == 1)) {
        r.note = "the record could not be set to Attempted(1): " + previously.note;
        return r;
    }

    std::uint64_t manager = 0;
    if (!try_read_u64(volume + kVolumeMissionManager, manager) || manager < 0x10000) {
        r.note = "the volume has no MissionManager at, so there is nothing to re-initialise";
        return r;
    }
    r.manager = static_cast<std::uintptr_t>(manager);
    r.managerClass = object_class_name(r.manager);
    const std::uintptr_t managerClass = resolve_uclass_address("DAMissionManager");
    if (managerClass == 0) {
        r.note = "the DAMissionManager UClass is not in the object array";
        return r;
    }
    const FunctionRef initialize = reflect_resolve_function(managerClass, "ReceiveInitialize");
    if (!initialize.usable) {
        r.note = "ReceiveInitialize is not usable on DAMissionManager: " + initialize.note;
        return r;
    }

    unsigned char frame[kInitFrameBytes] = {};
    const std::uint64_t volumeAddress = static_cast<std::uint64_t>(volume);
    std::memcpy(frame + 0x00, &volumeAddress, sizeof(volumeAddress));
    std::memcpy(frame + 0x08, &type, 1);
    std::memcpy(frame + 0x0C, &r.nameIndex, sizeof(r.nameIndex));
    std::memcpy(frame + 0x10, &r.nameNumber, sizeof(r.nameNumber));
    const std::uint8_t attempted = 1;
    std::memcpy(frame + 0x14, &attempted, 1);

    const std::uintptr_t managerAddress = r.manager;
    std::string initializeNote;
    const bool dispatched = run_on_game_thread(
        [&]() {
            r.initializeCalled =
                reflect_invoke_function(managerAddress, initialize, frame, initializeNote);
        },
        2000);
    if (!dispatched) {
        r.note = "the game-thread dispatch for ReceiveInitialize was refused or timed out (is 'gthook' "
                 "installed?)";
        return r;
    }
    r.initializeNote = initializeNote;
    if (!r.initializeCalled) {
        r.note = "ReceiveInitialize was NOT called: " + initializeNote;
        return r;
    }

    const FunctionRef startedMode = reflect_resolve_function(managerClass, "ReceiveStartedMissionMode");
    if (startedMode.usable) {
        unsigned char modeFrame[kInitFrameBytes] = {};
        std::memcpy(modeFrame + 0x00, &type, 1);
        std::string modeNote;
        const bool modeDispatched = run_on_game_thread(
            [&]() {
                r.startedModeCalled =
                    reflect_invoke_function(managerAddress, startedMode, modeFrame, modeNote);
            },
            2000);
        r.startedModeNote = modeDispatched ? modeNote
                                           : std::string("the game-thread dispatch was refused or timed out");
    } else {
        r.startedModeNote = "ReceiveStartedMissionMode could not be resolved: " + startedMode.note;
    }

    const MissionStartOutcome started = start_mission_mode(missionId, type);
    r.startCalled = started.called;
    r.startNote = started.note;
    r.modeAfterMeasured = started.afterIsMissionModeMeasured;
    r.modeAfter = started.afterIsMissionMode;
    for (const std::string& line : started.report_lines()) r.lines.push_back(line);
    return r;
}

std::vector<std::string> MissionRelaunchResult::report_lines() const {
    std::vector<std::string> out;
    char buf[512];

    std::snprintf(buf, sizeof(buf), "relaunch: '%s' as %s", missionId.c_str(),
                  mission_type_name(type));
    out.push_back(buf);

    if (volume != 0) {
        std::snprintf(buf, sizeof(buf),
                      "relaunch: volume '%s' [%s] @0x%llX; MissionManager '%s' @0x%llX; %u mission wall(s)%s",
                      volumeName.c_str(), volumeClass.c_str(),
                      static_cast<unsigned long long>(volume), managerClass.c_str(),
                      static_cast<unsigned long long>(manager), walls,
                      wallsMeasured ? "" : " (walls NOT measured)");
        out.push_back(buf);
    }

    out.insert(out.end(), unattemptedLevels.begin(), unattemptedLevels.end());
    out.insert(out.end(), attemptedLevels.begin(), attemptedLevels.end());
    out.insert(out.end(), clearedLevels.begin(), clearedLevels.end());
    if (unattemptedLevels.empty() && attemptedLevels.empty() && clearedLevels.empty()) {
        out.push_back("relaunch: this arena declares NO stream-level sets, so the state does not decide "
                      "what is in it -- the boss actor is level data and something else must activate it");
    }
    if (!levelsNote.empty()) out.push_back("relaunch: " + levelsNote);

    if (stateBeforeMeasured) {
        out.push_back(std::string("relaunch: the record BEFORE was ") + mission_state_name(stateBefore) +
                      (stateWritten ? " -- it was written to Attempted(1)"
                                    : " -- already Attempted(1), so nothing was written"));
    }
    if (stateAfterMeasured) {
        out.push_back(std::string("relaunch: the record AFTER is ") + mission_state_name(stateAfter) +
                      (stateAfter == 1 ? " <-- the game agrees"
                                       : " <-- THE GAME DOES NOT AGREE"));
    }

    out.push_back(std::string("relaunch: ReceiveInitialize ") +
                  (initializeCalled ? std::string("was CALLED on the manager")
                                    : "was NOT called"));
    out.push_back(std::string("relaunch: ReceiveStartedMissionMode ") +
                  (startedModeCalled ? std::string("was CALLED on the manager")
                                     : ("was NOT called -- " + startedModeNote)));
    out.push_back(std::string("relaunch: StartMissionMode ") +
                  (startCalled ? std::string("was CALLED") : ("was NOT called: " + startNote)));
    if (modeAfterMeasured) {
        out.push_back(std::string("relaunch: IsMissionMode after both calls = ") +
                      (modeAfter ? "true" : "false"));
    }
    if (!note.empty()) out.push_back("relaunch: STOPPED -- " + note);

    out.push_back("relaunch: WATCH THE ARENA NOW. A streamed sublevel takes a moment to come in; if the "
                  "boss is still the inert one, run 'mission levels " + missionId +
                  "' and compare which levels are loaded. The record stays Attempted: put it back with "
                  "'mission setstate " + missionId + " cleared' when the test is over, and do not save "
                  "while it is changed.");
    return out;
}

std::vector<std::string> trigger_mission_volume(const std::string& missionId) {
    std::vector<std::string> out;
    char buf[512];
    std::string why;
    std::uint32_t nameIndex = 0;
    std::uint32_t nameNumber = 0;
    if (!string_to_fname(missionId, nameIndex, nameNumber, why)) {
        out.push_back("trigger: NOTHING WAS CALLED -- " +
                      (why.empty() ? std::string("the id is empty") : why));
        return out;
    }
    std::string note;
    const std::uintptr_t volume = find_mission_volume(missionId, nameIndex, nameNumber, note);
    if (volume == 0) {
        out.push_back("trigger: NOTHING WAS CALLED -- " + note);
        return out;
    }
    const LiveObjects live = find_live_objects();
    if (live.pawn < 0x10000) {
        out.push_back("trigger: NOTHING WAS CALLED -- no live pawn is known, so there is no actor to "
                      "claim an overlap with");
        return out;
    }
    const std::uintptr_t pawn = live.pawn;
    const std::uintptr_t actorClass = resolve_uclass_address("Actor");
    if (actorClass == 0) {
        out.push_back("trigger: NOTHING WAS CALLED -- the Actor UClass is not in the object array");
        return out;
    }
    const FunctionRef endOverlap = reflect_resolve_function(actorClass, "ReceiveActorEndOverlap");
    const FunctionRef beginOverlap = reflect_resolve_function(actorClass, "ReceiveActorBeginOverlap");
    if (!beginOverlap.usable) {
        out.push_back("trigger: NOTHING WAS CALLED -- ReceiveActorBeginOverlap cannot be resolved: " +
                      beginOverlap.note);
        return out;
    }

    std::snprintf(buf, sizeof(buf),
                  "trigger: volume '%s' @0x%llX, other actor = the local pawn '%s' @0x%llX",
                  name_of_object(volume).c_str(), static_cast<unsigned long long>(volume),
                  name_of_object(pawn).c_str(), static_cast<unsigned long long>(pawn));
    out.push_back(buf);

    unsigned char endFrame[0x10] = {};
    unsigned char beginFrame[0x10] = {};
    const std::uint64_t pawnValue = static_cast<std::uint64_t>(pawn);
    std::memcpy(endFrame + 0x00, &pawnValue, sizeof(pawnValue));
    std::memcpy(beginFrame + 0x00, &pawnValue, sizeof(pawnValue));

    const std::uintptr_t volumeAddress = volume;
    std::string endNote;
    std::string beginNote;
    bool endCalled = false;
    bool beginCalled = false;
    const bool dispatched = run_on_game_thread(
        [&]() {
            if (endOverlap.usable) {
                endCalled = reflect_invoke_function(volumeAddress, endOverlap, endFrame, endNote);
            }
            beginCalled = reflect_invoke_function(volumeAddress, beginOverlap, beginFrame, beginNote);
        },
        2000);
    if (!dispatched) {
        out.push_back("trigger: the game-thread dispatch was REFUSED or timed out, so nothing was called "
                      "(is 'gthook' installed?)");
        return out;
    }

    std::string line = "trigger: ReceiveActorEndOverlap ";
    line += endOverlap.usable ? (endCalled ? std::string("was CALLED")
                                           : ("was NOT called: " + endNote))
                              : std::string("could NOT be resolved");
    line += "; ReceiveActorBeginOverlap ";
    line += beginCalled ? std::string("was CALLED") : ("was NOT called: " + beginNote);
    out.push_back(line);
    out.push_back("trigger: this command writes NOTHING -- it replays the overlap the player's own feet "
                  "would produce, so that the arena's staging can be asked for without leaving the room. "
                  "WATCH THE ARENA.");
    return out;
}

namespace {

struct ActorBits {
    bool hiddenMeasured = false;
    bool hidden = false;
    bool collisionMeasured = false;
    bool collisionEnabled = false;
    bool hiddenInGameMeasured = false;
    bool hiddenInGame = false;
    std::string line() const {
        std::string s = " hidden(:0x05)=";
        s += hiddenMeasured ? (hidden ? "true" : "false") : "NOT READ";
        s += " collisionEnabled(:0x03)=";
        s += collisionMeasured ? (collisionEnabled ? "true" : "false") : "NOT READ";
        s += " hiddenInGame(:0x02)=";
        s += hiddenInGameMeasured ? (hiddenInGame ? "true" : "false") : "NOT READ";
        return s;
    }
};

bool read_bit(std::uintptr_t address, std::uint32_t offset, std::uint8_t bitIndex, bool& measured) {
    std::uint8_t byte = 0;
    if (!try_read_bytes(address + offset, &byte, 1)) {
        measured = false;
        return false;
    }
    measured = true;
    return ((byte >> bitIndex) & 1) != 0;
}

ActorBits read_actor_bits(std::uintptr_t actor) {
    ActorBits b;
    b.hidden = read_bit(actor, 0x0058, 0x05, b.hiddenMeasured);
    b.collisionEnabled = read_bit(actor, 0x005C, 0x03, b.collisionMeasured);
    b.hiddenInGame = read_bit(actor, 0x014D, 0x02, b.hiddenInGameMeasured);
    return b;
}

std::string behavior_state_suffix(std::uintptr_t controller) {
    std::uint8_t state = 0;
    if (!try_read_bytes(controller + kAiBehaviorStateOff, &state, 1)) {
        return " AIBehaviorState=NOT READ";
    }
    return std::string(" AIBehaviorState=") + ai_state_name(state);
}

}
BossReviveResult mission_revive_boss(const std::string& namePart) {
    BossReviveResult r;
    if (namePart.empty()) {
        r.note = "needs a name part, e.g. 'mission revive SEC_M01A01'";
        return r;
    }

    hp_watch_refresh_targets(false);
    const HpWatchFrame frame = hp_watch_sample();
    if (!frame.ok) {
        r.note = "the health list is not available: " + frame.note;
        return r;
    }
    const HpWatchSample* match = nullptr;
    std::size_t matches = 0;
    for (const HpWatchSample& s : frame.samples) {
        if (s.target.name.find(namePart) == std::string::npos &&
            s.target.cls.find(namePart) == std::string::npos) {
            continue;
        }
        ++matches;
        if (match == nullptr) match = &s;
    }
    if (match == nullptr) {
        r.note = "nothing in this level matches '" + namePart + "'";
        return r;
    }
    if (matches > 1) {
        r.note = "'" + namePart + "' matches " + std::to_string(matches) +
                 " characters, so none was chosen -- use a longer part of the name";
        return r;
    }
    if (match->target.name.find("NPC") != std::string::npos) {
        r.note = "'" + match->target.name + "' is an NPC, and NPCs are not touched";
        return r;
    }

    const std::uintptr_t actor = match->target.actor;
    const std::uintptr_t set = match->target.set;
    r.attempted = true;
    r.character = match->target.name;
    r.healthMeasured = match->healthRead;
    r.healthBefore = match->health;
    r.maxHealth = match->maxHealth;

    std::uint64_t controller = 0;
    if (!try_read_u64(actor + 0x0258, controller) || controller < 0x10000) {
        r.note = "'" + r.character + "' has no controller at, so the AI half has nothing to act on";
        return r;
    }
    const std::uintptr_t controllerAddress = static_cast<std::uintptr_t>(controller);
    r.controller = object_class_name(controllerAddress);

    char buf[512];
    std::snprintf(buf, sizeof(buf), "revive: BEFORE '%s' controller '%s': health %s%s",
                  r.character.c_str(), r.controller.c_str(),
                  r.healthMeasured
                      ? (std::to_string(r.healthBefore) + " / " + std::to_string(r.maxHealth)).c_str()
                      : "NOT READ",
                  read_actor_bits(actor).line().c_str());
    r.lines.push_back(buf);
    r.lines.push_back("revive: BEFORE AI" + behavior_state_suffix(controllerAddress));

    const std::uintptr_t aiClass = resolve_uclass_address("DAAIController");
    const FunctionRef boot = aiClass != 0 ? reflect_resolve_function(aiClass, "BootAI") : FunctionRef{};
    const FunctionRef stateFn =
        aiClass != 0 ? reflect_resolve_function(aiClass, "ChangeAIBehaviorState") : FunctionRef{};
    const std::uintptr_t actorClass = resolve_uclass_address("Actor");
    const FunctionRef setCollision =
        actorClass != 0 ? reflect_resolve_function(actorClass, "SetActorEnableCollision") : FunctionRef{};
    if (!setCollision.usable) {
        r.note = "SetActorEnableCollision is not usable, and without it the rest of the kit is not worth "
                 "calling: " + setCollision.note;
        return r;
    }

    std::string healthNote;
    std::string collisionNote;
    float readBack = 0.0f;
    const bool ran = run_on_game_thread(
        [&]() {
            if (r.healthMeasured && r.maxHealth > 0.0f && r.healthBefore + 0.5f < r.maxHealth) {
                r.healthWritten = hp_write_health(set, r.maxHealth, &readBack, healthNote);
                if (r.healthWritten) {
                    r.notifyCalled = hp_notify_health_changed(set, r.healthBefore, healthNote);
                }
            }
            unsigned char collisionFrame[0x10] = {};
            const std::uint8_t enable = 1;
            std::memcpy(collisionFrame + 0x00, &enable, 1);
            r.collisionEnabled = reflect_invoke_function(actor, setCollision, collisionFrame, collisionNote);
            if (boot.usable) {
                std::string bootNote;
                r.bootCalled = reflect_invoke_function(controllerAddress, boot, nullptr, bootNote);
            }
            if (stateFn.usable) {
                unsigned char stateFrame[0x10] = {};
                stateFrame[0] = 1;
                std::string stateNote;
                r.stateCalled = reflect_invoke_function(controllerAddress, stateFn, stateFrame, stateNote);
            }
        },
        3000);
    if (!ran) {
        r.note = "the game-thread dispatch was REFUSED or timed out, so nothing was written (is 'gthook' "
                 "installed?)";
        return r;
    }
    r.healthNote = healthNote;

    std::snprintf(buf, sizeof(buf),
                  "revive: health %s (max %.1f), announced to the game: %s%s",
                  r.healthWritten ? "WAS WRITTEN" : "was NOT written",
                  static_cast<double>(r.maxHealth),
                  r.notifyCalled ? "yes" : "no",
                  healthNote.empty() ? "" : (" -- " + healthNote).c_str());
    r.lines.push_back(buf);
    r.lines.push_back(std::string("revive: SetActorEnableCollision(true) ") +
                      (r.collisionEnabled ? "was CALLED" : ("was NOT called: " + collisionNote)) +
                      "; BootAI " + (r.bootCalled ? "was CALLED" : "was NOT called") +
                      "; ChangeAIBehaviorState(Battle) " + (r.stateCalled ? "was CALLED" : "was NOT called"));

    const HpWatchFrame after = hp_watch_sample();
    bool afterRead = false;
    for (const HpWatchSample& s : after.samples) {
        if (s.target.actor != actor || !s.healthRead) continue;
        r.healthAfter = s.health;
        afterRead = true;
        std::snprintf(buf, sizeof(buf), "revive: AFTER '%s': health %.1f / %.1f%s",
                      r.character.c_str(), static_cast<double>(s.health),
                      static_cast<double>(s.maxHealth),
                      read_actor_bits(actor).line().c_str());
        r.lines.push_back(buf);
        break;
    }
    if (!afterRead) {
        r.lines.push_back("revive: AFTER '" + r.character + "': health NOT READ BACK" +
                          read_actor_bits(actor).line());
    }
    r.lines.push_back("revive: AFTER AI" + behavior_state_suffix(controllerAddress));
    return r;
}

std::vector<std::string> BossReviveResult::report_lines() const {
    std::vector<std::string> out = lines;
    if (!note.empty()) out.push_back("revive: STOPPED -- " + note);
    if (note.empty()) {
        out.push_back("revive: WATCH IT. If it now has collision and stands up, the cleared boss is "
                      "revivable and boss sync becomes 'revive + start'; if it is still inert, the actor "
                      "this game leaves behind cannot be re-used and the mirror has to be our own spawn.");
    }
    return out;
}

namespace {

constexpr std::uint32_t kAiBrainComponent = 0x02E0;
constexpr std::uint32_t kBrainFrameBytes  = 0x10;

std::string brain_running_line(std::uintptr_t controller) {
    std::uint64_t brain = 0;
    if (!try_read_u64(controller + kAiBrainComponent, brain) || brain < 0x10000) {
        return " BrainComponent=NONE, so this controller has no behavior tree to run";
    }
    const auto brainAddress = static_cast<std::uintptr_t>(brain);
    std::string line = " BrainComponent=0x" + hex_addr(brainAddress).substr(2) + " [" +
                       object_class_name(brainAddress) + "]";
    const std::uintptr_t brainClass = resolve_uclass_address("BrainComponent");
    if (brainClass == 0) {
        line += " IsRunning=UNRESOLVED (the BrainComponent UClass is not in the object array)";
        return line;
    }
    const FunctionRef isRunning = reflect_resolve_function(brainClass, "IsRunning");
    if (!isRunning.usable) {
        line += " IsRunning=UNRESOLVED (" + isRunning.note + ")";
        return line;
    }
    unsigned char frame[kBrainFrameBytes] = {};
    std::string note;
    if (!reflect_invoke_function(brainAddress, isRunning, frame, note)) {
        line += " IsRunning=NOT CALLED (" + note + ")";
        return line;
    }
    line += std::string(" IsRunning=") + (frame[0] != 0 ? "true" : "false");
    return line;
}

std::vector<std::string> find_behavior_trees(const std::string& part, std::uint32_t& total) {
    std::vector<std::string> out;
    total = 0;
    for_each_object_address([&](std::uintptr_t obj) {
        if (object_class_name(obj) != "BehaviorTree") return;
        const std::string name = name_of_object(obj);
        if (name.empty() || name.rfind("Default__", 0) == 0) return;
        ++total;
        if (!part.empty() && !contains_ci(name, part.c_str())) return;
        if (out.size() < 24) out.push_back(name);
    });
    return out;
}

}
std::vector<std::string> mission_brain(const std::string& namePart, const std::string& treePart) {
    std::vector<std::string> out;
    char buf[512];
    if (namePart.empty()) {
        out.push_back("brain: needs a character name part, e.g. 'mission brain SEC_M01A01'");
        return out;
    }

    hp_watch_refresh_targets(false);
    const HpWatchFrame frame = hp_watch_sample();
    const HpWatchSample* match = nullptr;
    std::size_t matches = 0;
    if (frame.ok) {
        for (const HpWatchSample& s : frame.samples) {
            if (s.target.name.find(namePart) == std::string::npos &&
                s.target.cls.find(namePart) == std::string::npos) {
                continue;
            }
            ++matches;
            if (match == nullptr) match = &s;
        }
    }
    if (match == nullptr) {
        out.push_back("brain: NOTHING READ -- " + (matches == 0 ? "no match for '" + namePart + "'"
                                                               : "too many matches"));
        return out;
    }
    std::uint64_t controller = 0;
    if (!try_read_u64(match->target.actor + 0x0258, controller) || controller < 0x10000) {
        out.push_back("brain: '" + match->target.name + "' has no controller, so there is no brain to ask");
        return out;
    }
    const std::uintptr_t controllerAddress = static_cast<std::uintptr_t>(controller);
    std::snprintf(buf, sizeof(buf), "brain: '%s' controller '%s'%s", match->target.name.c_str(),
                  object_class_name(controllerAddress).c_str(),
                  brain_running_line(controllerAddress).c_str());
    out.push_back(buf);

    std::uint32_t totalTrees = 0;
    const std::vector<std::string> trees = find_behavior_trees(treePart, totalTrees);
    {
        std::string line = "brain: " + std::to_string(totalTrees) + " behavior tree asset(s) in the object "
                           "table" + (treePart.empty() ? "" : (" matching '" + treePart + "'")) + ":";
        for (const std::string& one : trees) line += " '" + one + "'";
        if (trees.empty() && !treePart.empty()) line += " NONE";
        out.push_back(line);
    }

    if (treePart.empty()) {
        out.push_back("brain: give a tree name to run it on this controller: 'mission brain " + namePart +
                      " <tree>' -- RunBehaviorTree is the only way in, because StartLogic/RestartLogic are "
                      "not UFUNCTIONs");
        return out;
    }
    if (trees.empty()) {
        out.push_back("brain: NOTHING WAS RUN -- no behavior tree asset matches '" + treePart + "'");
        return out;
    }

    const std::uintptr_t aiClass = resolve_uclass_address("AIController");
    if (aiClass == 0) {
        out.push_back("brain: NOTHING WAS RUN -- the AIController UClass is not in the object array");
        return out;
    }
    const FunctionRef runTree = reflect_resolve_function(aiClass, "RunBehaviorTree");
    if (!runTree.usable) {
        out.push_back("brain: NOTHING WAS RUN -- RunBehaviorTree is not usable: " + runTree.note);
        return out;
    }

    std::uintptr_t asset = 0;
    const std::string wanted = trees.front();
    for_each_object_address([&](std::uintptr_t obj) {
        if (asset != 0) return;
        if (object_class_name(obj) != "BehaviorTree") return;
        if (name_of_object(obj) == wanted) asset = obj;
    });
    if (asset == 0) {
        out.push_back("brain: NOTHING WAS RUN -- '" + wanted + "' could not be found a second time");
        return out;
    }

    unsigned char callFrame[kBrainFrameBytes] = {};
    const std::uint64_t assetValue = static_cast<std::uint64_t>(asset);
    std::memcpy(callFrame + 0x00, &assetValue, sizeof(assetValue));
    std::string callNote;
    bool ran = false;
    const bool dispatched = run_on_game_thread(
        [&]() { ran = reflect_invoke_function(controllerAddress, runTree, callFrame, callNote); }, 2000);
    if (!dispatched) {
        out.push_back("brain: the game-thread dispatch was REFUSED or timed out, so nothing was run");
        return out;
    }
    unsigned char returned = 0;
    std::memcpy(&returned, callFrame + 0x08, sizeof(returned));
    std::snprintf(buf, sizeof(buf), "brain: RunBehaviorTree('%s') %s, returned %s",
                  wanted.c_str(), ran ? "was CALLED" : ("was NOT called: " + callNote).c_str(),
                  returned != 0 ? "true" : "false");
    out.push_back(buf);
    out.push_back("brain: AFTER" + brain_running_line(controllerAddress));
    out.push_back("brain: WATCH IT. A running tree with a Battle state and a live target is a boss that "
                  "should attack; if it still does not, the actor this game leaves behind cannot be "
                  "re-used and the mirror has to be our own spawn.");
    return out;
}

namespace {

constexpr std::uint32_t kCharacterMesh    = 0x0280;
constexpr std::uint32_t kCharacterCapsule = 0x0290;
constexpr std::uint32_t kCollisionFrameBytes = 0x10;

const char* collision_enabled_name(std::uint8_t value) {
    switch (value) {
        case 0:  return "NoCollision(0)";
        case 1:  return "QueryOnly(1)";
        case 2:  return "PhysicsOnly(2)";
        case 3:  return "QueryAndPhysics(3)";
        default: return "?";
    }
}

struct ComponentReport {
    std::uintptr_t component = 0;
    std::string    className;
    bool           enabledMeasured = false;
    std::uint8_t   enabled = 0;
    bool           profileMeasured = false;
    std::string    profile;
};

ComponentReport read_component(std::uintptr_t actor, std::uint32_t offset, bool& present) {
    ComponentReport r;
    std::uint64_t component = 0;
    present = try_read_u64(actor + offset, component) && component >= 0x10000;
    if (!present) return r;
    r.component = static_cast<std::uintptr_t>(component);
    r.className = object_class_name(r.component);

    const std::uintptr_t primitiveClass = resolve_uclass_address("PrimitiveComponent");
    if (primitiveClass == 0) return r;
    const FunctionRef getEnabled = reflect_resolve_function(primitiveClass, "GetCollisionEnabled");
    const FunctionRef getProfile = reflect_resolve_function(primitiveClass, "GetCollisionProfileName");
    if (getEnabled.usable) {
        unsigned char frame[kCollisionFrameBytes] = {};
        std::string note;
        if (reflect_invoke_function(r.component, getEnabled, frame, note)) {
            r.enabledMeasured = true;
            r.enabled = frame[0];
        }
    }
    if (getProfile.usable) {
        unsigned char frame[kCollisionFrameBytes] = {};
        std::string note;
        if (reflect_invoke_function(r.component, getProfile, frame, note)) {
            std::uint32_t index = 0;
            std::uint32_t number = 0;
            std::memcpy(&index, frame + 0x00, sizeof(index));
            std::memcpy(&number, frame + 0x04, sizeof(number));
            std::string resolved;
            std::string why;
            if (index != 0 && resolve_fname(index, resolved, why)) {
                r.profileMeasured = true;
                r.profile = resolved;
            } else if (index == 0) {
                r.profileMeasured = true;
                r.profile = "<none>";
            }
        }
    }
    return r;
}

std::string component_line(const char* label, const ComponentReport& c) {
    std::string s = std::string(" ") + label + "=" + c.className + " collisionEnabled=";
    s += c.enabledMeasured ? collision_enabled_name(c.enabled) : "NOT READ";
    s += " profile=";
    s += c.profileMeasured ? ("'" + c.profile + "'") : "NOT READ";
    return s;
}

}
std::vector<std::string> mission_collide(const std::string& namePart) {
    std::vector<std::string> out;
    if (namePart.empty()) {
        out.push_back("collide: needs a character name part, e.g. 'mission collide SEC_M01A01'");
        return out;
    }
    hp_watch_refresh_targets(false);
    const HpWatchFrame frame = hp_watch_sample();
    const HpWatchSample* match = nullptr;
    std::size_t matches = 0;
    if (frame.ok) {
        for (const HpWatchSample& s : frame.samples) {
            if (s.target.name.find(namePart) == std::string::npos &&
                s.target.cls.find(namePart) == std::string::npos) {
                continue;
            }
            ++matches;
            if (match == nullptr) match = &s;
        }
    }
    if (match == nullptr) {
        out.push_back("collide: NOTHING READ -- no single match for '" + namePart + "'");
        return out;
    }
    if (match->target.name.find("NPC") != std::string::npos) {
        out.push_back("collide: '" + match->target.name + "' is an NPC, and NPCs are not touched");
        return out;
    }
    const std::uintptr_t actor = match->target.actor;

    bool meshPresent = false;
    bool capsulePresent = false;
    const ComponentReport meshBefore = read_component(actor, kCharacterMesh, meshPresent);
    const ComponentReport capsuleBefore = read_component(actor, kCharacterCapsule, capsulePresent);
    out.push_back(std::string("collide: BEFORE '") + match->target.name + "'" + component_line("Mesh", meshBefore) +
                  component_line("Capsule", capsuleBefore));
    if (!meshPresent && !capsulePresent) {
        out.push_back("collide: NOTHING WRITTEN -- neither nor holds a component pointer, "
                      "so the offsets this file uses are not this build's");
        return out;
    }

    const std::uintptr_t primitiveClass = resolve_uclass_address("PrimitiveComponent");
    if (primitiveClass == 0) {
        out.push_back("collide: NOTHING WRITTEN -- the PrimitiveComponent UClass is not in the object array");
        return out;
    }
    const FunctionRef setEnabled = reflect_resolve_function(primitiveClass, "SetCollisionEnabled");
    const FunctionRef setResponse =
        reflect_resolve_function(primitiveClass, "SetCollisionResponseToAllChannels");
    if (!setEnabled.usable) {
        out.push_back("collide: NOTHING WRITTEN -- SetCollisionEnabled is not usable: " + setEnabled.note);
        return out;
    }

    const std::uintptr_t mesh = meshPresent ? meshBefore.component : 0;
    const std::uintptr_t capsule = capsulePresent ? capsuleBefore.component : 0;
    bool meshEnabled = false;
    bool capsuleEnabled = false;
    bool meshBlocked = false;
    bool capsuleBlocked = false;
    std::string note;

    const bool dispatched = run_on_game_thread(
        [&]() {
            const std::uint8_t queryAndPhysics = 3;
            const std::uint8_t block = 2;
            if (mesh != 0) {
                unsigned char f[kCollisionFrameBytes] = {};
                f[0] = queryAndPhysics;
                meshEnabled = reflect_invoke_function(mesh, setEnabled, f, note);
                if (setResponse.usable) {
                    unsigned char r[kCollisionFrameBytes] = {};
                    r[0] = block;
                    meshBlocked = reflect_invoke_function(mesh, setResponse, r, note);
                }
            }
            if (capsule != 0) {
                unsigned char f[kCollisionFrameBytes] = {};
                f[0] = queryAndPhysics;
                capsuleEnabled = reflect_invoke_function(capsule, setEnabled, f, note);
                if (setResponse.usable) {
                    unsigned char r[kCollisionFrameBytes] = {};
                    r[0] = block;
                    capsuleBlocked = reflect_invoke_function(capsule, setResponse, r, note);
                }
            }
        },
        2000);
    if (!dispatched) {
        out.push_back("collide: the game-thread dispatch was REFUSED or timed out, so nothing was written");
        return out;
    }
    out.push_back(std::string("collide: SetCollisionEnabled(QueryAndPhysics) Mesh ") +
                  (meshEnabled ? "was CALLED" : "was NOT called") + ", Capsule " +
                  (capsuleEnabled ? "was CALLED" : "was NOT called") +
                  "; SetCollisionResponseToAllChannels(Block) Mesh " +
                  (meshBlocked ? "was CALLED" : "was NOT called") + ", Capsule " +
                  (capsuleBlocked ? "was CALLED" : "was NOT called"));

    bool meshAfterPresent = false;
    bool capsuleAfterPresent = false;
    const ComponentReport meshAfter = read_component(actor, kCharacterMesh, meshAfterPresent);
    const ComponentReport capsuleAfter = read_component(actor, kCharacterCapsule, capsuleAfterPresent);
    out.push_back(std::string("collide: AFTER '") + match->target.name + "'" + component_line("Mesh", meshAfter) +
                  component_line("Capsule", capsuleAfter));
    out.push_back("collide: SHOOT IT. Collision enabled and responses at Block is what a hit trace needs; "
                  "if it still cannot be hit, the damage path is gated somewhere above collision and the "
                  "next instrument is the damage watch, not another collision flag.");
    return out;
}

namespace {
constexpr std::uint32_t kPlayerControllerCheatManager = 0x0338;
constexpr std::uint32_t kSaveGameModeFrameBytes       = 0x08;
std::string file_time_text(const FILETIME& ft) {
    FILETIME local = {};
    SYSTEMTIME st = {};
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToSystemTime(&local, &st);
    char b[64];
    std::snprintf(b, sizeof(b), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
                  st.wMinute, st.wSecond);
    return b;
}

std::string newest_save_time(std::size_t& count, std::uint64_t& ticks) {
    count = 0;
    ticks = 0;
    char base[512] = {};
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof(base));
    if (n == 0 || n >= sizeof(base)) return "NOT MEASURED (LOCALAPPDATA is not set)";
    const std::string root = std::string(base) + "\\DollsNest\\Saved\\SaveGames";
    std::string newest = "NOT MEASURED (no file found)";
    ULARGE_INTEGER best = {};
    best.QuadPart = 0;
    const auto consider = [&](const WIN32_FIND_DATAA& d) {
        ULARGE_INTEGER v = {};
        v.LowPart = d.ftLastWriteTime.dwLowDateTime;
        v.HighPart = d.ftLastWriteTime.dwHighDateTime;
        ++count;
        if (v.QuadPart > best.QuadPart) {
            best = v;
            ticks = v.QuadPart;
            newest = file_time_text(d.ftLastWriteTime);
        }
    };
    WIN32_FIND_DATAA d;
    HANDLE h = FindFirstFileA((root + "\\*").c_str(), &d);
    if (h == INVALID_HANDLE_VALUE) return "NOT MEASURED (" + root + " could not be listed)";
    std::vector<std::string> dirs;
    do {
        if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            const std::string name = d.cFileName;
            if (name != "." && name != "..") dirs.push_back(name);
        } else {
            consider(d);
        }
    } while (FindNextFileA(h, &d) != 0);
    FindClose(h);
    for (const std::string& dir : dirs) {
        HANDLE hd = FindFirstFileA((root + "\\" + dir + "\\*").c_str(), &d);
        if (hd == INVALID_HANDLE_VALUE) continue;
        do {
            if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) consider(d);
        } while (FindNextFileA(hd, &d) != 0);
        FindClose(hd);
    }
    return newest;
}
}
std::vector<std::string> mission_save_game() {
    std::vector<std::string> out;
    char buf[512];
    const LiveObjects live = find_live_objects();
    if (live.world == 0) {
        out.push_back("save: NOTHING WAS ASKED FOR -- there is no live world");
        return out;
    }

    std::size_t savesBefore = 0;
    std::uint64_t ticksBefore = 0;
    const std::string stampBefore = newest_save_time(savesBefore, ticksBefore);
    out.push_back("save: the newest file under the game's save folder BEFORE this attempt is " + stampBefore +
                  " (" + std::to_string(savesBefore) + " file(s) listed)");

    const auto report_after = [&out, ticksBefore](const char* what) {
        std::size_t after = 0;
        std::uint64_t ticks = 0;
        const std::string stamp = newest_save_time(after, ticks);
        out.push_back(std::string("save: the newest file AFTER ") + what + " is " + stamp +
                      (ticks > ticksBefore
                           ? " <-- A FILE WAS STAMPED, so the game really wrote its save"
                           : " <-- NOTHING WAS WRITTEN (an async save may still land a moment later; the "
                             "next attempt's BEFORE line will show it)"));
    };

    if (live.playerController >= 0x10000) {
        std::uint64_t cheat = 0;
        if (!try_read_u64(live.playerController + kPlayerControllerCheatManager, cheat) || cheat < 0x10000) {
            out.push_back("save: the local PlayerController has NO CheatManager ( is null), which is "
                          "what a Shipping build looks like -- trying the game instance instead");
        } else {
            const auto cheatAddress = static_cast<std::uintptr_t>(cheat);
            const std::uintptr_t cls =
                static_cast<std::uintptr_t>(read_u64(cheatAddress + kUObjectClassOffset));
            std::string chain;
            const std::uintptr_t link = chain_find_named(cls, "DACheatManager", chain);
            if (link == 0) {
                out.push_back("save: the CheatManager's class chain does not reach 'DACheatManager' (" + chain +
                              ") -- trying the game instance instead");
            } else {
                const FunctionRef cheatSave = reflect_resolve_function(link, "SaveGame");
                if (!cheatSave.usable) {
                    out.push_back("save: CheatManager::SaveGame is not a callable UFunction: " + cheatSave.note +
                                  " -- trying the game instance instead");
                } else {
                    bool called = false;
                    std::string note;
                    const bool dispatched = run_on_game_thread(
                        [&]() { called = reflect_invoke_function(cheatAddress, cheatSave, nullptr, note); }, 5000);
                    std::snprintf(buf, sizeof(buf),
                                  "save: CheatManager::SaveGame %s%s",
                                  dispatched ? (called ? "was CALLED -- the game has written its own save"
                                                      : "was NOT called")
                                             : "could not be dispatched to the game thread",
                                  (!dispatched || !called) && !note.empty() ? (" -- " + note).c_str() : "");
                    out.push_back(buf);
                    if (dispatched && called) {
                        report_after("CheatManager::SaveGame");
                        out.push_back("save: now LOAD that save again (title screen -> continue, or any route "
                                      "that reloads). The record is Attempted, so the arena comes up as an "
                                      "unfinished mission and the game stages the whole encounter itself.");
                        return out;
                    }
                }
            }
        }
    } else {
        out.push_back("save: no local PlayerController is known, so the cheat path cannot be tried");
    }

    if (live.world != 0) {
        std::uint64_t gameMode = 0;
        if (!try_read_u64(live.world + world_offsets::kAuthorityGameMode, gameMode) || gameMode < 0x10000) {
            out.push_back("save: UWorld::AuthorityGameMode is 0, so the game mode's own Save cannot be "
                          "asked for either");
        } else {
            const auto gameModeAddress = static_cast<std::uintptr_t>(gameMode);
            const std::uintptr_t gameModeClass =
                static_cast<std::uintptr_t>(read_u64(gameModeAddress + kUObjectClassOffset));
            std::string chain;
            const std::uintptr_t link = chain_find_named(gameModeClass, "DAGameMode", chain);
            if (link == 0) {
                out.push_back("save: the game mode's class chain does not reach 'DAGameMode' (" + chain + ")");
            } else {
                const FunctionRef saveVerb = reflect_resolve_function(link, "Save");
                if (!saveVerb.usable) {
                    out.push_back("save: DAGameMode::Save is not a callable UFunction: " + saveVerb.note);
                } else {
                    unsigned char saveGateFrame[kSaveGameModeFrameBytes] = {};
                    bool called = false;
                    std::string note;
                    const bool dispatched = run_on_game_thread(
                        [&]() {
                            called = reflect_invoke_function(gameModeAddress, saveVerb, saveGateFrame, note);
                        },
                        5000);
                    std::snprintf(buf, sizeof(buf), "save: DAGameMode::Save(false) %s%s",
                                  dispatched ? (called ? "was CALLED -- the game's own rest-point save"
                                                       : "was NOT called")
                                             : "could not be dispatched to the game thread",
                                  (!dispatched || !called) && !note.empty() ? (" -- " + note).c_str() : "");
                    out.push_back(buf);
                    if (dispatched && called) {
                        report_after("DAGameMode::Save(false)");
                        out.push_back("save: now LOAD that save again (title screen -> continue). The record is "
                                      "Attempted, so the arena comes up as an unfinished mission and the game "
                                      "stages the whole encounter itself.");
                        return out;
                    }
                }
            }
        }
    }

    out.push_back("save: NO SAVE ROUTE WAS AVAILABLE on this build. The candidate named by the deleted fallback "
                  "is the EMS plugin's async verb (EMSAsyncSaveGame::AsyncSaveActors), which needs its async "
                  "action activated -- a different shape of call. The two file stamps above are what will say "
                  "whether it worked.");
    return out;
}
}
