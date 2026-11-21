#include "dsh/bootstrap/bootstrap.h"

#include "dsh/game/game.h"
#include "dsh/platform/hook.h"
#include "dsh/platform/log.h"
#include "dsh/platform/file_digest.h"
#include "dsh/reflect/processevent.h"
#include "dsh/reflect/world.h"
#include "dsh/reflect/spawn.h"
#include "dsh/reflect/mission.h"
#include "dsh/reflect/enemies.h"
#include "dsh/reflect/gates.h"
#include "dsh/reflect/gamethread.h"
#include "dsh/game/health_sync.h"
#include "dsh/game/ammo_spawn.h"
#include "dsh/game/item_wire.h"
#include "dsh/game/net_runtime.h"
#include "dsh/game/puppet_session.h"
#include "dsh/game/pawn.h"
#include "dsh/net/bytes.h"
#include "dsh/net/transport.h"
#include "dsh/net/discovery.h"
#include "dsh/reflect/reflection.h"
#include "dsh/reflect/reflect.h"

#include "menu_text.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <string>

#if defined(DSH_HAVE_MINHOOK)
#include <MinHook.h>
#endif

#ifndef DSH_BUILD_STAMP
#define DSH_BUILD_STAMP "unknown"
#endif

void registry_selftest_for_log();

namespace dsh::bootstrap {
namespace {

constexpr std::uint32_t kApiVersion = 1;

std::atomic<bool> g_initialised{false};
std::atomic<bool> g_workerStarted{false};
std::wstring      g_commandLine;
void*             g_moduleBase = nullptr;

std::atomic<bool> g_inGameProcess{false};

bool in_game_process() { return g_inGameProcess.load(std::memory_order_relaxed); }

std::atomic<bool> g_engineCommandsAllowed{false};

std::string exe_dir() {
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::string s(path, n);
    const auto pos = s.find_last_of("\\/");
    return (pos == std::string::npos) ? std::string{} : s.substr(0, pos);
}

std::string hex_ptr(const void* p) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llX",
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p)));
    return buf;
}

std::string evidence_dir() {
    const std::string dir = exe_dir();
    if (dir.empty()) return {};

    const std::string hintPath = dir + "\\dsh-logdir.txt";
    HANDLE h = CreateFileA(hintPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        char buf[MAX_PATH] = {};
        DWORD got = 0;
        if (ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr) && got > 0) {
            std::string s(buf, got);

            while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
            if (!s.empty()) {
                CloseHandle(h);
                return s;
            }
        }
        CloseHandle(h);
    }
    return dir;
}

void append_to(const std::string& dir, const char* fileName, const char* line) {
    if (dir.empty()) {
        DSH_WARN("append: no evidence directory -- '%s' was NOT written (lost line: %s)",
                 fileName, line);
        return;
    }
    const std::string path = dir + "\\" + fileName;

    HANDLE h = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DSH_WARN("append: CreateFileA('%s') failed (err=%lu) -- the line was NOT written "
                 "(lost line: %s)", path.c_str(), GetLastError(), line);
        return;
    }
    const std::string l = std::string(line) + "\r\n";
    DWORD written = 0;
    if (!WriteFile(h, l.c_str(), static_cast<DWORD>(l.size()), &written, nullptr) ||
        written != static_cast<DWORD>(l.size())) {
        DSH_WARN("append: WriteFile('%s') wrote %lu of %zu byte(s) (err=%lu) -- the line is missing "
                 "or truncated", path.c_str(), static_cast<unsigned long>(written), l.size(),
                 GetLastError());
    }
    CloseHandle(h);
}

void append_marker(const char* line) {
    append_to(evidence_dir(), "dsh-module-marker.txt", line);
}

bool is_game_process(const std::string& exeBaseName) {
    return exeBaseName.find("DollsNest") != std::string::npos;
}

void run_selftest();
void run_hook_selftest();

bool g_runHookSelftestAtStartup = false;

#if defined(DSH_HAVE_MINHOOK)
namespace {

using ProcessEventFn = void(__fastcall*)(void*, void*, void*);

ProcessEventFn g_originalProcessEvent = nullptr;
std::uint32_t  g_ownWorkerThread = 0;
std::atomic<std::uint64_t> g_gameThreadDrains{0};

enum DamageWatchKind : std::uint8_t {
    kDmgNone = 0,
    kDmgPoint = 1,
    kDmgAny = 2,
    kDmgRadial = 3,
    kDmgRadialFalloff = 4,
};

constexpr std::uint32_t kDamageWatchFnCount = 5;
constexpr std::uint32_t kDamageRingSlots = 64;
constexpr std::uint32_t kDamageRingMask = kDamageRingSlots - 1;

struct DamageWatchRecord {
    std::uint8_t   kind = kDmgNone;
    std::uintptr_t damagedActor = 0;
    std::uintptr_t instigator = 0;
    std::uintptr_t causer = 0;

