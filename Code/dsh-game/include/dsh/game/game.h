#pragma once

#include <cstdint>
#include <string>

namespace dsh::game {

struct ProcessFacts {
    std::uint32_t pid = 0;
    std::string   exePath;
    std::string   exeBaseName;
    std::string   mainModuleBase;
    std::uint64_t mainModuleSize = 0;
};

[[nodiscard]] ProcessFacts gather_process_facts();

[[nodiscard]] std::string probe_summary();

}
