#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

HMODULE g_selfModule = nullptr;
HMODULE g_realVersion = nullptr;
wchar_t g_realPath[MAX_PATH] = {0};
wchar_t g_dir[MAX_PATH] = {0};
wchar_t g_exeName[MAX_PATH] = {0};
DWORD   g_pid = 0;

void cat_w(wchar_t* dst, int cap, const wchar_t* src) {
    const int used = lstrlenW(dst);
    if (used < 0 || used >= cap - 1) return;
    lstrcpynW(dst + used, src, cap - used);
}

bool file_exists(const wchar_t* path) {
    const DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void write_note(const wchar_t* body) {
    wchar_t stamp[96] = {0};
    wchar_t line[1024] = {0};
    SYSTEMTIME now{};
    wchar_t date[32] = {0};
    wchar_t clock[32] = {0};

    GetLocalTime(&now);
    GetDateFormatW(LOCALE_INVARIANT, 0, &now, L"yyyy-MM-dd", date, 32);
    GetTimeFormatW(LOCALE_INVARIANT, TIME_FORCE24HOURFORMAT, &now, L"HH:mm:ss", clock, 32);
    wsprintfW(stamp, L"%s %s pid=%lu exe=%s", date, clock, static_cast<unsigned long>(g_pid),
              g_exeName[0] != L'\0' ? g_exeName : L"(unknown)");

    lstrcpynW(line, stamp, 1024);
    cat_w(line, 1024, L" -- ");
    cat_w(line, 1024, body);
    cat_w(line, 1024, L"\r\n");

    wchar_t path[MAX_PATH] = {0};
    lstrcpynW(path, g_dir, MAX_PATH);
    cat_w(path, MAX_PATH, L"dsh-proxy-load.txt");

    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER size{};
    const bool isNew = GetFileSizeEx(file, &size) && size.QuadPart == 0;
    DWORD written = 0;
    if (isNew) {
        const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
        WriteFile(file, bom, 3, &written, nullptr);
    }

    const int wideChars = lstrlenW(line);
    char utf8[2048] = {0};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, wideChars, utf8,
                                          static_cast<int>(sizeof(utf8)), nullptr, nullptr);
    if (bytes > 0) {
        WriteFile(file, utf8, static_cast<DWORD>(bytes), &written, nullptr);
    }
    CloseHandle(file);
}

HMODULE real_version() {
    if (g_realVersion != nullptr) return g_realVersion;

    wchar_t systemDir[MAX_PATH] = {0};
    const UINT n = GetSystemDirectoryW(systemDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        write_note(L"proxy: GetSystemDirectory failed");
        return nullptr;
    }

    lstrcpynW(g_realPath, systemDir, MAX_PATH);
    cat_w(g_realPath, MAX_PATH, L"\\version.dll");

    g_realVersion = LoadLibraryW(g_realPath);
    if (g_realVersion == nullptr) {
        write_note(L"proxy: FAILED to load System32\\version.dll -- host version queries will fail");
    }
    return g_realVersion;
}

template <typename Fn>
Fn resolve(const char* name) {
    HMODULE real = real_version();
    if (real == nullptr) return nullptr;
    return reinterpret_cast<Fn>(GetProcAddress(real, name));
}

void unavailable(const char* name) {
    static int reported = 0;
    if (reported >= 4) return;
    ++reported;
    wchar_t buf[192] = {0};
    MultiByteToWideChar(CP_ACP, 0, name, lstrlenA(name), buf, 96);
    cat_w(buf, 192, L" -> real version.dll export unavailable");
    write_note(buf);
}

const char* const kExportNames[] = {
    "GetFileVersionInfoA",          "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",
    "GetFileVersionInfoExW",        "GetFileVersionInfoSizeA",    "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW",    "GetFileVersionInfoSizeW",    "GetFileVersionInfoW",
    "VerFindFileA",                 "VerFindFileW",               "VerInstallFileA",
    "VerInstallFileW",              "VerQueryValueA",             "VerQueryValueW",
};