    std::uintptr_t causerOwner = 0;
    std::uintptr_t causerInstigator = 0;
    float          params[5] = {0, 0, 0, 0, 0};
    std::uint64_t  ms = 0;
};

DamageWatchRecord           g_damageRing[kDamageRingSlots];
std::atomic<std::uint32_t>  g_damageRingWrite{0};
std::atomic<std::uint32_t>  g_damageRingRead{0};
std::atomic<std::uint64_t>  g_damageWatchSeen{0};
std::atomic<std::uint64_t>  g_damageWatchDropped{0};
std::atomic<bool>           g_damageWatchArmed{false};
std::atomic<std::uintptr_t> g_damageFn[kDamageWatchFnCount];

void damage_watch_push(const DamageWatchRecord& r) {
    const std::uint32_t w = g_damageRingWrite.load(std::memory_order_relaxed);
    const std::uint32_t next = (w + 1) & kDamageRingMask;
    if (next == g_damageRingRead.load(std::memory_order_acquire)) {
        g_damageWatchDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_damageRing[w] = r;
    g_damageRingWrite.store(next, std::memory_order_release);
}

void damage_watch_observe(void* function, void* params) {

    const bool armed = g_damageWatchArmed.load(std::memory_order_relaxed);
    const auto fn = reinterpret_cast<std::uintptr_t>(function);
    if (fn == 0 || params == nullptr) return;

    std::uint8_t kind = kDmgNone;
    for (std::uint32_t i = 1; i < kDamageWatchFnCount; ++i) {
        const std::uintptr_t want = g_damageFn[i].load(std::memory_order_relaxed);
        if (want != 0 && want == fn) {
            kind = static_cast<std::uint8_t>(i);
            break;
        }
    }
    if (kind == kDmgNone) return;

    DamageWatchRecord r;
    r.kind = kind;
    r.ms = GetTickCount64();
    const auto* frame = static_cast<const unsigned char*>(params);

    std::memcpy(&r.damagedActor, frame + 0x00, sizeof(std::uintptr_t));
    if (kind == kDmgRadial || kind == kDmgRadialFalloff) {

        std::memcpy(r.params, frame + 0x08, sizeof(r.params));

        std::memcpy(&r.causer, frame + (kind == kDmgRadial ? 0x50 : 0x68), sizeof(std::uintptr_t));
        if (r.causer >= 0x10000) {
            std::uint64_t ownerField = 0;
            std::memcpy(&ownerField, reinterpret_cast<const void*>(r.causer + 0x00E0), sizeof(ownerField));
            r.causerOwner = static_cast<std::uintptr_t>(ownerField);
        }
    } else {
        std::memcpy(r.params, frame + 0x08, sizeof(r.params));
        if (kind == kDmgPoint) {
            std::memcpy(&r.instigator, frame + 0xB0, sizeof(std::uintptr_t));
            std::memcpy(&r.causer, frame + 0xB8, sizeof(std::uintptr_t));
        } else {
            std::memcpy(&r.instigator, frame + 0x20, sizeof(std::uintptr_t));
            std::memcpy(&r.causer, frame + 0x28, sizeof(std::uintptr_t));
        }

        if (r.causer >= 0x10000) {
            std::uint64_t ownerField = 0;
            std::memcpy(&ownerField, reinterpret_cast<const void*>(r.causer + 0x00E0), sizeof(ownerField));
            r.causerOwner = static_cast<std::uintptr_t>(ownerField);
            std::uint64_t instField = 0;
            std::memcpy(&instField, reinterpret_cast<const void*>(r.causer + 0x0118), sizeof(instField));
            r.causerInstigator = static_cast<std::uintptr_t>(instField);
        }
    }
    g_damageWatchSeen.fetch_add(1, std::memory_order_relaxed);
    if (armed) damage_watch_push(r);

    if (dsh::game::damage_is_from_puppet(r.causerOwner)) {
        for (std::size_t i = 0; i < sizeof(r.params) / sizeof(r.params[0]); ++i) {
            const float zero = 0.0f;
            std::memcpy(const_cast<unsigned char*>(frame) + 0x08 + i * sizeof(float), &zero, sizeof(zero));
        }
    }

    if (kind == kDmgPoint || kind == kDmgAny) {
        dsh::game::note_damage_event(r.damagedActor, r.params, r.ms);
    }
    return;
}

std::atomic<std::uintptr_t> g_actionFn{0};
std::atomic<bool>           g_actionResolved{false};
std::atomic<std::uint64_t>  g_actionSeen{0};

void action_watch_observe(void* function, void* params) {
    if (function == nullptr || params == nullptr) return;
    if (!g_actionResolved.load(std::memory_order_relaxed)) {

        const std::uintptr_t cls = dsh::reflect::resolve_uclass_address("DACharacter");
        std::uintptr_t found = 0;
        if (cls != 0) {
            const dsh::reflect::FunctionRef ref =
                dsh::reflect::reflect_resolve_function(cls, "TryActivateCharacterActionAbility");
            if (ref.usable) found = ref.function;
        }
        g_actionFn.store(found, std::memory_order_relaxed);
        g_actionResolved.store(true);
        DSH_INFO("action: the action watch resolved '%s' on DACharacter -- %s",
                 "TryActivateCharacterActionAbility",
                 found == 0 ? "NOT FOUND (nothing will be reported: the name is not on that class in this "
                              "build, and the next round must find which class owns it)"
                            : "watching for it");
    }
    const std::uintptr_t want = g_actionFn.load(std::memory_order_relaxed);
    if (want == 0 || reinterpret_cast<std::uintptr_t>(function) != want) return;

    const auto* frame = static_cast<const unsigned char*>(params);
    std::uintptr_t object = 0;
    std::uint32_t  keyIndex = 0;
    std::uint32_t  keyNumber = 0;
    std::uint8_t   allowRemote = 0;
    std::memcpy(&object, frame + 0x00, sizeof(object));
    std::memcpy(&keyIndex, frame + 0x08, sizeof(keyIndex));
    std::memcpy(&keyNumber, frame + 0x0C, sizeof(keyNumber));
    std::memcpy(&allowRemote, frame + 0x10, sizeof(allowRemote));

    const std::uint64_t seen = g_actionSeen.fetch_add(1, std::memory_order_relaxed) + 1;
    if (seen > 40 && (seen % 50) != 0) return;
    std::string keyName;
    std::string keyNote;
    if (!dsh::reflect::resolve_fname(keyIndex, keyName, keyNote)) {
        keyName = "<unresolved: " + keyNote + ">";
    }
    DSH_INFO("action: TryActivateCharacterActionAbility(object=%s [%s], key='%s' (index %u, number %u), "
             "bAllowRemoteActivation=%u) -- call %llu",
             object >= 0x10000 ? dsh::reflect::object_own_name(object).c_str() : "<null>",
             object >= 0x10000 ? dsh::reflect::object_class_name(object).c_str() : "-",
             keyName.c_str(), keyIndex, keyNumber, static_cast<unsigned>(allowRemote),
             static_cast<unsigned long long>(seen));
}

std::unordered_map<std::uintptr_t, bool> g_sweepSeen;
std::uint64_t                            g_sweepLines = 0;

std::unordered_map<std::uintptr_t, std::pair<std::int32_t, bool>> g_actionEdgeKind;

std::uint64_t g_actionEdgeLines = 0;

std::uint64_t g_actionShotLines = 0;

std::uint64_t g_supplyEdgeLines = 0;

std::unordered_map<std::uintptr_t, bool> g_itemUseFn;

std::atomic<bool>          g_shotTracing{false};
std::atomic<std::uint64_t> g_shotTraceUntilMs{0};
std::atomic<std::int32_t>  g_shotTraceBudget{160};
thread_local std::unordered_set<std::uintptr_t> g_shotTraceSeen;
std::atomic<std::uint32_t> g_shotTraceShots{0};
void shot_trace_arm() {
    const std::uint32_t shot = g_shotTraceShots.fetch_add(1, std::memory_order_relaxed) + 1;
    if (shot > 3) return;
    g_shotTraceUntilMs.store(GetTickCount64() + 600, std::memory_order_relaxed);
    g_shotTracing.store(true, std::memory_order_relaxed);
    DSH_INFO("shot-trace: ARMED for 600 ms (shot %u of at most 3); every UFunction called for the first "
             "time in that window will be named",
             shot);
}

void shot_trace_observe(void* function, void* self) {

    if (!g_shotTracing.load(std::memory_order_relaxed)) {
        const std::uint64_t requested = dsh::game::process_event_trace_until_ms();
        const std::uint64_t nowMs = GetTickCount64();
        if (requested > nowMs) {
            g_shotTracing.store(true, std::memory_order_relaxed);
            g_shotTraceUntilMs.store(requested, std::memory_order_relaxed);
            g_shotTraceBudget.store(400, std::memory_order_relaxed);
            DSH_INFO("shot-trace: ARMED by a projectile until %llu ms (a window of %llu ms) -- every UFunction "
                     "called for the first time in it will be named",
                     static_cast<unsigned long long>(requested),
                     static_cast<unsigned long long>(requested - nowMs));
        } else {
            return;
        }
    }
    if (GetTickCount64() > g_shotTraceUntilMs.load(std::memory_order_relaxed)) {
        g_shotTracing.store(false, std::memory_order_relaxed);
        DSH_INFO("shot-trace: the window closed");
        return;
    }

    {
        static thread_local bool s_threadProbe = false;
        if (!s_threadProbe) {
            s_threadProbe = true;
            DSH_INFO("thread-probe: shot_trace_observe (armed) from thread %u",
                     static_cast<unsigned>(dsh::reflect::current_thread_id()));
        }
    }
    const auto fn = reinterpret_cast<std::uintptr_t>(function);
    if (fn == 0) return;
    if (!g_shotTraceSeen.insert(fn).second) return;
    if (g_shotTraceBudget.fetch_sub(1, std::memory_order_relaxed) - 1 < 0) {
        g_shotTracing.store(false, std::memory_order_relaxed);
        DSH_WARN("shot-trace: the 160-line budget is spent; no further calls will be traced this session");
        return;
    }

    std::uintptr_t outer = 0;
    std::memcpy(&outer, reinterpret_cast<const void*>(fn + 0x0020), sizeof(outer));
    const auto obj = reinterpret_cast<std::uintptr_t>(self);
    DSH_INFO("shot-trace: '%s' [declared by '%s'] called on '%s' [%s]",
             dsh::reflect::object_own_name(fn).c_str(),
             outer >= 0x10000 ? dsh::reflect::object_own_name(outer).c_str() : "<null>",
             obj >= 0x10000 ? dsh::reflect::object_own_name(obj).c_str() : "<null>",
             obj >= 0x10000 ? dsh::reflect::object_class_name(obj).c_str() : "-");
}

std::recursive_mutex g_sweepMutex;

void action_name_sweep_locked(void* function, void* self, void* params) {
    if (function == nullptr) return;
    const auto fn = reinterpret_cast<std::uintptr_t>(function);

    if (fn == dsh::game::ammo_params_function()) {
        if (params != nullptr && dsh::game::ammo_capture_enabled()) {
            const auto* frame = static_cast<const unsigned char*>(params);
            dsh::game::AmmoSpawnSample sample;
            std::uint64_t classRaw = 0;
            const auto actor = reinterpret_cast<std::uintptr_t>(self);
            sample.actor = actor >= 0x10000 ? actor : 0;

            std::uint64_t owner = 0;
            std::uint64_t instigator = 0;
            const bool ownerRead =
                sample.actor >= 0x10000 &&
                dsh::reflect::read_process_bytes(sample.actor + 0x00E0, &owner, sizeof(owner));
            const bool instigatorRead =
                sample.actor >= 0x10000 &&
                dsh::reflect::read_process_bytes(sample.actor + 0x0118, &instigator, sizeof(instigator));
            const dsh::reflect::LiveObjects live = dsh::reflect::find_live_objects();
            std::uint64_t instigatorController = 0;
            if (instigator >= 0x10000) {
                dsh::reflect::read_process_bytes(static_cast<std::uintptr_t>(instigator) + 0x0258,
                                                 &instigatorController, sizeof(instigatorController));
            }
            const bool mine =
                (live.pawn >= 0x10000 && ((ownerRead && owner == live.pawn) ||
                                          (instigatorRead && instigator == live.pawn))) ||
                (live.playerController >= 0x10000 && instigatorController == live.playerController);
            if (!mine) {

                dsh::game::note_ammo_foreign();
                static std::atomic<int> s_foreignLogged{0};
                if (s_foreignLogged.fetch_add(1) < 8) {
                    DSH_INFO("ammo: NOT CAPTURED -- this projectile is not this machine's player's. "
                             "puppet/pawn=0x%llX playerController=0x%llX | projectile Owner=0x%llX "
                             "(%s) Instigator=0x%llX (%s, controller %s) | class '%s'",
                             static_cast<unsigned long long>(live.pawn),
                             static_cast<unsigned long long>(live.playerController),
                             static_cast<unsigned long long>(owner),
                             owner >= 0x10000
                                 ? dsh::reflect::object_class_name(
                                       static_cast<std::uintptr_t>(owner)).c_str()
                                 : "-",
                             static_cast<unsigned long long>(instigator),
                             instigator >= 0x10000
                                 ? dsh::reflect::object_class_name(
                                       static_cast<std::uintptr_t>(instigator)).c_str()
                                 : "-",
                             instigatorController >= 0x10000
                                 ? dsh::reflect::object_class_name(
                                       static_cast<std::uintptr_t>(instigatorController)).c_str()
                                 : "-",
                             sample.actor >= 0x10000
                                 ? dsh::reflect::object_class_name(sample.actor).c_str()
                                 : "-");
                }
                return;
            }
            if (sample.actor >= 0x10000 &&
                dsh::reflect::read_process_bytes(sample.actor + 0x0010, &classRaw, sizeof(classRaw))) {
                sample.actorClass = static_cast<std::uintptr_t>(classRaw);
            }
            std::memcpy(sample.params, frame + 0x000, sizeof(sample.params));
            std::memcpy(&sample.speedCurve, sample.params + 0x18, sizeof(sample.speedCurve));
            std::memcpy(&sample.pointDamageCurve, sample.params + 0x48, sizeof(sample.pointDamageCurve));
            std::memcpy(&sample.damageType, sample.params + 0xA8, sizeof(sample.damageType));
            sample.ms = GetTickCount64();
            dsh::game::note_ammo_spawn(sample);
        }
        return;
    }

    if (dsh::game::action_replay_in_flight()) return;

    {
        const auto itemIt = g_itemUseFn.find(fn);
        if (itemIt != g_itemUseFn.end()) {
            if (itemIt->second) {
                unsigned long long itemClass = 0;
                if (params != nullptr) std::memcpy(&itemClass, params, sizeof(itemClass));
                dsh::game::note_item_use(static_cast<std::uintptr_t>(itemClass), GetTickCount64());
            }
            return;
        }
    }
    const auto kindIt = g_actionEdgeKind.find(fn);
    if (kindIt != g_actionEdgeKind.end()) {
        const std::int32_t actionId = kindIt->second.first;
        const bool pressed = kindIt->second.second;
        const auto selfForKnown = reinterpret_cast<std::uintptr_t>(self);

        std::int32_t module = 0;
        if (actionId == static_cast<std::int32_t>(dsh::game::kShotActionBase)) {
            const int slot = dsh::game::local_weapon_module_slot(selfForKnown);
            if (slot < 0) return;
            module = slot;
            if (++g_actionShotLines <= 40) {
                DSH_INFO("shot: %s weapon fired -- '%s' [%s], action id %d, weapon %d, one per projectile",
                         slot == 0 ? "FRONT" : (slot == 1 ? "LEFT" : "RIGHT"),
                         selfForKnown >= 0x10000 ? dsh::reflect::object_own_name(selfForKnown).c_str()
                                                 : "<null>",
                         selfForKnown >= 0x10000 ? dsh::reflect::object_class_name(selfForKnown).c_str()
                                                 : "-",
                         actionId, module);
            }
            shot_trace_arm();
        } else if (actionId == static_cast<std::int32_t>(dsh::game::kSupplyActionBase)) {

            const int slot = dsh::game::local_weapon_module_slot(selfForKnown);
            if (slot < 0) return;
            module = slot;
            if (++g_supplyEdgeLines <= 40) {
                DSH_INFO("supply: slot %d %s -- the game's own '%s', %llu supply edge(s) this session", slot,
                         pressed ? "STARTED" : "ENDED",
                         pressed ? "ServerOnSupplyStarted" : "ServerOnSupplyEnded",
                         static_cast<unsigned long long>(g_supplyEdgeLines));
            }
        } else if (++g_actionEdgeLines <= 200) {
            DSH_INFO("edge: action=%d pressed=%d on '%s' [%s] -- edge %llu", actionId,
                     pressed ? 1 : 0,
                     selfForKnown >= 0x10000 ? dsh::reflect::object_own_name(selfForKnown).c_str()
                                             : "<null>",
                     selfForKnown >= 0x10000 ? dsh::reflect::object_class_name(selfForKnown).c_str()
                                             : "-",
                     static_cast<unsigned long long>(g_actionEdgeLines));
        }
        if (actionId >= 0) {
            dsh::game::note_action_edge(static_cast<std::uint32_t>(actionId),
                                        static_cast<std::uint32_t>(module), pressed, GetTickCount64());
        }
        return;
    }
    if (g_sweepSeen.find(fn) != g_sweepSeen.end()) return;

    {
        const std::string shotName = dsh::reflect::object_own_name(fn);
        if (shotName == "OnShot" || shotName == "ServerOnShot") {
            const auto shotSelf = reinterpret_cast<std::uintptr_t>(self);
            const int  slot = dsh::game::local_weapon_module_slot(shotSelf);
            if (slot < 0) {
                g_actionEdgeKind[fn] = std::make_pair(static_cast<std::int32_t>(-1), true);
                DSH_INFO("shot: '%s' on '%s' [%s] is NOT a weapon module of this side, so it is not a shot "
                         "and will never be replayed",
                         shotName.c_str(),
                         shotSelf >= 0x10000 ? dsh::reflect::object_own_name(shotSelf).c_str() : "<null>",
                         shotSelf >= 0x10000 ? dsh::reflect::object_class_name(shotSelf).c_str() : "-");
                return;
            }
            if (shotName == "OnShot") {
                g_actionEdgeKind[fn] = std::make_pair(static_cast<std::int32_t>(-1), true);
                DSH_INFO("shot: '%s' on '%s' is the notification the C++ raises once the shot has happened, "
                         "and '%s' is the one the game calls through script -- counting this one too would "
                         "count every shot twice",
                         shotName.c_str(),
                         shotSelf >= 0x10000 ? dsh::reflect::object_own_name(shotSelf).c_str() : "<null>",
                         "ServerOnShot");
                return;
            }

            const auto shotId = static_cast<std::int32_t>(dsh::game::kShotActionBase);
            g_actionEdgeKind[fn] = std::make_pair(shotId, true);
            if (++g_actionShotLines <= 40) {
                DSH_INFO("shot: '%s' on '%s' [%s] -- this is THE SHOT EVENT, %s weapon, counting one per "
                         "call as action id %d",
                         shotName.c_str(),
                         shotSelf >= 0x10000 ? dsh::reflect::object_own_name(shotSelf).c_str() : "<null>",
                         shotSelf >= 0x10000 ? dsh::reflect::object_class_name(shotSelf).c_str() : "-",
                         slot == 0 ? "FRONT" : (slot == 1 ? "LEFT" : "RIGHT"), shotId);
            }
            dsh::game::note_action_edge(static_cast<std::uint32_t>(shotId), static_cast<std::uint32_t>(slot),
                                        true, GetTickCount64());

            shot_trace_arm();
            return;
        }
    }

    const bool bounded = g_sweepSeen.size() >= 8192;
    g_sweepSeen[fn] = true;
    if (bounded) return;

    const std::string name = dsh::reflect::object_own_name(fn);

    const auto selfForEdge = reinterpret_cast<std::uintptr_t>(self);

    const bool itemUsedEvent = name.find("ItemUsed") != std::string::npos;
    if (itemUsedEvent) {
        const dsh::reflect::LiveObjects live = dsh::reflect::find_live_objects();
        std::uint64_t widgetController = 0;
        dsh::reflect::read_process_bytes(selfForEdge + 0x02B0, &widgetController, sizeof(widgetController));
        const auto owner = static_cast<std::uintptr_t>(widgetController);
        const bool mine = live.playerController >= 0x10000 && owner == live.playerController;
        unsigned long long itemClass = 0;
        if (params != nullptr) std::memcpy(&itemClass, params, sizeof(itemClass));
        g_itemUseFn[fn] = mine;
        if (!mine) {

            DSH_INFO("item: '%s' on '%s' [%s] is NOT this machine's quick-slot widget (its is 0x%llX) "
                     "-- not captured",
                     name.c_str(),
                     selfForEdge >= 0x10000 ? dsh::reflect::object_own_name(selfForEdge).c_str() : "<null>",
                     selfForEdge >= 0x10000 ? dsh::reflect::object_class_name(selfForEdge).c_str() : "-",
                     static_cast<unsigned long long>(owner));
            return;
        }
        DSH_INFO("item: '%s' on '%s' [%s] -- this is the START of a quick-slot use (the item is 0x%llX '%s'); "
                 "the puppet now starts when this machine does, not when the item is consumed",
                 name.c_str(),
                 selfForEdge >= 0x10000 ? dsh::reflect::object_own_name(selfForEdge).c_str() : "<null>",
                 selfForEdge >= 0x10000 ? dsh::reflect::object_class_name(selfForEdge).c_str() : "-", itemClass,
                 itemClass >= 0x10000
                     ? dsh::reflect::object_own_name(static_cast<std::uintptr_t>(itemClass)).c_str()
                     : "<null>");
        dsh::game::note_item_use(static_cast<std::uintptr_t>(itemClass), GetTickCount64());
        return;
    }
    if (name == "HandleItemConsumed") {
        const dsh::reflect::LiveObjects live = dsh::reflect::find_live_objects();
        const bool mine = live.playerController >= 0x10000 && selfForEdge == live.playerController;

        g_itemUseFn[fn] = false;
        if (!mine) {
            dsh::game::note_item_use_foreign();
            DSH_INFO("item: 'HandleItemConsumed' on '%s' [%s] is NOT this machine's player controller "
                     "(0x%llX '%s') -- somebody else's consumption is never sent",
                     selfForEdge >= 0x10000 ? dsh::reflect::object_own_name(selfForEdge).c_str() : "<null>",
                     selfForEdge >= 0x10000 ? dsh::reflect::object_class_name(selfForEdge).c_str() : "-",
                     static_cast<unsigned long long>(live.playerController),
                     live.playerController >= 0x10000
                         ? dsh::reflect::object_class_name(live.playerController).c_str()
                         : "<none>");
            return;
        }
        unsigned long long itemClass = 0;
        if (params != nullptr) std::memcpy(&itemClass, params, sizeof(itemClass));
        DSH_INFO("item: 'HandleItemConsumed' on this machine's player controller -- the item is 0x%llX '%s' "
                 "[%s]. NOT captured: this is the END of the action (592-1 measured 2601 ms after its start), "
                 "and the start is what crosses the wire",
                 itemClass,
                 itemClass >= 0x10000
                     ? dsh::reflect::object_own_name(static_cast<std::uintptr_t>(itemClass)).c_str()
                     : "<null>",
                 itemClass >= 0x10000
                     ? dsh::reflect::object_class_name(static_cast<std::uintptr_t>(itemClass)).c_str()
                     : "-");
        return;
    }
    if (name.rfind("OnPressedAction", 0) == 0 || name.rfind("OnReleasedAction", 0) == 0) {

        const bool pressed = name.rfind("OnPressedAction", 0) == 0;
        int actionId = -1;
        if (name.find("Front") != std::string::npos) actionId = 0;
        else if (name.find("Left") != std::string::npos) actionId = 1;
        else if (name.find("Right") != std::string::npos) actionId = 2;
        else if (name.find("Utility") != std::string::npos) actionId = 3;
        if (++g_actionEdgeLines <= 80) {
            DSH_INFO("edge: %s on '%s' [%s] -- action=%d pressed=%d edge %llu", name.c_str(),
                     selfForEdge >= 0x10000 ? dsh::reflect::object_own_name(selfForEdge).c_str() : "<null>",
                     selfForEdge >= 0x10000 ? dsh::reflect::object_class_name(selfForEdge).c_str() : "-",
                     actionId, pressed ? 1 : 0,
                     static_cast<unsigned long long>(g_actionEdgeLines));
        }
        if (actionId >= 0) {

            dsh::game::note_action_edge(static_cast<std::uint32_t>(actionId), 0u, pressed, GetTickCount64());

            g_actionEdgeKind[fn] = std::make_pair(static_cast<std::int32_t>(actionId), pressed);
        }
    }

    if (name == "ServerOnSupplyStarted" || name == "ServerOnSupplyEnded") {
        const auto supplySelf = reinterpret_cast<std::uintptr_t>(self);
        const int slot = dsh::game::local_weapon_module_slot(supplySelf);
        const bool started = (name == "ServerOnSupplyStarted");
        if (slot < 0) {

            g_actionEdgeKind[fn] = std::make_pair(static_cast<std::int32_t>(-1), started);
            DSH_INFO("supply: '%s' on '%s' [%s] is NOT one of this side's weapon modules, so it is not this "
                     "machine's reload and will never be replayed",
                     name.c_str(),
                     supplySelf >= 0x10000 ? dsh::reflect::object_own_name(supplySelf).c_str() : "<null>",
                     supplySelf >= 0x10000 ? dsh::reflect::object_class_name(supplySelf).c_str() : "-");
            return;
        }
        g_actionEdgeKind[fn] = std::make_pair(static_cast<std::int32_t>(dsh::game::kSupplyActionBase), started);
        DSH_INFO("supply: '%s' on '%s' [%s] -- slot %d, action id %u, %s -- this is a RELOAD and it travels "
                 "as action kind %u",
                 name.c_str(),
                 supplySelf >= 0x10000 ? dsh::reflect::object_own_name(supplySelf).c_str() : "<null>",
                 supplySelf >= 0x10000 ? dsh::reflect::object_class_name(supplySelf).c_str() : "-", slot,
                 dsh::game::kSupplyActionBase, started ? "started" : "ended", dsh::game::kSupplyActionBase);
        dsh::game::note_action_edge(dsh::game::kSupplyActionBase, static_cast<std::uint32_t>(slot), started,
                                    GetTickCount64());
        return;
    }
    static const char* const kKeywords[] = {"Fire",  "Shot",   "Shoot",  "Attack", "Action",
                                            "Trigger", "Weapon", "Aim",   "Reload", "Melee"};
    bool interesting = false;
    for (const char* keyword : kKeywords) {
        if (name.find(keyword) != std::string::npos) {
            interesting = true;
            break;
        }
    }

    const bool itemVerb = name == "UseNormalItem" || name == "Use" || name == "Consume" ||
                          name == "ReceiveOnUsingNormalItem" || name == "HandleItemConsumed" ||
                          name == "HandleItemUsed";

    const bool supplyVerb = name.find("Supply") != std::string::npos;
    if (itemVerb || supplyVerb) interesting = true;
    if (!interesting) return;
    if (++g_sweepLines > 200) return;
    DSH_INFO("sweep: UFunction '%s' was called for the first time in this process%s%s", name.c_str(),
             itemVerb ? " (AN ITEM VERB -- see PENDING 5.7)" : "",
             supplyVerb ? " (A SUPPLY VERB -- this game's RELOAD)" : "");

    if (itemVerb) {
        unsigned long long classWord = 0;
        if (params != nullptr) std::memcpy(&classWord, params, sizeof(classWord));
        DSH_INFO("sweep: ...item class 0x%llX '%s' [%s]", classWord,
                 classWord >= 0x10000
                     ? dsh::reflect::object_own_name(static_cast<std::uintptr_t>(classWord)).c_str()
                     : "<null>",
                 classWord >= 0x10000
                     ? dsh::reflect::object_class_name(static_cast<std::uintptr_t>(classWord)).c_str()
                     : "-");
    }

    if (supplyVerb) {
        DSH_INFO("sweep: ...the supply call landed on weapon slot %d (-1 = not one of this side's)",
                 dsh::game::local_weapon_module_slot(selfForEdge));
    }

    const bool actionVerb = name.rfind("OnPressedAction", 0) == 0 ||
                            name.rfind("OnReleasedAction", 0) == 0 ||
                            name == "ActivateCharacterAction" || name == "DeactivateCharacterAction" ||
                            name == "CanActivateCharacterAction" ||

                            name.find("Fire") != std::string::npos ||
                            name.find("Shot") != std::string::npos ||
                            name.find("Trigger") != std::string::npos ||

                            itemVerb ||

                            supplyVerb;
    const auto selfAddr = reinterpret_cast<std::uintptr_t>(self);
    if (actionVerb && selfAddr >= 0x10000) {
        std::uintptr_t outer = 0;
        std::uintptr_t owner = 0;
        std::memcpy(&outer, reinterpret_cast<const void*>(selfAddr + 0x0020), sizeof(outer));
        std::memcpy(&owner, reinterpret_cast<const void*>(selfAddr + 0x00E0), sizeof(owner));
        DSH_INFO("sweep: ...called on '%s' [%s]; its Outer is %s [%s]; its Owner is %s [%s]",
                 dsh::reflect::object_own_name(selfAddr).c_str(),
                 dsh::reflect::object_class_name(selfAddr).c_str(),
                 outer >= 0x10000 ? dsh::reflect::object_own_name(outer).c_str() : "<null>",
                 outer >= 0x10000 ? dsh::reflect::object_class_name(outer).c_str() : "-",
                 owner >= 0x10000 ? dsh::reflect::object_own_name(owner).c_str() : "<null>",
                 owner >= 0x10000 ? dsh::reflect::object_class_name(owner).c_str() : "-");
    }
}

void action_name_sweep(void* function, void* self, void* params) {
    const std::lock_guard<std::recursive_mutex> lock(g_sweepMutex);
    action_name_sweep_locked(function, self, params);
}

void death_watch_observe(void* function, void* self) {
    const auto fn = reinterpret_cast<std::uintptr_t>(function);
    if (fn < 0x10000) return;
    const std::uintptr_t multicast = dsh::game::death_multicast_function();
    const std::uintptr_t receive = dsh::game::death_receive_function();
    if (multicast == 0 && receive == 0) return;
    const auto actor = reinterpret_cast<std::uintptr_t>(self);
    if (actor < 0x10000) return;
    if (fn == multicast) {
        dsh::game::note_death(actor, GetTickCount64(), true);
    } else if (fn == receive) {
        dsh::game::note_death(actor, GetTickCount64(), false);
    }
}

void checkpoint_watch_observe(void* function, void* params) {
    const auto fn = reinterpret_cast<std::uintptr_t>(function);
    if (fn < 0x10000) return;
    const std::uintptr_t dialogue = dsh::game::checkpoint_dialogue_function();
    const std::uintptr_t regression = dsh::game::checkpoint_regression_function();
    const std::uintptr_t save = dsh::game::checkpoint_save_function();
    if (dialogue == 0 && regression == 0 && save == 0) return;
    if (fn == dialogue) {
        dsh::game::note_checkpoint(1);
        return;
    }
    if (fn == regression) {
        dsh::game::note_checkpoint(2);
        return;
    }
    if (fn == save) {
        bool fromRegressionPoint = false;
        if (params != nullptr) std::memcpy(&fromRegressionPoint, params, sizeof(fromRegressionPoint));
        if (fromRegressionPoint) dsh::game::note_checkpoint(3);
    }
}

void __fastcall GameThreadProcessEventDetour(void* self, void* function, void* params) {

    dsh::reflect::game_thread_note_process_event();

    damage_watch_observe(function, params);

    shot_trace_observe(function, self);

    action_watch_observe(function, params);
    action_name_sweep(function, self, params);

    death_watch_observe(function, self);

    checkpoint_watch_observe(function, params);
    if (g_originalProcessEvent != nullptr) {
        g_originalProcessEvent(self, function, params);
    }

    if (dsh::reflect::current_thread_id() != g_ownWorkerThread) {
        if (dsh::reflect::game_thread_pending() != 0) {
            const std::uint32_t ran = dsh::reflect::game_thread_drain();
            g_gameThreadDrains.fetch_add(ran, std::memory_order_relaxed);
        }
    }
}

bool InstallGameThreadHook(std::uintptr_t processEventAddress) {
    if (processEventAddress == 0) return false;

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
        DSH_WARN("game-thread hook: MH_Initialize failed (%s)", MH_StatusToString(initStatus));
        return false;
    }

    g_ownWorkerThread = dsh::reflect::current_thread_id();

    MH_STATUS status = MH_CreateHook(reinterpret_cast<void*>(processEventAddress),
                                     reinterpret_cast<void*>(&GameThreadProcessEventDetour),
                                     reinterpret_cast<void**>(&g_originalProcessEvent));
    if (status != MH_OK) {
        DSH_WARN("game-thread hook: MH_CreateHook failed (%s)", MH_StatusToString(status));
        return false;
    }
    status = MH_EnableHook(reinterpret_cast<void*>(processEventAddress));
    if (status != MH_OK) {
        DSH_WARN("game-thread hook: MH_EnableHook failed (%s)", MH_StatusToString(status));
        return false;
    }
    DSH_INFO("game-thread hook: installed on ProcessEvent 0x%llX, worker thread %u",
             static_cast<unsigned long long>(processEventAddress), g_ownWorkerThread);
    return true;
}

const char* damage_watch_kind_name(std::uint8_t kind) {
    switch (kind) {
        case kDmgPoint:         return "ApplyDAPointDamage";
        case kDmgAny:           return "ApplyDADamage";
        case kDmgRadial:        return "ApplyDARadialDamage";
        case kDmgRadialFalloff: return "ApplyDARadialDamageWithFalloff";
        default:                return "?";
    }
}

std::string damage_watch_actor(const std::uintptr_t actor) {
    if (actor < 0x10000) return "NONE";
    const std::string cls = dsh::reflect::object_class_name(actor);
    return (cls.empty() ? std::string("<unnamed class>") : cls) + " '" +
           dsh::reflect::object_own_name(actor) + "'";
}

struct DamageWatchBucket {
    std::uint8_t  kind = kDmgNone;
    std::string   damagedClass;
    std::string   causerClass;
    std::uint64_t count = 0;
};
std::vector<DamageWatchBucket> g_damageBuckets;
constexpr std::size_t kMaxDamageBuckets = 12;

void damage_watch_count(const DamageWatchRecord& r) {
    const std::string damaged = r.damagedActor >= 0x10000
                                    ? dsh::reflect::object_class_name(r.damagedActor)
                                    : std::string("<none>");
    const std::string causer = r.causer >= 0x10000 ? dsh::reflect::object_class_name(r.causer)
                                                   : std::string("<none>");
    for (DamageWatchBucket& b : g_damageBuckets) {
        if (b.kind == r.kind && b.damagedClass == damaged && b.causerClass == causer) {
            ++b.count;
            return;
        }
    }
    if (g_damageBuckets.size() >= kMaxDamageBuckets) return;
    DamageWatchBucket b;
    b.kind = r.kind;
    b.damagedClass = damaged;
    b.causerClass = causer;
    b.count = 1;
    g_damageBuckets.push_back(std::move(b));
}

void damage_watch_report_buckets(const char* why) {
    if (g_damageBuckets.empty()) {
        DSH_INFO("damage watch: NO call has been counted yet (%s) -- so on THIS machine, in this window, "
                 "nothing applied damage through the three functions the watch knows. That is a fact about "
                 "the window, not about the game.", why);
        return;
    }
    DSH_INFO("damage watch: what has been seen, counted by function and the two classes involved (%s):", why);
    for (const DamageWatchBucket& b : g_damageBuckets) {
        DSH_INFO("damage watch: %s x%llu -- damaged '%s', caused by '%s'", damage_watch_kind_name(b.kind),
                 static_cast<unsigned long long>(b.count), b.damagedClass.c_str(), b.causerClass.c_str());
    }
}

struct HpWatchValue {
    float health = 0.0f;
    float shield = 0.0f;
    float maxHealth = 0.0f;
    float maxShield = 0.0f;
    bool  valid = false;
};
std::unordered_map<std::uintptr_t, HpWatchValue> g_hpWatchLast;
bool    g_hpWatchArmed = false;
bool    g_hpWatchIncludeNpc = false;
std::uint64_t g_hpWatchChanges = 0;
std::uint64_t g_hpWatchTicks = 0;
std::uint64_t g_hpWatchDroppedHealth = 0;
std::uint64_t g_hpWatchDroppedShield = 0;

void hp_watch_tick() {
    if (!g_hpWatchArmed) return;
    ++g_hpWatchTicks;
    static std::uint64_t s_refreshMs = 0;
    const std::uint64_t now = GetTickCount64();
    if (dsh::reflect::hp_watch_targets().empty() || now - s_refreshMs >= 60000) {
        dsh::reflect::hp_watch_refresh_targets(g_hpWatchIncludeNpc);
        s_refreshMs = now;
        if (g_hpWatchLast.empty()) {
            DSH_INFO("hp watch: %zu target(s) cached, sampling from now on -- the FIRST reading of each is "
                     "the baseline, so nothing is reported for the values as they stand",
                     dsh::reflect::hp_watch_targets().size());
        }
    }
    const dsh::reflect::HpWatchFrame frame = dsh::reflect::hp_watch_sample();
    if (!frame.ok) return;
    for (const dsh::reflect::HpWatchSample& s : frame.samples) {
        if (!s.healthRead && !s.shieldRead) continue;
        const auto it = g_hpWatchLast.find(s.target.set);
        if (it == g_hpWatchLast.end()) {
            HpWatchValue v;
            v.health = s.health;
            v.shield = s.shield;
            v.maxHealth = s.maxHealth;
            v.maxShield = s.maxShield;
            v.valid = true;
            g_hpWatchLast[s.target.set] = v;
            continue;
        }
        HpWatchValue& last = it->second;
        if (s.healthRead && s.health != last.health) {
            ++g_hpWatchChanges;
            if (s.health < last.health) ++g_hpWatchDroppedHealth;
            DSH_INFO("hp watch: %llu ms %s%s Health %.3f -> %.3f (delta %+.3f)", now,
                     s.target.isLocal ? "THE LOCAL PLAYER" : (s.target.npcClassified ? "NPC-CLASSIFIED (name rule) " : "enemy "), s.target.name.c_str(),
                     static_cast<double>(last.health), static_cast<double>(s.health),
                     static_cast<double>(s.health - last.health));
            last.health = s.health;
        }
        if (s.shieldRead && s.shield != last.shield) {
            ++g_hpWatchChanges;
            if (s.shield < last.shield) ++g_hpWatchDroppedShield;
            DSH_INFO("hp watch: %llu ms %s%s Shield %.3f -> %.3f (delta %+.3f)", now,
                     s.target.isLocal ? "THE LOCAL PLAYER" : (s.target.npcClassified ? "NPC-CLASSIFIED (name rule) " : "enemy "), s.target.name.c_str(),
                     static_cast<double>(last.shield), static_cast<double>(s.shield),
                     static_cast<double>(s.shield - last.shield));
            last.shield = s.shield;
        }
        last.maxHealth = s.maxHealth;
        last.maxShield = s.maxShield;
    }
}

struct HpPinTarget {
    std::uintptr_t set = 0;
    float          value = 0.0f;
    std::string    name;
};
std::vector<HpPinTarget> g_hpPinTargets;
bool          g_hpPinArmed = false;
std::uint64_t g_hpPinDrift = 0;
std::uint64_t g_hpPinWrites = 0;
std::uint64_t g_hpPinFailures = 0;
void hp_pin_tick() {
    if (!g_hpPinArmed) return;
    const dsh::reflect::HpWatchFrame frame = dsh::reflect::hp_watch_sample();
    if (!frame.ok) return;
    for (const dsh::reflect::HpWatchSample& s : frame.samples) {
        for (const HpPinTarget& p : g_hpPinTargets) {
            if (p.set != s.target.set) continue;
            if (!s.healthRead || s.health == p.value) break;
            ++g_hpPinDrift;
            float back = 0.0f;
            std::string note;
            if (dsh::reflect::hp_write_health(p.set, p.value, &back, note)) {
                ++g_hpPinWrites;
            } else {
                ++g_hpPinFailures;
                if (g_hpPinFailures <= 5) {
                    DSH_WARN("hp pin: could NOT put the value back on %s: %s", p.name.c_str(), note.c_str());
                }
            }
            break;
        }
    }
}

void target_watch_tick() {
    if (!dsh::reflect::target_watch_armed()) return;
    for (int i = 0; i < 4; ++i) {
        const std::uint64_t now = GetTickCount64();
        for (const std::string& one : dsh::reflect::target_watch_sample(now)) {
            DSH_INFO("target: %s", one.c_str());
        }
        if (i < 3) Sleep(50);
    }
}

bool damage_watch_set_armed(bool armed, std::string& note) {
    note.clear();
    if (!armed) {
        g_damageWatchArmed.store(false, std::memory_order_relaxed);

        damage_watch_report_buckets("at the moment the watch was disarmed");
        note = "DISARMED (the counts above are the whole measurement this watch produces)";
        return true;
    }
    const std::uintptr_t cls = dsh::reflect::resolve_uclass_address("DAGameplayStatics");
    if (cls == 0) {
        note = "the DAGameplayStatics UClass is not in the object array, so the watch cannot be armed";
        return false;
    }
    static const char* const kNames[kDamageWatchFnCount] = {"", "ApplyDAPointDamage", "ApplyDADamage",
                                                           "ApplyDARadialDamage",
                                                           "ApplyDARadialDamageWithFalloff"};
    std::string refused;
    for (std::uint32_t i = 1; i < kDamageWatchFnCount; ++i) {
        const dsh::reflect::FunctionRef fn = dsh::reflect::reflect_resolve_function(cls, kNames[i]);
        g_damageFn[i].store(fn.usable ? fn.function : 0, std::memory_order_relaxed);
        if (!fn.usable) refused += std::string(refused.empty() ? "" : "; ") + kNames[i];
    }
    if (g_damageFn[kDmgPoint].load() == 0 && g_damageFn[kDmgAny].load() == 0) {
        note = "neither ApplyDAPointDamage nor ApplyDADamage could be resolved, so there is nothing to "
               "watch: " + refused;
        return false;
    }
    g_damageWatchSeen.store(0, std::memory_order_relaxed);
    g_damageWatchDropped.store(0, std::memory_order_relaxed);
    g_damageRingWrite.store(0, std::memory_order_relaxed);
    g_damageRingRead.store(0, std::memory_order_relaxed);
    g_damageBuckets.clear();
    g_damageWatchArmed.store(true, std::memory_order_relaxed);
    note = "ARMED on '" + std::string(kNames[kDmgPoint]) + "'=0x" +
           std::to_string(static_cast<unsigned long long>(g_damageFn[kDmgPoint].load())) + ", '" +
           kNames[kDmgAny] + "'=0x" +
           std::to_string(static_cast<unsigned long long>(g_damageFn[kDmgAny].load())) + ", '" +
           kNames[kDmgRadial] + "'=0x" +
           std::to_string(static_cast<unsigned long long>(g_damageFn[kDmgRadial].load())) + " (counted, not "
           "decoded), and the engine's own UGameplayStatics entry is NOT hooked (if the game calls that one "
           "instead, this watch will see NOTHING and that is what 'nothing' means here)";
    if (!refused.empty()) note += "; could not be resolved: " + refused;
    return true;
}

void damage_watch_drain() {
    if (!g_damageWatchArmed.load(std::memory_order_relaxed)) return;
    std::uint32_t printed = 0;
    for (;;) {
        const std::uint32_t rd = g_damageRingRead.load(std::memory_order_relaxed);
        if (rd == g_damageRingWrite.load(std::memory_order_acquire)) break;
        const DamageWatchRecord r = g_damageRing[rd];
        g_damageRingRead.store((rd + 1) & kDamageRingMask, std::memory_order_release);
        damage_watch_count(r);
        if (printed >= 20) continue;
        ++printed;
        if (r.kind == kDmgRadial || r.kind == kDmgRadialFalloff) {
            DSH_INFO("damage watch: %s (world context 0x%llX) params=[kinetic %.3f, thermal %.3f, effKinetic "
                     "%.3f, effThermal %.3f, penetration %.3f] -- the RADIAL forms are counted only, their "
                     "frame is a different shape",
                     damage_watch_kind_name(r.kind),
                     static_cast<unsigned long long>(r.damagedActor), static_cast<double>(r.params[0]),
                     static_cast<double>(r.params[1]), static_cast<double>(r.params[2]),
                     static_cast<double>(r.params[3]), static_cast<double>(r.params[4]));
            continue;
        }
        DSH_INFO("damage watch: %s on %s; params=[kinetic %.3f, thermal %.3f, effKinetic %.3f, effThermal "
                 "%.3f, penetration %.3f]; instigator=%s; causer=%s; the causer's OWNER=%s and its "
                 "INSTIGATOR=%s (that pair is what says WHICH MACHINE fired)",
                 damage_watch_kind_name(r.kind), damage_watch_actor(r.damagedActor).c_str(),
                 static_cast<double>(r.params[0]), static_cast<double>(r.params[1]),
                 static_cast<double>(r.params[2]), static_cast<double>(r.params[3]),
                 static_cast<double>(r.params[4]), damage_watch_actor(r.instigator).c_str(),
                 damage_watch_actor(r.causer).c_str(), damage_watch_actor(r.causerOwner).c_str(),
                 damage_watch_actor(r.causerInstigator).c_str());
    }
    static std::uint64_t s_lastReportMs = 0;
    const std::uint64_t now = GetTickCount64();
    if (now - s_lastReportMs >= 10000) {
        s_lastReportMs = now;
        DSH_INFO("damage watch: standing totals -- %llu call(s) seen, %llu dropped by the ring, armed=%s",
                 static_cast<unsigned long long>(g_damageWatchSeen.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_damageWatchDropped.load(std::memory_order_relaxed)),
                 g_damageWatchArmed.load(std::memory_order_relaxed) ? "yes" : "no");
        damage_watch_report_buckets("every ten seconds");
    }
}

}
#else
namespace {
bool damage_watch_set_armed(bool, std::string& note) {
    note = "this build has no MinHook, so the damage watch cannot be armed";
    return false;
}
void damage_watch_drain() {}
}
#endif

