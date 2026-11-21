#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void say(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::fflush(stdout);
}

std::string to_utf8(const std::wstring& w) {
    char buf[4096] = {};
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, buf, sizeof(buf) - 1, nullptr, nullptr);
    return std::string(buf);
}

bool export_rva(const char* path, const char* want, std::uint32_t& outRva, std::string& why) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { why = "open failed"; return false; }
    LARGE_INTEGER sz{};
    GetFileSizeEx(file, &sz);
    const auto limit = static_cast<std::size_t>(sz.QuadPart);
    HANDLE map = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!map) { why = "map failed"; CloseHandle(file); return false; }
    const auto* base = static_cast<const std::uint8_t*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
    if (!base) { why = "view failed"; CloseHandle(map); CloseHandle(file); return false; }
    const auto fits = [limit](std::uint64_t o, std::uint64_t n) { return o <= limit && n <= limit - o; };

    bool found = false;
    do {
        if (!fits(0, sizeof(IMAGE_DOS_HEADER))) { why = "small"; break; }
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto ntOff = static_cast<std::uint64_t>(dos->e_lfanew);
        if (!fits(ntOff, sizeof(IMAGE_NT_HEADERS32))) { why = "no nt"; break; }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + ntOff);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        const std::uint64_t secOff =
            ntOff + offsetof(IMAGE_NT_HEADERS, OptionalHeader) + nt->FileHeader.SizeOfOptionalHeader;
        const auto secCount = nt->FileHeader.NumberOfSections;
        if (!secCount || !fits(secOff, std::uint64_t(secCount) * sizeof(IMAGE_SECTION_HEADER))) {
            why = "no sections";
            break;
        }
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
        std::uint64_t expOff = 0;
        if (!toOff(dir.VirtualAddress, expOff) || !fits(expOff, sizeof(IMAGE_EXPORT_DIRECTORY))) {
            why = "exp dir";
            break;
        }
        const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + expOff);
        std::uint64_t nOff = 0, oOff = 0, fOff = 0;
        if (!toOff(exp->AddressOfNames, nOff) || !toOff(exp->AddressOfNameOrdinals, oOff) ||
            !toOff(exp->AddressOfFunctions, fOff)) {
            why = "subtables";
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
            why = "ok";
            break;
        }
        if (!found) why = "name not found";
    } while (false);

    UnmapViewOfFile(base);
    CloseHandle(map);
    CloseHandle(file);
    return found;
}

struct Ctx {
    std::wstring target;
    std::wstring dll;
};

HANDLE spawn_and_inject(const Ctx& ctx, std::uintptr_t& dllBase) {
    std::wstring cmd = L"\"" + ctx.target + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        say("!! CreateProcess failed err=%lu\n", GetLastError());
        return nullptr;
    }
    CloseHandle(pi.hThread);
    HANDLE proc = pi.hProcess;
    Sleep(500);
    if (WaitForSingleObject(proc, 0) == WAIT_OBJECT_0) {
        say("!! target exited immediately\n");
        return nullptr;
    }

    const std::string dllUtf8 = to_utf8(ctx.dll);
    LPVOID remote = VirtualAllocEx(proc, nullptr, dllUtf8.size() + 1, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_READWRITE);
    if (!remote) { say("!! alloc failed\n"); return proc; }
    WriteProcessMemory(proc, remote, dllUtf8.c_str(), dllUtf8.size() + 1, nullptr);

    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    auto ll = GetProcAddress(k, "LoadLibraryA");
    HANDLE t = CreateRemoteThread(proc, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(ll),
                                  remote, 0, nullptr);
    WaitForSingleObject(t, 15000);
    DWORD r = 0;
    GetExitCodeThread(t, &r);
    CloseHandle(t);
    dllBase = static_cast<std::uintptr_t>(r);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    say("[ctx] target pid=%lu dllBase=0x%llX\n", pi.dwProcessId,
        static_cast<unsigned long long>(dllBase));
    return proc;
}

