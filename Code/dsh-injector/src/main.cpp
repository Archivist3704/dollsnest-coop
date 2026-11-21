#include <windows.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct RemoteBuffer {
    HANDLE  process = nullptr;
    LPVOID  address = nullptr;
    SIZE_T  size = 0;

    ~RemoteBuffer() {
        if (address && process) VirtualFreeEx(process, address, 0, MEM_RELEASE);
    }

    bool alloc(HANDLE proc, SIZE_T n) {
        process = proc;
        size = n;
        address = VirtualAllocEx(proc, nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        return address != nullptr;
    }
};

int fail(const char* what) {
    const DWORD err = GetLastError();
    std::printf("!! %s (GetLastError=%lu)\n", what, err);

    if (err == ERROR_ACCESS_DENIED) {
        std::printf("!! interpretation: the target process refused access.\n"
                    "!! * most likely: the target runs at a HIGHER integrity level than us\n"
                    "!! (e.g. Steam was started elevated, so the game inherited it).\n"
                    "!! Fix: run the injector from an elevated prompt as well.\n"
                    "!! * otherwise: we are inside a restricted job/sandbox that denies\n"
                    "!! OpenProcess on processes outside it.\n"
                    "!! Fix: run from a plain Explorer/Start-menu window instead.\n");
    } else if (err == ERROR_INVALID_PARAMETER) {
        std::printf("!! interpretation: that pid does not exist any more (the process\n"
                    "!! exited between discovery and injection). Re-check the pid.\n");
    }
    std::fflush(stdout);
    return 1;
}

void say(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::fflush(stdout);
}

bool parse_tasklist_csv(const std::wstring& cmd, DWORD& outPid) {
    FILE* pipe = _wpopen(cmd.c_str(), L"rt");
    if (!pipe) return false;

    bool anyOutput = false;
    char line[1024];
    DWORD found = 0;
    while (fgets(line, sizeof(line), pipe)) {
        anyOutput = true;

        char* q1 = std::strchr(line, '"');
        if (!q1) continue;
        char* q2 = std::strchr(q1 + 1, '"');
        if (!q2) continue;
        char* q3 = std::strchr(q2 + 1, '"');
        if (!q3) continue;
        char* q4 = std::strchr(q3 + 1, '"');
        if (!q4) continue;
        *q4 = '\0';
        const long parsed = std::strtol(q3 + 1, nullptr, 10);
        if (parsed > 0) {
            found = static_cast<DWORD>(parsed);
            break;
        }
    }
    _pclose(pipe);
    outPid = found;
    return anyOutput;
}

DWORD find_pid_toolhelp(const std::wstring& exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD found = 0;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, exeName.c_str()) == 0) {
                found = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return found;
}

DWORD find_pid_via_wmic(const std::wstring& exeName) {

    std::wstring cmd = L"wmic process where \"name='" + exeName +
                       L"'\" get ProcessId /format:list";
    FILE* pipe = _wpopen(cmd.c_str(), L"rt");
    if (!pipe) return 0;
    char line[512];
    DWORD found = 0;
    while (fgets(line, sizeof(line), pipe)) {
        char* eq = std::strchr(line, '=');
        if (!eq) continue;
        const long parsed = std::strtol(eq + 1, nullptr, 10);
        if (parsed > 0) {
            found = static_cast<DWORD>(parsed);
            break;
        }
    }
    _pclose(pipe);
    return found;
}

DWORD find_pid_via_powershell(const std::wstring& exeName) {
    std::wstring cmd = L"powershell -NoProfile -Command \"(Get-Process -Name '" +
                       exeName.substr(0, exeName.find(L'.')) +
                       L"' -ErrorAction SilentlyContinue | Select-Object -First 1).Id\"";
    FILE* pipe = _wpopen(cmd.c_str(), L"rt");
    if (!pipe) return 0;
    char line[128];
    DWORD found = 0;
    while (fgets(line, sizeof(line), pipe)) {
        const long parsed = std::strtol(line, nullptr, 10);
        if (parsed > 0) {
            found = static_cast<DWORD>(parsed);
            break;
        }
    }
    _pclose(pipe);
    return found;
}

