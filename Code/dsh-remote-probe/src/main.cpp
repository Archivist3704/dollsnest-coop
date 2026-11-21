#include <windows.h>

#include <cstdarg>
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

struct RemoteBuffer {
    HANDLE process = nullptr;
    LPVOID address = nullptr;
    ~RemoteBuffer() {
        if (address && process) VirtualFreeEx(process, address, 0, MEM_RELEASE);
    }
    bool alloc(HANDLE proc, SIZE_T n) {
        process = proc;
        address = VirtualAllocEx(proc, nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        return address != nullptr;
    }
};

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
        if (!fits(0, sizeof(IMAGE_DOS_HEADER))) { why = "too small"; break; }
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto ntOff = static_cast<std::uint64_t>(dos->e_lfanew);
        if (!fits(ntOff, sizeof(IMAGE_NT_HEADERS32))) { why = "no nt headers"; break; }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + ntOff);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        const std::uint64_t secOff =
            ntOff + offsetof(IMAGE_NT_HEADERS, OptionalHeader) + nt->FileHeader.SizeOfOptionalHeader;
        const auto secCount = nt->FileHeader.NumberOfSections;
        if (!secCount || !fits(secOff, std::uint64_t(secCount) * sizeof(IMAGE_SECTION_HEADER))) {
            why = "section table unusable";
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
            why = "export dir unmapped";
            break;
        }
        const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + expOff);
        std::uint64_t nOff = 0, oOff = 0, fOff = 0;
        if (!toOff(exp->AddressOfNames, nOff) || !toOff(exp->AddressOfNameOrdinals, oOff) ||
            !toOff(exp->AddressOfFunctions, fOff)) {
            why = "export subtables unmapped";
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
        if (!found) why = "export name not found";
    } while (false);

    UnmapViewOfFile(base);
    CloseHandle(map);
    CloseHandle(file);
    return found;
}

std::string to_utf8(const std::wstring& w) {
    char buf[4096] = {};
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, buf, sizeof(buf) - 1, nullptr, nullptr);
    return std::string(buf);
}

}

int wmain(int argc, wchar_t** argv) {
    std::wstring dll, mode = L"load", target;
    std::wstring exportName = L"DSH_Init";
    int seconds = 25;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--dll" && i + 1 < argc) dll = argv[++i];
        else if (a == L"--mode" && i + 1 < argc) mode = argv[++i];
        else if (a == L"--target" && i + 1 < argc) target = argv[++i];
        else if (a == L"--export" && i + 1 < argc) exportName = argv[++i];
        else if (a == L"--seconds" && i + 1 < argc) seconds = _wtoi(argv[++i]);
    }
    if (dll.empty()) { say("!! --dll required\n"); return 2; }
    if (target.empty()) {
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring s = self;
        const auto pos = s.find_last_of(L"\\/");
        target = s.substr(0, pos + 1) + L"dsh-test-target.exe";
    }

    const std::string dllUtf8 = to_utf8(dll);
    say("=== dsh-remote-probe mode=%ls ===\n", mode.c_str());
    say("[probe] dll : %s\n", dllUtf8.c_str());

    std::wstring cmd = L"\"" + target + L"\" --seconds " + std::to_wstring(seconds);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        say("!! CreateProcess failed err=%lu\n", GetLastError());
        return 2;
    }
    say("[probe] spawned pid=%lu\n", pi.dwProcessId);
    Sleep(700);
    if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
        say("!! target exited immediately\n");
        return 2;
    }
    HANDLE proc = pi.hProcess;

    RemoteBuffer remoteDll;
    if (!remoteDll.alloc(proc, dllUtf8.size() + 1)) { say("!! alloc for dll path failed\n"); return 3; }
    WriteProcessMemory(proc, remoteDll.address, dllUtf8.c_str(), dllUtf8.size() + 1, nullptr);

    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = GetProcAddress(kernel, "LoadLibraryA");
    HANDLE t = CreateRemoteThread(proc, nullptr, 0,
                                  reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary),
                                  remoteDll.address, 0, nullptr);
    WaitForSingleObject(t, 20000);
    DWORD loadResult = 0;
    GetExitCodeThread(t, &loadResult);
    CloseHandle(t);
    say("[probe] LoadLibraryA -> 0x%08lX\n", loadResult);
    if (!loadResult) { say("!! load failed\n"); return 3; }

    if (mode == L"load") {
        say("[probe] baseline only: DLL loaded, DSH_Init NOT called\n");
    } else if (mode == L"direct") {
        std::uint32_t callRva = 0;
        std::string why;
        const std::string exportUtf8 = to_utf8(exportName);
        if (!export_rva(dllUtf8.c_str(), exportUtf8.c_str(), callRva, why)) {
            say("!! cannot resolve %s (%s)\n", exportUtf8.c_str(), why.c_str());
            return 3;
        }
        const auto remoteFn = static_cast<std::uintptr_t>(loadResult) + callRva;

        RemoteBuffer remoteArgs;
        if (!remoteArgs.alloc(proc, 64)) { say("!! alloc for args failed\n"); return 3; }
        const char* args = exportUtf8.c_str();
        WriteProcessMemory(proc, remoteArgs.address, args, std::strlen(args) + 1, nullptr);

        say("[probe] %s rva=0x%llX remote=0x%llX arg=0x%p\n", exportUtf8.c_str(),
            static_cast<unsigned long long>(callRva),
            static_cast<unsigned long long>(remoteFn),
            remoteArgs.address);

        {
            unsigned char remoteBytes[16] = {};
            SIZE_T got = 0;
            if (ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(remoteFn), remoteBytes,
                                  sizeof(remoteBytes), &got)) {
                say("[probe] bytes@remoteFn:");
                for (SIZE_T i = 0; i < got && i < 16; ++i) say(" %02X", remoteBytes[i]);
                say("\n");
            } else {
                say("!! ReadProcessMemory at remoteFn failed err=%lu\n", GetLastError());
            }
        }

        say("[probe] creating remote thread ...\n");

        HANDLE ct = CreateRemoteThread(proc, nullptr, 0,
                                       reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteFn),
                                       remoteArgs.address, 0, nullptr);
        if (!ct) { say("!! CreateRemoteThread failed err=%lu\n", GetLastError()); return 3; }
        const DWORD w = WaitForSingleObject(ct, 20000);
        DWORD result = 0;
        if (w == WAIT_OBJECT_0) {
            GetExitCodeThread(ct, &result);
            say("[probe] %s returned %lu (0x%08lX)\n", exportUtf8.c_str(), result, result);
        } else {
            say("!! remote call did not return within 20 s\n");
        }
        CloseHandle(ct);
    } else {
        say("!! unknown mode '%ls' (use load | direct)\n", mode.c_str());
        return 2;
    }

    Sleep(700);
    const bool alive = (WaitForSingleObject(proc, 0) == WAIT_TIMEOUT);
    DWORD exitCode = 0;
    GetExitCodeProcess(proc, &exitCode);

    say("[probe] target alive=%s exitCode=0x%08lX%s\n", alive ? "YES" : "NO", exitCode,
        exitCode == 0xC0000005 ? " <-- ACCESS_VIOLATION in the remote call" : "");
    if (alive) TerminateProcess(proc, 0);
    CloseHandle(proc);
    CloseHandle(pi.hThread);
    say("=== done ===\n");
    return alive ? 0 : 1;
}
