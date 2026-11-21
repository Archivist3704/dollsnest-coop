#include "dsh/platform/log.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace dsh::platform {
namespace {

constexpr std::size_t kRingCapacity = 256;
constexpr std::size_t kRingLineMax  = 512;

std::mutex              g_mutex;
FILE*                   g_file = nullptr;
std::string             g_path;
std::atomic<LogLevel>   g_minLevel{LogLevel::info};
std::atomic<std::uint64_t> g_dropped{0};
std::atomic<std::uint64_t> g_seq{0};

std::atomic<std::uint64_t> g_truncated{0};
constexpr const char kTruncationMark[] = " ...[TRUNCATED]";

LARGE_INTEGER g_qpcFreq{};
LARGE_INTEGER g_qpcStart{};
std::atomic<bool> g_qpcReady{false};

struct RingEntry {
    std::uint64_t tickMs = 0;
    char          text[kRingLineMax] = {};
};
std::vector<RingEntry> g_ring;
std::size_t            g_ringHead = 0;
std::size_t            g_ringCount = 0;

std::uint64_t tick_ms() {
    if (!g_qpcReady.load(std::memory_order_acquire)) {
        QueryPerformanceFrequency(&g_qpcFreq);
        QueryPerformanceCounter(&g_qpcStart);
        g_qpcReady.store(true, std::memory_order_release);
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (g_qpcFreq.QuadPart == 0) return 0;
    return static_cast<std::uint64_t>(
        (now.QuadPart - g_qpcStart.QuadPart) * 1000 / g_qpcFreq.QuadPart);
}

const char* level_name(LogLevel l) {
    switch (l) {
        case LogLevel::trace: return "TRACE";
        case LogLevel::debug: return "DEBUG";
        case LogLevel::info:  return "INFO ";
        case LogLevel::warn:  return "WARN ";
        case LogLevel::error: return "ERROR";
        case LogLevel::fatal: return "FATAL";
    }
    return "?????";
}

const char* short_file(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') last = p + 1;
    }
    return last;
}

std::string game_dir() {
    char buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::string s(buf, n);
    const auto pos = s.find_last_of("\\/");
    return pos == std::string::npos ? std::string{} : s.substr(0, pos);
}

std::string local_appdata_logs_dir() {
    char buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::string s(buf, n);
    s += "\\DollsNest\\Saved\\Logs";
    return s;
}

bool ensure_dir(const std::string& dir) {
    if (dir.empty()) return false;
    if (CreateDirectoryA(dir.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

FILE* try_open(const std::string& dir, std::string& outPath) {
    if (!ensure_dir(dir)) return nullptr;
    std::string p = dir + "\\dollsnest-coop.log";

    FILE* f = _fsopen(p.c_str(), "a", _SH_DENYNO);
    if (!f) return nullptr;
    outPath = p;
    return f;
}

void push_ring(std::uint64_t tick, const char* text) {
    if (g_ring.empty()) g_ring.resize(kRingCapacity);
    RingEntry& e = g_ring[g_ringHead];
    e.tickMs = tick;
    std::snprintf(e.text, sizeof(e.text), "%s", text);
    g_ringHead = (g_ringHead + 1) % kRingCapacity;
    if (g_ringCount < kRingCapacity) ++g_ringCount;
}

}

void log_init(std::string_view explicitDirOrFile) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) return;

    std::string dir;
    if (!explicitDirOrFile.empty()) {
        dir.assign(explicitDirOrFile);
    } else {
        dir = game_dir();
    }

    g_file = try_open(dir, g_path);
    if (!g_file) {

        const std::string fallback = local_appdata_logs_dir();
        std::string fallbackPath;
        g_file = try_open(fallback, fallbackPath);
        if (g_file) {
            g_path = fallbackPath;
            std::fprintf(g_file,
                "!! primary log directory unusable, fell back to LOCALAPPDATA\n"
                "!! attempted: %s\n!! using : %s\n",
                dir.c_str(), g_path.c_str());
        }
    }

    if (g_file) {
        std::fprintf(g_file,
            "\n"
            "============================================================\n"
            " dollsnest-coop log opened\n"
            " tick=0 path=%s\n"
            "============================================================\n",
            g_path.c_str());
        std::fflush(g_file);
    }
}

void log_shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;
    std::fprintf(g_file, "[log] closing: lines=%llu dropped=%llu truncated=%llu\n",
                 static_cast<unsigned long long>(g_seq.load()),
                 static_cast<unsigned long long>(g_dropped.load()),
                 static_cast<unsigned long long>(g_truncated.load()));
    std::fflush(g_file);
    std::fclose(g_file);
    g_file = nullptr;
}