DWORD find_process_by_name(const std::wstring& exeName, std::string* methodOut = nullptr) {
    const auto note = [&](const char* m) {
        if (methodOut) *methodOut = m;
    };

    {
        DWORD pidValue = 0;
        const std::wstring cmd =
            L"tasklist /FI \"IMAGENAME eq " + exeName + L"\" /FO CSV /NH";
        if (parse_tasklist_csv(cmd, pidValue) && pidValue != 0) {
            note("tasklist");
            return pidValue;
        }
    }

    {
        const DWORD pidValue = find_pid_via_powershell(exeName);
        if (pidValue != 0) {
            note("powershell");
            return pidValue;
        }
    }

    {
        const DWORD pidValue = find_pid_via_wmic(exeName);
        if (pidValue != 0) {
            note("wmic");
            return pidValue;
        }
    }

    {
        const DWORD pidValue = find_pid_toolhelp(exeName);
        if (pidValue != 0) {
            note("toolhelp");
            return pidValue;
        }
    }

    note("none");
    return 0;
}

bool wait_for_main_module(DWORD pid, int timeoutMs, std::wstring& outPath, HMODULE& outBase) {
    const DWORD step = 100;
    for (int waited = 0; waited < timeoutMs; waited += step) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W mod{};
            mod.dwSize = sizeof(mod);
            if (Module32FirstW(snap, &mod)) {
                outPath = mod.szExePath;
                outBase = mod.hModule;
                CloseHandle(snap);
                return true;
            }
            CloseHandle(snap);
        }
        Sleep(step);
    }
    return false;
}

using FnInit = int(__cdecl*)(const char*);
using FnStamp = const char*(__cdecl*)(void);
using FnApi = unsigned int(__cdecl*)(void);

struct PeExports {
    std::uintptr_t imageBase = 0;

    std::vector<std::pair<std::string, std::uint32_t>> entries;

    const char*   reason = "not attempted";
    std::uint64_t fileSize = 0;

    [[nodiscard]] bool find(const char* name, std::uint32_t& outRva) const {
        for (const auto& [n, rva] : entries) {
            if (n == name) { outRva = rva; return true; }
        }
        return false;
    }
};

