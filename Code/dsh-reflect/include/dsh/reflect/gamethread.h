#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace dsh::reflect {

void game_thread_push(std::function<void()> work);

std::uint32_t game_thread_drain();

[[nodiscard]] std::uint32_t game_thread_pending();

using GameThreadHookInstaller = bool (*)(std::uintptr_t processEventAddress);
void set_game_thread_hook_installer(GameThreadHookInstaller installer);

bool game_thread_start_hook();

bool run_on_game_thread(std::function<void()> work, int timeoutMs);

void game_thread_pause_all(bool paused);
[[nodiscard]] bool game_thread_is_paused();

[[nodiscard]] bool game_thread_hook_installed();

[[nodiscard]] std::uint32_t game_thread_dropped();

void game_thread_note_process_event();
[[nodiscard]] std::uint32_t game_thread_owner_id();
[[nodiscard]] std::uint32_t game_thread_refused_drains();

[[nodiscard]] std::uintptr_t process_event_address();
[[nodiscard]] std::uint32_t  current_thread_id();

[[nodiscard]] std::string game_thread_report();

[[nodiscard]] std::string game_thread_dispatch_report();

}
