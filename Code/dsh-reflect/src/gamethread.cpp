#include "dsh/reflect/gamethread.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "dsh/platform/log.h"

namespace dsh::reflect {
namespace {

std::mutex                              g_mutex;
std::vector<std::function<void()>>      g_queue;

std::atomic<std::uint32_t>              g_pending{0};
std::atomic<std::uint32_t>              g_gameThreadId{0};
std::atomic<std::uint32_t>              g_drained{0};
std::atomic<std::uint32_t>              g_pushed{0};
std::atomic<bool>                       g_hookTried{false};
std::atomic<bool>                       g_hookOk{false};

std::atomic<bool>                       g_paused{false};
std::atomic<std::uint32_t>              g_dropped{0};

GameThreadHookInstaller g_installer = nullptr;

constexpr std::size_t   kThreadTrackSlots = 16;
constexpr std::uint32_t kDrainOwnerMinCalls = 32;
std::atomic<std::uint32_t> g_threadTrack[kThreadTrackSlots];
std::atomic<std::uint32_t> g_threadCalls[kThreadTrackSlots];
std::atomic<std::uint32_t> g_drainOwner{0};
std::atomic<std::uint32_t> g_refusedDrains{0};
std::atomic<std::uint32_t> g_undecidedDrains{0};

std::atomic<std::uint64_t> g_dispatchPushed{0};
std::atomic<std::uint64_t> g_dispatchRan{0};
std::atomic<std::uint64_t> g_dispatchCancelled{0};
std::atomic<std::uint64_t> g_dispatchWaitTotalUs{0};
std::atomic<std::uint64_t> g_dispatchWaitMaxUs{0};
std::atomic<std::uint64_t> g_dispatchExecTotalUs{0};
std::atomic<std::uint64_t> g_dispatchExecMaxUs{0};
std::atomic<std::uint64_t> g_drainBatches{0};
std::atomic<std::uint64_t> g_drainMaxItems{0};
std::atomic<std::uint64_t> g_drainMaxBatchUs{0};
std::atomic<std::uint64_t> g_drainBatchesOver5ms{0};

std::uint64_t now_us() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point epoch = clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - epoch).count());
}

