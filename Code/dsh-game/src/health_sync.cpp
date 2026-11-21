#include "dsh/game/health_sync.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "dsh/platform/log.h"
#include "dsh/reflect/enemies.h"
#include "dsh/reflect/gamethread.h"
#include "dsh/reflect/health_core.h"
#include "dsh/reflect/world.h"

namespace dsh::game {
namespace {

constexpr std::uint32_t kDamageEventSlots = 64;
constexpr std::uint32_t kDamageEventMask = kDamageEventSlots - 1;
DamageEventHit             g_damageEvents[kDamageEventSlots];
std::atomic<std::uint32_t> g_damageEventWrite{0};
std::atomic<std::uint32_t> g_damageEventRead{0};
std::atomic<std::uint64_t> g_damageEventRecorded{0};
std::atomic<std::uint64_t> g_damageEventDropped{0};
std::atomic<std::uint64_t> g_damageEventDrained{0};

}
void note_damage_event(std::uintptr_t actor, const float params[5], std::uint64_t ms) {
    if (actor < 0x10000 || params == nullptr) return;
    const float kinetic = params[2];
    const float thermal = params[3];
    if (!std::isfinite(kinetic) || !std::isfinite(thermal)) return;

    if (kinetic <= 0.0f && thermal <= 0.0f) return;
    const std::uint32_t w = g_damageEventWrite.load(std::memory_order_relaxed);
    const std::uint32_t next = (w + 1) & kDamageEventMask;
    if (next == g_damageEventRead.load(std::memory_order_acquire)) {
        g_damageEventDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    DamageEventHit& slot = g_damageEvents[w];
    slot.actor = actor;
    slot.effectiveKinetic = kinetic;
    slot.effectiveThermal = thermal;
    slot.ms = ms;
    g_damageEventWrite.store(next, std::memory_order_release);
    g_damageEventRecorded.fetch_add(1, std::memory_order_relaxed);
}

DamageEventStats damage_event_stats() {
    DamageEventStats s;
    s.recorded = g_damageEventRecorded.load(std::memory_order_relaxed);
    s.dropped = g_damageEventDropped.load(std::memory_order_relaxed);
    s.drained = g_damageEventDrained.load(std::memory_order_relaxed);
    return s;
}

namespace {
constexpr std::size_t kActionEdgeCapacity = 64;
std::atomic<std::uint32_t> g_actionEdgeWrite{0};
std::atomic<std::uint32_t> g_actionEdgeRead{0};
std::atomic<std::uint64_t> g_actionEdgeNoted{0};
std::atomic<std::uint64_t> g_actionEdgeDropped{0};
std::atomic<std::uint64_t> g_actionEdgeDrained{0};

thread_local bool          g_actionReplayInFlight = false;
std::atomic<std::uint64_t> g_actionEchoDropped{0};

std::atomic<bool>          g_actionCaptureEnabled{true};
struct ActionEdgeSlot {
    std::uint32_t action = 0;
    std::atomic<std::uint32_t> module{0};
    std::atomic<std::uint8_t> pressed{0};
    std::atomic<std::uint64_t> ms{0};
};
ActionEdgeSlot g_actionEdgeRing[kActionEdgeCapacity];
}
void note_action_edge(std::uint32_t action, std::uint32_t module, bool pressed, std::uint64_t ms) {

    if (!g_actionCaptureEnabled.load(std::memory_order_relaxed)) return;
    if (g_actionReplayInFlight) {
        g_actionEchoDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::uint32_t write = g_actionEdgeWrite.load(std::memory_order_relaxed);
    const std::uint32_t read = g_actionEdgeRead.load(std::memory_order_acquire);
    if (write - read >= kActionEdgeCapacity) {
        g_actionEdgeDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ActionEdgeSlot& slot = g_actionEdgeRing[write % kActionEdgeCapacity];
    slot.action = action;
    slot.module.store(module, std::memory_order_relaxed);
    slot.pressed.store(pressed ? 1u : 0u, std::memory_order_relaxed);
    slot.ms.store(ms, std::memory_order_relaxed);
    g_actionEdgeWrite.store(write + 1, std::memory_order_release);
    g_actionEdgeNoted.fetch_add(1, std::memory_order_relaxed);
}

bool take_action_edge(ActionEdge& out) {
    const std::uint32_t read = g_actionEdgeRead.load(std::memory_order_relaxed);
    const std::uint32_t write = g_actionEdgeWrite.load(std::memory_order_acquire);
    if (read == write) return false;
    const ActionEdgeSlot& slot = g_actionEdgeRing[read % kActionEdgeCapacity];
    out.action = slot.action;
    out.module = slot.module.load(std::memory_order_relaxed);
    out.pressed = slot.pressed.load(std::memory_order_relaxed) != 0;
    out.ms = slot.ms.load(std::memory_order_relaxed);
    g_actionEdgeRead.store(read + 1, std::memory_order_release);
    g_actionEdgeDrained.fetch_add(1, std::memory_order_relaxed);
    return true;
}

ActionEdgeStats action_edge_stats() {
    ActionEdgeStats s;
    s.noted = g_actionEdgeNoted.load(std::memory_order_relaxed);
    s.dropped = g_actionEdgeDropped.load(std::memory_order_relaxed);
    s.drained = g_actionEdgeDrained.load(std::memory_order_relaxed);
    s.echoDropped = g_actionEchoDropped.load(std::memory_order_relaxed);
    return s;
}

void set_action_replay_in_flight(bool inFlight) { g_actionReplayInFlight = inFlight; }

bool action_replay_in_flight() { return g_actionReplayInFlight; }

void set_action_capture_enabled(bool enabled) {
    g_actionCaptureEnabled.store(enabled, std::memory_order_relaxed);
}

bool action_capture_enabled() { return g_actionCaptureEnabled.load(std::memory_order_relaxed); }

namespace {
constexpr std::size_t kDeathCapacity = 16;
std::atomic<std::uint32_t>  g_deathWrite{0};
std::atomic<std::uint32_t>  g_deathRead{0};
std::atomic<std::uint64_t>  g_deathDropped{0};
std::atomic<std::uintptr_t> g_deathMulticast{0};
std::atomic<std::uintptr_t> g_deathReceive{0};
struct DeathSlot {
    std::atomic<std::uintptr_t> actor{0};
    std::atomic<std::uint64_t>  ms{0};
    std::atomic<std::uint8_t>   multicast{0};
};
DeathSlot g_deathRing[kDeathCapacity];
}
void set_death_functions(std::uintptr_t multicastOnDeath, std::uintptr_t receiveOnDeath) {
    g_deathMulticast.store(multicastOnDeath, std::memory_order_relaxed);
    g_deathReceive.store(receiveOnDeath, std::memory_order_relaxed);
}

std::uintptr_t death_multicast_function() {
    return g_deathMulticast.load(std::memory_order_relaxed);
}

std::uintptr_t death_receive_function() {
    return g_deathReceive.load(std::memory_order_relaxed);
}

void note_death(std::uintptr_t actor, std::uint64_t ms, bool multicast) {
    const std::uint32_t write = g_deathWrite.load(std::memory_order_relaxed);
    const std::uint32_t read = g_deathRead.load(std::memory_order_acquire);
    if (write - read >= kDeathCapacity) {
        g_deathDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    DeathSlot& slot = g_deathRing[write % kDeathCapacity];
    slot.actor.store(actor, std::memory_order_relaxed);
    slot.ms.store(ms, std::memory_order_relaxed);
    slot.multicast.store(multicast ? 1u : 0u, std::memory_order_relaxed);
    g_deathWrite.store(write + 1, std::memory_order_release);
}

bool take_death(DeathNote& out) {
    const std::uint32_t read = g_deathRead.load(std::memory_order_relaxed);
    const std::uint32_t write = g_deathWrite.load(std::memory_order_acquire);
    if (read == write) return false;
    const DeathSlot& slot = g_deathRing[read % kDeathCapacity];
    out.actor = slot.actor.load(std::memory_order_relaxed);
    out.ms = slot.ms.load(std::memory_order_relaxed);
    out.multicast = slot.multicast.load(std::memory_order_relaxed) != 0;
    g_deathRead.store(read + 1, std::memory_order_release);
    return true;
}

std::uint64_t deaths_dropped() { return g_deathDropped.load(std::memory_order_relaxed); }

namespace {
std::atomic<std::uintptr_t> g_checkpointDialogue{0};
std::atomic<std::uintptr_t> g_checkpointRegression{0};
std::atomic<std::uintptr_t> g_checkpointSave{0};
std::atomic<std::uint32_t>  g_checkpointHits{0};
std::atomic<std::uint8_t>   g_checkpointKind{0};
}
void set_checkpoint_functions(std::uintptr_t dialogue, std::uintptr_t regressionPoint, std::uintptr_t save) {
    g_checkpointDialogue.store(dialogue, std::memory_order_relaxed);
    g_checkpointRegression.store(regressionPoint, std::memory_order_relaxed);
    g_checkpointSave.store(save, std::memory_order_relaxed);
}

std::uintptr_t checkpoint_dialogue_function() {
    return g_checkpointDialogue.load(std::memory_order_relaxed);
}

std::uintptr_t checkpoint_regression_function() {
    return g_checkpointRegression.load(std::memory_order_relaxed);
}

std::uintptr_t checkpoint_save_function() {
    return g_checkpointSave.load(std::memory_order_relaxed);
}

void note_checkpoint(std::uint8_t signal) {
    g_checkpointKind.store(signal, std::memory_order_relaxed);
    g_checkpointHits.fetch_add(1, std::memory_order_release);
}

bool take_checkpoint(std::uint8_t& signal) {
    if (g_checkpointHits.load(std::memory_order_acquire) == 0) return false;

    g_checkpointHits.store(0, std::memory_order_release);
    signal = g_checkpointKind.load(std::memory_order_relaxed);
    return true;
}

namespace {
std::atomic<std::uintptr_t> g_localModuleFront{0};
std::atomic<std::uintptr_t> g_localModuleLeft{0};
std::atomic<std::uintptr_t> g_localModuleRight{0};
}
void set_local_weapon_modules(std::uintptr_t front, std::uintptr_t left, std::uintptr_t right) {
    g_localModuleFront.store(front >= 0x10000 ? front : 0, std::memory_order_relaxed);
    g_localModuleLeft.store(left >= 0x10000 ? left : 0, std::memory_order_relaxed);
    g_localModuleRight.store(right >= 0x10000 ? right : 0, std::memory_order_relaxed);
}

int local_weapon_module_slot(std::uintptr_t actor) {
    if (actor < 0x10000) return -1;
    if (g_localModuleFront.load(std::memory_order_relaxed) == actor) return 0;
    if (g_localModuleLeft.load(std::memory_order_relaxed) == actor) return 1;
    if (g_localModuleRight.load(std::memory_order_relaxed) == actor) return 2;
    return -1;
}

namespace {
constexpr std::size_t      kMaxPuppetOwners = 4;
std::atomic<std::uintptr_t> g_puppetOwners[kMaxPuppetOwners];
std::atomic<std::uint64_t>  g_puppetDamageDropped{0};
}
void set_puppet_owners(const std::uintptr_t* actors, std::size_t count) {
    for (std::size_t i = 0; i < kMaxPuppetOwners; ++i) {
        const std::uintptr_t value = (actors != nullptr && i < count) ? actors[i] : 0;
        g_puppetOwners[i].store(value, std::memory_order_relaxed);
    }
}

bool damage_is_from_puppet(std::uintptr_t causerOwner) {

    if (causerOwner < 0x10000) return false;
    for (std::size_t i = 0; i < kMaxPuppetOwners; ++i) {
        if (g_puppetOwners[i].load(std::memory_order_relaxed) == causerOwner) {
            g_puppetDamageDropped.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

std::uint64_t puppet_damage_dropped() {
    return g_puppetDamageDropped.load(std::memory_order_relaxed);
}

namespace {

constexpr float kHealthEpsilon = 0.5f;

constexpr float kHealthZero = 0.5f;

std::unordered_map<std::uintptr_t, float> g_lastSent;

std::unordered_map<std::uintptr_t, std::string> g_nameBySet;
std::unordered_set<std::uintptr_t> g_deadStated;

struct HealthTarget {
    std::uintptr_t set = 0;
    std::uintptr_t actor = 0;
};
std::unordered_map<std::string, HealthTarget> g_targetByName;
std::uint64_t g_setByNameBuiltMs = 0;

void rebuild_name_index() {

    DSH_TIME_WALK("health:name-index");
    g_targetByName.clear();
    for (const dsh::reflect::HpWatchTarget& t : dsh::reflect::hp_watch_targets()) {
        if (t.set < 0x10000 || t.name.empty()) continue;
        g_targetByName.emplace(t.name, HealthTarget{t.set, t.actor});
    }
    g_setByNameBuiltMs = GetTickCount64();
}

constexpr std::uint64_t kHpTargetsTtlMs = 60000;
constexpr std::uint64_t kHpTargetsRetryMs = 5000;
constexpr std::uint64_t kHpTargetsMissMs = 5000;
std::uint64_t g_targetsBuiltMs = 0;
std::uint64_t g_targetsEmptyMs = 0;
std::uint64_t g_targetsMissMs = 0;
void refresh_health_targets(std::uint64_t nowMs) {
    dsh::reflect::hp_watch_refresh_targets(false);
    g_targetsBuiltMs = nowMs;
    g_targetsEmptyMs = dsh::reflect::hp_watch_targets().empty() ? nowMs : 0;
}

void keep_health_targets(std::uint64_t nowMs) {
    if (g_targetsBuiltMs == 0 || nowMs - g_targetsBuiltMs >= kHpTargetsTtlMs) {
        refresh_health_targets(nowMs);
        return;
    }
    if (g_targetsEmptyMs != 0 && nowMs - g_targetsEmptyMs >= kHpTargetsRetryMs) {
        refresh_health_targets(nowMs);
    }
}

bool refresh_health_targets_after_miss(std::uint64_t nowMs) {
    if (g_targetsMissMs != 0 && nowMs - g_targetsMissMs < kHpTargetsMissMs) return false;
    g_targetsMissMs = nowMs;
    refresh_health_targets(nowMs);
    rebuild_name_index();
    return true;
}

void forget_health_targets() {
    dsh::reflect::hp_watch_forget_targets();
    g_targetsBuiltMs = 0;
    g_targetsEmptyMs = 0;
    g_targetsMissMs = 0;
}

struct HealthUpdate {
    std::uintptr_t set = 0;
    std::uintptr_t actor = 0;
    float          oldValue = 0.0f;
    float          newValue = 0.0f;
    std::string    name;
};

void queue_health_updates(std::vector<HealthUpdate> updates) {
    if (updates.empty()) return;
    if (!dsh::reflect::game_thread_hook_installed()) {
        DSH_WARN("health: the game-thread hook is NOT installed, so %zu written value(s) were NOT announced "
                 "to the game and no death was applied -- run 'gthook' before playing",
                 updates.size());
        return;
    }
    auto shared = std::make_shared<std::vector<HealthUpdate>>(std::move(updates));
    dsh::reflect::game_thread_push([shared]() {
        for (const HealthUpdate& u : *shared) {
            std::string note;
            if (!dsh::reflect::hp_notify_health_changed(u.set, u.oldValue, note)) {
                DSH_WARN("health: OnRep_Health for '%s' was NOT called: %s", u.name.c_str(), note.c_str());
            }
            if (u.newValue <= kHealthZero) {
                dsh::reflect::hp_apply_lethal_damage_on_game_thread(u.set, u.actor, u.name);
            }
        }
    });
}

}

void forget_dead_world() {
    static std::uintptr_t s_world = 0;
    const std::uintptr_t world = dsh::reflect::find_live_objects().world;
    if (world == s_world) return;
    s_world = world;
    g_lastSent.clear();
    g_nameBySet.clear();
    g_deadStated.clear();
    g_targetByName.clear();

    forget_health_targets();
}

std::vector<HealthEntry> host_collect_health_changes(bool force, std::string& note) {
    note.clear();
    std::vector<HealthEntry> out;
    forget_dead_world();

    const std::uint64_t nowMs = GetTickCount64();
    keep_health_targets(nowMs);
    const dsh::reflect::HpWatchFrame frame = dsh::reflect::hp_watch_sample();
    if (!frame.ok) {
        note = "the frame could not be sampled: " + frame.note;
        return out;
    }
    std::uint32_t unreadable = 0;
    std::unordered_set<std::uintptr_t> seen;
    for (const dsh::reflect::HpWatchSample& s : frame.samples) {
        if (s.target.set < 0x10000) continue;

        if (s.target.isLocal) continue;
        seen.insert(s.target.set);
        g_nameBySet[s.target.set] = s.target.name;
        if (!s.healthRead) {
            ++unreadable;
            continue;
        }

        const auto it = g_lastSent.find(s.target.set);
        const bool dropped = it == g_lastSent.end() || s.health < it->second - kHealthEpsilon;
        if (!force && !dropped) continue;
        g_lastSent[s.target.set] = s.health;
        HealthEntry e;
        e.enemyName = s.target.name;
        e.sequence = 0;
        e.value = s.health;
        out.push_back(std::move(e));
    }

    for (const auto& known : g_nameBySet) {
        if (seen.count(known.first) != 0) continue;
        if (g_deadStated.count(known.first) != 0) continue;
        g_deadStated.insert(known.first);
        g_lastSent[known.first] = 0.0f;
        HealthEntry e;
        e.enemyName = known.second;
        e.sequence = 0;
        e.value = 0.0f;
        out.push_back(std::move(e));
    }
    if (unreadable != 0) {
        note = std::to_string(unreadable) + " character(s) had no readable health this time and were NOT "
               "broadcast as zero";
    }
    return out;
}

std::vector<std::string> HealthApplyResult::report_lines() const {
    std::vector<std::string> lines;
    if (!note.empty()) lines.push_back("health broadcast: " + note);
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "health broadcast: %u entr(y/ies) decoded of %u received; %u written and read back, %u "
                  "failed, %u name(s) not in my levels",
                  decoded, entries, written, failed, nameNotFound);
    lines.push_back(buf);
    for (const std::string& f : failures) lines.push_back("health broadcast: " + f);
    return lines;
}

bool client_apply_health_broadcast(std::span<const std::uint8_t> payload, HealthApplyResult& out) {
    out = HealthApplyResult{};
    forget_dead_world();
    std::vector<HealthEntry> entries;
    std::string decodeNote;
    if (!decode_health(payload, false, entries, decodeNote)) {
        out.note = "REFUSED: " + decodeNote;
        return false;
    }
    out.entries = static_cast<std::uint32_t>(entries.size());
    out.decoded = out.entries;
    out.received = out.entries;
    if (entries.empty()) {
        out.note = "the payload carried no entries";
        return true;
    }
    if (g_targetByName.empty() || GetTickCount64() - g_setByNameBuiltMs > 10000) rebuild_name_index();

    std::unordered_map<std::uintptr_t, float> healthBySet;
    {
        const dsh::reflect::HpWatchFrame frame = dsh::reflect::hp_watch_sample();
        if (frame.ok) {
            for (const dsh::reflect::HpWatchSample& s : frame.samples) {
                if (s.healthRead && s.target.set >= 0x10000) healthBySet[s.target.set] = s.health;
            }
        }
    }

    bool rebuiltOnce = false;
    std::vector<HealthUpdate> updates;
    for (const HealthEntry& e : entries) {
        auto it = g_targetByName.find(e.enemyName);
        if (it == g_targetByName.end()) {

            if (!rebuiltOnce) {
                const bool refreshed = refresh_health_targets_after_miss(GetTickCount64());
                rebuiltOnce = true;
                if (refreshed) {

                    healthBySet.clear();
                    const dsh::reflect::HpWatchFrame frame = dsh::reflect::hp_watch_sample();
                    if (frame.ok) {
                        for (const dsh::reflect::HpWatchSample& s : frame.samples) {
                            if (s.healthRead && s.target.set >= 0x10000) healthBySet[s.target.set] = s.health;
                        }
                    }
                }
            }
            it = g_targetByName.find(e.enemyName);
            if (it == g_targetByName.end()) {
                ++out.nameNotFound;
                if (out.failures.size() < 4) {
                    out.failures.push_back("'" + e.enemyName + "' is not in my levels, so nothing was written");
                }
                continue;
            }
        }
        if (!std::isfinite(e.value) || e.value < 0.0f) {
            ++out.failed;
            continue;
        }
        const auto healthIt = healthBySet.find(it->second.set);
        if (healthIt == healthBySet.end()) {
            ++out.failed;
            if (out.failures.size() < 4) {
                out.failures.push_back("'" + e.enemyName + "' health could not be read, so the value was not "
                                       "compared and nothing was written");
            }
            continue;
        }
        const float current = healthIt->second;

        if (e.value >= current - kHealthEpsilon) continue;
        float back = 0.0f;
        std::string writeNote;
        if (!dsh::reflect::hp_write_health(it->second.set, e.value, &back, writeNote)) {
            ++out.failed;
            if (out.failures.size() < 4) {
                out.failures.push_back("'" + e.enemyName + "' was NOT written: " + writeNote);
            }
            continue;
        }
        ++out.written;
        g_lastSent[it->second.set] = e.value;
        updates.push_back(HealthUpdate{it->second.set, it->second.actor, current, e.value, e.enemyName});
    }
    queue_health_updates(std::move(updates));
    return true;
}

}
namespace dsh::game {

void health_sync_reset() {
    g_lastSent.clear();
    g_targetByName.clear();
    g_nameBySet.clear();
    g_deadStated.clear();
    g_setByNameBuiltMs = 0;
}

std::vector<HealthEntry> client_collect_damage_reports(std::uint32_t firstSequence, std::string& note) {
    note.clear();
    std::vector<HealthEntry> out;
    forget_dead_world();

    const std::uint64_t nowMs = GetTickCount64();

    keep_health_targets(nowMs);
    const dsh::reflect::HpWatchFrame frame = dsh::reflect::hp_watch_sample();
    if (!frame.ok) {
        note = "the survey could not be sampled: " + frame.note;
        return out;
    }
    std::uint32_t sequence = firstSequence;
    std::uint32_t unreadable = 0;
    std::unordered_set<std::uintptr_t> seen;
    for (const dsh::reflect::HpWatchSample& s : frame.samples) {
        if (s.target.set < 0x10000 || s.target.isLocal) continue;
        seen.insert(s.target.set);
        g_nameBySet[s.target.set] = s.target.name;
        if (!s.healthRead) {
            ++unreadable;
            continue;
        }
        const auto it = g_lastSent.find(s.target.set);
        if (it != g_lastSent.end() && s.health >= it->second - kHealthEpsilon) continue;
        g_lastSent[s.target.set] = s.health;
        HealthEntry e;
        e.enemyName = s.target.name;
        e.sequence = sequence++;
        e.value = s.health;
        out.push_back(std::move(e));
    }

    for (const auto& known : g_nameBySet) {
        if (seen.count(known.first) != 0) continue;
        if (g_deadStated.count(known.first) != 0) continue;
        g_deadStated.insert(known.first);
        g_lastSent[known.first] = 0.0f;
        HealthEntry e;
        e.enemyName = known.second;
        e.sequence = sequence++;
        e.value = 0.0f;
        out.push_back(std::move(e));
    }
    if (unreadable != 0) {
        note = std::to_string(unreadable) + " character(s) had no readable health this time and were NOT reported";
    }
    return out;
}

std::vector<std::string> HealthReportResult::report_lines() const {
    std::vector<std::string> lines;
    if (!note.empty()) lines.push_back("health report: " + note);
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "health report: %u entr(y/ies) received; %u applied, %u duplicate(s) refused, %u "
                  "implausible, %u name(s) not in my levels, %u write failure(s)",
                  received, applied, duplicate, refused, nameNotFound, writeFailed);
    lines.push_back(buf);
    for (const std::string& one : this->lines) lines.push_back("health report: " + one);
    return lines;
}

bool host_apply_health_report(std::span<const std::uint8_t> payload, HealthReportResult& out) {
    out = HealthReportResult{};
    forget_dead_world();
    std::vector<HealthEntry> entries;
    std::string decodeNote;
    if (!decode_health(payload, true, entries, decodeNote)) {
        out.note = "REFUSED: " + decodeNote;
        return false;
    }
    out.received = static_cast<std::uint32_t>(entries.size());
    if (entries.empty()) { out.note = "the payload carried no entries"; return true; }
    if (g_targetByName.empty() || GetTickCount64() - g_setByNameBuiltMs > 10000) rebuild_name_index();

    std::unordered_map<std::uintptr_t, float> healthBySet;
    {
        const dsh::reflect::HpWatchFrame frame = dsh::reflect::hp_watch_sample();
        if (frame.ok) {
            for (const dsh::reflect::HpWatchSample& s : frame.samples) {
                if (s.healthRead && s.target.set >= 0x10000) healthBySet[s.target.set] = s.health;
            }
        }
    }
    bool rebuiltOnce = false;
    std::vector<HealthUpdate> updates;
    for (const HealthEntry& e : entries) {
        auto it = g_targetByName.find(e.enemyName);
        if (it == g_targetByName.end()) {

            if (!rebuiltOnce) {
                const bool refreshed = refresh_health_targets_after_miss(GetTickCount64());
                rebuiltOnce = true;
                if (refreshed) {

                    healthBySet.clear();
                    const dsh::reflect::HpWatchFrame frame = dsh::reflect::hp_watch_sample();
                    if (frame.ok) {
                        for (const dsh::reflect::HpWatchSample& s : frame.samples) {
                            if (s.healthRead && s.target.set >= 0x10000) healthBySet[s.target.set] = s.health;
                        }
                    }
                }
            }
            it = g_targetByName.find(e.enemyName);
            if (it == g_targetByName.end()) {
                ++out.nameNotFound;
                if (out.lines.size() < 4) out.lines.push_back("'" + e.enemyName + "' is not in my levels");
                continue;
            }
        }
        if (!std::isfinite(e.value) || e.value < 0.0f) {
            ++out.refused;
            continue;
        }
        const auto healthIt = healthBySet.find(it->second.set);
        if (healthIt == healthBySet.end()) {
            ++out.refused;
            if (out.lines.size() < 4) out.lines.push_back("'" + e.enemyName + "' health could not be read");
            continue;
        }
        const float health = healthIt->second;

        if (e.value >= health - kHealthEpsilon) {
            ++out.duplicate;
            continue;
        }
        float back = 0.0f;
        std::string writeNote;
        if (!dsh::reflect::hp_write_health(it->second.set, e.value, &back, writeNote)) {
            ++out.writeFailed;
            if (out.lines.size() < 4) out.lines.push_back("'" + e.enemyName + "' NOT written: " + writeNote);
            continue;
        }
        ++out.applied;
        out.enemyNames.push_back(e.enemyName);
        updates.push_back(HealthUpdate{it->second.set, it->second.actor, health, e.value, e.enemyName});
        if (out.lines.size() < 4) {
            char one[200];
            std::snprintf(one, sizeof(one), "'%s' %.1f -> %.1f (the client states %.1f)",
                          e.enemyName.c_str(), static_cast<double>(health), static_cast<double>(e.value),
                          static_cast<double>(e.value));
            out.lines.push_back(one);
        }
    }
    queue_health_updates(std::move(updates));
    return true;
}

bool health_resolve_actor(const std::string& name, std::uintptr_t& actorOut, std::string& note) {
    actorOut = 0;
    note.clear();
    if (name.empty()) {
        note = "no name was given";
        return false;
    }
    forget_dead_world();
    auto it = g_targetByName.find(name);
    if (it == g_targetByName.end()) {

        (void)refresh_health_targets_after_miss(GetTickCount64());
        it = g_targetByName.find(name);
    }
    if (it == g_targetByName.end()) {
        note = "'" + name + "' is not one of this world's enemies";
        return false;
    }
    if (it->second.actor < 0x10000) {
        note = "'" + name + "' is in the index but its actor pointer is null";
        return false;
    }
    actorOut = it->second.actor;
    return true;
}

}