bool read_pe_exports(const std::string& path, PeExports& out) {
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { out.reason = "CreateFile failed"; return false; }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart <= 0) {
        out.reason = "GetFileSizeEx failed or empty";
        CloseHandle(file);
        return false;
    }
    const auto limit = static_cast<std::size_t>(fileSize.QuadPart);
    out.fileSize = static_cast<std::uint64_t>(limit);

    HANDLE mapping = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) { out.reason = "CreateFileMapping failed"; CloseHandle(file); return false; }

    const auto* base = static_cast<const std::uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (!base) {
        out.reason = "MapViewOfFile failed";
        CloseHandle(mapping);
        CloseHandle(file);
        return false;
    }

    const auto fits = [limit](std::uint64_t offset, std::uint64_t size) {
        return offset <= limit && size <= limit - offset;
    };
    const auto at = [base](std::uint64_t offset) { return base + offset; };

    bool ok = false;
    do {
        if (!fits(0, sizeof(IMAGE_DOS_HEADER))) { out.reason = "too small for DOS header"; break; }
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(at(0));
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) { out.reason = "bad MZ signature"; break; }
        if (dos->e_lfanew <= 0) { out.reason = "bad e_lfanew"; break; }

        const auto ntOff = static_cast<std::uint64_t>(dos->e_lfanew);
        if (!fits(ntOff, sizeof(IMAGE_NT_HEADERS32))) { out.reason = "too small for NT headers"; break; }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(at(ntOff));
        if (nt->Signature != IMAGE_NT_SIGNATURE) { out.reason = "bad PE signature"; break; }
        if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
            out.reason = "not an x64 image (machine mismatch)";
            break;
        }
        if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            out.reason = "not PE32+";
            break;
        }
        if (!fits(ntOff + offsetof(IMAGE_NT_HEADERS, OptionalHeader) + sizeof(IMAGE_OPTIONAL_HEADER64),
                  0)) {
            out.reason = "truncated optional header";
            break;
        }

        out.imageBase = static_cast<std::uintptr_t>(nt->OptionalHeader.ImageBase);

        const auto sectionCount = nt->FileHeader.NumberOfSections;
        const std::uint64_t sectionTableOff =
            ntOff + offsetof(IMAGE_NT_HEADERS, OptionalHeader) + nt->FileHeader.SizeOfOptionalHeader;
        if (sectionCount == 0 || !fits(sectionTableOff, std::uint64_t(sectionCount) * sizeof(IMAGE_SECTION_HEADER))) {
            out.reason = "section table out of range";
            break;
        }

        const auto rva_to_offset = [&](std::uint32_t rva, std::uint64_t& offset) -> bool {
            const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(at(sectionTableOff));
            for (std::uint32_t s = 0; s < sectionCount; ++s) {
                const auto& sec = sections[s];
                const std::uint64_t vaddr = sec.VirtualAddress;

                const std::uint64_t span = std::max<std::uint64_t>(sec.Misc.VirtualSize, sec.SizeOfRawData);
                if (rva < vaddr || rva >= vaddr + span) continue;
                const std::uint64_t delta = rva - vaddr;
                if (delta >= sec.SizeOfRawData) return false;
                offset = sec.PointerToRawData + delta;
                return true;
            }

            if (rva < nt->OptionalHeader.SizeOfHeaders) {
                offset = rva;
                return true;
            }
            return false;
        };

        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (dir.VirtualAddress == 0 || dir.Size == 0) {
            out.reason = "image has no export directory";
            ok = true;
            break;
        }

        std::uint64_t exportDirOff = 0;
        if (!rva_to_offset(dir.VirtualAddress, exportDirOff)) {
            out.reason = "export directory RVA is not mapped to a file offset";
            break;
        }
        if (!fits(exportDirOff, sizeof(IMAGE_EXPORT_DIRECTORY))) {
            out.reason = "export directory extends past end of file";
            break;
        }
        const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(at(exportDirOff));

        std::uint64_t namesOff = 0;
        std::uint64_t ordinalsOff = 0;
        std::uint64_t functionsOff = 0;
        if (!rva_to_offset(exp->AddressOfNames, namesOff) ||
            !rva_to_offset(exp->AddressOfNameOrdinals, ordinalsOff) ||
            !rva_to_offset(exp->AddressOfFunctions, functionsOff)) {
            out.reason = "export sub-tables are not mapped to file offsets";
            break;
        }

        const std::uint64_t count = exp->NumberOfNames;
        if (count == 0) { out.reason = "export table has no named entries"; ok = true; break; }
        if (count > 100000) { out.reason = "implausible export count"; break; }
        if (!fits(namesOff, count * sizeof(DWORD))) { out.reason = "name table out of range"; break; }
        if (!fits(ordinalsOff, count * sizeof(WORD))) { out.reason = "ordinal table out of range"; break; }
        if (!fits(functionsOff, sizeof(DWORD))) { out.reason = "function table out of range"; break; }

        const auto* names = reinterpret_cast<const DWORD*>(at(namesOff));
        const auto* ordinals = reinterpret_cast<const WORD*>(at(ordinalsOff));

        for (std::uint64_t i = 0; i < count; ++i) {
            std::uint64_t nameOff = 0;
            if (!rva_to_offset(names[i], nameOff) || !fits(nameOff, 2)) continue;
            const char* name = reinterpret_cast<const char*>(at(nameOff));
            const std::size_t maxLen = limit - nameOff;
            std::size_t len = 0;
            while (len < maxLen && name[len] != '\0') ++len;
            if (len == maxLen) continue;

            const std::uint64_t funcSlot =
                functionsOff + static_cast<std::uint64_t>(ordinals[i]) * sizeof(DWORD);
            if (!fits(funcSlot, sizeof(DWORD))) continue;
            const DWORD fnRva = *reinterpret_cast<const DWORD*>(at(funcSlot));

            if (fnRva >= dir.VirtualAddress && fnRva < dir.VirtualAddress + dir.Size) continue;

            out.entries.emplace_back(std::string(name, len), fnRva);
        }
        out.reason = "ok";
        ok = true;
    } while (false);

    UnmapViewOfFile(base);
    CloseHandle(mapping);
    CloseHandle(file);
    return ok;
}

}