void bump_max(std::atomic<std::uint64_t>& slot, std::uint64_t value) {
    std::uint64_t current = slot.load(std::memory_order_relaxed);
    while (value > current &&
           !slot.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

}

void game_thread_push(std::function<void()> work) {
    if (!work) return;
    {
        const std::lock_guard<std::mutex> lock(g_mutex);
        g_queue.push_back(std::move(work));
        g_pending.store(static_cast<std::uint32_t>(g_queue.size()), std::memory_order_relaxed);
    }
    g_pushed.fetch_add(1, std::memory_order_relaxed);
}

void game_thread_note_process_event() {
    const std::uint32_t self = current_thread_id();
    if (self == 0) return;
    std::size_t slot = kThreadTrackSlots;
    for (std::size_t i = 0; i < kThreadTrackSlots; ++i) {
        if (g_threadTrack[i].load(std::memory_order_relaxed) == self) {
            slot = i;
            break;
        }
    }
    if (slot == kThreadTrackSlots) {
        for (std::size_t i = 0; i < kThreadTrackSlots; ++i) {
            std::uint32_t expected = 0;
            if (g_threadTrack[i].compare_exchange_strong(expected, self, std::memory_order_relaxed)) {
                g_threadCalls[i].store(1, std::memory_order_relaxed);
                slot = i;
                break;
            }
        }
        if (slot == kThreadTrackSlots) return;
    } else {
        g_threadCalls[slot].fetch_add(1, std::memory_order_relaxed);
    }
    const std::uint32_t mine = g_threadCalls[slot].load(std::memory_order_relaxed);
    const std::uint32_t owner = g_drainOwner.load(std::memory_order_relaxed);
    if (owner == 0) {
        if (mine < kDrainOwnerMinCalls) return;
        std::uint32_t expected = 0;
        if (!g_drainOwner.compare_exchange_strong(expected, self, std::memory_order_relaxed)) return;
        DSH_INFO("game-thread: the drain owner is thread %u -- it is the first to pass %u ProcessEvent calls, and "
                 "from now on ONLY that thread may run this queue (the engine calls ProcessEvent from worker "
                 "threads too, and draining there is what put an engine write on a task thread in 643-1)",
                 static_cast<unsigned>(self), static_cast<unsigned>(kDrainOwnerMinCalls));
        return;
    }
    if (self == owner) return;
    std::uint32_t ownerCalls = 0;
    for (std::size_t i = 0; i < kThreadTrackSlots; ++i) {
        if (g_threadTrack[i].load(std::memory_order_relaxed) == owner) {
            ownerCalls = g_threadCalls[i].load(std::memory_order_relaxed);
            break;
        }
    }
    if (mine <= ownerCalls) return;
    g_drainOwner.store(self, std::memory_order_relaxed);
    DSH_WARN("game-thread: the drain owner MOVED to thread %u (%u calls) from thread %u (%u calls) -- that should "
             "not happen outside a load; if it does, this queue can be run by the wrong thread again",
             static_cast<unsigned>(self), static_cast<unsigned>(mine), static_cast<unsigned>(owner),
             static_cast<unsigned>(ownerCalls));
}

std::uint32_t game_thread_owner_id() { return g_drainOwner.load(std::memory_order_relaxed); }

std::uint32_t game_thread_refused_drains() { return g_refusedDrains.load(std::memory_order_relaxed); }

std::uint32_t game_thread_drain() {
    game_thread_note_process_event();
    const std::uint32_t self = current_thread_id();
    const std::uint32_t owner = g_drainOwner.load(std::memory_order_relaxed);
    if (owner == 0) {

        g_undecidedDrains.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    if (self != owner) {
        const std::uint32_t refused = g_refusedDrains.fetch_add(1, std::memory_order_relaxed) + 1;
        if (refused == 1 || refused == 10 || refused == 100 || (refused % 1000) == 0) {
            DSH_WARN("game-thread: REFUSED to drain on thread %u -- the owner is %u, and the item stays queued for "
                     "it (refusals so far: %u)", static_cast<unsigned>(self), static_cast<unsigned>(owner),
                     static_cast<unsigned>(refused));
        }
        return 0;
    }
    std::uint32_t expected = 0;
    g_gameThreadId.compare_exchange_strong(expected, self, std::memory_order_relaxed);

    std::vector<std::function<void()>> batch;
    {
        const std::lock_guard<std::mutex> lock(g_mutex);
        if (g_queue.empty()) {
            g_pending.store(0, std::memory_order_relaxed);
            return 0;
        }
        if (g_paused.load(std::memory_order_relaxed)) {

            const std::uint32_t dropped = static_cast<std::uint32_t>(g_queue.size());
            g_queue.clear();
            g_pending.store(0, std::memory_order_relaxed);
            g_dropped.fetch_add(dropped, std::memory_order_relaxed);
            return 0;
        }
        batch.swap(g_queue);
        g_pending.store(0, std::memory_order_relaxed);
    }

    const std::size_t batchItems = batch.size();
    const std::uint64_t batchStartUs = now_us();
    for (auto& work : batch) {

        try {
            work();
        } catch (...) {
            DSH_WARN("game-thread: a queued item threw");
        }
    }
    const std::uint64_t batchEndUs = now_us();
    const std::uint64_t batchUs = batchEndUs >= batchStartUs ? batchEndUs - batchStartUs : 0;
    g_drainBatches.fetch_add(1, std::memory_order_relaxed);
    bump_max(g_drainMaxItems, static_cast<std::uint64_t>(batchItems));
    bump_max(g_drainMaxBatchUs, batchUs);
    if (batchUs > 5000) g_drainBatchesOver5ms.fetch_add(1, std::memory_order_relaxed);
    g_drained.fetch_add(static_cast<std::uint32_t>(batch.size()), std::memory_order_relaxed);
    return static_cast<std::uint32_t>(batch.size());
}

std::uint32_t game_thread_pending() {

    return g_pending.load(std::memory_order_relaxed);
}

void game_thread_pause_all(bool paused) {
    if (paused) {
        const std::uint32_t pending = game_thread_pending();
        g_paused.store(true, std::memory_order_relaxed);
        DSH_WARN("game-thread: PAUSED -- %u item(s) were queued and every future item is refused until the "
                 "pause is lifted. This is what a caller says before it replaces the world: running queued "
                 "work during that teardown is the 2026-10-18 access violation.", pending);
        return;
    }
    g_paused.store(false, std::memory_order_relaxed);
    DSH_INFO("game-thread: pause LIFTED -- %u item(s) were dropped while it was set",
             g_dropped.load(std::memory_order_relaxed));
}

bool game_thread_is_paused() { return g_paused.load(std::memory_order_relaxed); }

bool game_thread_hook_installed() { return g_hookOk.load(std::memory_order_relaxed); }

std::uint32_t game_thread_dropped() { return g_dropped.load(std::memory_order_relaxed); }

void set_game_thread_hook_installer(GameThreadHookInstaller installer) {
    g_installer = installer;
}

bool game_thread_start_hook() {
    if (g_hookOk.load(std::memory_order_relaxed)) return true;
    if (g_installer == nullptr) {
        DSH_WARN("game-thread: no hook installer registered, engine work will stay on the worker");
        return false;
    }

    bool expected = false;
    if (!g_hookTried.compare_exchange_strong(expected, true)) {
        return g_hookOk.load(std::memory_order_relaxed);
    }

    const std::uintptr_t processEvent = process_event_address();
    if (processEvent == 0) {
        DSH_WARN("game-thread: cannot hook, ProcessEvent is unresolved");

        g_hookTried.store(false, std::memory_order_relaxed);
        return false;
    }
    const bool ok = g_installer(processEvent);
    g_hookOk.store(ok, std::memory_order_relaxed);
    if (!ok) {

        g_hookTried.store(false, std::memory_order_relaxed);
    }
    DSH_INFO("game-thread: hook %s", ok ? "installed" : "FAILED");
    return ok;
}

std::string game_thread_report() {

    std::string calls;
    for (std::size_t i = 0; i < kThreadTrackSlots; ++i) {
        const std::uint32_t id = g_threadTrack[i].load(std::memory_order_relaxed);
        if (id == 0) continue;
        char one[48] = {};
        std::snprintf(one, sizeof(one), "%s%u:%u", calls.empty() ? "" : " ",
                      static_cast<unsigned>(id), static_cast<unsigned>(g_threadCalls[i].load(std::memory_order_relaxed)));
        calls += one;
    }
    return "queued=" + std::to_string(game_thread_pending()) +
           " pushed=" + std::to_string(g_pushed.load(std::memory_order_relaxed)) +
           " drained=" + std::to_string(g_drained.load(std::memory_order_relaxed)) +
           " gameThreadId=" + std::to_string(g_gameThreadId.load(std::memory_order_relaxed)) +
           " owner=" + std::to_string(g_drainOwner.load(std::memory_order_relaxed)) +
           " refusedDrains=" + std::to_string(g_refusedDrains.load(std::memory_order_relaxed)) +
           " undecidedDrains=" + std::to_string(g_undecidedDrains.load(std::memory_order_relaxed)) +
           " processEventCalls(thread:count)=" + (calls.empty() ? "none" : calls) +
           " hook=" + (g_hookOk.load(std::memory_order_relaxed) ? "installed" : "absent");
}

std::string game_thread_dispatch_report() {
    const std::uint64_t pushed = g_dispatchPushed.load(std::memory_order_relaxed);
    const std::uint64_t ran = g_dispatchRan.load(std::memory_order_relaxed);
    const std::uint64_t cancelled = g_dispatchCancelled.load(std::memory_order_relaxed);
    const double waitAvgMs = ran == 0 ? 0.0
        : static_cast<double>(g_dispatchWaitTotalUs.load(std::memory_order_relaxed)) / 1000.0 /
              static_cast<double>(ran);
    const double execAvgMs = ran == 0 ? 0.0
        : static_cast<double>(g_dispatchExecTotalUs.load(std::memory_order_relaxed)) / 1000.0 /
              static_cast<double>(ran);
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "pushed=%llu ran=%llu cancelled=%llu | wait max=%.1f avg=%.1f ms | exec max=%.1f "
                  "avg=%.1f ms | batches=%llu items max=%llu batch max=%.1f ms over5ms=%llu",
                  static_cast<unsigned long long>(pushed), static_cast<unsigned long long>(ran),
                  static_cast<unsigned long long>(cancelled),
                  static_cast<double>(g_dispatchWaitMaxUs.load(std::memory_order_relaxed)) / 1000.0,
                  waitAvgMs,
                  static_cast<double>(g_dispatchExecMaxUs.load(std::memory_order_relaxed)) / 1000.0,
                  execAvgMs,
                  static_cast<unsigned long long>(g_drainBatches.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_drainMaxItems.load(std::memory_order_relaxed)),
                  static_cast<double>(g_drainMaxBatchUs.load(std::memory_order_relaxed)) / 1000.0,
                  static_cast<unsigned long long>(g_drainBatchesOver5ms.load(std::memory_order_relaxed)));
    return buf;
}

bool run_on_game_thread(std::function<void()> work, int timeoutMs) {
    if (!work) return false;

    if (g_paused.load(std::memory_order_relaxed)) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const std::uint32_t gt = g_drainOwner.load(std::memory_order_relaxed);
    if (gt != 0 && gt == current_thread_id()) {
        work();
        return true;
    }

    if (!g_hookOk.load(std::memory_order_relaxed)) {
        static std::atomic<bool> warned{false};
        bool expected = false;
        if (warned.compare_exchange_strong(expected, true)) {
            DSH_WARN("game-thread: refusing to run engine work off-thread -- run 'gthook' first "
                     "(the trampoline is installed on request, not on demand)");
        }
        return false;
    }

    auto state = std::make_shared<std::atomic<int>>(0);
    const std::uint64_t pushedUs = now_us();
    g_dispatchPushed.fetch_add(1, std::memory_order_relaxed);
    game_thread_push([work = std::move(work), state, pushedUs]() {
        int expected = 0;
        if (!state->compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
            return;
        }

        const std::uint64_t ranUs = now_us();
        work();
        const std::uint64_t doneUs = now_us();
        const std::uint64_t waitUs = ranUs >= pushedUs ? ranUs - pushedUs : 0;
        const std::uint64_t execUs = doneUs >= ranUs ? doneUs - ranUs : 0;
        g_dispatchRan.fetch_add(1, std::memory_order_relaxed);
        g_dispatchWaitTotalUs.fetch_add(waitUs, std::memory_order_relaxed);
        g_dispatchExecTotalUs.fetch_add(execUs, std::memory_order_relaxed);
        bump_max(g_dispatchWaitMaxUs, waitUs);
        bump_max(g_dispatchExecMaxUs, execUs);
        state->store(2, std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const int now = state->load(std::memory_order_acquire);
        if (now == 2) return true;
        if (std::chrono::steady_clock::now() >= deadline) {
            int expected = 0;
            if (state->compare_exchange_strong(expected, 3, std::memory_order_acq_rel)) {
                g_dispatchCancelled.fetch_add(1, std::memory_order_relaxed);
                DSH_WARN("game-thread: timed out waiting %d ms and the item was CANCELLED before it "
                         "ran -- it will never run, so the caller may return safely", timeoutMs);
                return false;
            }
            DSH_WARN("game-thread: timed out waiting %d ms but the item is ALREADY RUNNING -- waiting "
                     "for it to finish rather than returning into a frame it is still using",
                     timeoutMs);
            while (state->load(std::memory_order_acquire) != 2) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

}
