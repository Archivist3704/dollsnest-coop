#include <windows.h>

#include <cstdio>
#include <string>

namespace {

using FnInit = int(__cdecl*)(const char*);
using FnStamp = const char*(__cdecl*)(void);
using FnApi = unsigned int(__cdecl*)(void);
using FnShutdown = void(__cdecl*)(void);

template <typename T>
T fn_cast(FARPROC p) {
    return reinterpret_cast<T>(reinterpret_cast<void*>(p));
}

}

int main(int argc, char** argv) {
    const char* dll = (argc > 1) ? argv[1] : "dollsnest-coop.dll";
    const char* args = (argc > 2) ? argv[2] : "local-probe";

    std::printf("[probe] LoadLibraryEx(%s)\n", dll);

    HMODULE mod = LoadLibraryA(dll);
    if (!mod) {
        std::printf("!! LoadLibraryA failed, GetLastError=%lu\n", GetLastError());
        return 2;
    }
    std::printf("[probe] loaded at 0x%p\n", static_cast<void*>(mod));

    auto api = fn_cast<FnApi>(GetProcAddress(mod, "DSH_ApiVersion"));
    auto stamp = fn_cast<FnStamp>(GetProcAddress(mod, "DSH_BuildStamp"));
    auto init = fn_cast<FnInit>(GetProcAddress(mod, "DSH_Init"));
    auto shutdown = fn_cast<FnShutdown>(GetProcAddress(mod, "DSH_Shutdown"));

    if (!init) { std::printf("!! DSH_Init not found\n"); return 2; }

    std::printf("[probe] api=%u stamp=%s\n", api ? api() : 0u, stamp ? stamp() : "(none)");

    std::printf("[probe] calling DSH_Init ...\n");
    std::fflush(stdout);
    const int rc = init(args);
    std::printf("[probe] DSH_Init returned %d\n", rc);

    if (shutdown) {
        std::printf("[probe] calling DSH_Shutdown ...\n");
        shutdown();
    }
    std::printf("[probe] done, survived\n");
    return rc;
}