bool damage_watch_set_armed_entry(bool armed, std::string& note);
void damage_watch_drain_entry();
bool damage_watch_set_armed_entry(bool armed, std::string& note) {
    return damage_watch_set_armed(armed, note);
}
void damage_watch_drain_entry() { damage_watch_drain(); }

void poll_command_file();

extern std::uint64_t g_sessionCommandOffset;
extern bool          g_sessionCommandOffsetKnown;
void pump_commands_during_session();

dsh::net::BeaconState   g_beacon;
dsh::net::SearchState   g_search;
std::unique_ptr<dsh::net::ITransport> g_discoverySocket;
std::uint64_t           g_beaconUntilMs = 0;
std::uint64_t           g_searchUntilMs = 0;

struct CoopMenu {
    bool                     open = false;
    dsh::reflect::MenuHandle handle;
    std::uint64_t            openedMs = 0;
    std::uint64_t            lastStatusMs = 0;

    bool                     inSessionRows = false;
};
CoopMenu g_coopMenu;

dsh::reflect::MenuHandle g_menuBuilt;
std::atomic<bool>        g_menuBuiltReady{false};

std::atomic<bool>        g_menuBuiltInSessionRows{false};

constexpr std::uint64_t kMenuAbandonMs = 60000;

constexpr std::uint16_t kMenuSessionPort = 47777;

constexpr int           kMenuSessionSeconds = 0;

constexpr std::uint32_t kMenuJoinSearchMs = 6000;

constexpr std::uint64_t kMenuActDelayMs = 2000;

struct PendingAction {
    int           row = -1;
    std::uint64_t dueMs = 0;
    bool          inSessionRows = false;
};
PendingAction g_pendingAction;

bool g_joinPending = false;

std::uint64_t monotonic_ms() {
    return static_cast<std::uint64_t>(GetTickCount64());
}

bool endpoint_to_ip_port(const std::string& endpointBytes, std::string& ip, std::uint16_t& port) {
    if (endpointBytes.size() != 16) return false;
    const unsigned char* b = reinterpret_cast<const unsigned char*>(endpointBytes.data());
    port = static_cast<std::uint16_t>(b[2] | (b[3] << 8));
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[4], b[5], b[6], b[7]);
    ip = buf;
    return true;
}

std::string endpoint_bytes_to_text(const dsh::net::Endpoint& e) {
    return std::string(reinterpret_cast<const char*>(e.bytes.data()), e.bytes.size());
}

bool endpoint_for_ipv4(std::uint32_t ip, std::uint16_t port, dsh::net::Endpoint& out) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(ip);
    out.bytes.resize(sizeof(a));
    std::memcpy(out.bytes.data(), &a, sizeof(a));
    return true;
}

constexpr std::uint32_t kSweepMaxPerSubnet = 512;
constexpr std::uint64_t kSweepAfterMs = 2000;

bool sweep_due(const dsh::net::SearchState& search, std::uint64_t openedMs, std::uint64_t& lastSweepMs,
               std::uint64_t nowMs) {
    if (!search.active) return false;
    if (search.packetsSeen != 0) return false;
    if (nowMs - openedMs < kSweepAfterMs) return false;
    if (lastSweepMs != 0 && nowMs - lastSweepMs < kSweepAfterMs) return false;
    lastSweepMs = nowMs;
    return true;
}

void run_sweep(dsh::net::ITransport& socket, const std::vector<std::uint8_t>& probe,
               std::uint32_t& sweptTotal, std::string& summary) {
    const std::vector<std::uint32_t> addresses =
        dsh::net::unicast_probe_addresses(kSweepMaxPerSubnet);
    unsigned sent = 0;
    for (const std::uint32_t a : addresses) {
        dsh::net::Endpoint to;
        if (!endpoint_for_ipv4(a, dsh::net::kDiscoveryPort, to)) continue;
        if (socket.send(to, probe)) ++sent;
    }
    sweptTotal += sent;
    if (summary.empty()) {
        summary = std::to_string(addresses.size()) + " address(es) in this machine's subnets were asked "
                   "directly, " + std::to_string(sent) + " datagram(s) sent";
        if (addresses.empty()) {
            summary = "there is no subnet small enough to ask every address in (a subnet wider than " +
                      std::to_string(kSweepMaxPerSubnet) +
                      " host addresses is not swept, because sweeping part of one can only miss)";
        }
    }
}

std::uint64_t g_searchOpenedMs = 0;
std::uint64_t g_sweepLastMs = 0;
std::uint32_t g_sweepTotal = 0;
std::string   g_sweepSummary;

std::string peer_file_path() {
    const std::string dir = evidence_dir();
    if (dir.empty()) return {};
    return dir + "\\peers.txt";
}

std::string ipv4_text(std::uint32_t hostOrder) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (hostOrder >> 24) & 0xFFu, (hostOrder >> 16) & 0xFFu,
                  (hostOrder >> 8) & 0xFFu, hostOrder & 0xFFu);
    return buf;
}