int wmain(int argc, wchar_t** argv) {

    if (argc >= 3 && _wcsicmp(argv[1], L"--find-pid") == 0) {
        std::string method;
        const DWORD found = find_process_by_name(argv[2], &method);
        if (found == 0) {

            std::printf("not-found (tried: tasklist, powershell, wmic, toolhelp)\n");
            std::printf("hint: if you can see the process in Task Manager, pass its pid "
                        "directly with --pid <N>\n");
            return 2;
        }
        std::printf("%lu\n", static_cast<unsigned long>(found));
        std::fprintf(stderr, "[find-pid] matched via %s\n", method.c_str());
        return 0;
    }

    if (argc >= 2 && _wcsicmp(argv[1], L"--list-processes") == 0) {
        const wchar_t* filter = (argc >= 3) ? argv[2] : nullptr;

        const std::wstring cmd = filter
            ? (std::wstring(L"tasklist /FI \"IMAGENAME eq ") + filter + L"\" /FO CSV /NH")
            : std::wstring(L"tasklist /FO CSV /NH");

        FILE* pipe = _wpopen(cmd.c_str(), L"rt");
        if (!pipe) {
            std::printf("!! cannot run tasklist\n");
            return 1;
        }
        char line[1024];
        int rows = 0;
        while (fgets(line, sizeof(line), pipe)) {

            std::printf("%s", line);
            if (std::strchr(line, '"')) ++rows;
        }
        _pclose(pipe);
        std::printf("-- %d row(s) with a quoted field\n", rows);
        return rows > 0 ? 0 : 2;
    }

    if (argc < 3) {
        std::printf(
            "dsh-injector -- load a DLL into a running process\n"
            "\n"
            "usage: dsh-injector <target.exe|--pid N> <dll path> [options]\n"
            "\n"
            "options:\n"
            " --command-line \"...\" string passed to DSH_Init if it is called\n"
            " --remote-init also call DSH_Init over CreateRemoteThread.\n"
            " OFF by default: on this machine that call faults\n"
            " and takes the target process with it, while the\n"
            " DLL's DllMain + worker thread do the real work.\n"
            " See docs/evidence/EVIDENCE-0005-injection-chain.md\n"
            " --wait wait for the target process to appear\n"
            " --timeout-ms N how long to wait (with --wait)\n"
            "\n"
            "exit codes: 0 ok, 1 usage/IO error, 2 target not found, 3 load failed,\n"
            " 4 DSH_Init reported failure (only with --remote-init)\n");
        return 1;
    }

    std::wstring dllWide;
    std::wstring commandLine;
    DWORD pid = 0;
    bool waitForIt = false;
    bool remoteInit = false;
    int waitTimeoutMs = 120000;

    int i = 1;
    if (_wcsicmp(argv[i], L"--wait") == 0) {
        waitForIt = true;
        ++i;
        if (i >= argc) { std::printf("!! --wait needs a target name\n"); return 1; }
    }

    std::wstring target;
    if (_wcsicmp(argv[i], L"--pid") == 0) {
        if (i + 1 >= argc) { std::printf("!! --pid needs a number\n"); return 1; }
        pid = static_cast<DWORD>(_wtoi(argv[i + 1]));
        i += 2;
    } else {
        target = argv[i];
        ++i;
    }

    if (i >= argc) { std::printf("!! missing DLL path\n"); return 1; }
    dllWide = argv[i];
    ++i;

    for (; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--command-line") == 0 && i + 1 < argc) {
            commandLine = argv[++i];
        } else if (_wcsicmp(argv[i], L"--timeout-ms") == 0 && i + 1 < argc) {
            waitTimeoutMs = _wtoi(argv[++i]);
        } else if (_wcsicmp(argv[i], L"--remote-init") == 0) {
            remoteInit = true;
        } else {
            std::printf("!! unknown argument: %ls\n", argv[i]);
            return 1;
        }
    }

    wchar_t fullDll[MAX_PATH] = {};
    if (GetFullPathNameW(dllWide.c_str(), MAX_PATH, fullDll, nullptr) == 0) {
        std::printf("!! cannot resolve DLL path: %ls\n", dllWide.c_str());
        return 1;
    }
    if (GetFileAttributesW(fullDll) == INVALID_FILE_ATTRIBUTES) {
        std::printf("!! DLL does not exist: %ls\n", fullDll);
        return 1;
    }

    std::printf("[inj] dll : %ls\n", fullDll);

    if (pid == 0) {
        std::printf("[inj] looking for process: %ls\n", target.c_str());
        for (int attempt = 0; pid == 0 && attempt < (waitForIt ? waitTimeoutMs / 250 : 1); ++attempt) {
            pid = find_process_by_name(target);
            if (pid == 0 && waitForIt) Sleep(250);
        }
        if (pid == 0) {
            std::printf("!! target process not found: %ls\n", target.c_str());
            return 2;
        }
    }
    std::printf("[inj] target : pid=%lu\n", pid);

    std::wstring mainPath;
    HMODULE mainBase = nullptr;
    if (!wait_for_main_module(pid, waitForIt ? waitTimeoutMs : 15000, mainPath, mainBase)) {

        std::printf("[inj] warn : could not enumerate modules (protected or still starting); "
                    "continuing anyway\n");
    } else {
        std::printf("[inj] main : %ls base=0x%p\n", mainPath.c_str(), mainBase);
    }

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                  PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) return fail("OpenProcess failed");
    std::printf("[inj] handle : ok\n");

    const std::string dllUtf8 = [&] {
        char buf[MAX_PATH * 2] = {};
        WideCharToMultiByte(CP_UTF8, 0, fullDll, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
        return std::string(buf);
    }();

    const SIZE_T dllBytes = dllUtf8.size() + 1;

    RemoteBuffer remoteDll;
    if (!remoteDll.alloc(proc, dllBytes)) return fail("VirtualAllocEx for dll path failed");
    if (!WriteProcessMemory(proc, remoteDll.address, dllUtf8.c_str(), dllBytes, nullptr)) {
        return fail("WriteProcessMemory for dll path failed");
    }

    const std::string cmdUtf8 = [&] {
        char buf[2048] = {};
        WideCharToMultiByte(CP_UTF8, 0, commandLine.c_str(), -1, buf, sizeof(buf) - 1, nullptr, nullptr);
        return std::string(buf);
    }();

    const SIZE_T argsBytes = cmdUtf8.size() + 1;
    RemoteBuffer remoteArgs;
    if (!remoteArgs.alloc(proc, argsBytes)) return fail("VirtualAllocEx for args failed");
    if (!WriteProcessMemory(proc, remoteArgs.address, cmdUtf8.c_str(), argsBytes, nullptr)) {
        return fail("WriteProcessMemory for args failed");
    }
    std::printf("[inj] args : %zu bytes at 0x%p\n", argsBytes, remoteArgs.address);

    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (!kernel) return fail("GetModuleHandleW(kernel32) failed");
    FARPROC loadLibrary = GetProcAddress(kernel, "LoadLibraryA");
    if (!loadLibrary) return fail("GetProcAddress(LoadLibraryA) failed");

    say("[inj] step : CreateRemoteThread(LoadLibraryA)\n");
    HANDLE thread = CreateRemoteThread(proc, nullptr, 0,
                                       reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary),
                                       remoteDll.address, 0, nullptr);
    if (!thread) return fail("CreateRemoteThread failed");

    if (WaitForSingleObject(thread, 30000) != WAIT_OBJECT_0) {
        std::printf("!! LoadLibrary did not finish in 30s\n");
        CloseHandle(thread);
        return 3;
    }
    DWORD loadResult = 0;
    GetExitCodeThread(thread, &loadResult);
    CloseHandle(thread);

    if (loadResult == 0) {

        std::printf("!! LoadLibraryA returned NULL in the target -- DLL did not load.\n"
                    "!! consequence: nothing will be logged and no hook will exist.\n"
                    "!! check: bitness (must be x64) and that the DLL's dependencies are present\n");
        return 3;
    }
    say("[inj] loaded : module handle in target = 0x%08lX\n", loadResult);

    if (!remoteInit) {
        std::printf("[inj] OK (DLL loaded; remote DSH_Init skipped by design)\n");
        std::printf("[inj] the DLL initialises itself from DllMain; verify via its\n"
                    "[inj] evidence files (dsh-module-marker.txt / dollsnest-coop.log)\n");
        CloseHandle(proc);
        return 0;
    }

    PeExports exports;
    if (!read_pe_exports(dllUtf8, exports)) {
        say("!! cannot read the PE export table (%s, file %llu bytes) -- skipping DSH_Init. "
            "DllMain already ran, so the log should exist.\n",
            exports.reason, static_cast<unsigned long long>(exports.fileSize));
        CloseHandle(proc);
        return 0;
    }
    std::uint32_t initRva = 0;
    if (!exports.find("DSH_Init", initRva)) {
        say("!! DSH_Init is not exported by this DLL (%zu exports parsed, reason=%s) -- is it ours?\n",
            exports.entries.size(), exports.reason);
        CloseHandle(proc);
        return 3;
    }

    const auto remoteBase = static_cast<std::uintptr_t>(loadResult);

    const auto remoteInitAddr = remoteBase + initRva;

    say("[inj] exports: %zu names; DSH_Init rva=0x%llX -> remote 0x%llX\n",
        exports.entries.size(),
        static_cast<unsigned long long>(initRva),
        static_cast<unsigned long long>(remoteInitAddr));

    HANDLE initThread = CreateRemoteThread(
        proc, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteInitAddr),
        remoteArgs.address, 0, nullptr);
    if (!initThread) {
        std::printf("!! CreateRemoteThread(DSH_Init) failed -- DLL is loaded but not initialised\n");
        CloseHandle(proc);
        return 3;
    }
    DWORD initResult = 0;
    if (WaitForSingleObject(initThread, 30000) == WAIT_OBJECT_0) {
        GetExitCodeThread(initThread, &initResult);
    } else {
        std::printf("!! DSH_Init did not return within 30s\n");
        initResult = 0xFFFFFFFF;
    }
    CloseHandle(initThread);
    CloseHandle(proc);

    std::printf("[inj] DSH_Init returned %lu\n", initResult);
    if (initResult != 0) {
        std::printf("!! DSH_Init reported failure (see the log for the reason)\n");
        return 4;
    }
    std::printf("[inj] OK\n");
    return 0;
}