bool log_is_open() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_file != nullptr;
}

std::string_view log_path() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_path;
}

void log_write(LogLevel level, const char* file, int line, const char* fmt, ...) {
    if (static_cast<int>(level) < static_cast<int>(g_minLevel.load())) return;

    char body[1024];

    constexpr std::size_t kRoom = sizeof(body) - sizeof(kTruncationMark);
    va_list args;
    va_start(args, fmt);
    const int wanted = std::vsnprintf(body, kRoom, fmt, args);
    va_end(args);
    if (wanted < 0 || static_cast<std::size_t>(wanted) >= kRoom) {
        std::strcat(body, kTruncationMark);
        g_truncated.fetch_add(1, std::memory_order_relaxed);
    }

    const std::uint64_t tick = tick_ms();
    const unsigned long tid = GetCurrentThreadId();

    char full[1400];
    std::snprintf(full, sizeof(full), "[%8llu][t%05lu][%s] %s (%s:%d)\n",
                  static_cast<unsigned long long>(tick), tid,
                  level_name(level), body, short_file(file), line);

    std::lock_guard<std::mutex> lock(g_mutex);
    g_seq.fetch_add(1, std::memory_order_relaxed);
    if (!g_file) { g_dropped.fetch_add(1, std::memory_order_relaxed); return; }
    if (std::fwrite(full, 1, std::strlen(full), g_file) != std::strlen(full)) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    std::fflush(g_file);
}

void log_breadcrumb(const char* file, int line, const char* fmt, ...) {
    char body[512];

    constexpr std::size_t kRoom = sizeof(body) - sizeof(kTruncationMark);
    va_list args;
    va_start(args, fmt);
    const int wanted = std::vsnprintf(body, kRoom, fmt, args);
    va_end(args);
    if (wanted < 0 || static_cast<std::size_t>(wanted) >= kRoom) {
        std::strcat(body, kTruncationMark);
        g_truncated.fetch_add(1, std::memory_order_relaxed);
    }

    const std::uint64_t tick = tick_ms();
    char full[640];
    std::snprintf(full, sizeof(full), "%8llu %s (%s:%d)",
                  static_cast<unsigned long long>(tick), body,
                  short_file(file), line);

    std::lock_guard<std::mutex> lock(g_mutex);
    push_ring(tick, full);
    if (!g_file) return;
    std::fprintf(g_file, "[ ][bread ] %s\n", full);
    std::fflush(g_file);
}

void log_dump_breadcrumbs() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;
    std::fprintf(g_file, "[log] --- last %llu breadcrumbs (oldest first) ---\n",
                 static_cast<unsigned long long>(g_ringCount));
    const std::size_t start = (g_ringCount < kRingCapacity)
                                  ? 0
                                  : g_ringHead;
    for (std::size_t i = 0; i < g_ringCount; ++i) {
        const RingEntry& e = g_ring[(start + i) % kRingCapacity];
        std::fprintf(g_file, "[bread] %s\n", e.text);
    }
    std::fprintf(g_file, "[log] --- end breadcrumbs ---\n");
    std::fflush(g_file);
}

}