std::size_t read_peer_file(std::vector<std::uint32_t>& out, std::string& note) {
    out.clear();
    note.clear();
    const std::string path = peer_file_path();
    if (path.empty()) {
        note = "there is no evidence directory, so there is no peer list";
        return 0;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        note = "no peers.txt at " + path + " (broadcast only)";
        return 0;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    std::string parseNote;
    if (!dsh::net::parse_peer_addresses(text, out, parseNote)) {
        DSH_WARN("discovery: the peer list is REFUSED AS A WHOLE, so NOTHING in it is used -- %s (file %s). "
                 "A parser that dropped only the bad line would produce a machine that cannot be found for "
                 "no stated reason",
                 parseNote.c_str(), path.c_str());
        out.clear();
        note = "the peer list at " + path + " was refused: " + parseNote;
        return 0;
    }
    if (out.empty()) {
        note = "peers.txt holds no address (" + path + ")";
        return 0;
    }
    std::string listed;
    for (std::size_t i = 0; i < out.size() && i < 8; ++i) {
        if (!listed.empty()) listed += ", ";
        listed += ipv4_text(out[i]);
    }
    if (out.size() > 8) listed += ", ...";
    note = std::to_string(out.size()) + " address(es)";
    DSH_INFO("discovery: peer list -- %zu address(es) from %s: %s", out.size(), path.c_str(),
             listed.c_str());
    return out.size();
}

unsigned send_to_listed_peers(dsh::net::ITransport& socket, const std::vector<std::uint8_t>& payload,
                              const std::vector<std::uint32_t>& peers, unsigned& failed) {
    unsigned sent = 0;
    for (const std::uint32_t address : peers) {
        dsh::net::Endpoint to;
        if (!endpoint_for_ipv4(address, dsh::net::kDiscoveryPort, to)) continue;
        if (socket.send(to, payload)) {
            ++sent;
        } else {
            ++failed;
        }
    }
    return sent;
}

std::vector<std::uint32_t> g_peerAddresses;
std::string                g_peerNote;
std::uint64_t              g_peerIgnored = 0;

bool endpoint_ipv4(const dsh::net::Endpoint& e, std::uint32_t& address) {
    if (e.bytes.size() != 16) return false;
    const unsigned char* b = e.bytes.data();
    address = (static_cast<std::uint32_t>(b[4]) << 24) | (static_cast<std::uint32_t>(b[5]) << 16) |
              (static_cast<std::uint32_t>(b[6]) << 8) | static_cast<std::uint32_t>(b[7]);
    return true;
}

std::string endpoint_text(const dsh::net::Endpoint& e) {
    std::string ip;
    std::uint16_t port = 0;
    if (!endpoint_to_ip_port(endpoint_bytes_to_text(e), ip, port)) return "an unreadable endpoint";
    return ip + ":" + std::to_string(static_cast<unsigned>(port));
}

bool endpoint_is_listed(const dsh::net::Endpoint& e, const std::vector<std::uint32_t>& list) {
    if (list.empty()) return true;
    std::uint32_t address = 0;
    if (!endpoint_ipv4(e, address)) return false;
    for (const std::uint32_t listed : list) {
        if (listed == address) return true;
    }
    return false;
}

std::string current_map_key(const char* why) {
    const dsh::reflect::MapIdentity map = dsh::reflect::current_map_identity();
    DSH_INFO("map: %s -- %s", why, map.line().c_str());

    const dsh::game::PawnHandle pawn = dsh::game::locate_local_pawn();
    dsh::sync::PoseSample pose;
    std::string poseNote;
    if (pawn.valid && dsh::game::read_pawn_pose(pawn, pose, poseNote)) {
        DSH_INFO("map: the local pawn stands at (%.0f, %.0f, %.0f) cm", pose.x, pose.y, pose.z);
    } else {
        DSH_INFO("map: the local pawn's position could NOT be read (%s)",
                 pawn.valid ? poseNote.c_str() : pawn.note.c_str());
    }
    return map.key;
}

void begin_search(std::uint32_t windowMs) {
    g_search.start(windowMs, monotonic_ms());
    g_search.mapKey = current_map_key("the join window is opening");
    g_searchOpenedMs = monotonic_ms();
    g_sweepLastMs = 0;
    g_sweepTotal = 0;
    g_sweepSummary.clear();

    read_peer_file(g_peerAddresses, g_peerNote);
    if (!g_peerAddresses.empty()) {
        DSH_INFO("discovery: the %s in the peer list are ALSO probed DIRECTLY, every second -- that is the "
                 "path that does not need a broadcast to cross a virtual LAN",
                 g_peerNote.c_str());
    }
}

void join_first_room();

void discovery_stop(const char* why) {
    if (g_beacon.active) {
        DSH_INFO("discovery: stopping the announcement after %llu sent (%s)",
                 static_cast<unsigned long long>(g_beacon.sent), why);
        g_beacon.stop();
    }
    if (g_search.active) {
        DSH_INFO("discovery: search finished -- %s", g_search.summary().c_str());
        for (const dsh::net::Room& r : g_search.rooms) {
            DSH_INFO("discovery: ROOM '%s' session port %u, heard %u time(s)",
                     r.name.c_str(), static_cast<unsigned>(r.sessionPort), r.announcements);
        }
        g_search.stop();
    }
    g_beaconUntilMs = 0;
    g_searchUntilMs = 0;

    if (g_joinPending) {
        g_joinPending = false;
        join_first_room();
    }
}

dsh::net::ITransport* discovery_socket_listening(std::string& note) {
    note.clear();
    if (!g_discoverySocket && !(g_discoverySocket = dsh::net::make_udp_transport())) {
        note = "could not create a transport";
        return nullptr;
    }

    bool onTheRightPort = false;
    if (g_discoverySocket->is_open()) {
        const dsh::net::Endpoint local = g_discoverySocket->local_endpoint();
        std::string ip;
        std::uint16_t port = 0;
        onTheRightPort = endpoint_to_ip_port(endpoint_bytes_to_text(local), ip, port) &&
                         port == dsh::net::kDiscoveryPort;
        if (!onTheRightPort) {

            DSH_INFO("discovery: the discovery socket was open on port %u (not %u) -- closing it and "
                     "binding the discovery port, because an announcing socket is open on an ephemeral "
                     "port and a search started after one must not inherit it",
                     static_cast<unsigned>(port), static_cast<unsigned>(dsh::net::kDiscoveryPort));
            g_discoverySocket->close();
        }
    }
    if (!onTheRightPort && !g_discoverySocket->listen("", dsh::net::kDiscoveryPort)) {
        note = "could not listen on UDP " + std::to_string(dsh::net::kDiscoveryPort) +
               " -- is another instance of this mod on this machine already listening there?";
        return nullptr;
    }
    return g_discoverySocket.get();
}

std::unique_ptr<dsh::net::ITransport> open_discovery_socket(std::string& note) {
    note.clear();
    std::unique_ptr<dsh::net::ITransport> socket = dsh::net::make_udp_transport();
    if (!socket) {
        note = "could not create a transport";
        return nullptr;
    }
    if (!socket->listen("", dsh::net::kDiscoveryPort)) {
        note = "could not listen on UDP " + std::to_string(dsh::net::kDiscoveryPort) +
               " -- is another instance of this mod on this machine already listening there?";
        return nullptr;
    }
    return socket;
}

void answer_probe(dsh::net::ITransport& socket, const dsh::net::Endpoint& from,
                  const dsh::net::BeaconState& beacon, unsigned& answered, std::string& firstFrom) {
    if (!socket.send(from, beacon.payload())) return;
    ++answered;
    if (firstFrom.empty()) {
        std::string ip;
        std::uint16_t port = 0;
        firstFrom = endpoint_to_ip_port(endpoint_bytes_to_text(from), ip, port)
                        ? (ip + ":" + std::to_string(static_cast<unsigned>(port)))
                        : ("an endpoint of " + std::to_string(from.bytes.size()) + " byte(s)");
    }
}

void discovery_tick(std::uint64_t nowMs) {
    if (!g_beacon.active && !g_search.active) return;

    if (g_beaconUntilMs != 0 && nowMs >= g_beaconUntilMs) discovery_stop("its window ended");
    if (g_searchUntilMs != 0 && nowMs >= g_searchUntilMs) discovery_stop("its window ended");
    if (!g_beacon.active && !g_search.active) return;

    if (!g_discoverySocket) return;

    if (g_beacon.active && g_beacon.tick(nowMs)) {

        if (!g_peerAddresses.empty()) {
            unsigned peerFailed = 0;
            (void)send_to_listed_peers(*g_discoverySocket, g_beacon.payload(), g_peerAddresses, peerFailed);
        } else {

            for (const dsh::net::BroadcastTarget& target : dsh::net::broadcast_targets_cached(30000)) {
                dsh::net::Endpoint to;
                if (!endpoint_for_ipv4(target.address, dsh::net::kDiscoveryPort, to)) continue;
                (void)g_discoverySocket->send(to, g_beacon.payload());
            }
        }
    }

    if (g_search.active && g_search.probe_due(nowMs)) {
        const std::vector<std::uint8_t> probe = g_search.probe_payload();
        if (!g_peerAddresses.empty()) {

            unsigned peerFailed = 0;
            (void)send_to_listed_peers(*g_discoverySocket, probe, g_peerAddresses, peerFailed);
        } else {
            for (const dsh::net::BroadcastTarget& target : dsh::net::broadcast_targets_cached(30000)) {
                dsh::net::Endpoint to;
                if (!endpoint_for_ipv4(target.address, dsh::net::kDiscoveryPort, to)) continue;
                (void)g_discoverySocket->send(to, probe);
            }
        }
    }

    if (g_peerAddresses.empty() && g_search.active &&
        sweep_due(g_search, g_searchOpenedMs, g_sweepLastMs, nowMs)) {
        run_sweep(*g_discoverySocket, g_search.probe_payload(), g_sweepTotal, g_sweepSummary);
        DSH_INFO("discovery: 2 s of broadcast probing reached nothing, so the fallback runs -- %s",
                 g_sweepSummary.c_str());
    }

    if (!g_discoverySocket) return;
    std::vector<dsh::net::Datagram> got;
    g_discoverySocket->poll(got);
    for (const dsh::net::Datagram& d : got) {

        if (!endpoint_is_listed(d.from, g_peerAddresses)) {
            if (g_peerIgnored++ < 8) {
                DSH_INFO("discovery: IGNORED a datagram from %s -- peers.txt lists %s, so nothing from "
                         "outside that list is answered or joined while the list is active",
                         endpoint_text(d.from).c_str(), g_peerNote.c_str());
            }
            continue;
        }

        if (g_beacon.active && dsh::net::is_probe(d.payload)) {
            unsigned answered = 0;
            std::string firstFrom;
            answer_probe(*g_discoverySocket, d.from, g_beacon, answered, firstFrom);
            if (answered != 0) {
                DSH_INFO("discovery: ANSWERED a probe from %s -- the reply is a unicast beacon to the "
                         "address that asked, which is the direction that does not depend on a network "
                         "forwarding broadcasts", firstFrom.c_str());
            }
            continue;
        }
        if (g_search.active && g_search.observe(d.from.bytes, d.payload)) {
            const dsh::net::Room& r = g_search.rooms.back();
            std::string ip;
            std::uint16_t port = 0;
            const bool known = endpoint_to_ip_port(r.address, ip, port);
            DSH_INFO("discovery: FOUND a room -- '%s' on session port %u, from %s", r.name.c_str(),
                     static_cast<unsigned>(r.sessionPort),
                     known ? (ip + " (the address to join)").c_str()
                           : "an endpoint that could not be read as an address");
        }
    }
    if (g_search.active && g_search.done(nowMs)) discovery_stop("the window closed");
}

std::uint64_t     g_netWatchUntilMs = 0;
std::uint64_t     g_netWatchNextMs = 0;
std::uintptr_t    g_netWatchWorld = 0;
std::uintptr_t    g_netWatchDriver = 0;
std::uint64_t     g_netWatchChanges = 0;
std::uint64_t     g_netWatchSamples = 0;

void net_end_session_for_travel(const std::string& line) {
    DSH_WARN("net: '%s' TRAVELS, so it cannot run inside a session. The session is being ENDED now; the "
             "command itself is left where the main loop will read it the moment `run_puppet_session` "
             "returns -- one reader, one dispatch, no queue to run it twice.", line.c_str());
    dsh::game::request_session_stop("a travel was requested: '" + line + "'");
}

void end_session_now(const std::string& why) {
    DSH_WARN("session: ENDING because %s -- the teardown runs inside the session (puppets destroyed, model "
             "memory cleared), and this machine can host or join again from the F7 menu", why.c_str());
    dsh::game::request_session_stop(why);
}

void net_watch_begin(std::uintptr_t worldBefore, std::uintptr_t driverBefore) {
    g_netWatchWorld = worldBefore;
    g_netWatchDriver = driverBefore;
    g_netWatchUntilMs = monotonic_ms() + 20000;
    g_netWatchNextMs = 0;
    g_netWatchChanges = 0;
    g_netWatchSamples = 0;
    DSH_INFO("net watch: watching for 20 s, about once a second (each sample is one object-table walk, ~590 ms "
             "measured) -- the world and its NetDriver are re-read; the baseline is world=0x%llX NetDriver=0x%llX",
             static_cast<unsigned long long>(worldBefore),
             static_cast<unsigned long long>(driverBefore));
}

void net_watch_tick() {
    if (g_netWatchUntilMs == 0) return;
    const std::uint64_t now = monotonic_ms();
    if (now >= g_netWatchUntilMs) {
        g_netWatchUntilMs = 0;
        const dsh::reflect::NetLayerState s = dsh::reflect::net_layer_state();
        if (g_netWatchDriver == 0 && s.netDriver == 0) {
            DSH_WARN("net watch: THE 20 s WINDOW CLOSED WITH NO NETDRIVER -- %llu sample(s), %llu world "
                     "change(s). %s", static_cast<unsigned long long>(g_netWatchSamples),
                     static_cast<unsigned long long>(g_netWatchChanges),
                     g_netWatchChanges == 0
                         ? "Nothing at all changed on this machine: the travel did not take effect."
                         : "The world list DID change but no driver ever appeared -- the engine travelled and "
                           "did not listen.",
                     s.line().c_str());
        } else if (s.netDriver == 0) {
            DSH_WARN("net watch: the 20 s window closed -- the world DID change (0x%llX -> 0x%llX) but "
                     "UWorld::NetDriver is STILL ZERO. The engine travelled without listening. %s",
                     static_cast<unsigned long long>(g_netWatchWorld),
                     static_cast<unsigned long long>(s.world), s.line().c_str());
        } else {
            DSH_INFO("net watch: the 20 s window closed with a NETDRIVER PRESENT -- %s", s.line().c_str());
        }

        dsh::reflect::game_thread_pause_all(false);
        return;
    }
    if (now < g_netWatchNextMs) return;
    g_netWatchNextMs = now + 1000;
    const dsh::reflect::NetLayerState s = dsh::reflect::net_layer_state();
    ++g_netWatchSamples;
    const bool changed = s.world != g_netWatchWorld || s.netDriver != g_netWatchDriver;
    if (changed) ++g_netWatchChanges;

    DSH_INFO("net watch: sample #%llu at +%llu ms%s -- %s",
             static_cast<unsigned long long>(g_netWatchSamples),
             static_cast<unsigned long long>(now - (g_netWatchUntilMs - 20000)),
             changed ? " (CHANGE)" : "", s.line().c_str());
    g_netWatchWorld = s.world;
    g_netWatchDriver = s.netDriver;
}

void handle_command(const std::string& line);

std::atomic<bool>     g_beaconStop{false};
std::atomic<bool>     g_beaconRunning{false};
std::atomic<unsigned> g_beaconSent{0};
std::thread           g_beaconThread;

void beacon_thread_proc(std::uint16_t sessionPort, std::string mapKey) {

    std::string socketNote;
    std::unique_ptr<dsh::net::ITransport> socket = open_discovery_socket(socketNote);
    if (!socket) {
        DSH_WARN("beacon thread: %s -- so THE HOST CANNOT BE FOUND. It is still hosting; a client has to be "
                 "given the address by hand", socketNote.c_str());
        g_beaconRunning.store(false, std::memory_order_release);
        return;
    }

    dsh::net::BeaconState beacon;
    beacon.start(sessionPort, "Dolls Nest session", std::move(mapKey), monotonic_ms());

    std::vector<std::uint32_t> peerAddresses;
    std::string peerNote;
    read_peer_file(peerAddresses, peerNote);
    if (!peerAddresses.empty()) {
        DSH_INFO("beacon thread: announcing session port %u every second to the %s in peers.txt ONLY -- NO "
                 "broadcast is sent, so this host cannot be found on a physical LAN. This machine's "
                 "interfaces are %s",
                 static_cast<unsigned>(sessionPort), peerNote.c_str(),
                 dsh::net::local_ipv4_summary().c_str());
    } else {
        std::string targetSummary;
        const std::vector<dsh::net::BroadcastTarget>& targets = dsh::net::broadcast_targets_cached(0);
        for (const dsh::net::BroadcastTarget& target : targets) {
            if (!targetSummary.empty()) targetSummary += " | ";
            targetSummary += target.text;
        }
        DSH_INFO("beacon thread: announcing session port %u every second to %zu target(s): %s",
                 static_cast<unsigned>(sessionPort), targets.size(), targetSummary.c_str());
        DSH_INFO("beacon thread: this machine's own interfaces are %s -- if the other end's log shows a "
                 "subnet that is not in the target list above, the two machines are not on a network this "
                 "can reach. (%s)",
                 dsh::net::local_ipv4_summary().c_str(), peerNote.c_str());
    }

    unsigned sentThisWindow = 0;
    unsigned failedThisWindow = 0;
    unsigned sentTotal = 0;
    unsigned failedTotal = 0;
    unsigned probesAnswered = 0;
    std::string firstProbeFrom;
    std::uint64_t nextReportMs = monotonic_ms() + 30000;

    while (!g_beaconStop.load(std::memory_order_acquire)) {
        const std::uint64_t now = monotonic_ms();
        if (beacon.tick(now)) {
            if (!peerAddresses.empty()) {

                unsigned peerFailed = 0;
                const unsigned peerSent =
                    send_to_listed_peers(*socket, beacon.payload(), peerAddresses, peerFailed);
                sentThisWindow += peerSent;
                sentTotal += peerSent;
                failedThisWindow += peerFailed;
                failedTotal += peerFailed;
            } else {

                for (const dsh::net::BroadcastTarget& target : dsh::net::broadcast_targets_cached(30000)) {
                    dsh::net::Endpoint to;
                    if (!endpoint_for_ipv4(target.address, dsh::net::kDiscoveryPort, to)) continue;
                    if (socket->send(to, beacon.payload())) {
                        ++sentThisWindow;
                        ++sentTotal;
                    } else {
                        ++failedThisWindow;
                        ++failedTotal;
                    }
                }
            }
            g_beaconSent.store(sentTotal, std::memory_order_relaxed);
        }

        std::vector<dsh::net::Datagram> got;
        socket->poll(got);
        for (const dsh::net::Datagram& d : got) {

            if (!endpoint_is_listed(d.from, peerAddresses)) continue;
            if (!dsh::net::is_probe(d.payload)) continue;
            answer_probe(*socket, d.from, beacon, probesAnswered, firstProbeFrom);
        }

        if (now >= nextReportMs) {
            nextReportMs = now + 30000;
            DSH_INFO("beacon thread: %u announcement(s) sent and %u refused in the last 30 s (%u sent, %u "
                     "refused since it started); %u probe(s) ANSWERED%s; transport: %s",
                     sentThisWindow, failedThisWindow, sentTotal, failedTotal, probesAnswered,
                     firstProbeFrom.empty() ? "" : (" (the first from " + firstProbeFrom + ")").c_str(),
                     std::string(socket->stats_line()).c_str());
            sentThisWindow = 0;
            failedThisWindow = 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    DSH_INFO("beacon thread: stopped after %u announcement(s) (%u refused) and %u probe(s) answered%s",
             g_beaconSent.load(), failedTotal, probesAnswered,
             firstProbeFrom.empty() ? "" : (" (the first from " + firstProbeFrom + ")").c_str());
    g_beaconRunning.store(false, std::memory_order_release);
}

void beacon_thread_start(std::uint16_t sessionPort, const char* why) {
    if (g_beaconRunning.load(std::memory_order_acquire)) {
        DSH_INFO("beacon thread: one is ALREADY announcing; it is left alone (%s)", why);
        return;
    }
    if (g_beaconThread.joinable()) g_beaconThread.join();
    g_beaconSent.store(0);
    g_beaconStop.store(false);
    g_beaconRunning.store(true);
    std::string mapKey = current_map_key("this host is announcing, so this is the map it offers");
    g_beaconThread = std::thread(beacon_thread_proc, sessionPort, std::move(mapKey));
    DSH_INFO("beacon thread: started (%s)", why);
}

void beacon_thread_stop(const char* why) {
    if (!g_beaconThread.joinable()) return;
    g_beaconStop.store(true, std::memory_order_release);
    g_beaconThread.join();
    DSH_INFO("beacon thread: stopped (%s)", why);
}

void run_menu_session(bool asHost, const std::string& address, std::uint16_t port, const char* who) {

    const std::string durationText = "with no time limit (end it from the F7 menu)";
    DSH_INFO("co-op menu: starting the %s session on port %u %s -- from here the command tick is "
             "BLOCKED for that whole time, so the F7 menu and the command file do nothing until it ends "
             "(the same is true of 'puppetnet serve'/'join')",
             asHost ? "HOST" : "CLIENT", static_cast<unsigned>(port), durationText.c_str());

    g_sessionCommandOffsetKnown = false;
    dsh::game::set_session_tick_hook(&pump_commands_during_session);
    const auto sr = dsh::game::run_puppet_session(asHost, address, port, kMenuSessionSeconds);
    dsh::game::set_session_tick_hook(nullptr);
    DSH_INFO("co-op menu: the %s session (%s) ended -- ok=%s actor(s)=%d machine modules merged=%d/%d "
             "chunks sent=%d detail=%s",
             asHost ? "HOST" : "CLIENT", who, sr.ok ? "yes" : "no", sr.actorsSpawned,
             sr.machineModulesMerged, 10, sr.chunksSent, sr.detail.c_str());
}

void join_first_room() {
    if (g_search.rooms.empty()) {

        DSH_WARN("co-op menu: the search window closed and NO room on THIS map was heard, so NOTHING is "
                 "joined -- %s. If that says nothing was heard AT ALL, then no announcement reached this "
                 "machine: nothing is announcing, or the two machines are not on the same virtual LAN, or a "
                 "firewall dropped the broadcast. It does NOT mean the search failed", g_search.summary().c_str());

        DSH_WARN("co-op menu: THIS machine's interfaces are %s -- compare that with the target list the "
                 "host's 'beacon thread: announcing ... to N target(s)' line prints. If the host aimed at "
                 "subnets that are not here, the two machines are not on a network a beacon can cross",
                 dsh::net::local_ipv4_summary().c_str());

        DSH_WARN("co-op menu: the peer list -- %s. If the host's virtual-LAN address is in it, that host WAS "
                 "probed directly; its own log then says whether it answered",
                 g_peerNote.c_str());
        return;
    }
    const dsh::net::Room& room = g_search.rooms.front();
    std::string ip;
    std::uint16_t port = room.sessionPort;
    if (!endpoint_to_ip_port(room.address, ip, port)) {
        DSH_WARN("co-op menu: the room '%s' was heard but its endpoint is %zu byte(s) and cannot be read "
                 "as an address, so it is NOT joined rather than joined at a guessed address",
                 room.name.c_str(), room.address.size());
        return;
    }
    DSH_INFO("co-op menu: joining the room '%s' -- heard %u time(s) from %s, session port %u",
             room.name.c_str(), room.announcements, ip.c_str(), static_cast<unsigned>(room.sessionPort));
    run_menu_session(false, ip, room.sessionPort, "menu join row");
}

void act_on_menu_choice(std::int32_t row, bool inSessionRows) {

    if (inSessionRows) {
        if (row == 1) {
            end_session_now("the operator chose 'end the session' in the F7 menu");
        } else {
            DSH_INFO("co-op menu: row %d of the in-session menu -- keep playing, nothing is changed (row 1 is "
                     "the one that ends the session)", row);
        }
        return;
    }

    if (dsh::game::session_running()) {
        DSH_INFO("co-op menu: row %d came from a menu whose rows were host / join / not now, and a session "
                 "has STARTED since it was shown -- so nothing is done. (This is the second half of the race "
                 "that used to turn a 'host' answer into 'end the session')", row);
        return;
    }
    switch (row) {
        case 0:
            DSH_INFO("co-op menu: row 0 -- HOST. Announcing first (the announcement is on its own thread, "
                     "because the session call below blocks this one) and then hosting");
            beacon_thread_start(kMenuSessionPort, "the co-op menu's host row");
            run_menu_session(true, std::string(), kMenuSessionPort, "menu host row");
            beacon_thread_stop("the host session ended");
            return;
        case 1: {
            DSH_INFO("co-op menu: row 1 -- JOIN. Listening on UDP %u for %u ms; the first room heard is "
                     "joined when the window closes",
                     static_cast<unsigned>(dsh::net::kDiscoveryPort), kMenuJoinSearchMs);
            DSH_INFO("co-op menu: this machine's interfaces are %s", dsh::net::local_ipv4_summary().c_str());
            std::string socketNote;
            if (discovery_socket_listening(socketNote) == nullptr) {
                DSH_WARN("co-op menu: %s -- so NOTHING is listened for and nothing is joined",
                         socketNote.c_str());
                return;
            }
            begin_search(kMenuJoinSearchMs);
            g_searchUntilMs = 0;
            g_joinPending = true;
            return;
        }
        default:
            DSH_INFO("co-op menu: row %d -- nothing is started", row);
            return;
    }
}

dsh::menu_text::Language menu_language_from_tag(const std::string& raw) {
    std::string t;
    t.reserve(raw.size());
    for (const char c : raw) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (t == "zh-hans" || t == "zh-cn" || t == "zh-sg" || t == "zh") {
        return dsh::menu_text::Language::SimplifiedChinese;
    }
    if (t == "zh-hant" || t == "zh-tw" || t == "zh-hk" || t == "zh-mo") {
        return dsh::menu_text::Language::TraditionalChinese;
    }
    if (t == "ja" || t == "ja-jp") return dsh::menu_text::Language::Japanese;
    return dsh::menu_text::Language::English;
}

std::string read_culture_from_ini(const std::wstring& path) {
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string bytes;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) != 0 && got > 0) {
        bytes.append(buf, got);
        if (bytes.size() > 256 * 1024) break;
    }
    CloseHandle(f);
    if (bytes.empty()) return {};

    const bool utf16 = bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
                       static_cast<unsigned char>(bytes[1]) == 0xFE;
    if (utf16) {
        const auto* wide = reinterpret_cast<const wchar_t*>(bytes.data() + 2);
        const std::size_t units = (bytes.size() - 2) / sizeof(wchar_t);
        const wchar_t key[] = L"Culture=";
        const std::size_t keyLen = 8;
        for (std::size_t i = 0; i + keyLen <= units; ++i) {
            bool match = true;
            for (std::size_t k = 0; k < keyLen; ++k) {
                if (wide[i + k] != key[k]) { match = false; break; }
            }
            if (!match) continue;
            std::string value;
            for (std::size_t k = i + keyLen; k < units; ++k) {
                const wchar_t c = wide[k];
                if (c == L'\r' || c == L'\n' || c == 0) break;
                if (c <= 0x7F) value.push_back(static_cast<char>(c));
            }
            return value;
        }
        return {};
    }
    const char key[] = "Culture=";
    const std::size_t keyLen = 8;
    for (std::size_t i = 0; i + keyLen <= bytes.size(); ++i) {
        if (std::memcmp(bytes.data() + i, key, keyLen) != 0) continue;
        std::string value;
        for (std::size_t k = i + keyLen; k < bytes.size(); ++k) {
            const char c = bytes[k];
            if (c == '\r' || c == '\n' || c == 0) break;
            value.push_back(c);
        }
        return value;
    }
    return {};
}

dsh::menu_text::Language menu_language(std::string& source) {
    source.clear();
    if (const char* env = std::getenv("DSH_MENU_LANG"); env != nullptr && *env != '\0') {
        source = std::string("DSH_MENU_LANG=") + env;
        return menu_language_from_tag(env);
    }
    wchar_t local[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        const std::wstring path =
            std::wstring(local) + L"\\DollsNest\\Saved\\Config\\WindowsNoEditor\\GameUserSettings.ini";
        const std::string culture = read_culture_from_ini(path);
        if (!culture.empty()) {
            source = "the game's GameUserSettings.ini, Culture=" + culture;
            return menu_language_from_tag(culture);
        }
    }
    const LANGID id = GetUserDefaultUILanguage();
    source = "the Windows UI language";
    switch (PRIMARYLANGID(id)) {
        case LANG_CHINESE:
            switch (SUBLANGID(id)) {
                case SUBLANG_CHINESE_TRADITIONAL:
                case SUBLANG_CHINESE_HONGKONG:
                case SUBLANG_CHINESE_MACAU:
                    return dsh::menu_text::Language::TraditionalChinese;
                default:
                    return dsh::menu_text::Language::SimplifiedChinese;
            }
        case LANG_JAPANESE:
            return dsh::menu_text::Language::Japanese;
        default:
            return dsh::menu_text::Language::English;
    }
}

void open_coop_menu() {
    DSH_INFO("co-op menu: F7 pressed -- asking the game to show its own option list");

    std::string langSource;
    const dsh::menu_text::Language lang = menu_language(langSource);
    DSH_INFO("co-op menu: the menu text is %s (from %s)", dsh::menu_text::tag(lang), langSource.c_str());

    const bool hookReady = dsh::reflect::game_thread_start_hook();
    DSH_INFO("co-op menu: the game-thread trampoline is %s -- %s", hookReady ? "ready" : "NOT ready",
             dsh::reflect::game_thread_report().c_str());

    const bool dispatched = dsh::reflect::run_on_game_thread(
        [lang]() {
            const dsh::game::PawnHandle pawn = dsh::game::locate_local_pawn();
            if (!pawn.valid) {
                DSH_WARN("co-op menu: there is no local pawn to hang the menu on, so nothing was shown "
                         "(the menu needs a causer object) -- %s", pawn.note.c_str());
                return;
            }

            DSH_INFO("co-op menu: the causer is 0x%llX '%s' (class '%s'), read from the controller now",
                     static_cast<unsigned long long>(pawn.pawn), pawn.pawnName.c_str(),
                     pawn.pawnClass.c_str());

            const bool inSession = dsh::game::session_running();

            const dsh::menu_text::Text& text = dsh::menu_text::for_language(lang);
            const std::vector<std::wstring> options =
                inSession
                    ? std::vector<std::wstring>{text.keep, text.end}
                    : std::vector<std::wstring>{text.host, text.join, text.quit};
            DSH_INFO("co-op menu: showing %s", inSession ? "the IN-SESSION rows (keep / end)" :
                                                           "the usual rows (host / join / not now)");
            g_menuBuiltInSessionRows.store(inSession, std::memory_order_relaxed);
            g_menuBuilt = dsh::reflect::menu_show_select(pawn.pawn, options, text.caption, text.subtitle);
            g_menuBuiltReady.store(true, std::memory_order_release);
        },
        5000);

    if (!dispatched) {
        DSH_WARN("co-op menu: the game-thread dispatch was REFUSED or TIMED OUT, so the game was never "
                 "asked to show anything. The trampoline report above names which part is not ready; "
                 "nothing was queued, because `run_on_game_thread` guarantees a cancelled item never runs");
    }
}

std::string engine_command_list();

DWORD WINAPI worker_proc(LPVOID) {

    {

        char modulePath[MAX_PATH] = {0};
        const DWORD got = GetModuleFileNameA(reinterpret_cast<HMODULE>(g_moduleBase), modulePath,
                                            static_cast<DWORD>(sizeof(modulePath)));
        char line[696];
        std::snprintf(line, sizeof(line),
                      "marker pid=%lu moduleBase=%s stamp=%s thread=worker dll=%s",
                      static_cast<unsigned long>(GetCurrentProcessId()),
                      hex_ptr(g_moduleBase).c_str(), DSH_BUILD_STAMP,
                      got == 0 ? "<unreadable>" : modulePath);
        append_marker(line);
    }

    dsh::platform::log_init(evidence_dir());
    if (!dsh::platform::log_is_open()) {

        append_marker("!! log_init failed: no log sink. marker file is the only evidence.");
        return 1;
    }

    DSH_INFO("================================================================");
    DSH_INFO("dollsnest-coop bootstrap (worker) build=%s api=%u", DSH_BUILD_STAMP, kApiVersion);
    DSH_INFO("log file : %s", std::string(dsh::platform::log_path()).c_str());
    DSH_INFO("================================================================");

    const auto facts = dsh::game::gather_process_facts();
    DSH_INFO("process: pid=%lu", static_cast<unsigned long>(facts.pid));
    DSH_INFO("process: exe=%s", facts.exePath.empty() ? "(unknown)" : facts.exePath.c_str());
    DSH_INFO("process: main module base=%s size=%llu", facts.mainModuleBase.c_str(),
             static_cast<unsigned long long>(facts.mainModuleSize));

    {
        char modulePath[MAX_PATH] = {0};
        const DWORD got = GetModuleFileNameA(reinterpret_cast<HMODULE>(g_moduleBase), modulePath,
                                            static_cast<DWORD>(sizeof(modulePath)));
        DSH_INFO("bootstrap: loaded from %s",
                 got == 0 ? "<the module's own path could not be read>"
                          : std::string(modulePath, modulePath + got).c_str());
    }
    DSH_INFO("bootstrap: our module base=%s", hex_ptr(g_moduleBase).c_str());

    {
        wchar_t modulePath[MAX_PATH] = {0};
        const DWORD got = GetModuleFileNameW(
            reinterpret_cast<HMODULE>(g_moduleBase), modulePath,
            static_cast<DWORD>(sizeof(modulePath) / sizeof(modulePath[0])));
        const auto digest = got == 0 ? dsh::platform::FileDigest{}
                                     : dsh::platform::file_sha256_prefix(modulePath);
        if (digest.ok) {
            DSH_INFO("bootstrap: file sha256=%s size=%llu", digest.hex,
                     static_cast<unsigned long long>(digest.size));
        } else {
            DSH_WARN("bootstrap: our own file could not be hashed (%s) -- the stamp above is the only identity",
                     digest.error[0] != '\0' ? digest.error : "no path");
        }
    }

    const bool exeNameKnown = !facts.exeBaseName.empty();
    const bool inGame = exeNameKnown && is_game_process(facts.exeBaseName);
    const bool gateArmed = exeNameKnown && !inGame;
    g_engineCommandsAllowed.store(!gateArmed, std::memory_order_relaxed);

    g_inGameProcess.store(inGame, std::memory_order_relaxed);

    if (!exeNameKnown) {
        DSH_WARN("process: the executable name could not be read (GetModuleFileNameA failed), so this "
                 "process cannot be identified as the game or as something else. The engine-command "
                 "gate is therefore NOT armed -- %s will run, "
                 "which is the behaviour this code had before the gate existed, stated rather than "
                 "assumed.", engine_command_list().c_str());
    } else if (gateArmed) {
        DSH_WARN("not the game process ('%s') -- engine-touching commands are DISABLED here: %s refuse "
                 "before running anything. The capability "
                 "report below is compile-time information and reads no engine memory; logging, the "
                 "marker file and the rest of the command channel still work",
                 facts.exeBaseName.c_str(), engine_command_list().c_str());
    } else {
        DSH_INFO("process: '%s' identified as the game -- the engine-command gate is not needed",
                 facts.exeBaseName.c_str());
    }

    dsh::reflect::report_capability();
    {
        const std::string probe = dsh::game::probe_summary();
        std::size_t i = 0;
        while (i < probe.size()) {
            const std::size_t nl = probe.find('\n', i);
            const std::string line =
                probe.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
            if (!line.empty()) DSH_INFO("%s", line.c_str());
            if (nl == std::string::npos) break;
            i = nl + 1;
        }
    }
    if (gateArmed) {
        DSH_INFO("process gate: ARMED -- %s will be REFUSED in "
                 "this process; shutdown is unaffected", engine_command_list().c_str());
    }

    run_selftest();

    if (g_runHookSelftestAtStartup) {
        run_hook_selftest();
    }

#if defined(DSH_HAVE_MINHOOK)
    dsh::reflect::set_game_thread_hook_installer(&InstallGameThreadHook);
#endif

    DSH_INFO("bootstrap: initialised (Phase 0.2). Next targets: 0.1 exports, 0.3 world/pawn, "
             "0.5 native spawn, 0.6 ProcessEvent");

    g_initialised.store(true);
    append_marker("marker init-complete");

    DSH_INFO("command channel: watching %s\\dsh-commands.txt",
             evidence_dir().c_str());
    poll_command_file();
    return 0;
}

std::string command_file_path() {
    const std::string dir = evidence_dir();
    if (dir.empty()) return {};

    if (in_game_process()) return dir + "\\dsh-commands.txt";
    char name[64];
    std::snprintf(name, sizeof(name), "\\dsh-commands-%lu.txt",
                  static_cast<unsigned long>(GetCurrentProcessId()));
    return dir + name;
}

bool command_start_offset(const std::string& path, unsigned long long& offset, std::string& why) {
    offset = 0;
    why.clear();
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return true;
        why = "cannot be opened (err=" + std::to_string(static_cast<unsigned long>(err)) + ")";
        return false;
    }
    LARGE_INTEGER sz{};
    const bool sized = GetFileSizeEx(h, &sz) != 0;
    const DWORD err = sized ? 0 : GetLastError();
    CloseHandle(h);
    if (!sized) {
        why = "its size cannot be read (err=" + std::to_string(static_cast<unsigned long>(err)) + ")";
        return false;
    }
    offset = static_cast<unsigned long long>(sz.QuadPart);
    return true;
}