void note_export_health() {
    HMODULE real = real_version();
    wchar_t missing[512] = {0};
    int found = 0;
    const int total = static_cast<int>(sizeof(kExportNames) / sizeof(kExportNames[0]));
    for (int i = 0; i < total; ++i) {
        if (real != nullptr && GetProcAddress(real, kExportNames[i]) != nullptr) {
            ++found;
            continue;
        }
        if (missing[0] != L'\0') cat_w(missing, 512, L", ");
        wchar_t name[64] = {0};
        MultiByteToWideChar(CP_ACP, 0, kExportNames[i], lstrlenA(kExportNames[i]), name, 64);
        cat_w(missing, 512, name);
    }

    wchar_t line[640] = {0};
    wchar_t numbers[64] = {0};
    wsprintfW(numbers, L"%d/%d forwarded", found, total);
    lstrcpynW(line, numbers, 640);
    if (found == total) {
        cat_w(line, 640, L" from ");
        cat_w(line, 640, g_realPath);
    } else {
        cat_w(line, 640, L" -- MISSING: ");
        cat_w(line, 640, missing);
    }
    write_note(line);
}

bool this_is_the_game() {
    return lstrcmpiW(g_exeName, L"DollsNest-Win64-Shipping.exe") == 0;
}

DWORD WINAPI loader_thread(LPVOID) {
    write_note(L"proxy attached");

    if (!this_is_the_game()) {
        write_note(L"this process is NOT DollsNest-Win64-Shipping.exe -- nothing loaded, nothing touched");
        return 0;
    }

    {
        wchar_t off[MAX_PATH] = {0};
        lstrcpynW(off, g_dir, MAX_PATH);
        cat_w(off, MAX_PATH, L"dsh-no-mod.txt");
        if (file_exists(off)) {
            write_note(L"dsh-no-mod.txt is present -- the mod was NOT loaded (delete the file to re-enable)");
            return 0;
        }
    }

    note_export_health();

    wchar_t payload[MAX_PATH] = {0};
    lstrcpynW(payload, g_dir, MAX_PATH);
    cat_w(payload, MAX_PATH, L"dollsnest-coop.dll");

    HANDLE probe = CreateFileW(payload, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (probe == INVALID_HANDLE_VALUE) {
        wchar_t line[MAX_PATH + 128] = {0};
        lstrcpynW(line, L"payload NOT FOUND at ", MAX_PATH + 128);
        cat_w(line, MAX_PATH + 128, payload);
        cat_w(line, MAX_PATH + 128, L" -- nothing to load");
        write_note(line);
        return 0;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(probe, &size);
    CloseHandle(probe);

    {
        wchar_t numbers[96] = {0};
        wsprintfW(numbers, L" bytes=%lu", static_cast<unsigned long>(size.QuadPart));
        wchar_t line[MAX_PATH + 160] = {0};
        lstrcpynW(line, L"payload ", MAX_PATH + 160);
        cat_w(line, MAX_PATH + 160, payload);
        cat_w(line, MAX_PATH + 160, numbers);
        write_note(line);
    }

    HMODULE module = LoadLibraryW(payload);
    if (module == nullptr) {
        wchar_t numbers[96] = {0};
        wsprintfW(numbers, L"LoadLibraryW failed, GetLastError=%lu",
                  static_cast<unsigned long>(GetLastError()));
        write_note(numbers);
        return 0;
    }

    write_note(L"payload loaded -- it initialises itself from its own DllMain");
    return 0;
}

}

extern "C" DWORD WINAPI proxy_GetFileVersionInfoSizeA(LPCSTR a, LPDWORD b) {
    using Fn = decltype(&proxy_GetFileVersionInfoSizeA);
    static Fn fn = nullptr;
    if (fn == nullptr) fn = resolve<Fn>("GetFileVersionInfoSizeA");
    if (fn) return fn(a, b);
    unavailable("GetFileVersionInfoSizeA");
    return 0;
}

extern "C" DWORD WINAPI proxy_GetFileVersionInfoSizeW(LPCWSTR a, LPDWORD b) {
    using Fn = decltype(&proxy_GetFileVersionInfoSizeW);
    static Fn fn = nullptr;
    if (fn == nullptr) fn = resolve<Fn>("GetFileVersionInfoSizeW");
    if (fn) return fn(a, b);
    unavailable("GetFileVersionInfoSizeW");
    return 0;
}

extern "C" DWORD WINAPI proxy_GetFileVersionInfoSizeExA(DWORD f, LPCSTR a, LPDWORD b) {
    using Fn = decltype(&proxy_GetFileVersionInfoSizeExA);
    static Fn fn = nullptr;
    if (fn == nullptr) fn = resolve<Fn>("GetFileVersionInfoSizeExA");
    if (fn) return fn(f, a, b);
    unavailable("GetFileVersionInfoSizeExA");
    return 0;
}

extern "C" DWORD WINAPI proxy_GetFileVersionInfoSizeExW(DWORD f, LPCWSTR a, LPDWORD b) {
    using Fn = decltype(&proxy_GetFileVersionInfoSizeExW);
    static Fn fn = nullptr;
    if (fn == nullptr) fn = resolve<Fn>("GetFileVersionInfoSizeExW");
    if (fn) return fn(f, a, b);
    unavailable("GetFileVersionInfoSizeExW");
    return 0;
}

extern "C" BOOL WINAPI proxy_GetFileVersionInfoA(LPCSTR a, DWORD b, DWORD c, LPVOID d) {
    using Fn = decltype(&proxy_GetFileVersionInfoA);
    static Fn fn = nullptr;
    if (fn == nullptr) fn = resolve<Fn>("GetFileVersionInfoA");
    if (fn) return fn(a, b, c, d);
    unavailable("GetFileVersionInfoA");
    return FALSE;
}

extern "C" BOOL WINAPI proxy_GetFileVersionInfoW(LPCWSTR a, DWORD b, DWORD c, LPVOID d) {
    using Fn = decltype(&proxy_GetFileVersionInfoW);
    static Fn fn = nullptr;
    if (fn == nullptr) fn = resolve<Fn>("GetFileVersionInfoW");
    if (fn) return fn(a, b, c, d);
    unavailable("GetFileVersionInfoW");
    return FALSE;
}

extern "C" BOOL WINAPI proxy_GetFileVersionInfoExA(DWORD f, LPCSTR a, DWORD b, DWORD c, LPVOID d) {
    using Fn = decltype(&proxy_GetFileVersionInfoExA);
    static Fn fn = nullptr;
    if (fn == nullptr) fn = resolve<Fn>("GetFileVersionInfoExA");
    if (fn) return fn(f, a, b, c, d);
    unavailable("GetFileVersionInfoExA");
    return FALSE;
}

extern "C" BOOL WINAPI proxy_GetFileVersionInfoExW(DWORD f, LPCWSTR a, DWORD b, DWORD c, LPVOID d) {
    using Fn = decltype(&proxy_GetFileVersionInfoExW);
    static Fn fn = nullptr;
    if (fn == nullptr) fn = resolve<Fn>("GetFileVersionInfoExW");
    if (fn) return fn(f, a, b, c, d);
    unavailable("GetFileVersionInfoExW");
    return FALSE;
}

extern "C" BOOL WINAPI proxy_GetFileVersionInfoByHandle(DWORD a, HANDLE b, DWORD c, LPVOID d) {
    using Fn = decltype(&proxy_GetFileVersionInfoByHandle);
    static Fn fn = nullptr;
    if (fn == nullptr) fn = resolve<Fn>("GetFileVersionInfoByHandle");
    if (fn) return fn(a, b, c, d);
    unavailable("GetFileVersionInfoByHandle");
    return FALSE;
}

extern "C" BOOL WINAPI proxy_VerQueryValueA(LPCVOID a, LPCSTR b, LPVOID* c, PUINT d) {
    using Fn = decltype(&proxy_VerQueryValueA);
    static Fn fn = nullptr;
    if (fn == nullptr) fn = resolve<Fn>("VerQueryValueA");
    if (fn) return fn(a, b, c, d);
    unavailable("VerQueryValueA");
    return FALSE;
}

extern "C" BOOL WINAPI proxy_VerQueryValueW(LPCVOID a, LPCWSTR b, LPVOID* c, PUINT d) {
    using Fn = decltype(&proxy_VerQueryValueW);
    static Fn fn = nullptr;
    if (fn == nullptr) fn = resolve<Fn>("VerQueryValueW");
    if (fn) return fn(a, b, c, d);
    unavailable("VerQueryValueW");
    return FALSE;
}

extern "C" DWORD WINAPI proxy_VerFindFileA(DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPSTR e, PUINT f,
                                           LPSTR g, PUINT h) {
    using Fn = decltype(&proxy_VerFindFileA);
    static Fn fn = nullptr;
    if (fn == nullptr) fn = resolve<Fn>("VerFindFileA");
    if (fn) return fn(a, b, c, d, e, f, g, h);
    unavailable("VerFindFileA");
    return 0;
}

extern "C" DWORD WINAPI proxy_VerFindFileW(DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPWSTR e,
                                           PUINT f, LPWSTR g, PUINT h) {
    using Fn = decltype(&proxy_VerFindFileW);
    static Fn fn = nullptr;
    if (fn == nullptr) fn = resolve<Fn>("VerFindFileW");
    if (fn) return fn(a, b, c, d, e, f, g, h);
    unavailable("VerFindFileW");
    return 0;
}

extern "C" DWORD WINAPI proxy_VerInstallFileA(DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPCSTR e,
                                              LPSTR f, PUINT g) {
    using Fn = decltype(&proxy_VerInstallFileA);
    static Fn fn = nullptr;
    if (fn == nullptr) fn = resolve<Fn>("VerInstallFileA");
    if (fn) return fn(a, b, c, d, e, f, g);
    unavailable("VerInstallFileA");
    return 0;
}

extern "C" DWORD WINAPI proxy_VerInstallFileW(DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPCWSTR e,
                                              LPWSTR f, PUINT g) {
    using Fn = decltype(&proxy_VerInstallFileW);
    static Fn fn = nullptr;
    if (fn == nullptr) fn = resolve<Fn>("VerInstallFileW");
    if (fn) return fn(a, b, c, d, e, f, g);
    unavailable("VerInstallFileW");
    return 0;
}

extern "C" BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_selfModule = module;
        g_pid = GetCurrentProcessId();
        DisableThreadLibraryCalls(module);

        wchar_t selfPath[MAX_PATH] = {0};
        if (GetModuleFileNameW(module, selfPath, MAX_PATH) != 0) {
            lstrcpynW(g_dir, selfPath, MAX_PATH);
            wchar_t* lastSlash = nullptr;
            for (wchar_t* p = g_dir; *p != L'\0'; ++p) {
                if (*p == L'\\') lastSlash = p;
            }
            if (lastSlash != nullptr) {
                *(lastSlash + 1) = L'\0';
            }
        }

        wchar_t exePath[MAX_PATH] = {0};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) != 0) {
            const wchar_t* base = exePath;
            for (const wchar_t* p = exePath; *p != L'\0'; ++p) {
                if (*p == L'\\') base = p + 1;
            }
            lstrcpynW(g_exeName, base, MAX_PATH);
        }

        if (g_dir[0] == L'\0') {
            return TRUE;
        }

        HANDLE thread = CreateThread(nullptr, 0, &loader_thread, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        } else {
            write_note(L"could not create the loader thread");
        }
    }
    return TRUE;
}
