#include <windows.h>

#include <cstdio>
#include <cstring>

extern "C" __declspec(dllexport) void __cdecl PingDll_Write(const char* tag) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = nullptr;
    for (char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') slash = p;
    }
    if (slash) *slash = '\0';

    char full[MAX_PATH + 32] = {};
    std::snprintf(full, sizeof(full), "%s\\pingdll.txt", path);

    HANDLE h = CreateFileA(full, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    char line[192] = {};
    std::snprintf(line, sizeof(line), "pingdll pid=%lu tag=%s\r\n",
                  static_cast<unsigned long>(GetCurrentProcessId()), tag ? tag : "(null)");
    DWORD written = 0;
    WriteFile(h, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
    CloseHandle(h);
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
