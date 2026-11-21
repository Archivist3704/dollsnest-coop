#include "dsh/reflect/scanner.h"

#include "dsh/platform/log.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

#pragma comment(lib, "psapi.lib")

namespace dsh::reflect {
namespace {

std::mutex                            g_moduleCacheMutex;
std::vector<ModuleInfo>               g_moduleCache;
std::uint64_t                         g_moduleCacheStamp = 0;

std::atomic<std::uint64_t>            g_pointerCheckCount{0};
std::atomic<std::uint64_t>            g_rangeRefreshCount{0};

std::string base_name(const std::string& path) {
    const auto pos = path.find_last_of("\\/");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

std::vector<ModuleInfo> enumerate_modules() {
    std::vector<ModuleInfo> out;

    HMODULE mods[512] = {};
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        DSH_WARN("scanner: EnumProcessModules failed err=%lu -- module list unavailable",
                 GetLastError());
        return out;
    }

    const DWORD count = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < count && i < 512; ++i) {
        MODULEINFO info{};
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &info, sizeof(info))) continue;

        char path[MAX_PATH] = {};
        GetModuleFileNameExA(GetCurrentProcess(), mods[i], path, MAX_PATH);

        ModuleInfo m;
        m.fullPath = path;
        m.name = base_name(m.fullPath);
        m.base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
        m.size = info.SizeOfImage;
        out.push_back(std::move(m));
    }
    return out;
}

const std::vector<ModuleInfo>& modules_cached() {
    const std::uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_moduleCacheMutex);
    if (g_moduleCache.empty() || now - g_moduleCacheStamp > 2000) {
        g_moduleCache = enumerate_modules();
        g_moduleCacheStamp = now;
    }
    return g_moduleCache;
}

const std::uint8_t* safe_map(std::uintptr_t address, std::size_t size) {
    if (address == 0 || size == 0) return nullptr;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0) return nullptr;
    if (mbi.State != MEM_COMMIT) return nullptr;
    const DWORD bad = PAGE_NOACCESS | PAGE_GUARD;
    if ((mbi.Protect & bad) != 0) return nullptr;

    const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (address + size > regionEnd) return nullptr;
    return reinterpret_cast<const std::uint8_t*>(address);
}

}

ModuleInfo main_module() {
    ModuleInfo m;
    const HMODULE base = GetModuleHandleA(nullptr);
    m.base = reinterpret_cast<std::uintptr_t>(base);
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) > 0) {
        m.fullPath = path;
        m.name = base_name(m.fullPath);
    }
    MODULEINFO info{};
    if (GetModuleInformation(GetCurrentProcess(), base, &info, sizeof(info))) {
        m.size = info.SizeOfImage;
    }
    return m;
}

bool looks_like_image_pointer(std::uintptr_t v) {
    if (v < 0x10000) return false;
    if ((v & 0x7) != 0) return false;

    struct Snapshot {
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> ranges;
        std::uint64_t stamp = 0;
    };
    thread_local Snapshot snap;

    const std::uint64_t now = GetTickCount64();
    if (snap.ranges.empty() || now - snap.stamp > 2000) {
        snap.ranges.clear();
        for (const auto& m : modules_cached()) {
            if (m.size == 0) continue;
            snap.ranges.emplace_back(m.base, m.base + m.size);
        }
        std::sort(snap.ranges.begin(), snap.ranges.end());
        snap.stamp = now;
        ++g_rangeRefreshCount;
    }
    ++g_pointerCheckCount;

    const auto it = std::upper_bound(
        snap.ranges.begin(), snap.ranges.end(), v,
        [](std::uintptr_t value, const std::pair<std::uintptr_t, std::uintptr_t>& r) {
            return value < r.first;
        });
    if (it == snap.ranges.begin()) return false;
    const auto& cand = *(it - 1);
    return v >= cand.first && v < cand.second;
}

std::uint64_t pointer_check_count() { return g_pointerCheckCount.load(std::memory_order_relaxed); }
std::uint64_t range_refresh_count() { return g_rangeRefreshCount.load(std::memory_order_relaxed); }
void reset_scanner_counters() {
    g_pointerCheckCount.store(0, std::memory_order_relaxed);
    g_rangeRefreshCount.store(0, std::memory_order_relaxed);
}

bool try_read_u64(std::uintptr_t address, std::uint64_t& out) {
    const std::uint8_t* p = safe_map(address, sizeof(std::uint64_t));
    if (!p) return false;
    std::memcpy(&out, p, sizeof(out));
    return true;
}

bool try_read_u32(std::uintptr_t address, std::uint32_t& out) {
    const std::uint8_t* p = safe_map(address, sizeof(std::uint32_t));
    if (!p) return false;
    std::memcpy(&out, p, sizeof(out));
    return true;
}

bool try_read_bytes(std::uintptr_t address, void* dst, std::size_t n) {
    const std::uint8_t* p = safe_map(address, n);
    if (!p) return false;
    std::memcpy(dst, p, n);
    return true;
}

std::string describe_region(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0) {
        return "unmapped";
    }
    char buf[192];
    const char* state = (mbi.State == MEM_COMMIT) ? "commit"
                      : (mbi.State == MEM_FREE)   ? "free"
                                                  : "reserve";
    const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    std::string owner = "?";
    for (const auto& m : modules_cached()) {
        if (m.contains(address)) { owner = m.name; break; }
    }
    std::snprintf(buf, sizeof(buf), "%s base=0x%llX size=0x%llX protect=0x%lX type=0x%lX in=%s",
                  state, static_cast<unsigned long long>(base),
                  static_cast<unsigned long long>(mbi.RegionSize),
                  static_cast<unsigned long>(mbi.Protect),
                  static_cast<unsigned long>(mbi.Type), owner.c_str());
    return buf;
}

std::size_t module_count() {
    return modules_cached().size();
}

std::vector<SectionInfo> sections_of(const ModuleInfo& module) {
    std::vector<SectionInfo> out;
    if (module.base == 0) return out;

    const auto base = reinterpret_cast<const std::uint8_t*>(module.base);
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(base), &mbi, sizeof(mbi)) == 0) return out;
    if (mbi.State != MEM_COMMIT) return out;
    const DWORD bad = PAGE_NOACCESS | PAGE_GUARD | PAGE_READONLY;
    if ((mbi.Protect & bad) != 0) return out;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return out;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return out;
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return out;

    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        SectionInfo s;

        char name[9] = {};
        std::memcpy(name, section->Name, 8);
        s.name = name;
        s.start = module.base + section->VirtualAddress;
        s.size = section->Misc.VirtualSize != 0 ? section->Misc.VirtualSize : section->SizeOfRawData;
        s.characteristics = section->Characteristics;
        out.push_back(std::move(s));
    }
    return out;
}

}