std::uint64_t g_sessionCommandOffset = 0;
bool          g_sessionCommandOffsetKnown = false;

bool session_command_allowed(const std::string& line) {

    if (line.rfind("world net", 0) == 0) return false;

    if (line.rfind("ai", 0) == 0) return true;
    if (line.rfind("progress", 0) == 0) return true;
    if (line.rfind("world", 0) == 0) return true;
    if (line.rfind("mission", 0) == 0) return true;
    if (line == "objdiff report") return true;

    if (line == "parts") return true;

    if (line == "hide") return true;

    if (line == "fix") return true;
    return false;
}

void menu_pump() {

    if (g_pendingAction.row >= 0 && monotonic_ms() >= g_pendingAction.dueMs) {
        const int row = g_pendingAction.row;
        const bool inSessionRows = g_pendingAction.inSessionRows;
        g_pendingAction.row = -1;
        DSH_INFO("co-op menu: acting on the row chosen %llu ms ago -- row %d (%s)",
                 static_cast<unsigned long long>(kMenuActDelayMs), row,
                 inSessionRows ? "it was shown as the in-session menu (keep / end)"
                               : "it was shown as the usual menu (host / join / not now)");
        act_on_menu_choice(row, inSessionRows);
    }

    if (g_menuBuiltReady.exchange(false, std::memory_order_acquire)) {
        const dsh::reflect::MenuHandle built = g_menuBuilt;
        if (built.shown) {
            g_coopMenu.open = true;
            g_coopMenu.handle = built;
            g_coopMenu.inSessionRows = g_menuBuiltInSessionRows.load(std::memory_order_relaxed);
            g_coopMenu.openedMs = monotonic_ms();
            g_coopMenu.lastStatusMs = 0;
            DSH_INFO("co-op menu: SHOWN -- %s", built.note.c_str());

            const dsh::reflect::MenuTaskState base = dsh::reflect::menu_read_task_state(built.task);
            DSH_INFO("co-op menu: task 0x%llX baseline -- OnCancelled carries %u "
                     "binding(s), OnFinished carries %u, readable=%s",
                     static_cast<unsigned long long>(built.task), base.onCancelled, base.onFinished,
                     base.readable ? "yes" : "NO");
        } else {
            DSH_WARN("co-op menu: NOT shown -- %s", built.note.c_str());
        }
    }

    if (g_coopMenu.open) {
        const std::uint64_t now = monotonic_ms();
        const std::int32_t picked = dsh::reflect::menu_read_choice(g_coopMenu.handle.selectedAddress);

        if (picked != g_coopMenu.handle.selected) {
            DSH_INFO("co-op menu: THE CHOICE ARRIVED -- the output slot at 0x%llX changed from %d to %d "
                     "after %llu ms, so the game DOES write the chosen row through the address the factory "
                     "was handed and a frame that stays alive is what makes it readable",
                     static_cast<unsigned long long>(g_coopMenu.handle.selectedAddress),
                     g_coopMenu.handle.selected, picked,
                     static_cast<unsigned long long>(now - g_coopMenu.openedMs));
            g_coopMenu.open = false;

            const bool inSessionRows = g_coopMenu.inSessionRows;
            if (!inSessionRows && (picked == 0 || picked == 1)) {
                g_pendingAction.row = picked;
                g_pendingAction.dueMs = now + kMenuActDelayMs;
                g_pendingAction.inSessionRows = inSessionRows;
                DSH_INFO("co-op menu: row %d starts something, so it is acted on in %llu ms -- the game is "
                         "still unwinding the dialogue it has just finished, and starting a blocking session "
                         "inside that is what crashed an earlier attempt",
                         picked, static_cast<unsigned long long>(kMenuActDelayMs));
            } else {
                act_on_menu_choice(picked, inSessionRows);
            }
        } else if (now - g_coopMenu.openedMs >= kMenuAbandonMs) {
            DSH_WARN("co-op menu: unanswered for %llu s and the output slot at 0x%llX still reads %d. This "
                     "side STOPS WAITING for it and a later write is not acted on. THE GAME'S TASK IS NOT "
                     "TOUCHED: calling a UFunction on a task the engine may have collected is the "
                     "use-after-free that was removed from this file once already, and each menu has its own "
                     "frame slot, so forgetting this one leaves the game's own pointer valid. Press F7 to "
                     "open a new one",
                     static_cast<unsigned long long>(now - g_coopMenu.openedMs),
                     static_cast<unsigned long long>(g_coopMenu.handle.selectedAddress), picked);
            g_coopMenu.open = false;
        } else if (g_coopMenu.lastStatusMs == 0 || now - g_coopMenu.lastStatusMs >= 5000) {
            g_coopMenu.lastStatusMs = now;

            const dsh::reflect::MenuTaskState taskState =
                dsh::reflect::menu_read_task_state(g_coopMenu.handle.task);
            DSH_INFO("co-op menu: still waiting for a row -- %llu s open, task 0x%llX, output slot at 0x%llX "
                     "reads %d; OnCancelled=%u OnFinished=%u (%s)",
                     static_cast<unsigned long long>((now - g_coopMenu.openedMs) / 1000),
                     static_cast<unsigned long long>(g_coopMenu.handle.task),
                     static_cast<unsigned long long>(g_coopMenu.handle.selectedAddress), picked,
                     taskState.onCancelled, taskState.onFinished,
                     taskState.readable ? "the task is still there"
                                        : "THE TASK IS NO LONGER READABLE, so the game has collected it -- "
                                          "the menu is gone and no row was written into the slot");
        }
    }
}

void pump_commands_during_session() {

    menu_pump();
    if ((GetAsyncKeyState(VK_F7) & 0x0001) != 0) {
        if (g_coopMenu.open) {
            DSH_INFO("co-op menu: F7 pressed during a session and a menu is ALREADY open (output slot at "
                     "0x%llX reads %d) -- answer it, or press F8 to stop waiting for it",
                     static_cast<unsigned long long>(g_coopMenu.handle.selectedAddress),
                     dsh::reflect::menu_read_choice(g_coopMenu.handle.selectedAddress));
        } else {
            DSH_INFO("co-op menu: F7 pressed DURING a session -- opening the in-session menu (keep / end)");
            open_coop_menu();
        }
    }

    const std::string path = command_file_path();
    if (path.empty()) return;

    if (!g_sessionCommandOffsetKnown) {

        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        g_sessionCommandOffset = ec ? 0 : static_cast<std::uint64_t>(size);
        g_sessionCommandOffsetKnown = true;
        DSH_INFO("command channel: a session is running -- pumping read-only commands from offset %llu; "
                 "everything else is REFUSED by name until the session ends",
                 static_cast<unsigned long long>(g_sessionCommandOffset));
        return;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) return;
    if (static_cast<std::uint64_t>(size) < g_sessionCommandOffset) {

        g_sessionCommandOffset = static_cast<std::uint64_t>(size);
        return;
    }
    if (static_cast<std::uint64_t>(size) == g_sessionCommandOffset) return;

    in.seekg(static_cast<std::streamoff>(g_sessionCommandOffset), std::ios::beg);
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    g_sessionCommandOffset = static_cast<std::uint64_t>(size);
    in.close();

    std::size_t start = 0;
    while (start <= all.size()) {
        std::size_t nl = all.find('\n', start);
        const std::string raw = all.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        start = (nl == std::string::npos) ? all.size() + 1 : nl + 1;

        std::string line = raw;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        while (!line.empty() && line.front() == ' ') line.erase(line.begin());
        if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
            line = line.substr(1, line.size() - 2);
        }
        if (line.empty()) continue;

        DSH_INFO("command received (inside a session): '%s'", line.c_str());

        if (line.rfind("world net", 0) == 0) {
            net_end_session_for_travel(line);
            continue;
        }
        if (!session_command_allowed(line)) {
            DSH_WARN("command '%s' REFUSED: a session is running and only read-only commands are allowed "
                     "inside one. 'ai', 'progress', 'world' and 'mission' work; 'puppetnet' and the F7 menu "
                     "would start a session inside a session, and 'shutdown' would unload this DLL underneath "
                     "it. Run it after the session ends.", line.c_str());
            continue;
        }
        handle_command(line);
    }
}

void poll_command_file() {
    const std::string path = command_file_path();
    if (path.empty()) return;

    unsigned long long offset = 0;
    std::string offsetProblem;
    bool offsetKnown = command_start_offset(path, offset, offsetProblem);
    if (offsetKnown) {
        DSH_INFO("command channel: watching %s (start offset=%llu)",
                 path.c_str(), static_cast<unsigned long long>(offset));
    } else {

        DSH_ERROR("command channel: '%s' exists but %s -- NOT reading it from zero, because that "
                  "would replay a previous session's commands", path.c_str(), offsetProblem.c_str());
    }

    int tick = 0;
    for (;;) {
        Sleep(250);
        ++tick;

        damage_watch_drain_entry();
    hp_watch_tick();
    hp_pin_tick();
    target_watch_tick();
    net_watch_tick();

        menu_pump();
        discovery_tick(monotonic_ms());

        if ((GetAsyncKeyState(VK_F7) & 0x0001) != 0) {
            if (g_coopMenu.open) {

                DSH_INFO("co-op menu: F7 pressed, but one is ALREADY open (task 0x%llX, open for %llu "
                         "s, output slot at 0x%llX reads %d). No second request is made. Answer the open "
                         "one, or press F8 to stop waiting for it",
                         static_cast<unsigned long long>(g_coopMenu.handle.task),
                         static_cast<unsigned long long>((monotonic_ms() - g_coopMenu.openedMs) / 1000),
                         static_cast<unsigned long long>(g_coopMenu.handle.selectedAddress),
                         dsh::reflect::menu_read_choice(g_coopMenu.handle.selectedAddress));
            } else {
                open_coop_menu();
            }
        }

        if ((GetAsyncKeyState(VK_F8) & 0x0001) != 0) {
            if (g_coopMenu.open) {
                DSH_INFO("co-op menu: F8 -- STOPPING THE WAIT for the menu built at 0x%llX (its output slot "
                         "at 0x%llX reads %d). The game's task is NOT touched, and its frame slot is not "
                         "reused until %d more menus have been built, so a late write lands in memory that "
                         "is still ours. Press F7 for a new menu",
                         static_cast<unsigned long long>(g_coopMenu.handle.task),
                         static_cast<unsigned long long>(g_coopMenu.handle.selectedAddress),
                         dsh::reflect::menu_read_choice(g_coopMenu.handle.selectedAddress), 4 - 1);
                g_coopMenu.open = false;
            } else {
                DSH_INFO("co-op menu: F8 -- no menu is open, so there is nothing to stop waiting for");
            }
        }

        if (!offsetKnown) {
            offsetKnown = command_start_offset(path, offset, offsetProblem);
            if (!offsetKnown) {
                if ((tick % 40) == 0) {
                    DSH_WARN("command channel: still not reading '%s' after %d s -- it exists but %s; "
                             "no command can be read until this changes", path.c_str(), tick / 4,
                             offsetProblem.c_str());
                }
                continue;
            }
            DSH_INFO("command channel: starting offset established at %llu -- reading from here",
                     static_cast<unsigned long long>(offset));
        }

        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {

            if (tick == 1 || (tick % 40) == 0) {
                DSH_INFO("command channel: tick=%d file not present yet (err=%lu)", tick,
                          GetLastError());
            }
            continue;
        }

        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(h, &sz)) {
            DSH_WARN("command channel: GetFileSizeEx failed err=%lu", GetLastError());
            CloseHandle(h);
            continue;
        }
        const auto fileSize = static_cast<unsigned long long>(sz.QuadPart);

        if (fileSize < offset) {
            DSH_INFO("command channel: tick=%d the file SHRANK from %llu to %llu byte(s) -- it was "
                     "rewritten, not appended, so the offset goes back to zero and its contents are read "
                     "from the start",
                     tick, offset, fileSize);
            offset = 0;
        }
        if (fileSize <= offset) {
            if (tick == 1 || (tick % 40) == 0) {
                DSH_INFO("command channel: tick=%d size=%llu offset=%llu (nothing new)", tick,
                          fileSize, offset);
            }
            CloseHandle(h);
            continue;
        }
        DSH_INFO("command channel: tick=%d size=%llu offset=%llu -> reading",
                 tick, fileSize, offset);

        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) {
            CloseHandle(h);
            continue;
        }

        char buf[4096] = {};
        DWORD got = 0;
        if (ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr) && got > 0) {
            offset += got;
            std::string chunk(buf, got);
            std::size_t start = 0;
            while (start < chunk.size()) {
                const std::size_t nl = chunk.find('\n', start);
                std::string line = chunk.substr(start, nl == std::string::npos ? std::string::npos
                                                                              : nl - start);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();

                while (!line.empty() && line.front() == ' ') line.erase(line.begin());
                if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
                    line = line.substr(1, line.size() - 2);
                    while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.pop_back();
                    while (!line.empty() && line.front() == ' ') line.erase(line.begin());
                }
                if (!line.empty()) handle_command(line);
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
        }
        CloseHandle(h);
    }
}

const char* const kKnownCommands[] = {
    "gthook", "loopback", "puppetnet", "shutdown",
    "objdiff",
    "mission",
    "world",
    "progress",
    "ai",
    "parts",
    "hide",
    "fix",
};

std::string known_command_list() {
    std::string out;
    for (const char* name : kKnownCommands) {
        if (!out.empty()) out += ", ";
        out += name;
    }
    return out;
}

const char* const kEngineCommands[] = {"gthook", "loopback", "puppetnet",
                                       "objdiff",
                                       "mission",
                                       "world",
                                       "progress",
                                       "ai",
                                       "parts",
                                       "hide",
                                       "fix"};

std::string engine_command_list() {
    std::string out;
    for (const char* name : kEngineCommands) {
        if (!out.empty()) out += ", ";
        out += name;
    }
    return out;
}

bool command_touches_engine(const std::string& line) {
    for (const char* name : kEngineCommands) {
        if (line.rfind(name, 0) == 0) return true;
    }
    return false;
}

FARPROC find_steam_export(HMODULE module, const char* prefix) {
    if (module == nullptr || prefix == nullptr) return nullptr;
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + static_cast<std::uintptr_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return nullptr;
    const auto exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    const auto names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    const std::size_t prefixLength = std::strlen(prefix);
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        const auto name = reinterpret_cast<const char*>(base + names[i]);
        if (std::strncmp(name, prefix, prefixLength) == 0) return GetProcAddress(module, name);
    }
    return nullptr;
}

