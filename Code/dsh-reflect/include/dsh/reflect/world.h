#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace dsh::reflect {

namespace world_offsets {
inline constexpr std::uint32_t kPersistentLevel   = 0x0030;
inline constexpr std::uint32_t kNetDriver         = 0x0038;
inline constexpr std::uint32_t kNetworkManager    = 0x0058;
inline constexpr std::uint32_t kAuthorityGameMode = 0x0118;
inline constexpr std::uint32_t kGameState         = 0x0120;
inline constexpr std::uint32_t kLevels            = 0x0138;

inline constexpr std::uint32_t kComponentsNeedEndOfFrameUpdate            = 0x0250;
inline constexpr std::uint32_t kComponentsNeedEndOfFrameUpdateOnGameThread = 0x0260;
}

namespace level_offsets {
inline constexpr std::uint32_t kActors       = 0x0098;
inline constexpr std::uint32_t kOwningWorld  = 0x00B8;
}

namespace actor_offsets {

inline constexpr std::uint32_t kControllerPawn = 0x0250;

inline constexpr std::uint32_t kAcknowledgedPawn = 0x02A0;
}

struct LiveObjects {
    std::uintptr_t world = 0;
    std::uintptr_t level = 0;
    std::uintptr_t playerController = 0;
    std::uintptr_t pawn = 0;
    std::uint32_t  worldObjectCount = 0;
};

[[nodiscard]] LiveObjects find_live_objects();

[[nodiscard]] LiveObjects rescan_live_objects();

struct WorldSample {
    std::uintptr_t address = 0;
    std::string    name;
    std::uintptr_t persistentLevel = 0;
    std::uint64_t  rawNetDriver = 0;
    std::uintptr_t netDriver = 0;
    std::uintptr_t authorityGameMode = 0;
};

struct NetObjectReading {
    std::uintptr_t address = 0;
    std::string    className;
    std::uintptr_t driver = 0;
    std::string    driverClass;
    std::string    connectionClass;
    std::uint32_t  clientConnections = 0;
    std::uintptr_t serverConnection = 0;
    float          connectionTimeout = 0.0f;
};

struct WorldSurvey {
    std::vector<WorldSample> worlds;
    std::vector<NetObjectReading> drivers;
    std::vector<NetObjectReading> pending;
    std::uint32_t scanned = 0;
    [[nodiscard]] std::string line() const;
};

[[nodiscard]] WorldSurvey survey_worlds();

struct MapIdentity {
    std::uintptr_t world = 0;
    std::uintptr_t level = 0;
    std::uintptr_t package = 0;
    std::uintptr_t pawnWorld = 0;
    std::string   key;
    std::string   keySource;
    std::string   worldName;
    std::string   pawnWorldName;
    std::string   levelName;
    std::string   packageName;
    std::string   note;
    [[nodiscard]] std::string line() const;
};

[[nodiscard]] MapIdentity current_map_identity();

struct NetDriverDefinitionInfo {
    std::string defName;
    std::string driverClass;
    std::string fallbackClass;

    bool driverClassPresent = false;
    bool fallbackClassPresent = false;
};

struct NetDriverDefinitionSurvey {
    std::uintptr_t engine = 0;
    std::string    engineClass;
    std::uint32_t  count = 0;
    std::vector<NetDriverDefinitionInfo> definitions;

    std::uintptr_t gameMode = 0;
    std::uint8_t   gameModeFlags = 0;
    bool           seamlessTravel = false;
    std::string    note;
    [[nodiscard]] std::string line() const;
};

[[nodiscard]] NetDriverDefinitionSurvey survey_net_driver_definitions();

void walk_note(const char* what, long long microseconds);

class ScopedWalkTimer {
public:
    explicit ScopedWalkTimer(const char* what)
        : what_(what), begin_(std::chrono::steady_clock::now()) {}
    ~ScopedWalkTimer() {
        const long long us = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - begin_)
                                 .count();
        walk_note(what_, us);
    }
    ScopedWalkTimer(const ScopedWalkTimer&) = delete;
    ScopedWalkTimer& operator=(const ScopedWalkTimer&) = delete;

private:
    const char* what_;
    std::chrono::steady_clock::time_point begin_;
};

#define DSH_TIME_WALK_PASTE_(a, b) a##b
#define DSH_TIME_WALK_PASTE(a, b) DSH_TIME_WALK_PASTE_(a, b)
#define DSH_TIME_WALK(label) \
    ::dsh::reflect::ScopedWalkTimer DSH_TIME_WALK_PASTE(dsh_walk_timer_, __LINE__)(label)

struct DriverClassSwap {
    bool        swapped = false;
    std::string defName;
    std::string before;
    std::string after;
    std::string note;
};

[[nodiscard]] DriverClassSwap prefer_ip_net_driver();

[[nodiscard]] DriverClassSwap restore_net_driver_class();

[[nodiscard]] std::uintptr_t live_world_address();

[[nodiscard]] bool resolve_fname(std::uint32_t index, std::string& out, std::string& note);

[[nodiscard]] std::string object_name_of_actor(std::uintptr_t actor);

}
