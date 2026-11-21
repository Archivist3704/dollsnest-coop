#include <windows.h>
#include <tlhelp32.h>

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

void dump_region(HANDLE proc, const char* label, std::uintptr_t addr) {
    unsigned char buf[24] = {};
    SIZE_T got = 0;
    const BOOL ok = ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(addr), buf, sizeof(buf), &got);
    if (ok) {
        char hex[3 * 24 + 1] = {};
        for (SIZE_T i = 0; i < got; ++i) {
            char t[4];
            std::snprintf(t, sizeof(t), "%02X ", buf[i]);
            std::strcat(hex, t);
        }
        say(" %-22s 0x%llX RPM ok (%llu bytes): %s\n", label,
            static_cast<unsigned long long>(addr), static_cast<unsigned long long>(got), hex);
    } else {
        say(" %-22s 0x%llX RPM FAILED err=%lu\n", label,
            static_cast<unsigned long long>(addr), GetLastError());
    }

    MEMORY_BASIC_INFORMATION mbi{};
    const SIZE_T q = VirtualQueryEx(proc, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi));
    if (q) {
        say(" %-22s region base=0x%llX size=0x%llX type=%lu protect=0x%lX%s\n", "",
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(mbi.BaseAddress)),
            static_cast<unsigned long long>(mbi.RegionSize),
            mbi.Type, mbi.Protect,
            (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY)) ? " [executable]"
                                                     : " [NOT executable]");
    } else {
        say(" %-22s VirtualQueryEx FAILED err=%lu\n", "", GetLastError());
    }
}

}

int wmain(int argc, wchar_t** argv) {
    std::wstring target, dll;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--target" && i + 1 < argc) target = argv[++i];
        else if (a == L"--dll" && i + 1 < argc) dll = argv[++i];
    }
    if (target.empty() || dll.empty()) { say("!! --target and --dll required\n"); return 2; }

    const std::string dllUtf8 = to_utf8(dll);
    say("dsh-mem-probe\n");

    std::wstring cmd = L"\"" + target + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        say("!! CreateProcess failed err=%lu\n", GetLastError());
        return 2;
    }
    CloseHandle(pi.hThread);
    HANDLE proc = pi.hProcess;
    const DWORD pid = pi.dwProcessId;
    Sleep(600);
    say("target pid=%lu\n", pid);

    LPVOID ctrl = VirtualAllocEx(proc, nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (ctrl) {
        const char* text = "control-buffer-contents";
        WriteProcessMemory(proc, ctrl, text, std::strlen(text) + 1, nullptr);
    }
    say("\n[1] control buffer (must succeed if our measurement works)\n");
    if (ctrl) dump_region(proc, "control VirtualAllocEx", reinterpret_cast<std::uintptr_t>(ctrl));
    else say(" !! VirtualAllocEx failed err=%lu\n", GetLastError());

    LPVOID remotePath = VirtualAllocEx(proc, nullptr, dllUtf8.size() + 1, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_READWRITE);
    WriteProcessMemory(proc, remotePath, dllUtf8.c_str(), dllUtf8.size() + 1, nullptr);
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    auto ll = GetProcAddress(k, "LoadLibraryA");
    HANDLE t = CreateRemoteThread(proc, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(ll),
                                  remotePath, 0, nullptr);
    WaitForSingleObject(t, 15000);
    DWORD dllBase = 0;
    GetExitCodeThread(t, &dllBase);
    CloseHandle(t);
    say("\n[2] injected: LoadLibraryA returned 0x%08lX\n", dllBase);
    if (!dllBase) { say("!! load failed\n"); TerminateProcess(proc, 0); return 2; }

    const std::wstring baseName = dll.substr(dll.find_last_of(L"\\/") + 1);

    say("\n[3] target module list (looking for %ls)\n", baseName.c_str());
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE) {
            say(" !! snapshot failed err=%lu\n", GetLastError());
        } else {
            MODULEENTRY32W me{};
            me.dwSize = sizeof(me);
            int n = 0;
            bool found = false;
            if (Module32FirstW(snap, &me)) {
                do {
                    ++n;
                    if (_wcsicmp(me.szModule, baseName.c_str()) == 0) {
                        found = true;
                        say(" FOUND %ls base=0x%llX size=0x%lX\n", me.szModule,
                            static_cast<unsigned long long>(
                                reinterpret_cast<std::uintptr_t>(me.hModule)),
                            me.modBaseSize);
                    }
                } while (Module32NextW(snap, &me));
            }
            say(" scanned %d modules; %s\n", n, found ? "our DLL IS listed" : "our DLL is NOT listed");
            CloseHandle(snap);
        }
    }

    say("\n[4] memory at the reported base\n");
    dump_region(proc, "moduleBase (PE header)", dllBase);
    const std::uintptr_t ctfBase = 0x205F0000;
    if (ctfBase != dllBase) {
        say("\n[5] memory at the base we observed in earlier runs (0x%llX)\n",
            static_cast<unsigned long long>(ctfBase));
        dump_region(proc, "observed base", ctfBase);
    }

    TerminateProcess(proc, 0);
    CloseHandle(proc);
    return 0;
}