std::string steam_id64_text() {
    HMODULE steam = GetModuleHandleA("steam_api64.dll");
    if (steam == nullptr) {
        return "steam_api64.dll is NOT loaded in this process, so this machine is not running through Steam "
               "and a SteamNetDriver can answer nothing";
    }
    using SteamUserAccessor = void* (*)();
    using GetSteamIdFn = unsigned long long (*)(void*);
    const auto user = reinterpret_cast<SteamUserAccessor>(find_steam_export(steam, "SteamAPI_SteamUser_v"));
    const auto getSteamId =
        reinterpret_cast<GetSteamIdFn>(find_steam_export(steam, "SteamAPI_ISteamUser_GetSteamID"));
    if (user == nullptr || getSteamId == nullptr) {
        return "steam_api64.dll is loaded but does not export the two functions this needs "
               "(SteamAPI_SteamUser_v*/SteamAPI_ISteamUser_GetSteamID)";
    }
    void* steamUser = user();
    if (steamUser == nullptr) {
        return "SteamUser returned null -- Steam is not initialised in this process (start the game FROM "
               "Steam, logged in)";
    }
    const unsigned long long id = getSteamId(steamUser);
    if (id == 0) {
        return "ISteamUser::GetSteamID returned 0 -- nobody is logged on to Steam in this process, so there "
               "is no ID to give a client";
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%llu", id);
    return std::string(buffer);
}

void handle_command(const std::string& line) {
    DSH_INFO("command received: '%s'", line.c_str());

    if (!g_engineCommandsAllowed.load(std::memory_order_relaxed) && command_touches_engine(line)) {
        const std::string engineCommands = engine_command_list();
        DSH_ERROR("command '%s' REFUSED: this process is not the game, and this command calls into "
                  "the engine. NOTHING was executed. Engine-touching commands are: %s.",
                  line.c_str(), engineCommands.c_str());
        return;
    }

    if (line == "parts") {

        dsh::game::request_parts_dump();
        DSH_INFO("command 'parts': asked the running session for a full dump of every doll's parts. The next "
                 "probe tick (at most 30 s away) prints it. Nothing was written.");
        return;
    }

    if (line == "hide") {

        dsh::game::request_parts_hide();
        DSH_INFO("command 'hide': asked the running session for the BLINK TEST -- it will hide this side's doll "
                 "parts one at a time for 1.2 s each and say which one in the log. WATCH THE SCREEN: whichever "
                 "step makes the frozen piece disappear names the component that draws it. Every component is "
                 "shown again by the end.");
        return;
    }

    if (line == "fix") {

        dsh::game::request_parts_fix();
        DSH_INFO("command 'fix': asked the running session to rebuild the render state of every primitive of this "
                 "side's dolls (all of them hidden for about 0.4 s, then shown again). If a part was standing "
                 "still because its render state was stale, it is back on its doll now.");
        return;
    }
    if (line == "gthook") {
        DSH_INFO("command 'gthook': installing the game-thread trampoline on demand");
        const bool ok = dsh::reflect::game_thread_start_hook();
        DSH_INFO("command 'gthook': %s -- %s", ok ? "installed" : "FAILED",
                 dsh::reflect::game_thread_report().c_str());
        return;
    }

    if (line == "ai" || line.rfind("ai ", 0) == 0) {
        const std::string rest = line.size() > 3 ? line.substr(3) : std::string();

        if (rest.rfind("hurt", 0) == 0) {
            std::string argument = rest.size() > 4 ? rest.substr(4) : std::string();
            while (!argument.empty() && (argument.front() == ' ' || argument.front() == '\t')) {
                argument.erase(argument.begin());
            }
            if (argument.empty()) {
                DSH_ERROR("command 'ai hurt': needs ONE enemy name, e.g. 'ai hurt SMG35'. Nothing was done.");
                return;
            }
            DSH_INFO("command 'ai hurt %s': calling UAISense_Damage::ReportDamageEvent(world, '%s', puppet, 1, "
                     "puppet pos, enemy pos) -- the engine's own damage sense, the one every enemy has "
                     "configured. NO health is changed; this only files a stimulus.", argument.c_str(),
                     argument.c_str());
            dsh::reflect::AiNudgeResult nudge;
            if (!dsh::reflect::run_on_game_thread(
                    [&]() { nudge = dsh::reflect::ai_report_damage_to_enemy(argument, 1.0f); }, 12000)) {
                DSH_ERROR("command 'ai hurt': the game thread did not run it within 12 s. NOTHING was done.");
                return;
            }
            for (const std::string& one : nudge.report_lines()) DSH_INFO("%s", one.c_str());
            return;
        }

        if (rest.rfind("attitudes", 0) == 0) {
            DSH_INFO("command 'ai attitudes': asking the game's OWN GetDATeamAttitude(team, NoTeam(255)) for "
                     "each of the 15 teams it declares. Read-only.");
            dsh::reflect::AiAttitudeTable table;
            if (!dsh::reflect::run_on_game_thread(
                    [&]() { table = dsh::reflect::ai_team_attitudes_towards_no_team(); }, 12000)) {
                DSH_ERROR("command 'ai attitudes': the game thread did not run it within 12 s.");
                return;
            }
            for (const std::string& one : table.report_lines()) DSH_INFO("%s", one.c_str());
            return;
        }

        if (rest.rfind("team enemy", 0) == 0) {
            std::string argument = rest.size() > 10 ? rest.substr(10) : std::string();
            while (!argument.empty() && (argument.front() == ' ' || argument.front() == '\t')) {
                argument.erase(argument.begin());
            }
            const std::size_t space = argument.find_last_of(' ');
            if (space == std::string::npos) {
                DSH_ERROR("command 'ai team enemy': needs an enemy AND a team, e.g. "
                          "'ai team enemy SMG35 100'. Nothing was done.");
                return;
            }
            int team = -1;
            std::string number = argument.substr(space + 1);
            bool digits = !number.empty();
            for (const char c : number) { if (c < '0' || c > '9') digits = false; }
            if (digits) { team = 0; for (const char c : number) team = team * 10 + (c - '0'); }
            std::string enemy = argument.substr(0, space);
            if (enemy.empty() || team < 0 || team > 255) {
                DSH_ERROR("command 'ai team enemy': needs an enemy AND a team value 0..255, e.g. "
                          "'ai team enemy SMG35 100'. Nothing was done.");
                return;
            }
            DSH_INFO("command 'ai team enemy %s %d': SetDATeam on ONE enemy, on the game thread. This is a "
                     "WRITE; `ai` afterwards shows the numbers, `ai team enemy %s 10` puts a Metametria enemy "
                     "back where it was.", enemy.c_str(), team, enemy.c_str());
            dsh::reflect::AiNudgeResult nudge;
            if (!dsh::reflect::run_on_game_thread(
                    [&]() { nudge = dsh::reflect::ai_set_enemy_team(enemy, team); }, 12000)) {
                DSH_ERROR("command 'ai team enemy': the game thread did not run it within 12 s. NOTHING was "
                          "done.");
                return;
            }
            for (const std::string& one : nudge.report_lines()) DSH_INFO("%s", one.c_str());
            return;
        }

        if (rest.rfind("push", 0) == 0 || rest.rfind("see", 0) == 0 || rest.rfind("register", 0) == 0) {
            const bool isPush = rest.rfind("push", 0) == 0;
            const bool isSee = rest.rfind("see", 0) == 0;
            const std::size_t verbLength = isPush ? 4u : (isSee ? 3u : 8u);
            std::string argument = rest.size() > verbLength ? rest.substr(verbLength) : std::string();
            while (!argument.empty() && (argument.front() == ' ' || argument.front() == '\t')) {
                argument.erase(argument.begin());
            }
            bool enable = true;
            if (!isPush) {
                const std::size_t off = argument.find(" off");
                if (off != std::string::npos) {
                    enable = false;
                    argument = argument.substr(0, off);
                }
                const std::size_t on = argument.find(" on");
                if (on != std::string::npos) argument = argument.substr(0, on);
            }
            while (!argument.empty() && argument.back() == ' ') argument.pop_back();
            if (argument.empty()) {
                DSH_ERROR("command 'ai %s': needs ONE enemy name (or a fragment that matches exactly one). "
                          "Nothing was done.", isPush ? "push" : (isSee ? "see" : "register"));
                return;
            }
            const char* verb = isPush ? "push" : (isSee ? "see" : "register");
            DSH_INFO("command 'ai %s %s%s': ONE enemy, on the game thread. %s", verb, argument.c_str(),
                     isPush ? "" : (enable ? " on" : " off"),
                     isPush ? ("feeding ADAAIController::ModifyTargetingSystemTarget one synthetic perception "
                               "update saying the puppet was just sensed")
                            : (isSee ? (enable ? "setting bDetectFriendlies on every sense config of its that has "
                                                 "an affiliation filter"
                                               : "clearing bDetectFriendlies on those same configs")
                                     : (enable ? "RegisterSourceToAIPerceptionSystem(puppet, sense) for each of "
                                                 "the senses that enemy listens with"
                                               : "UnregisterSourceToAIPerceptionSystem for those same senses")));
            dsh::reflect::AiNudgeResult nudge;
            const bool ran = dsh::reflect::run_on_game_thread(
                [&]() {
                    nudge = isPush ? dsh::reflect::ai_push_perception(argument)
                                   : (isSee ? dsh::reflect::ai_set_enemy_friendlies(argument, enable)
                                            : dsh::reflect::ai_register_puppet(argument, enable));
                },
                12000);
            if (!ran) {
                DSH_ERROR("command 'ai %s': the game thread did not run it within 12 s -- is the hook "
                          "installed? type 'gthook' first. NOTHING was done.", verb);
                return;
            }
            for (const std::string& one : nudge.report_lines()) DSH_INFO("%s", one.c_str());
            return;
        }

        if (rest.rfind("team", 0) == 0) {
            std::string argument = rest.size() > 4 ? rest.substr(4) : std::string();
            while (!argument.empty() && (argument.front() == ' ' || argument.front() == '\t')) {
                argument.erase(argument.begin());
            }
            int team = -1;
            if (argument == "player") {
                team = 0;
            } else if (argument == "noteam") {
                team = 255;
            } else if (!argument.empty()) {
                int parsed = 0;
                bool digitsOnly = true;
                for (const char c : argument) {
                    if (c < '0' || c > '9') { digitsOnly = false; break; }
                    parsed = parsed * 10 + (c - '0');
                }
                if (digitsOnly) team = parsed;
            }
            if (team < 0 || team > 255) {
                DSH_ERROR("command 'ai team': needs a team from the game's own EDATeam -- 'ai team player' (0) "
                          "or 'ai team noteam' (255). Nothing was written.");
                return;
            }
            DSH_INFO("command 'ai team %d': putting the PUPPET on team %d through the game's own "
                     "SetDATeam(AActor*, const EDATeam&), then reading it back and re-running the whole probe. "
                     "This is a WRITE, on the game thread, and it is the branch the 477-1 numbers pointed at "
                     "(the puppet is NoTeam, and the game's own attitude function calls it Friendly).",
                     team, team);
            dsh::reflect::AiTeamWriteResult teamWrite;
            dsh::reflect::AiTargetingSurvey afterWrite;
            const bool ran = dsh::reflect::run_on_game_thread(
                [&]() {
                    teamWrite = dsh::reflect::ai_set_puppet_team(team);
                    afterWrite = dsh::reflect::survey_ai_targeting(3);
                },
                12000);
            if (!ran) {
                DSH_ERROR("command 'ai team': the game thread did not run it within 12 s -- is the hook "
                          "installed? type 'gthook' first. NOTHING was written.");
                return;
            }
            for (const std::string& one : teamWrite.report_lines()) DSH_INFO("%s", one.c_str());
            for (const std::string& one : afterWrite.report_lines()) DSH_INFO("%s", one.c_str());
            return;
        }

        std::uint32_t detail = 3;

        if (!rest.empty()) {
            bool digitsOnly = true;
            for (const char c : rest) {
                if (c < '0' || c > '9') { digitsOnly = false; break; }
            }
            if (!digitsOnly) {
                DSH_ERROR("command 'ai': '%s' is not something this command takes, so NOTHING was done. It "
                          "accepts: 'ai' (the probe), 'ai <1..8>' (how many enemies to detail), "
                          "'ai team player' (put the puppet on team Player(0) through the game's own "
                          "SetDATeam), and 'ai team noteam' (back to NoTeam(255)).", rest.c_str());
                return;
            }
        }
        if (line.size() >= 4 && line[3] >= '0' && line[3] <= '9') {
            detail = static_cast<std::uint32_t>(line[3] - '0');
        }
        DSH_INFO("command 'ai': asking the GAME what it thinks of the puppet -- its team, its senses' "
                 "affiliation filter, its perception list, its candidate list with bFiltered, and then its own "
                 "CheckCanditateTarget and CheckFilteredTarget with each K2_* rule. %u nearest enemy/enemies "
                 "detailed. Read-only; nothing is written and any side effect of asking is printed.",
                 detail);
        dsh::reflect::AiTargetingSurvey aiSurvey;
        if (!dsh::reflect::run_on_game_thread(
                [&]() { aiSurvey = dsh::reflect::survey_ai_targeting(detail); }, 8000)) {
            DSH_ERROR("command 'ai': the game thread did not run the probe within 8 s -- is the hook installed? "
                      "type 'gthook' first. NOTHING was measured.");
            return;
        }
        for (const std::string& one : aiSurvey.report_lines()) DSH_INFO("%s", one.c_str());
        return;
    }

    if (line.rfind("objdiff", 0) == 0) {

        if (line.find(" mark") != std::string::npos) {
            DSH_INFO("command 'objdiff': %s", dsh::reflect::obj_diff_mark().c_str());
        } else if (line.find(" report") != std::string::npos || line == "objdiff") {
            DSH_INFO("command 'objdiff': %s", dsh::reflect::obj_diff_report().c_str());
        } else {
            DSH_WARN("command 'objdiff': use 'objdiff mark' before the load and 'objdiff report' "
                     "after");
        }
        return;
    }

    if (line.rfind("mission", 0) == 0) {
        if (line.find("verbs") != std::string::npos) {
            DSH_INFO("command 'mission verbs': asking the three mission classes for six start-shaped verbs. "
                     "This walks the object table once per name, so it takes several seconds by design.");
            for (const std::string& one : dsh::reflect::survey_mission_verbs()) DSH_INFO("%s", one.c_str());
            return;
        }        if (line.find("probe") != std::string::npos) {
            DSH_INFO("command 'mission probe': listing every AI-driven character with its controller, so a boss that was beaten but not destroyed can be spotted. Read-only.");
            for (const std::string& one : dsh::reflect::survey_mission_actors()) DSH_INFO("%s", one.c_str());
            return;
        }

        if (line.rfind("mission uncomplete", 0) == 0) {
            auto uncomplete_trim = [](std::string s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                while (!s.empty() &&
                       (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
                return s;
            };
            const std::string id = uncomplete_trim(line.substr(std::string("mission uncomplete").size()));
            if (id.empty()) {
                DSH_ERROR("command 'mission uncomplete': needs at least one mission id, e.g. "
                          "'mission uncomplete M01A01_Boss'. Nothing was written.");
                return;
            }

            DSH_INFO("command 'mission uncomplete': putting the named record(s) back to Attempted(1), then the "
                     "GAME writes its own save. Only the missions the HOST has not cleared should be named "
                     "here -- this machine's other bosses are left exactly as they are. Nothing is put back "
                     "afterwards: the game writes Cleared itself when a boss is beaten.");
            std::vector<std::string> ids;
            {
                std::string rest = id;
                while (!rest.empty()) {
                    const std::size_t space = rest.find_first_of(" \t");
                    const std::string token =
                        space == std::string::npos ? rest : rest.substr(0, space);
                    if (!token.empty()) ids.push_back(token);
                    rest = space == std::string::npos ? std::string() : uncomplete_trim(rest.substr(space));
                }
            }
            std::uint32_t written = 0;
            for (const std::string& oneId : ids) {
                const dsh::reflect::BossStateWriteResult write =
                    dsh::reflect::set_boss_state(oneId, 1);
                for (const std::string& one : write.report_lines()) DSH_INFO("%s", one.c_str());
                if (write.written || (write.beforeMeasured && write.before == 1)) ++written;
            }
            if (written == 0) {
                DSH_WARN("command 'mission uncomplete': no record was written, so the save was NOT asked for");
                return;
            }
            DSH_INFO("command 'mission uncomplete': %u of %u named record(s) are now Attempted(1)",
                     written, static_cast<std::uint32_t>(ids.size()));
            for (const std::string& one : dsh::reflect::mission_save_game()) DSH_INFO("%s", one.c_str());
            return;
        }

        if (line.rfind("mission save", 0) == 0) {
            DSH_INFO("command 'mission save': asking the GAME to write its own save -- first "
                     "CheatManager::SaveGame, then DAGameInstance::ReceiveSave(0, 1, 1) if there is no "
                     "cheat manager (the normal case in a Shipping build). Then load that save again.");
            for (const std::string& one : dsh::reflect::mission_save_game()) {
                DSH_INFO("%s", one.c_str());
            }
            return;
        }

        if (line.rfind("mission collide", 0) == 0) {
            auto collide_trim = [](std::string s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                while (!s.empty() &&
                       (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
                return s;
            };
            const std::string part = collide_trim(line.substr(std::string("mission collide").size()));
            if (part.empty()) {
                DSH_ERROR("command 'mission collide': needs a name part, e.g. 'mission collide SEC_M01A01'. "
                          "Nothing was written.");
                return;
            }
            DSH_INFO("command 'mission collide': reading Mesh and Capsule collision "
                     "state, then SetCollisionEnabled(QueryAndPhysics) and "
                     "SetCollisionResponseToAllChannels(Block) on both, for the one AI character matching "
                     "'%s'.",
                     part.c_str());
            for (const std::string& one : dsh::reflect::mission_collide(part)) {
                DSH_INFO("%s", one.c_str());
            }
            return;
        }

        if (line.rfind("mission brain", 0) == 0) {
            auto brain_trim = [](std::string s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                while (!s.empty() &&
                       (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
                return s;
            };
            const std::string args = brain_trim(line.substr(std::string("mission brain").size()));
            if (args.empty()) {
                DSH_ERROR("command 'mission brain': needs a character name part, e.g. 'mission brain "
                          "SEC_M01A01' (optionally followed by a behavior tree name part). Nothing was read.");
                return;
            }
            std::string charPart = args;
            std::string treePart;
            const std::size_t lastSpace = args.find_last_of(" \t");
            if (lastSpace != std::string::npos) {
                charPart = brain_trim(args.substr(0, lastSpace));
                treePart = brain_trim(args.substr(lastSpace + 1));
            }
            DSH_INFO("command 'mission brain': reading the controller's BrainComponent and the behavior "
                     "tree assets%s.",
                     treePart.empty() ? " (nothing is run)" : (", then running the first one matching '" + treePart + "'").c_str());
            for (const std::string& one : dsh::reflect::mission_brain(charPart, treePart)) {
                DSH_INFO("%s", one.c_str());
            }
            return;
        }

        if (line.rfind("mission revive", 0) == 0) {
            auto revive_trim = [](std::string s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                while (!s.empty() &&
                       (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
                return s;
            };
            const std::string part = revive_trim(line.substr(std::string("mission revive").size()));
            if (part.empty()) {
                DSH_ERROR("command 'mission revive': needs a name part, e.g. 'mission revive SEC_M01A01'. "
                          "Nothing was written.");
                return;
            }
            DSH_INFO("command 'mission revive': health -> max (written AND announced, the same two steps the "
                     "health mirror uses), SetActorEnableCollision(true), BootAI, "
                     "ChangeAIBehaviorState(Battle) -- on the one AI character matching '%s'. Every field is "
                     "printed before and after.",
                     part.c_str());
            const dsh::reflect::BossReviveResult revive = dsh::reflect::mission_revive_boss(part);
            for (const std::string& one : revive.report_lines()) DSH_INFO("%s", one.c_str());
            return;
        }

        if (line.rfind("mission trigger", 0) == 0) {
            auto trigger_trim = [](std::string s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                while (!s.empty() &&
                       (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
                return s;
            };
            const std::string id = trigger_trim(line.substr(std::string("mission trigger").size()));
            if (id.empty()) {
                DSH_ERROR("command 'mission trigger': needs a mission id, e.g. 'mission trigger "
                          "M01A01_Boss'. Nothing was called.");
                return;
            }
            DSH_INFO("command 'mission trigger': replaying the volume's own overlap events with the local "
                     "pawn (leave, then enter). This writes nothing.");
            for (const std::string& one : dsh::reflect::trigger_mission_volume(id)) {
                DSH_INFO("%s", one.c_str());
            }
            return;
        }

        if (line.rfind("mission levels", 0) == 0) {
            auto levels_trim = [](std::string s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                while (!s.empty() &&
                       (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
                return s;
            };
            const std::string id = levels_trim(line.substr(std::string("mission levels").size()));
            if (id.empty()) {
                DSH_ERROR("command 'mission levels': needs a mission id, e.g. 'mission levels "
                          "M01A01_Boss'. Nothing was read.");
                return;
            }
            DSH_INFO("command 'mission levels': reading the mission volume's three stream-level sets and "
                     "the levels that are loaded right now. Read-only.");
            for (const std::string& one : dsh::reflect::survey_mission_levels(id)) {
                DSH_INFO("%s", one.c_str());
            }
            return;
        }

        if (line.rfind("mission relaunch", 0) == 0) {
            auto relaunch_trim = [](std::string s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                while (!s.empty() &&
                       (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
                return s;
            };
            const std::string id = relaunch_trim(line.substr(std::string("mission relaunch").size()));
            if (id.empty()) {
                DSH_ERROR("command 'mission relaunch': needs a mission id, e.g. 'mission relaunch "
                          "M01A01_Boss'. Nothing was called.");
                return;
            }
            DSH_INFO("command 'mission relaunch': record -> Attempted(1), then the volume's mission "
                     "manager is re-initialised with that state, then StartMissionMode. One command, three "
                     "steps, each printed.");
            const dsh::reflect::MissionRelaunchResult relaunch =
                dsh::reflect::relaunch_mission(id);
            for (const std::string& one : relaunch.report_lines()) DSH_INFO("%s", one.c_str());
            return;
        }

        if (line.rfind("mission setstate", 0) == 0) {
            auto state_trim = [](std::string s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                while (!s.empty() &&
                       (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
                return s;
            };
            const std::string args = state_trim(line.substr(std::string("mission setstate").size()));
            std::string id = args;
            std::string stateText;
            const std::size_t lastSpace = args.find_last_of(" \t");
            if (lastSpace != std::string::npos) {
                id = state_trim(args.substr(0, lastSpace));
                stateText = state_trim(args.substr(lastSpace + 1));
            }
            int state = -1;
            if (stateText == "unattempted" || stateText == "0") state = 0;
            else if (stateText == "attempted" || stateText == "1") state = 1;
            else if (stateText == "cleared" || stateText == "2") state = 2;
            if (id.empty() || state < 0) {
                DSH_ERROR("command 'mission setstate': needs a mission id and a state, e.g. 'mission "
                          "setstate M01A01_Boss attempted' (unattempted|attempted|cleared). Nothing was "
                          "written.");
                return;
            }
            DSH_INFO("command 'mission setstate': writing the mission record for '%s' as %s, in the loaded "
                     "save object only. The game's own accessor is read before and after, so a miss cannot "
                     "look like a success.",
                     id.c_str(), state == 0 ? "Unattempted(0)" : (state == 1 ? "Attempted(1)" : "Cleared(2)"));
            const dsh::reflect::BossStateWriteResult write =
                dsh::reflect::set_boss_state(id, static_cast<std::uint8_t>(state));
            for (const std::string& one : write.report_lines()) DSH_INFO("%s", one.c_str());
            return;
        }

        if (line.rfind("mission ai", 0) == 0) {
            auto ai_trim = [](std::string s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                while (!s.empty() &&
                       (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
                return s;
            };
            const std::string part = ai_trim(line.substr(std::string("mission ai").size()));
            DSH_INFO("command 'mission ai': reading every AI character%s and asking each controller for its "
                     "behaviour state, whether it thinks a battle should start, and who its main target is "
                     "-- plus the game mode's IsMissionMode/IsDuringCutscene. Read-only; resolving the "
                     "verbs walks the object table, so this takes a few seconds by design.",
                     part.empty() ? "" : (" matching '" + part + "'").c_str());
            for (const std::string& one : dsh::reflect::survey_boss_ai(part)) DSH_INFO("%s", one.c_str());
            return;
        }

        if (line.rfind("mission wake", 0) == 0) {
            auto wake_trim = [](std::string s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                while (!s.empty() &&
                       (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
                return s;
            };
            const std::string part = wake_trim(line.substr(std::string("mission wake").size()));
            if (part.empty()) {
                DSH_ERROR("command 'mission wake': needs a name part, e.g. 'mission wake SEC_M01A01'. "
                          "Nothing was called.");
                return;
            }
            DSH_INFO("command 'mission wake': calling BootAI + ChangeAIBehaviorState(Battle(1)) on the one "
                     "AI character matching '%s', on the game thread. Watch it: attacking means the AI is "
                     "the whole remaining problem, and no mission state has to be touched.",
                     part.c_str());
            const dsh::reflect::BossWakeResult wake = dsh::reflect::mission_wake_boss(part);
            for (const std::string& one : wake.lines) DSH_INFO("%s", one.c_str());
            if (!wake.note.empty()) DSH_WARN("command 'mission wake': NOTHING WAS CHANGED -- %s",
                                             wake.note.c_str());
            return;
        }

        if (line.rfind("mission start", 0) == 0) {

            auto mission_trim = [](std::string s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                while (!s.empty() &&
                       (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
                return s;
            };
            const std::string args =
                mission_trim(line.substr(std::string("mission start").size()));
            if (args.empty()) {
                DSH_ERROR("command 'mission start': needs a mission id, e.g. 'mission start "
                          "M01A01_Boss01' or 'mission start M01A01_Boss01 arena'. Run 'mission' first "
                          "to see the ids this level declares. Nothing was called.");
                return;
            }
            std::string id = args;
            std::uint8_t type = 0;
            const std::size_t lastSpace = args.find_last_of(" \t");
            if (lastSpace != std::string::npos) {
                const std::string tail = mission_trim(args.substr(lastSpace + 1));
                if (tail == "boss" || tail == "0") {
                    type = 0;
                    id = mission_trim(args.substr(0, lastSpace));
                } else if (tail == "arena" || tail == "1") {
                    type = 1;
                    id = mission_trim(args.substr(0, lastSpace));
                } else {
                    DSH_ERROR("command 'mission start': '%s' is not a mission type -- use 'boss' (default) "
                              "or 'arena'. Nothing was called.", tail.c_str());
                    return;
                }
            }
            if (id.empty()) {
                DSH_ERROR("command 'mission start': the mission id is empty. Nothing was called.");
                return;
            }
            DSH_INFO("command 'mission start': asking the game to start mission mode for '%s' as %s. "
                     "One UFunction is called, on the game thread; the readings before and after it are "
                     "the game's own, and nothing is ended, cleared or written to a save.",
                     id.c_str(), type == 0 ? "Boss(0)" : "Arena(1)");
            const dsh::reflect::MissionStartOutcome outcome =
                dsh::reflect::start_mission_mode(id, type);
            for (const std::string& one : outcome.report_lines()) DSH_INFO("%s", one.c_str());
            return;
        }

        DSH_INFO("command 'mission': reading the world's mission volumes and what the loaded save says "
                 "about each of them -- one object-array walk, one level walk, and the game's own "
                 "GetBossState/GetDefeatedBossNum. Nothing is sent anywhere by this command.");
        const dsh::reflect::MissionTable table = dsh::reflect::read_mission_table();
        for (const std::string& reportLine : table.report_lines()) {
            DSH_INFO("mission: %s", reportLine.c_str());
        }
        return;
    }

    if (line.rfind("world", 0) == 0) {
        auto boss_trim = [](std::string s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
            return s;
        };

        if (line.rfind("world bossmove", 0) == 0) {
            const std::string args = boss_trim(line.substr(std::string("world bossmove").size()));
            std::string namePart = args;
            float distanceCm = 5000.0f;
            const std::size_t lastSpace = args.find_last_of(" \t");
            if (lastSpace != std::string::npos) {
                const std::string tail = boss_trim(args.substr(lastSpace + 1));
                const char* tailBegin = tail.c_str();
                char* tailEnd = nullptr;
                const float parsed = std::strtof(tailBegin, &tailEnd);
                if (tailEnd != tailBegin && std::isfinite(parsed) && parsed > 0.0f) {
                    distanceCm = parsed;
                    namePart = boss_trim(args.substr(0, lastSpace));
                }
            }
            if (namePart.empty()) {
                DSH_ERROR("command 'world bossmove': needs a name part, e.g. 'world bossmove SEC_M01A01 5000'.");
                return;
            }
            const dsh::reflect::BossMoveResult r =
                dsh::reflect::move_ai_character_near_player(namePart, distanceCm);
            if (!r.applyOk && r.hadAfter) {
                DSH_ERROR("command 'world bossmove': %s", r.note.c_str());
                return;
            }
            DSH_INFO("command 'world bossmove': '%s' %s at (%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f), now %.0f cm "
                     "from you (asked %.0f cm)%s",
                     r.name.c_str(), r.applyOk ? "MOVED" : "was NOT moved",
                     static_cast<double>(r.beforeX), static_cast<double>(r.beforeY), static_cast<double>(r.beforeZ),
                     static_cast<double>(r.afterX), static_cast<double>(r.afterY), static_cast<double>(r.afterZ),
                     static_cast<double>(r.distanceToPawnCm), static_cast<double>(distanceCm),
                     r.note.empty() ? "" : (" -- " + r.note).c_str());
            return;
        }

        if (line.rfind("world bootai", 0) == 0) {
            const std::string namePart = boss_trim(line.substr(std::string("world bootai").size()));
            if (namePart.empty()) {
                DSH_ERROR("command 'world bootai': needs a name part, e.g. 'world bootai SEC_M01A01'.");
                return;
            }
            const dsh::reflect::AiBootResult r = dsh::reflect::boot_one_ai_character(namePart);
            if (!r.made) DSH_ERROR("command 'world bootai': %s", r.note.c_str());
            else DSH_INFO("command 'world bootai': %s -- watch it: standing up and attacking means the AI path "
                          "works and no mission state has to be touched", r.note.c_str());
            return;
        }

        auto command_trim = [](std::string s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
            return s;
        };

        if (line.rfind("world hurt", 0) == 0) {
            const std::string args = command_trim(line.substr(std::string("world hurt").size()));
            const std::size_t lastSpace = args.find_last_of(" \t");
            if (lastSpace == std::string::npos) {
                DSH_ERROR("command 'world hurt': needs a name part and an amount, e.g. 'world hurt "
                          "Middle_M01 1000'. Nothing was called.");
                return;
            }
            const std::string namePart = command_trim(args.substr(0, lastSpace));
            const std::string amountText = command_trim(args.substr(lastSpace + 1));
            const char* amountBegin = amountText.c_str();
            char* amountEnd = nullptr;
            const float amount = std::strtof(amountBegin, &amountEnd);
            if (namePart.empty() || amountEnd == amountBegin || !std::isfinite(amount) || amount <= 0.0f) {
                DSH_ERROR("command 'world hurt': '%s' is not a positive amount, or the name part is "
                          "empty. Nothing was called.", amountText.c_str());
                return;
            }
            DSH_INFO("command 'world hurt': asking the game to apply %.1f damage to the one enemy whose "
                     "name contains '%s', through `DAGameplayStatics.ApplyDADamage` from the game thread "
                     "(needs 'gthook').", static_cast<double>(amount), namePart.c_str());
            const dsh::reflect::HpDamageProbe probe =
                dsh::reflect::hp_probe_apply_damage(namePart, amount);
            if (!probe.resolved || !probe.callMade) {
                DSH_ERROR("command 'world hurt': NOTHING WAS APPLIED -- %s", probe.note.c_str());
                return;
            }
            DSH_INFO("command 'world hurt': target '%s' (%s). %s", probe.name.c_str(),
                     probe.actorClass.c_str(), probe.note.c_str());
            if (!probe.readAfter) {
                DSH_WARN("command 'world hurt': the health could not be read back, so the log cannot say "
                         "whether it moved -- WATCH THE ENEMY.");
            } else if (probe.before - probe.after > 0.5f) {
                DSH_INFO("command 'world hurt': IT WORKED -- health %.1f -> %.1f (-%.1f). Now watch the "
                         "enemy itself: flinch, and if the number was enough, death and drops.",
                         static_cast<double>(probe.before), static_cast<double>(probe.after),
                         static_cast<double>(probe.before - probe.after));
            } else {
                DSH_WARN("command 'world hurt': NO HEALTH CHANGE (%.1f -> %.1f). bEffective=%s: the "
                         "function ran and the target took nothing, which means the damage number does "
                         "not reach the attribute through this static.",
                         static_cast<double>(probe.before), static_cast<double>(probe.after),
                         probe.effective ? "true" : "false");
            }
            return;
        }

        if (line.rfind("world hpnotify", 0) == 0) {
            const std::string args = command_trim(line.substr(std::string("world hpnotify").size()));
            std::string namePart = args;
            float value = 0.0f;
            const std::size_t lastSpace = args.find_last_of(" \t");
            if (lastSpace != std::string::npos) {
                const std::string tail = command_trim(args.substr(lastSpace + 1));
                const char* tailBegin = tail.c_str();
                char* tailEnd = nullptr;
                const float parsed = std::strtof(tailBegin, &tailEnd);
                if (tailEnd != tailBegin && std::isfinite(parsed) && parsed > 0.0f) {
                    value = parsed;
                    namePart = boss_trim(args.substr(0, lastSpace));
                }
            }
            if (namePart.empty()) {
                DSH_ERROR("command 'world hpnotify': needs a name part, e.g. 'world hpnotify Middle_M01' "
                          "or 'world hpnotify Middle_M01 12000'. Nothing was written.");
                return;
            }
            DSH_INFO("command 'world hpnotify': writing %s to the one enemy whose name contains '%s', "
                     "then calling `OnRep_Health` on the game thread (needs 'gthook').",
                     value > 0.0f ? "that value" : "70% of the value found", namePart.c_str());
            const dsh::reflect::HpNotifyProbe probe =
                dsh::reflect::hp_probe_set_and_notify(namePart, value);
            if (!probe.resolved || !probe.writeLanded) {
                DSH_ERROR("command 'world hpnotify': NOTHING WAS WRITTEN -- %s", probe.note.c_str());
                return;
            }
            DSH_INFO("command 'world hpnotify': target '%s'. %s", probe.name.c_str(), probe.note.c_str());
            DSH_INFO("command 'world hpnotify': LOOK AT THE SCREEN. If the number moved, a write plus this "
                     "notify is enough and the mirror needs no damage call. If it did not, the notify is "
                     "not the path the HUD listens to and the next question is which delegate it does.");
            return;
        }

        if (line.rfind("world hp near", 0) == 0) {
            std::size_t maxLines = 12;
            const std::string tail = command_trim(line.substr(std::string("world hp near").size()));
            if (!tail.empty()) {
                const char* tailBegin = tail.c_str();
                char* tailEnd = nullptr;
                const long parsed = std::strtol(tailBegin, &tailEnd, 10);
                if (tailEnd != tailBegin && parsed > 0) maxLines = static_cast<std::size_t>(parsed);
            }
            DSH_INFO("command 'world hp near': listing the enemies the probes can resolve, nearest first.");
            for (const std::string& one : dsh::reflect::hp_probe_near_list(maxLines)) {
                DSH_INFO("%s", one.c_str());
            }
            return;
        }

        if (line.rfind("world net", 0) == 0) {
            const std::string netArgs = line.substr(std::string("world net").size());

            if (netArgs.find("steamid") != std::string::npos) {
                const std::string id = steam_id64_text();
                DSH_INFO("command 'world net steamid': %s", id.c_str());
                if (id.find_first_not_of("0123456789") == std::string::npos) {
                    DSH_INFO("command 'world net steamid': this machine's SteamID64 is '%s'. On the OTHER machine "
                             "run: world net connect %s", id.c_str(), id.c_str());
                }
                return;
            }

            if (netArgs.find("preferip") != std::string::npos) {
                const dsh::reflect::DriverClassSwap swap = dsh::reflect::prefer_ip_net_driver();
                if (!swap.swapped) {
                    DSH_ERROR("command 'world net preferip': NOT swapped -- %s", swap.note.c_str());
                } else {
                    DSH_INFO("command 'world net preferip': definition '%s' class '%s' -> '%s'%s. The NEXT "
                             "'world net listen' asks the engine for that driver; nothing has travelled yet.",
                             swap.defName.c_str(), swap.before.c_str(), swap.after.c_str(),
                             swap.note.empty() ? "" : (" [" + swap.note + "]").c_str());
                }
                return;
            }
            if (netArgs.find("restoreip") != std::string::npos) {
                const dsh::reflect::DriverClassSwap back = dsh::reflect::restore_net_driver_class();
                if (!back.swapped) {
                    DSH_ERROR("command 'world net restoreip': NOT restored -- %s", back.note.c_str());
                } else {
                    DSH_INFO("command 'world net restoreip': definition '%s' class '%s' -> '%s' restored.",
                             back.defName.c_str(), back.before.c_str(), back.after.c_str());
                }
                return;
            }

            const std::size_t connectAt = netArgs.find("connect");
            const bool isConnect = connectAt != std::string::npos;

            if (netArgs.find("drivers") != std::string::npos) {
                const dsh::reflect::NetDriverDefinitionSurvey drivers =
                    dsh::reflect::survey_net_driver_definitions();
                DSH_INFO("command 'world net drivers': %s", drivers.line().c_str());
                if (drivers.definitions.empty()) {
                    DSH_WARN("command 'world net drivers': NOTHING could be listed, so which driver '?listen' "
                             "builds is still unknown -- see the note on the line above.");
                } else {
                    DSH_INFO("command 'world net drivers': '?listen' builds the definition named 'GameNetDriver'. "
                             "If none of the above is an IpNetDriver, then an address-based connection is NOT "
                             "configured in this build and 'world net connect <ip>' cannot work through the "
                             "engine's own driver.");
                }
                return;
            }
            const std::size_t verbAt = isConnect ? connectAt : netArgs.find("listen");
            if (verbAt == std::string::npos) {
                DSH_WARN("command 'world net': use 'world net listen <map> [options]' on the HOST, or "
                         "'world net connect <address>' on the CLIENT. Nothing was done.");
                return;
            }
            std::vector<std::string> words;
            std::string current;
            for (const char c : netArgs.substr(verbAt + (isConnect ? 7 : 6))) {
                if (c == ' ' || c == '\t') {
                    if (!current.empty()) {
                        words.push_back(current);
                        current.clear();
                    }
                } else {
                    current.push_back(c);
                }
            }
            if (!current.empty()) words.push_back(current);
            if (words.empty()) {
                DSH_WARN("command 'world net %s': nothing was named. Use 'world net listen <map> [options]' or "
                         "'world net connect <address>'. Nothing was done.", isConnect ? "connect" : "listen");
                return;
            }
            if (isConnect) {

                g_hpWatchArmed = false;
                g_hpPinArmed = false;
                const dsh::reflect::NetLayerState before = dsh::reflect::net_layer_state();
                DSH_INFO("command 'world net connect %s': BEFORE -- %s", words[0].c_str(),
                         before.line().c_str());
                std::string connectNote;
                const bool tried = dsh::reflect::net_client_connect(words[0], connectNote);
                DSH_INFO("command 'world net connect': AT-ONCE AFTER -- %s",
                         dsh::reflect::net_layer_state().line().c_str());
                if (!tried) {
                    DSH_ERROR("command 'world net connect': NOTHING was attempted -- %s", connectNote.c_str());
                } else {
                    net_watch_begin(before.world, before.netDriver);
                    DSH_INFO("command 'world net connect': the client travel was handed to the engine. A connection "
                             "shows up as the world changing AND a NetDriver appearing in a 'net watch' sample; "
                             "a refused connection leaves this world alone, which is why the samples, not this "
                             "line, are the answer.");
                }
                return;
            }
            const std::string mapName = words[0];

            bool preferIp = false;
            if (words.size() > 1 && words[1] == "ip") preferIp = true;
            const std::string options = (words.size() > 1 && !preferIp) ? words[1] : "listen";
            if (preferIp) {
                const dsh::reflect::DriverClassSwap swap = dsh::reflect::prefer_ip_net_driver();
                if (!swap.swapped) {
                    DSH_ERROR("command 'world net listen %s ip': the driver class could NOT be swapped (%s), so "
                              "this travel would build the Steam driver. NOTHING has travelled.", mapName.c_str(),
                              swap.note.c_str());
                    return;
                }
                DSH_INFO("command 'world net listen %s ip': definition '%s' class '%s' -> '%s' -- the travel below "
                         "asks the engine for the ADDRESS-BASED driver.", mapName.c_str(), swap.defName.c_str(),
                         swap.before.c_str(), swap.after.c_str());
            }

            g_hpWatchArmed = false;
            g_hpPinArmed = false;

            const dsh::reflect::NetLayerState before = dsh::reflect::net_layer_state();
            DSH_INFO("command 'world net listen %s' (options '%s'): BEFORE -- %s", mapName.c_str(),
                     options.c_str(), before.line().c_str());
            DSH_INFO("command 'world net listen': this TRAVELS. The world this process is standing in is about "
                     "to be replaced, so nothing queued on the game thread is allowed to run while it happens "
                     "(that is the gamethread pause, and it is what the 2026-10-18 crash taught).");

            std::string note;
            const bool invoked = dsh::reflect::net_open_level_listen(mapName, options, note);
            const dsh::reflect::NetLayerState after = dsh::reflect::net_layer_state();
            DSH_INFO("command 'world net listen': AT-ONCE AFTER -- %s", after.line().c_str());
            DSH_INFO("command 'world net listen': that read was taken the instant the call returned. A travel is "
                     "NOT obliged to have happened by then -- and on 2026-10-18 this very read said 'unchanged, "
                     "no NetDriver' while the crash stack showed the travel's own teardown running. The NET "
                     "WATCH below is the measurement; this line is only the starting point.");
            if (!invoked) {
                DSH_ERROR("command 'world net listen': NOTHING was travelled to -- %s", note.c_str());
                DSH_ERROR("command 'world net listen': the engine's UFunction must run ON THE GAME THREAD, and "
                          "that trampoline is installed on request, not on demand (patching a function while the "
                          "process is busy is what took the client down before). If the log above says it is "
                          "absent, run 'gthook' first, then this command again.");
            } else {
                net_watch_begin(before.world, before.netDriver);
                DSH_INFO("command 'world net listen': the call was made. Read the next 'net watch:' line -- a "
                         "CHANGE means the travel took effect (and its text says whether a NetDriver came with "
                         "it); '0 change(s)' when the window closes means it never did. Try "
                         "'world net listen %s ?listen' if the option was the problem.", mapName.c_str());
            }
            return;
        }

        if (line.find("enemies") != std::string::npos) {
            DSH_INFO("command 'world enemies': listing every AI-driven character in the loaded levels, "
                     "with the actor's own name and its location -- the two candidates for the identity two "
                     "machines have to agree on. Nothing is written and nothing is moved.");
            const dsh::reflect::EnemySurvey survey = dsh::reflect::survey_enemies();
            for (const std::string& reportLine : survey.report_lines()) {
                DSH_INFO("world: %s", reportLine.c_str());
            }
            return;
        }

        if (line.find("gates") != std::string::npos) {
            DSH_INFO("command 'world gates': listing every actor in the loaded levels whose CLASS NAME "
                     "contains a door/wall/gate word, with the actor's own name and location. Read-only; no "
                     "verb is guessed at.");
            const dsh::reflect::GateSurvey gates = dsh::reflect::survey_gates();
            for (const std::string& reportLine : gates.report_lines()) {
                DSH_INFO("world: %s", reportLine.c_str());
            }
            return;
        }

        if (line.find("aggro") != std::string::npos) {
            DSH_INFO("command 'world aggro': printing, per AI-driven character, its main target, its WHOLE "
                     "candidate list with each candidate's score and the game's own bFiltered flag, and how "
                     "many actors its perception knows. Read-only; nothing is written, moved or called except "
                     "the two read-back getters named in each line.");
            const dsh::reflect::AggroSurvey aggro = dsh::reflect::survey_aggro();
            for (const std::string& reportLine : aggro.report_lines()) {
                DSH_INFO("world: %s", reportLine.c_str());
            }
            return;
        }

        if (line.find("offer") != std::string::npos) {
            DSH_INFO("command 'world offer': adding this machine's PUPPET(s) to every AI enemy's candidate list "
                     "where they are not already there -- no clearing, no forcing, no NPC touched. Use it in a "
                     "session, on BOTH machines, with both dolls near the SAME enemy; then "
                     "'world target watch on', stand together for 60 s, then 'world target watch off'.");
            const dsh::reflect::TargetOfferReport offer = dsh::reflect::offer_puppets_as_candidates();
            for (const std::string& one : offer.report_lines()) DSH_INFO("world: %s", one.c_str());
            return;
        }

        if (line.find("target") != std::string::npos) {
            if (line.find("off") == std::string::npos) {
                const dsh::reflect::TargetWatchArm arm = dsh::reflect::target_watch_arm();
                if (arm.ok) {
                    DSH_INFO("command 'world target watch on': watching %u enemy/enemies against %u doll(s). "
                             "Every change of a main target prints at once, with each doll's score at that "
                             "moment; 'off' prints, per enemy, how many changes and the longest hold.",
                             arm.enemies, arm.machines);
                } else {
                    DSH_WARN("command 'world target watch on': nothing to watch -- %s", arm.note.c_str());
                }
            } else {
                for (const std::string& one : dsh::reflect::target_watch_disarm()) {
                    DSH_INFO("world: %s", one.c_str());
                }
            }
            return;
        }

        if (line.find("weapons") != std::string::npos) {
            DSH_INFO("command 'world weapons': printing, for the local pawn and every puppet, the weapon-like "
                     "objects it owns. Read-only, bounded scan of the actor itself, no object-table walk.");
            const dsh::reflect::LiveObjects liveObjects = dsh::reflect::find_live_objects();
            for (const std::string& one : dsh::reflect::survey_owned_objects(liveObjects.pawn, "LOCAL PAWN")) {
                DSH_INFO("world: %s", one.c_str());
            }

            if (line.find("deep") != std::string::npos && liveObjects.pawn >= 0x10000) {
                const std::uintptr_t slotOffsets[] = {0x1610, 0x1618, 0x1620, 0x1628, 0x1630};
                for (const std::uintptr_t slotOffset : slotOffsets) {

                    for (const std::string& one :
                         dsh::reflect::survey_weapon_slot(liveObjects.pawn, slotOffset, "SLOT")) {
                        DSH_INFO("world: %s", one.c_str());
                    }
                }
            }
            return;
        }

        if (line.find("pin") != std::string::npos && line.find("hp") != std::string::npos) {
            const bool on = line.find("on") != std::string::npos;
            if (on) {
                dsh::reflect::hp_watch_refresh_targets(false);
                const dsh::reflect::HpWatchFrame frame = dsh::reflect::hp_watch_sample();
                g_hpPinTargets.clear();
                g_hpPinDrift = 0;
                g_hpPinWrites = 0;
                g_hpPinFailures = 0;
                if (frame.ok) {
                    for (const dsh::reflect::HpWatchSample& s : frame.samples) {
                        HpPinTarget t;
                        t.set = s.target.set;
                        t.value = s.health;
                        t.name = s.target.name;
                        g_hpPinTargets.push_back(std::move(t));
                    }
                }
                g_hpPinArmed = true;
                DSH_INFO("command 'world hp pin on': ARMED on %zu target(s); each target's health is now "
                         "written back every tick. SHOOT ONE: if its health does not move and it does not "
                         "die, the write path works. The read-back inside the writer says whether the game "
                         "replaced the value.", g_hpPinTargets.size());
            } else {
                g_hpPinArmed = false;
                DSH_INFO("command 'world hp pin off': DISARMED. %llu time(s) the game's value differed from "
                         "the pinned one; %llu write(s) landed and read back; %llu failed.",
                         static_cast<unsigned long long>(g_hpPinDrift),
                         static_cast<unsigned long long>(g_hpPinWrites),
                         static_cast<unsigned long long>(g_hpPinFailures));
            }
            return;
        }

        if (line.find("watch") != std::string::npos && line.find("hp") != std::string::npos) {
            const bool on = line.find("on") != std::string::npos;
            if (on) {
                g_hpWatchArmed = true;
                g_hpWatchIncludeNpc = line.find(" all") != std::string::npos;
                g_hpWatchLast.clear();
                g_hpWatchChanges = 0;
                g_hpWatchTicks = 0;
                g_hpWatchDroppedHealth = 0;
                g_hpWatchDroppedShield = 0;
                dsh::reflect::hp_watch_refresh_targets(g_hpWatchIncludeNpc);
                DSH_INFO("command 'world hp watch on': ARMED. It samples the local player and every enemy "
                         "about four times a second and prints ONLY the fields that moved (Health, "
                         "Shield), with the delta and the millisecond. Read-only: nothing is written. "
                         "The point is to see the SHAPE of a hit -- one change or several, in which order, "
                         "at what cadence -- before the mirror relies on it. %zu target(s) cached.",
                         dsh::reflect::hp_watch_targets().size());
            } else {
                g_hpWatchArmed = false;
                DSH_INFO("command 'world hp watch off': DISARMED after %llu sample(s). %llu change(s) seen: "
                         "%llu health drop(s), %llu shield drop(s). A mirror may only treat a drop as damage "
                         "if drops are all it ever sees -- and this count is what says so.",
                         static_cast<unsigned long long>(g_hpWatchTicks),
                         static_cast<unsigned long long>(g_hpWatchChanges),
                         static_cast<unsigned long long>(g_hpWatchDroppedHealth),
                         static_cast<unsigned long long>(g_hpWatchDroppedShield));
            }
            return;
        }
        if (line.find("hp") != std::string::npos) {

            const bool me = line.find("me") != std::string::npos;
            DSH_INFO("command 'world hp%s': printing, per AI-driven character, the eight floats at "
                     ".. of every attribute set it owns%s. Read-only; no NPC is read at all. SHOOT "
                     "ONE AND SEE WHICH NUMBER MOVES.",
                     me ? " me" : "",
                     me ? ", plus the LOCAL PLAYER's own sets with twenty-four floats from" : "");
            const dsh::reflect::HpSurvey hp = dsh::reflect::survey_hp(me);
            for (const std::string& reportLine : hp.report_lines()) {
                DSH_INFO("world: %s", reportLine.c_str());
            }
            return;
        }

        if (line.find("damage") != std::string::npos) {
            const bool on = line.find("off") == std::string::npos;
            std::string note;
            const bool ok = damage_watch_set_armed_entry(on, note);
            if (ok) {
                DSH_INFO("command 'world damage watch %s': %s. It prints one line per damage call with the "
                         "FIVE numbers of FDADamageParams, the damaged actor and the causer; it changes "
                         "nothing the game does.", on ? "on" : "off", note.c_str());
            } else {
                DSH_WARN("command 'world damage watch %s' REFUSED: %s", on ? "on" : "off", note.c_str());
            }
            return;
        }
        DSH_WARN("command 'world': expected 'world enemies' or 'world gates' -- got '%s'. Nothing was "
                 "read.", line.c_str());
        return;
    }

    if (line.rfind("progress", 0) == 0) {

        DSH_INFO("command 'progress': reading the save object's containers AS CONTAINERS (Data/Num/Max + "
                 "the sparse-array free indices) and resolving the first few elements. Read-only; no save "
                 "file is opened.");
        const dsh::reflect::SaveProbe probe = dsh::reflect::probe_save_containers();

        {
            std::string state = probe.saveFound ? std::string("found")
                                                : ("NOT found -- " + probe.note);
            DSH_INFO("progress: save object at 0x%llX (%s). RUN THIS BEFORE AND AFTER A TRAVEL AND COMPARE THE "
                     "ADDRESS: unchanged = it outlived the world, changed = the world owned it",
                     static_cast<unsigned long long>(probe.saveObject), state.c_str());
        }
        for (const std::string& probeLine : probe.report_lines()) {
            DSH_INFO("progress: %s", probeLine.c_str());
        }
        return;
    }

    if (line == "loopback") {

        DSH_INFO("command 'loopback': reading the real pawn pose and sending it over "
                 "real UDP loopback for 5 s");
        const auto lr = dsh::game::run_loopback_pose_test(5);
        DSH_INFO("command 'loopback': ok=%s handshake=%llu ms sent=%d received=%d "
                 "worstPos=%.4f m worstYaw=%.4f deg detail=%s",
                 lr.ok ? "yes" : "no",
                 static_cast<unsigned long long>(lr.handshakeMs), lr.samplesSent,
                 lr.samplesReceived, static_cast<double>(lr.worstPositionErrorM),
                 static_cast<double>(lr.worstYawErrorDeg), lr.detail.c_str());
        return;
    }

    if (line.rfind("puppetnet", 0) == 0) {

        std::size_t argPos = 9;
        while (argPos < line.size() && line[argPos] == ' ') ++argPos;
        const std::size_t verbEnd = line.find(' ', argPos);
        const std::string verb = verbEnd == std::string::npos
                                     ? line.substr(argPos)
                                     : line.substr(argPos, verbEnd - argPos);
        std::string rest =
            verbEnd == std::string::npos ? std::string() : line.substr(verbEnd + 1);
        while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
        std::string first = rest, second;
        const std::size_t sp = rest.find(' ');
        if (sp != std::string::npos) {
            first = rest.substr(0, sp);
            second = rest.substr(sp + 1);
        }
        const int secs = second.empty() ? 60 : std::atoi(second.c_str());
        bool asHost = false;
        std::uint16_t port = 0;
        std::string addr;

        if (verb == "text") {
            const std::wstring wanted = first.empty() ? std::wstring(L"Dolls Nest co-op")
                                                      : std::wstring(first.begin(), first.end());
            unsigned char ftext[0x18] = {};
            std::string textNote;
            bool made = false;

            if (dsh::reflect::run_on_game_thread(
                    [&]() { made = dsh::reflect::make_ftext(wanted, ftext, textNote); }, 5000)) {
                if (made) {
                    char hex[0x18 * 3 + 1] = {};
                    for (int i = 0; i < 0x18; ++i) {
                        char pair[4];
                        std::snprintf(pair, sizeof(pair), "%02X ", ftext[i]);
                        std::strncat(hex, pair, 3);
                    }
                    DSH_INFO("command 'puppetnet text': MADE an FText for %zu character(s) -- the engine's "
                             "own Conv_StringToText returned 0x18 bytes: %s",
                             wanted.size(), hex);
                } else {
                    DSH_WARN("command 'puppetnet text': the FText could NOT be made: %s", textNote.c_str());
                }
            } else {
                DSH_WARN("command 'puppetnet text': the game-thread dispatch was refused or timed out, so "
                         "nothing was called");
            }
            return;
        }

        if (verb == "broadcast" || verb == "rooms") {
            const std::uint64_t now = monotonic_ms();
            std::string socketNote;
            if (discovery_socket_listening(socketNote) == nullptr) {
                DSH_WARN("command 'puppetnet %s': %s -- so NOTHING is being %s", verb.c_str(),
                         socketNote.c_str(),
                         verb == "broadcast" ? "announced" : "listened for");
                return;
            }
            if (verb == "broadcast") {
                const std::uint16_t sessionPort =
                    static_cast<std::uint16_t>(first.empty() ? 47777 : std::atoi(first.c_str()));
                const int secs = second.empty() ? 0 : std::atoi(second.c_str());
                g_beacon.start(sessionPort, "Dolls Nest session",
                               current_map_key("announcing (puppetnet broadcast)"), now);
                g_beaconUntilMs = secs > 0 ? now + static_cast<std::uint64_t>(secs) * 1000 : 0;

                read_peer_file(g_peerAddresses, g_peerNote);
                DSH_INFO("command 'puppetnet broadcast': announcing session port %u on UDP %u %s -- this "
                         "RETURNS AT ONCE and the announcement is driven by the command tick. It also "
                         "ANSWERS any probe that arrives, which is the direction that does not need the "
                         "network to forward a broadcast. MODE: %s",
                         static_cast<unsigned>(sessionPort),
                         static_cast<unsigned>(dsh::net::kDiscoveryPort),
                         secs > 0 ? ("for " + std::to_string(secs) + " s").c_str() : "until stopped",
                         g_peerAddresses.empty()
                             ? "broadcast to every subnet (no peers.txt)"
                             : ("UNICAST ONLY to the " + g_peerNote +
                                " in peers.txt -- no broadcast is sent"));
                return;
            }
            const int secs = first.empty() ? 8 : std::atoi(first.c_str());
            g_search.start(static_cast<std::uint32_t>(secs) * 1000, now);
            g_search.mapKey = current_map_key("listening for rooms (puppetnet rooms)");
            g_searchUntilMs = now + static_cast<std::uint64_t>(secs) * 1000;
            DSH_INFO("command 'puppetnet rooms': listening on UDP %u for %d s -- this RETURNS AT ONCE and "
                     "the search is driven by the command tick, so the game is not held up",
                     static_cast<unsigned>(dsh::net::kDiscoveryPort), secs);
            return;
        }

        if (verb == "announce" || verb == "find") {
            std::string socketNote;
            dsh::net::ITransport* transport = discovery_socket_listening(socketNote);
            if (transport == nullptr) {
                DSH_WARN("command 'puppetnet %s': %s -- so NOTHING was %s", verb.c_str(),
                         socketNote.c_str(), verb == "announce" ? "announced" : "found");
                return;
            }

            if (verb == "announce") {
                const std::uint16_t sessionPort =
                    static_cast<std::uint16_t>(first.empty() ? 47777 : std::atoi(first.c_str()));
                const int secs = second.empty() ? 60 : std::atoi(second.c_str());

                dsh::net::BeaconState beacon;
                const std::uint64_t started = monotonic_ms();
                beacon.start(sessionPort, "Dolls Nest session",
                             current_map_key("announcing (puppetnet announce)"), started);
                std::string targetSummary;
                for (const dsh::net::BroadcastTarget& target : dsh::net::broadcast_targets_cached(0)) {
                    if (!targetSummary.empty()) targetSummary += " | ";
                    targetSummary += target.text;
                }
                DSH_INFO("command 'puppetnet announce': announcing session port %u on UDP %u once a second "
                         "for %d s to %s -- and ANSWERING any probe that arrives, which is the direction that "
                         "does not need the network to forward a broadcast",
                         static_cast<unsigned>(sessionPort),
                         static_cast<unsigned>(dsh::net::kDiscoveryPort), secs, targetSummary.c_str());

                unsigned sent = 0;
                unsigned answered = 0;
                std::string firstFrom;
                std::vector<dsh::net::Datagram> got;

                std::vector<std::uint32_t> peerAddresses;
                std::string peerNote;
                read_peer_file(peerAddresses, peerNote);
                for (int i = 0; i < secs; ++i) {
                    if (beacon.tick(monotonic_ms())) {
                        if (!peerAddresses.empty()) {
                            unsigned peerFailed = 0;
                            sent += send_to_listed_peers(*transport, beacon.payload(), peerAddresses,
                                                         peerFailed);
                        } else {
                            for (const dsh::net::BroadcastTarget& target :
                                 dsh::net::broadcast_targets_cached(30000)) {
                                dsh::net::Endpoint to;
                                if (!endpoint_for_ipv4(target.address, dsh::net::kDiscoveryPort, to)) continue;
                                if (transport->send(to, beacon.payload())) ++sent;
                            }
                        }
                    }
                    got.clear();
                    transport->poll(got);
                    for (const dsh::net::Datagram& d : got) {
                        if (!endpoint_is_listed(d.from, peerAddresses)) continue;
                        if (!dsh::net::is_probe(d.payload)) continue;
                        answer_probe(*transport, d.from, beacon, answered, firstFrom);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
                DSH_INFO("command 'puppetnet announce': %u announcement(s) sent and %u probe(s) answered%s "
                         "in %d s",
                         sent, answered,
                         firstFrom.empty() ? "" : (" (the first probe from " + firstFrom + ")").c_str(), secs);
                return;
            }

            const int secs = first.empty() ? 5 : std::atoi(first.c_str());
            begin_search(static_cast<std::uint32_t>(secs) * 1000);
            DSH_INFO("command 'puppetnet find': listening on UDP %u for %d s and PROBING every second, so a "
                     "host answers this machine's own address. This machine's interfaces are %s. MODE: %s",
                     static_cast<unsigned>(dsh::net::kDiscoveryPort), secs,
                     dsh::net::local_ipv4_summary().c_str(),
                     g_peerAddresses.empty()
                         ? "broadcast to every subnet (no peers.txt) -- a room from ANY address is accepted"
                         : ("UNICAST ONLY to the " + g_peerNote +
                            " in peers.txt -- no broadcast is sent and a room from any other address is "
                            "IGNORED"));

            std::vector<dsh::net::Datagram> got;
            std::uint64_t now = monotonic_ms();
            while (!g_search.done(now)) {
                if (g_search.probe_due(now)) {
                    const std::vector<std::uint8_t> probe = g_search.probe_payload();
                    if (!g_peerAddresses.empty()) {

                        unsigned peerFailed = 0;
                        (void)send_to_listed_peers(*transport, probe, g_peerAddresses, peerFailed);
                    } else {
                        for (const dsh::net::BroadcastTarget& target :
                             dsh::net::broadcast_targets_cached(30000)) {
                            dsh::net::Endpoint to;
                            if (!endpoint_for_ipv4(target.address, dsh::net::kDiscoveryPort, to)) continue;
                            (void)transport->send(to, probe);
                        }
                    }
                }
                if (g_peerAddresses.empty() &&
                    sweep_due(g_search, g_searchOpenedMs, g_sweepLastMs, now)) {
                    run_sweep(*transport, g_search.probe_payload(), g_sweepTotal, g_sweepSummary);
                    DSH_INFO("command 'puppetnet find': 2 s of broadcast probing reached nothing, so the "
                             "fallback runs -- %s", g_sweepSummary.c_str());
                }
                got.clear();
                transport->poll(got);
                for (const dsh::net::Datagram& d : got) {
                    if (!endpoint_is_listed(d.from, g_peerAddresses)) continue;
                    if (!g_search.observe(d.from.bytes, d.payload)) continue;
                    const dsh::net::Room& r = g_search.rooms.back();
                    std::string ip;
                    std::uint16_t peerPort = 0;
                    const bool known = endpoint_to_ip_port(r.address, ip, peerPort);
                    DSH_INFO("command 'puppetnet find': FOUND a session -- '%s' on session port %u, from "
                             "%s; join it with 'puppetnet join %s:%u <seconds>'",
                             r.name.c_str(), static_cast<unsigned>(r.sessionPort),
                             known ? (ip + " (the address to join)").c_str()
                                   : "an endpoint that could not be read as an address",
                             known ? ip.c_str() : "<that peer's ip>",
                             static_cast<unsigned>(r.sessionPort));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                now = monotonic_ms();
            }
            DSH_INFO("command 'puppetnet find': %s; the sweep asked %u address(es) in total",
                     g_search.summary().c_str(), g_sweepTotal);
            return;
        }

        if (verb == "serve") {
            asHost = true;
            port = static_cast<std::uint16_t>(std::atoi(first.c_str()));
            if (port == 0) port = 47777;
        } else if (verb == "join") {

            const std::size_t colon = first.rfind(':');
            if (colon != std::string::npos) {
                addr = first.substr(0, colon);
                port = static_cast<std::uint16_t>(std::atoi(first.c_str() + colon + 1));
            }
        } else {
            DSH_WARN("command 'puppetnet': expected 'puppetnet serve <port> [seconds]' or "
                     "'puppetnet join <ip:port> [seconds]', got '%s' -- nothing was run",
                     line.c_str());
            return;
        }

        const std::string secsText = secs > 0 ? ("for " + std::to_string(secs) + " s")
                                              : std::string("with no time limit (end it from the F7 menu)");
        DSH_INFO("command '%s': %s %s", line.c_str(), asHost ? "HOST" : "CLIENT", secsText.c_str());
        g_sessionCommandOffsetKnown = false;
        dsh::game::set_session_tick_hook(&pump_commands_during_session);
        const auto sr = dsh::game::run_puppet_session(asHost, addr, port, secs);
        dsh::game::set_session_tick_hook(nullptr);

        DSH_INFO("command '%s': ok=%s actor(s)=%d machine modules merged=%d/%d chunks "
                 "sent=%d detail=%s",
                 line.c_str(), sr.ok ? "yes" : "no", sr.actorsSpawned,
                 sr.machineModulesMerged, 10, sr.chunksSent, sr.detail.c_str());
        return;
    }

    if (line == "shutdown") {
        DSH_INFO("shutdown requested via command channel");
        dsh::platform::log_dump_breadcrumbs();
        dsh::platform::log_shutdown();
        return;
    }

    DSH_WARN("unknown command '%s' -- ignored (known: %s)", line.c_str(),
             known_command_list().c_str());
}

void run_selftest() {

    registry_selftest_for_log();
}

namespace {

using GetTickCountFn = DWORD(WINAPI*)(void);

GetTickCountFn g_originalGetTickCount = nullptr;
std::atomic<std::uint64_t> g_getTickCountCalls{0};

DWORD WINAPI GetTickCountDetour(void) {
    g_getTickCountCalls.fetch_add(1, std::memory_order_relaxed);
    dsh::platform::hook_note_call("selftest:GetTickCount");

    return g_originalGetTickCount();
}

}
void run_hook_selftest() {
    DSH_INFO("hook selftest: starting (MinHook available = %s)",
             dsh::platform::hooks_available() ? "yes" : "no");

    if (!dsh::platform::hooks_available()) {
        DSH_ERROR("hook selftest: hooking is NOT available in this build. Consequence: "
                  "ProcessEvent interception is impossible, so no gameplay capture or "
                  "replay. Fix: fetch MinHook and rebuild.");
        return;
    }

    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (!kernel) {
        DSH_ERROR("hook selftest: kernel32 not loaded?! skipping");
        return;
    }
    void* target = reinterpret_cast<void*>(GetProcAddress(kernel, "GetTickCount"));
    if (!target) {
        DSH_ERROR("hook selftest: GetProcAddress(GetTickCount) failed -- skipping");
        return;
    }

    const std::uint64_t before = g_getTickCountCalls.load();

    void* original = nullptr;
    const auto result = dsh::platform::install_hook("selftest:GetTickCount", target,
                                                   reinterpret_cast<void*>(&GetTickCountDetour),
                                                   &original);
    if (!result.installed) {
        DSH_ERROR("hook selftest: INSTALL FAILED (%s). Consequence: ProcessEvent cannot be "
                  "hooked, so Phase 0.6 is blocked until this is fixed.",
                  result.note.c_str());
        return;
    }
    g_originalGetTickCount = reinterpret_cast<GetTickCountFn>(original);

    volatile DWORD direct = 0;
    for (int i = 0; i < 4; ++i) {
        direct = GetTickCount();
    }
    Sleep(120);

    const std::uint64_t after = g_getTickCountCalls.load();
    const std::uint64_t viaOurCounter = dsh::platform::hook_call_count("selftest:GetTickCount");

    DSH_INFO("hook selftest: direct hits=%llu, counter=%llu (direct calls made=4)",
             static_cast<unsigned long long>(after - before),
             static_cast<unsigned long long>(viaOurCounter));
    DSH_INFO("hook selftest: original forwarded ok (returned %lu, non-zero = forwarding works)",
             static_cast<unsigned long>(direct));

    const bool fired = (after - before) >= 4;
    const bool counterWorks = viaOurCounter >= 4;

    MH_RemoveHook(target);
    DSH_INFO("hook selftest: hook removed");

    if (fired && counterWorks) {
        DSH_INFO("hook selftest: PASS -- detours work in this process. Phase 0.6 is now a "
                 "question of FINDING ProcessEvent, not of whether hooking works.");
    } else {
        DSH_ERROR("hook selftest: FAIL -- detour did not fire (fired=%s counter=%s). "
                  "Consequence: ProcessEvent hooking is unreliable in this environment.",
                  fired ? "yes" : "no", counterWorks ? "ok" : "not counting");
    }
}

}

void on_process_attach(void* moduleBase, HMODULE ) {
    g_moduleBase = moduleBase;
    const char* exeName = "?";
    (void)exeName;

    {
        const DWORD pidValue = GetCurrentProcessId();
        char line[512];
        std::snprintf(line, sizeof(line), "marker pid=%lu moduleBase=%s stamp=%s phase=attach",
                      static_cast<unsigned long>(pidValue), hex_ptr(moduleBase).c_str(),
                      DSH_BUILD_STAMP);
        append_marker(line);
    }

    bool expected = false;
    if (!g_workerStarted.compare_exchange_strong(expected, true)) return;

    HANDLE t = CreateThread(nullptr, 0, worker_proc, nullptr, 0, nullptr);
    if (t) {
        CloseHandle(t);
    } else {
        append_marker("!! CreateThread(worker) failed: initialisation will not happen");
    }
}

void on_process_detach() {

    beacon_thread_stop("the process is detaching");
    if (g_initialised.load()) {
        dsh::platform::log_dump_breadcrumbs();
        dsh::platform::log_write(dsh::platform::LogLevel::info, __FILE__, __LINE__,
                                 "DLL_PROCESS_DETACH");
        dsh::platform::log_shutdown();
    }
}

std::int32_t explicit_init(const char* commandLineUtf8) {
    g_commandLine.clear();
    if (commandLineUtf8) {
        const int len = MultiByteToWideChar(CP_UTF8, 0, commandLineUtf8, -1, nullptr, 0);
        if (len > 0) {
            g_commandLine.resize(static_cast<std::size_t>(len));
            MultiByteToWideChar(CP_UTF8, 0, commandLineUtf8, -1, g_commandLine.data(), len);
        }
    }
    bool expected = false;
    if (!g_initialised.compare_exchange_strong(expected, true)) {
        DSH_WARN("explicit initialisation called again -- ignored");
    }
    DSH_INFO("explicit init called (command line %s)",
             g_commandLine.empty() ? "empty" : "received");
    return 0;
}

std::uint32_t api_version() { return kApiVersion; }
const char*   build_stamp() { return DSH_BUILD_STAMP; }

}

#include "dsh/sync/entity_registry.h"

void registry_selftest_for_log() {
    using namespace dsh;
    sync::EntityRegistry reg;

    EntityId id{};
    const auto created = reg.create(kHostPeer, 1, 0xDEADBEEFull,
                                   reg.generation(), id);
    DSH_INFO("selftest: registry create -> %s (id %u:%llu)",
             created == sync::CreateResult::created ? "created" : "FAILED", id.authority.value,
             static_cast<unsigned long long>(id.counter));

    EntityId id2{};
    const auto created2 = reg.create(kHostPeer, 2, 0x1234ull,
                                    reg.generation(), id2);
    DSH_INFO("selftest: second create -> %s (id %u:%llu distinct=%s)",
             created2 == sync::CreateResult::created ? "created" : "UNEXPECTED",
             id2.authority.value, static_cast<unsigned long long>(id2.counter),
             (id2 != id) ? "yes" : "NO -- ids collided");

    EntityId stale{};
    const auto staleRc = reg.create(kHostPeer, 1, 0, reg.generation() + 7, stale);
    DSH_INFO("selftest: stale-generation create -> %s (expected rejected_invalid_id)",
             staleRc == sync::CreateResult::rejected_invalid_id ? "rejected_invalid_id"
                                                               : "UNEXPECTED");

    const std::uint64_t packed = sync::pack(id);
    DSH_INFO("selftest: pack/unpack -> %s (packed=0x%llX)",
             (sync::unpack(packed) == id) ? "ok" : "MISMATCH",
             static_cast<unsigned long long>(packed));

    DSH_INFO("selftest: unknown-id destroy -> %s (expected false)",
             reg.destroy(EntityId{kHostPeer, 999999}) ? "true -- UNEXPECTED" : "false");

    const std::size_t dropped = reg.on_world_replaced();
    DSH_INFO("selftest: on_world_replaced -> dropped %zu, generation now %u", dropped,
             reg.generation());
    DSH_INFO("selftest: old id after world replace -> %s (expected gone)",
             reg.find(id) == nullptr ? "gone" : "STILL PRESENT -- stale handle survived");
}