void call_remote(HANDLE proc, const char* label, std::uintptr_t fn, LPVOID arg) {
    say("\n--- call: %s (fn=0x%llX, arg=0x%p) ---\n", label,
        static_cast<unsigned long long>(fn), arg);

    unsigned char bytes[16] = {};
    SIZE_T got = 0;
    if (ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(fn), bytes, sizeof(bytes), &got)) {
        char hex[3 * 16 + 1] = {};
        for (SIZE_T i = 0; i < got && i < 16; ++i) {
            char b[4];
            std::snprintf(b, sizeof(b), "%02X ", bytes[i]);
            std::strcat(hex, b);
        }
        say(" code bytes: %s\n", hex);
    } else {
        say(" !! ReadProcessMemory(callee) failed err=%lu\n", GetLastError());
    }

    HANDLE t = CreateRemoteThread(proc, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(fn),
                                  arg, 0, nullptr);
    if (!t) { say(" !! CreateRemoteThread failed err=%lu\n", GetLastError()); return; }
    const DWORD w = WaitForSingleObject(t, 15000);
    DWORD result = 0;
    if (w == WAIT_OBJECT_0) {
        GetExitCodeThread(t, &result);
        say(" returned %lu (0x%08lX)\n", result, result);
    } else {
        say(" !! did not return within 15 s\n");
    }
    CloseHandle(t);

    const bool alive = (WaitForSingleObject(proc, 0) == WAIT_TIMEOUT);
    DWORD ec = 0;
    GetExitCodeProcess(proc, &ec);
    say(" target alive=%s exit=0x%08lX%s\n", alive ? "YES" : "NO", ec,
        ec == 0xC0000005 ? " <-- ACCESS_VIOLATION" : "");
}

}

int wmain(int argc, wchar_t** argv) {
    Ctx ctx;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--target" && i + 1 < argc) ctx.target = argv[++i];
        else if (a == L"--dll" && i + 1 < argc) ctx.dll = argv[++i];
    }
    if (ctx.target.empty() || ctx.dll.empty()) {
        say("!! --target and --dll are required\n");
        return 2;
    }

    say("dsh-ctf-probe\n");
    say("target: %ls\n", ctx.target.c_str());
    say("dll : %ls\n", ctx.dll.c_str());

    const std::string dllUtf8 = to_utf8(ctx.dll);
    std::uint32_t pingRva = 0;
    std::string why;
    const bool havePing = export_rva(dllUtf8.c_str(), "PingDll_Write", pingRva, why);
    say("PingDll_Write rva=%s (%s)\n",
        havePing ? std::to_string(pingRva).c_str() : "n/a", why.c_str());

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    const auto localGetTickCount = reinterpret_cast<std::uintptr_t>(GetProcAddress(k32, "GetTickCount"));
    const auto localSleep = reinterpret_cast<std::uintptr_t>(GetProcAddress(k32, "Sleep"));

    {
        std::uintptr_t dllBase = 0;
        HANDLE p = spawn_and_inject(ctx, dllBase);
        if (p) {
            call_remote(p, "kernel32!GetTickCount (leaf, no args)", localGetTickCount, nullptr);
            TerminateProcess(p, 0);
            CloseHandle(p);
        }
    }

    {
        std::uintptr_t dllBase = 0;
        HANDLE p = spawn_and_inject(ctx, dllBase);
        if (p) {
            call_remote(p, "kernel32!Sleep(1000)", localSleep,
                        reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(1000)));
            TerminateProcess(p, 0);
            CloseHandle(p);
        }
    }

    if (havePing) {
        std::uintptr_t dllBase = 0;
        HANDLE p = spawn_and_inject(ctx, dllBase);
        if (p && dllBase) {
            const char* arg = "ctf-probe";
            LPVOID ra = VirtualAllocEx(p, nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            WriteProcessMemory(p, ra, arg, std::strlen(arg) + 1, nullptr);
            call_remote(p, "dsh-ping-dll!PingDll_Write", dllBase + pingRva, ra);
            TerminateProcess(p, 0);
            CloseHandle(p);
        } else if (p) {
            TerminateProcess(p, 0);
            CloseHandle(p);
        }
    }

    say("\n=== interpretation ===\n");
    say(" GetTickCount surviving => CreateRemoteThread CAN call arbitrary functions;\n");
    say(" the fault is specific to our DLLs.\n");
    say(" GetTickCount faulting => remote calls to arbitrary functions are unusable\n");
    say(" here; change the injection strategy instead of\n");
    say(" debugging the DLL.\n");
    return 0;
}
