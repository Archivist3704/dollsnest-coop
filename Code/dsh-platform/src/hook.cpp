#include "dsh/platform/hook.h"

#include "dsh/platform/log.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <unordered_map>

#if defined(DSH_HAVE_MINHOOK)
#include <MinHook.h>
#endif

namespace dsh::platform {
namespace {

struct HookRecord {
    std::string        id;
    void*              target = nullptr;
    void*              detour = nullptr;
    void*              original = nullptr;
    std::atomic<std::uint64_t> calls{0};
};

std::mutex                             g_mutex;
std::unordered_map<std::string, HookRecord*> g_byId;
std::atomic<bool>                      g_initialised{false};

bool in_executable_region(void* p) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                       PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & exec) != 0;
}

}

bool hooks_available() {
#if defined(DSH_HAVE_MINHOOK)
    return true;
#else
    return false;
#endif
}

bool hooks_initialize() {
    if (!hooks_available()) {
        DSH_FATAL("hooking UNAVAILABLE: this build was compiled without MinHook. "
                  "Consequence: no ProcessEvent interception, so no gameplay event "
                  "capture or replay is possible. Fix: restore Code/third_party/minhook and rebuild.");
        return false;
    }
#if defined(DSH_HAVE_MINHOOK)
    if (g_initialised.load()) return true;

    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        DSH_FATAL("MH_Initialize failed (%s) -- no hooks will be installed",
                  MH_StatusToString(status));
        return false;
    }
    g_initialised.store(true);
    DSH_INFO("MinHook initialised (hooks available)");
    return true;
#else
    return false;
#endif
}

HookResult install_hook(const char* id, void* target, void* detour, void** originalOut) {
    HookResult result;
    if (!id || !target || !detour) {
        result.note = "null argument";
        DSH_ERROR("install_hook('%s'): %s -- refusing", id ? id : "<null>", result.note.c_str());
        return result;
    }

    if (!hooks_available()) {
        result.note = "MinHook not compiled in";
        DSH_ERROR("install_hook('%s'): %s -- the hook will not exist", id, result.note.c_str());
        return result;
    }

    if (!in_executable_region(target)) {
        result.note = "target is not in an executable region";
        DSH_ERROR("install_hook('%s'): %s (target=%p, %s) -- refusing", id, result.note.c_str(),
                  target, "this usually means the address was computed wrong");
        return result;
    }

#if defined(DSH_HAVE_MINHOOK)
    if (!hooks_initialize()) {
        result.note = "MinHook not initialised";
        return result;
    }

    MH_STATUS status = MH_CreateHook(target, detour, originalOut);
    if (status != MH_OK) {
        result.note = MH_StatusToString(status);
        DSH_ERROR("install_hook('%s'): MH_CreateHook failed (%s) at %p", id, result.note.c_str(),
                  target);
        return result;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK) {
        result.note = MH_StatusToString(status);
        DSH_ERROR("install_hook('%s'): MH_EnableHook failed (%s) at %p", id, result.note.c_str(),
                  target);
        MH_RemoveHook(target);
        return result;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto* rec = new HookRecord();
        rec->id = id;
        rec->target = target;
        rec->detour = detour;
        rec->original = originalOut ? *originalOut : nullptr;
        g_byId[rec->id] = rec;
        result.counter = 0;
    }

    result.installed = true;
    result.note = "ok";
    DSH_INFO("install_hook('%s'): installed. target=%p detour=%p original=%p", id, target, detour,
             originalOut ? *originalOut : nullptr);
    return result;
#else
    result.note = "MinHook not compiled in";
    return result;
#endif
}

std::uint64_t hook_call_count(const char* id) {
    if (!id) return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_byId.find(id);
    if (it == g_byId.end()) return 0;
    return it->second->calls.load(std::memory_order_relaxed);
}

void hook_note_call(const char* id) {
    if (!id) return;

    HookRecord* rec = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_byId.find(id);
        if (it == g_byId.end()) return;
        rec = it->second;
    }
    rec->calls.fetch_add(1, std::memory_order_relaxed);
}

}
