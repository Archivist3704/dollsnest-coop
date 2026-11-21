#pragma once

#include <cstdint>
#include <string>

namespace dsh::platform {

[[nodiscard]] bool hooks_available();

bool hooks_initialize();

struct HookResult {
    bool          installed = false;
    std::uint64_t counter = 0;
    std::string   note;
};

[[nodiscard]] HookResult install_hook(const char* id, void* target, void* detour,
                                      void** originalOut);

[[nodiscard]] std::uint64_t hook_call_count(const char* id);

void hook_note_call(const char* id);

}
