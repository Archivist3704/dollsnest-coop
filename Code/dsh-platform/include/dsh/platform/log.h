#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace dsh::platform {

enum class LogLevel : int {
    trace = 0,
    debug = 1,
    info  = 2,
    warn  = 3,
    error = 4,
    fatal = 5,
};

void log_init(std::string_view explicitDirOrFile = {});
void log_shutdown();

[[nodiscard]] bool log_is_open();

[[nodiscard]] std::string_view log_path();

void log_write(LogLevel level, const char* file, int line, const char* fmt, ...);

void log_dump_breadcrumbs();

void log_breadcrumb(const char* file, int line, const char* fmt, ...);

}

#define DSH_LOG_AT(lvl, ...) ::dsh::platform::log_write((lvl), __FILE__, __LINE__, __VA_ARGS__)

#define DSH_TRACE(...) DSH_LOG_AT(::dsh::platform::LogLevel::trace, __VA_ARGS__)
#define DSH_DEBUG(...) DSH_LOG_AT(::dsh::platform::LogLevel::debug, __VA_ARGS__)
#define DSH_INFO(...)  DSH_LOG_AT(::dsh::platform::LogLevel::info,  __VA_ARGS__)
#define DSH_WARN(...)  DSH_LOG_AT(::dsh::platform::LogLevel::warn,  __VA_ARGS__)
#define DSH_ERROR(...) DSH_LOG_AT(::dsh::platform::LogLevel::error, __VA_ARGS__)

#define DSH_FATAL(...) DSH_LOG_AT(::dsh::platform::LogLevel::fatal, __VA_ARGS__)

#define DSH_BREADCRUMB(...) ::dsh::platform::log_breadcrumb(__FILE__, __LINE__, __VA_ARGS__)
