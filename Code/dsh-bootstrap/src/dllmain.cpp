#include <windows.h>

#include <cstdio>
#include <cstring>

#include "dsh/bootstrap/bootstrap.h"

namespace dsh::bootstrap {
void          on_process_attach(void* moduleBase, HMODULE module);
void          on_process_detach();
std::int32_t  explicit_init(const char* commandLineUtf8);
std::uint32_t api_version();
const char*   build_stamp();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID ) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:

            DisableThreadLibraryCalls(module);
            dsh::bootstrap::on_process_attach(module, module);
            break;

        case DLL_PROCESS_DETACH:
            dsh::bootstrap::on_process_detach();
            break;

        default:
            break;
    }
    return TRUE;
}

extern "C" std::int32_t __cdecl DSH_Init(const char* commandLineUtf8) {
    return dsh::bootstrap::explicit_init(commandLineUtf8);
}

extern "C" void __cdecl DSH_Shutdown() {
    dsh::bootstrap::on_process_detach();
}

extern "C" const char* __cdecl DSH_BuildStamp() {
    return dsh::bootstrap::build_stamp();
}

extern "C" std::uint32_t __cdecl DSH_ApiVersion() {
    return dsh::bootstrap::api_version();
}

extern "C" void __cdecl DSH_TestPing(const char* tag) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = nullptr;
    for (char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') slash = p;
    }
    if (slash) *slash = '\0';

    char full[MAX_PATH + 40] = {};
    std::snprintf(full, sizeof(full), "%s\\dsh-ping.txt", path);

    HANDLE h = CreateFileA(full, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    char line[256] = {};
    std::snprintf(line, sizeof(line), "ping pid=%lu tag=%s\r\n",
                  static_cast<unsigned long>(GetCurrentProcessId()), tag ? tag : "(null)");
    DWORD written = 0;
    WriteFile(h, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
    CloseHandle(h);
}
