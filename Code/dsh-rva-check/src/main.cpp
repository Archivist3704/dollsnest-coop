#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

void say(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::fflush(stdout);
}

bool export_rva(const char* path, const char* want, std::uint32_t& outRva) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(file, &sz);
    const auto limit = static_cast<std::size_t>(sz.QuadPart);
    HANDLE map = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!map) { CloseHandle(file); return false; }
    const auto* base = static_cast<const std::uint8_t*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
    if (!base) { CloseHandle(map); CloseHandle(file); return false; }
    const auto fits = [limit](std::uint64_t o, std::uint64_t n) { return o <= limit && n <= limit - o; };

    bool found = false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto ntOff = static_cast<std::uint64_t>(dos->e_lfanew);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + ntOff);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const std::uint64_t secOff =
        ntOff + offsetof(IMAGE_NT_HEADERS, OptionalHeader) + nt->FileHeader.SizeOfOptionalHeader;
    const auto secCount = nt->FileHeader.NumberOfSections;
    const auto* secs = reinterpret_cast<const IMAGE_SECTION_HEADER*>(base + secOff);
    const auto toOff = [&](std::uint32_t rva, std::uint64_t& out) -> bool {
        for (std::uint32_t s = 0; s < secCount; ++s) {
            const std::uint64_t v = secs[s].VirtualAddress;
            const std::uint64_t span =
                std::max<std::uint64_t>(secs[s].Misc.VirtualSize, secs[s].SizeOfRawData);
            if (rva < v || rva >= v + span) continue;
            const std::uint64_t d = rva - v;
            if (d >= secs[s].SizeOfRawData) return false;
            out = secs[s].PointerToRawData + d;
            return true;
        }
        if (rva < nt->OptionalHeader.SizeOfHeaders) { out = rva; return true; }
        return false;
    };
    std::uint64_t expOff = 0, nOff = 0, oOff = 0, fOff = 0;
    do {
        if (!toOff(dir.VirtualAddress, expOff)) break;
        if (!fits(expOff, sizeof(IMAGE_EXPORT_DIRECTORY))) break;
        const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + expOff);
        if (!toOff(exp->AddressOfNames, nOff) || !toOff(exp->AddressOfNameOrdinals, oOff) ||
            !toOff(exp->AddressOfFunctions, fOff)) {
            break;
        }
        const auto* names = reinterpret_cast<const DWORD*>(base + nOff);
        const auto* ords = reinterpret_cast<const WORD*>(base + oOff);
        for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
            std::uint64_t no = 0;
            if (!toOff(names[i], no) || !fits(no, 2)) continue;
            if (std::strcmp(reinterpret_cast<const char*>(base + no), want) != 0) continue;
            outRva = *reinterpret_cast<const DWORD*>(base + fOff + ords[i] * sizeof(DWORD));
            found = true;
            break;
        }
    } while (false);

    UnmapViewOfFile(base);
    CloseHandle(map);
    CloseHandle(file);
    return found;
}

void dump_bytes(const char* label, const void* p, int n) {
    const auto* b = static_cast<const unsigned char*>(p);
    char hex[3 * 32 + 1] = {};
    for (int i = 0; i < n; ++i) {
        char t[4];
        std::snprintf(t, sizeof(t), "%02X ", b[i]);
        std::strcat(hex, t);
    }
    say(" %-14s %p : %s\n", label, p, hex);
}

}

int main(int argc, char** argv) {
    if (argc < 3) {
        say("usage: dsh-rva-check <dll> <export name> [more exports...]\n");
        return 2;
    }
    const char* dll = argv[1];
    say("dsh-rva-check %s\n", dll);

    HMODULE mod = LoadLibraryA(dll);
    if (!mod) { say("!! LoadLibrary failed err=%lu\n", GetLastError()); return 2; }
    const auto modBase = reinterpret_cast<std::uintptr_t>(mod);
    say("module base 0x%llX\n", static_cast<unsigned long long>(modBase));

    {
        HANDLE f = CreateFileA(dll, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            HANDLE m = CreateFileMappingA(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (m) {
                const auto* b = static_cast<const std::uint8_t*>(MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0));
                if (b) {
                    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
                    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(b + dos->e_lfanew);
                    say("PE ImageBase 0x%llX\n",
                        static_cast<unsigned long long>(nt->OptionalHeader.ImageBase));
                    UnmapViewOfFile(b);
                }
                CloseHandle(m);
            }
            CloseHandle(f);
        }
    }

    for (int i = 2; i < argc; ++i) {
        const char* name = argv[i];
        auto local = reinterpret_cast<std::uintptr_t>(GetProcAddress(mod, name));
        std::uint32_t peRva = 0;
        const bool havePe = export_rva(dll, name, peRva);

        say("\nexport '%s'\n", name);
        if (!local) { say(" !! GetProcAddress failed\n"); continue; }
        const auto localRva = local - modBase;
        say(" GetProcAddress 0x%llX -> localRva 0x%llX\n",
            static_cast<unsigned long long>(local), static_cast<unsigned long long>(localRva));
        say(" PE export rva 0x%llX%s\n", static_cast<unsigned long long>(peRva),
            havePe ? "" : " (not found!)");
        if (havePe) {
            const auto delta = static_cast<long long>(localRva) - static_cast<long long>(peRva);
            say(" DELTA %lld %s\n", delta,
                delta == 0 ? "(export RVA is the real body -- safe to relocate)"
                           : "(export RVA is NOT the body -- relocating it will fault)");
        }

        dump_bytes("GetProcAddress", reinterpret_cast<const void*>(local), 16);
        if (havePe) {
            dump_bytes("peRva", reinterpret_cast<const void*>(modBase + peRva), 16);
        }
    }

    return 0;
}
