#include <windows.h>
#include <psapi.h>

#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "psapi.lib")

namespace {

std::string exe_dir() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s = path;
    const auto pos = s.find_last_of("\\/");
    return (pos == std::string::npos) ? s : s.substr(0, pos);
}

void append_line(const std::string& file, const std::string& line) {
    HANDLE h = CreateFileA(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string l = line + "\r\n";
    DWORD written = 0;
    WriteFile(h, l.c_str(), static_cast<DWORD>(l.size()), &written, nullptr);
    CloseHandle(h);
}

std::string module_list_report() {
    std::string out;
    HMODULE mods[512] = {};
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "EnumProcessModules FAILED err=%lu",
                      static_cast<unsigned long>(GetLastError()));
        return buf;
    }
    const DWORD count = needed / sizeof(HMODULE);
    char buf[512];
    std::snprintf(buf, sizeof(buf), "module count=%lu", static_cast<unsigned long>(count));
    out = buf;
    for (DWORD i = 0; i < count && i < 512; ++i) {
        char name[MAX_PATH] = {};
        if (GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, MAX_PATH) == 0) continue;
        char line[512];
        std::snprintf(line, sizeof(line), "\n [%lu] %s base=0x%llX", static_cast<unsigned long>(i),
                      name, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(mods[i])));
        out += line;
    }
    return out;
}

}

int main(int argc, char** argv) {
    int seconds = 60;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = std::atoi(argv[i + 1]);
    }
    if (seconds <= 0) seconds = 60;

    const std::string dir = exe_dir();
    const DWORD pidValue = GetCurrentProcessId();
    char pidBuf[32];
    std::snprintf(pidBuf, sizeof(pidBuf), "%lu", static_cast<unsigned long>(pidValue));

    const std::string pidFile = dir + "\\ctf-pid.txt";
    const std::string reportFile = dir + "\\ctf-report.txt";
    const std::string stopFile = dir + "\\ctf-stop.txt";

    {
        HANDLE h = CreateFileA(pidFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(h, pidBuf, static_cast<DWORD>(std::strlen(pidBuf)), &w, nullptr);
            CloseHandle(h);
        }
    }
    DeleteFileA(reportFile.c_str());
    DeleteFileA(stopFile.c_str());

    append_line(reportFile, std::string("ctf-target pid=") + pidBuf + " started");

    const int slices = seconds * 10;
    for (int i = 0; i < slices; ++i) {
        Sleep(100);
        if (GetFileAttributesA(stopFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
            append_line(reportFile, "ctf-target: stop file seen, exiting");
            break;
        }

        if (i == 10 || (i > 10 && (i % 50) == 0)) {
            char head[256];
            std::snprintf(head, sizeof(head), "--- self-report t=%d.%ds pid=%lu ---",
                          i / 10, i % 10, static_cast<unsigned long>(pidValue));
            append_line(reportFile, head);
            append_line(reportFile, module_list_report());
        }
    }

    append_line(reportFile, "ctf-target exiting normally");
    return 0;
}
