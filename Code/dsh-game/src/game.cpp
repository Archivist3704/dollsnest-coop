#include "dsh/game/game.h"

#include "dsh/platform/log.h"
#include "dsh/reflect/reflect.h"

#include <windows.h>
#include <psapi.h>

#include <cstdio>

#pragma comment(lib, "psapi.lib")

namespace dsh::game {
namespace {

std::string hex64(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

}

ProcessFacts gather_process_facts() {
    ProcessFacts f;
    f.pid = GetCurrentProcessId();

    char buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        f.exePath.assign(buf, n);
        const auto pos = f.exePath.find_last_of("\\/");
        f.exeBaseName = (pos == std::string::npos) ? f.exePath : f.exePath.substr(pos + 1);
    }

    const HMODULE base = GetModuleHandleA(nullptr);
    f.mainModuleBase = hex64(reinterpret_cast<std::uint64_t>(base));

    HMODULE mods[1024] = {};
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        MODULEINFO info{};
        if (GetModuleInformation(GetCurrentProcess(), base, &info, sizeof(info))) {
            f.mainModuleSize = info.SizeOfImage;
        }
    }
    return f;
}

std::string probe_summary() {
    std::string out;
    out += "game layer capability report\n";

#if DSH_HAVE_LAYOUT
    out += " layout headers : included at compile time\n";
#else
    out += " layout headers : not included -- by design. Offsets live in\n"
           " offsets.h and are verified at runtime; engine reflection\n"
           " tables are located at runtime. This is not a limitation.\n";
#endif
    out += " object lookup, processevent and native spawn: available (see dsh-reflect)\n";
    return out;
}

}
