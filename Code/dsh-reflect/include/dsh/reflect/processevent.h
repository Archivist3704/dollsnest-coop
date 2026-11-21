#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dsh::reflect {

struct ProcessEventCandidate {
    std::uint32_t vtableIndex = 0;
    std::uintptr_t address = 0;
    int           score = 0;
    std::string   evidence;

    [[nodiscard]] bool plausible() const noexcept { return score >= 2; }
};

struct ProcessEventSearch {
    bool                              found = false;
    ProcessEventCandidate             best;
    std::vector<ProcessEventCandidate> all;

    std::uintptr_t uobjectVtable = 0;
    std::uint32_t  vtableSlotsScanned = 0;
    std::uint32_t  readableSlots = 0;
    std::string    note;

    [[nodiscard]] std::string summary() const;
};

[[nodiscard]] ProcessEventSearch find_process_event();

}
