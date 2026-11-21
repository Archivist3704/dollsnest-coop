#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dsh::reflect {

struct ObjectArrayCandidate {
    std::uintptr_t containerAddress = 0;
    std::uintptr_t itemsPointer = 0;
    std::uint64_t  itemsOffset = 0;
    std::uint32_t  reportedCount = 0;
    std::uint32_t  objectsChecked = 0;
    std::uint32_t  objectsValid = 0;
    std::uint32_t  vtablesValid = 0;

    std::uint32_t  namedChecked = 0;
    std::uint32_t  namedValid = 0;

    [[nodiscard]] double confidence() const noexcept {
        return objectsChecked ? static_cast<double>(objectsValid) / objectsChecked : 0.0;
    }
    [[nodiscard]] double named_confidence() const noexcept {
        return namedChecked ? static_cast<double>(namedValid) / namedChecked : 0.0;
    }
};

struct ObjectArraySearch {
    bool                          found = false;
    ObjectArrayCandidate          best;
    std::vector<ObjectArrayCandidate> strong;
    std::uint64_t                 regionsScanned = 0;
    std::uint64_t                 regionsSkipped = 0;
    std::uint64_t                 candidatesExamined = 0;
    std::uint64_t                 bytesScanned = 0;
    std::string                   note;

    [[nodiscard]] std::string summary() const;
};

[[nodiscard]] ObjectArraySearch find_object_array(double minValidFraction = 0.75,
                                                  std::uint32_t probeCount = 64);

struct NameTableCandidate {
    std::uintptr_t containerAddress = 0;
    std::uintptr_t entriesPointer = 0;
    std::uint64_t  entriesOffset = 0;
    std::uint32_t  namesChecked = 0;
    std::uint32_t  namesValid = 0;
    std::vector<std::string> samples;

    [[nodiscard]] double confidence() const noexcept {
        return namesChecked ? static_cast<double>(namesValid) / namesChecked : 0.0;
    }
};

struct NameTableSearch {
    bool                             found = false;
    NameTableCandidate               best;
    std::vector<NameTableCandidate>  strong;
    std::uint64_t                    candidatesExamined = 0;
    std::string                      note;

    [[nodiscard]] std::string summary() const;
};

[[nodiscard]] bool name_of(std::uint32_t index, std::string& out);

[[nodiscard]] bool class_name_of_object(std::uintptr_t object, std::string& out);

std::uint64_t for_each_object(std::uint64_t maxObjects,
                              const std::function<void(std::uintptr_t)>& fn);

struct ReflectionState {
    bool              objectArrayReady = false;
    bool              nameTableReady = false;
    std::uintptr_t    objectArrayContainer = 0;
    std::uintptr_t    objectItems = 0;
    std::uintptr_t    nameTableContainer = 0;
    std::uintptr_t    nameEntries = 0;
    std::uint32_t     objectCount = 0;
};

[[nodiscard]] const ReflectionState& reflection_state();

bool initialise_reflection(bool forceRescan = false);

enum class ObjectLiveness : std::uint8_t {
    live = 0,
    gone = 1,
    indexOutOfRange = 2,
    notReadable = 3,
};

[[nodiscard]] inline const char* to_string(ObjectLiveness v) noexcept {
    switch (v) {
        case ObjectLiveness::live:            return "live";
        case ObjectLiveness::gone:            return "gone";
        case ObjectLiveness::indexOutOfRange: return "indexOutOfRange";
        case ObjectLiveness::notReadable:     return "notReadable";
    }
    return "?";
}

[[nodiscard]] inline ObjectLiveness judge_object_slot(bool slotReadable, std::uintptr_t self,
                                                      std::uint32_t index, std::uint32_t liveElements,
                                                      std::uintptr_t objectAtItsIndex) noexcept {
    if (liveElements == 0) return ObjectLiveness::indexOutOfRange;
    if (index >= liveElements) return ObjectLiveness::indexOutOfRange;
    if (!slotReadable) return ObjectLiveness::notReadable;
    if (objectAtItsIndex != self) return ObjectLiveness::gone;
    return ObjectLiveness::live;
}

[[nodiscard]] bool object_is_live(std::uintptr_t object, std::string& note);

[[nodiscard]] std::string liveness_selfcheck_report();

}
