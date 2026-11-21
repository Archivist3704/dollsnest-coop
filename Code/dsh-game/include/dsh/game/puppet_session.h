#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace dsh::game {

struct PuppetSessionResult {
    bool ok = false;
    int  chunksSent = 0;

    int  actorsSpawned = 0;
    int  machineModulesMerged = 0;
    std::string detail;
};

[[nodiscard]] PuppetSessionResult run_puppet_session(bool asHost, const std::string& address,
                                                    std::uint16_t port, int seconds);

void set_session_tick_hook(std::function<void()> hook);

void request_session_stop(const std::string& reason);
[[nodiscard]] bool session_stop_requested();
[[nodiscard]] std::string session_stop_reason();

void request_parts_dump();

void request_parts_hide();

void request_parts_fix();

[[nodiscard]] bool session_running();

}
