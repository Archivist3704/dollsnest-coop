#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dsh/game/health_wire.h"

namespace dsh::game {

inline constexpr std::uint64_t kHealthBroadcastMs = 250;

[[nodiscard]] std::vector<HealthEntry> host_collect_health_changes(bool force, std::string& note);

struct HealthApplyResult {

    std::uint32_t received = 0;
    std::uint32_t entries = 0;
    std::uint32_t written = 0;
    std::uint32_t failed = 0;
    std::uint32_t decoded = 0;
    std::uint32_t nameNotFound = 0;
    std::string   note;

    std::vector<std::string> appliedNames;
    std::vector<std::string> failures;
    [[nodiscard]] std::vector<std::string> report_lines() const;
};

[[nodiscard]] bool client_apply_health_broadcast(std::span<const std::uint8_t> payload,
                                                 HealthApplyResult& out);

[[nodiscard]] std::vector<HealthEntry> client_collect_damage_reports(std::uint32_t firstSequence,
                                                                   std::string& note);

struct DamageEventHit {
    std::uintptr_t actor = 0;
    float          effectiveKinetic = 0.0f;
    float          effectiveThermal = 0.0f;
    std::uint64_t  ms = 0;
};

void note_damage_event(std::uintptr_t actor, const float params[5], std::uint64_t ms);

struct DamageEventStats {
    std::uint64_t recorded = 0;
    std::uint64_t dropped = 0;
    std::uint64_t drained = 0;
};
[[nodiscard]] DamageEventStats damage_event_stats();

struct ActionEdge {
    std::uint32_t action = 0;
    std::uint32_t module = 0;
    bool          pressed = false;
    std::uint64_t ms = 0;
};
struct ActionEdgeStats {
    std::uint64_t noted = 0;
    std::uint64_t dropped = 0;
    std::uint64_t drained = 0;
    std::uint64_t echoDropped = 0;
};
void note_action_edge(std::uint32_t action, std::uint32_t module, bool pressed, std::uint64_t ms);
[[nodiscard]] bool take_action_edge(ActionEdge& out);
[[nodiscard]] ActionEdgeStats action_edge_stats();

struct DeathNote {
    std::uintptr_t actor = 0;
    std::uint64_t  ms = 0;
    bool           multicast = false;
};
void set_death_functions(std::uintptr_t multicastOnDeath, std::uintptr_t receiveOnDeath);
[[nodiscard]] std::uintptr_t death_multicast_function();
[[nodiscard]] std::uintptr_t death_receive_function();
void note_death(std::uintptr_t actor, std::uint64_t ms, bool multicast);
[[nodiscard]] bool take_death(DeathNote& out);
[[nodiscard]] std::uint64_t deaths_dropped();

enum class CheckpointSignal : std::uint8_t {
    none = 0,
    dialogue = 1,
    regressionPoint = 2,
    save = 3,
};
void set_checkpoint_functions(std::uintptr_t dialogue, std::uintptr_t regressionPoint, std::uintptr_t save);
[[nodiscard]] std::uintptr_t checkpoint_dialogue_function();
[[nodiscard]] std::uintptr_t checkpoint_regression_function();
[[nodiscard]] std::uintptr_t checkpoint_save_function();
void note_checkpoint(std::uint8_t signal);

[[nodiscard]] bool take_checkpoint(std::uint8_t& signal);

inline constexpr std::uint32_t kShotActionBase = 16;

inline constexpr std::uint32_t kSupplyActionBase = 17;

void set_local_weapon_modules(std::uintptr_t front, std::uintptr_t left, std::uintptr_t right);

[[nodiscard]] int local_weapon_module_slot(std::uintptr_t actor);

void set_puppet_owners(const std::uintptr_t* actors, std::size_t count);
[[nodiscard]] bool damage_is_from_puppet(std::uintptr_t causerOwner);

[[nodiscard]] std::uint64_t puppet_damage_dropped();

void set_action_replay_in_flight(bool inFlight);
[[nodiscard]] bool action_replay_in_flight();

void set_action_capture_enabled(bool enabled);
[[nodiscard]] bool action_capture_enabled();

struct HealthReportResult {
    std::uint32_t received = 0;
    std::uint32_t applied = 0;
    std::uint32_t duplicate = 0;
    std::uint32_t refused = 0;
    std::uint32_t nameNotFound = 0;
    std::uint32_t writeFailed = 0;
    std::string   note;
    std::vector<std::string> lines;

    std::vector<std::string> enemyNames;
    [[nodiscard]] std::vector<std::string> report_lines() const;
};

[[nodiscard]] bool host_apply_health_report(std::span<const std::uint8_t> payload, HealthReportResult& out);

void health_sync_reset();

[[nodiscard]] bool health_resolve_actor(const std::string& name, std::uintptr_t& actorOut,
                                        std::string& note);

}
