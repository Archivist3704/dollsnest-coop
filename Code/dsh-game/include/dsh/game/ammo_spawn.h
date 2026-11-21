#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dsh/reflect/spawn.h"
namespace dsh::game {

inline constexpr std::size_t kAmmoParamsBytes = 0x120;

inline constexpr std::size_t kAmmoSpeedCurveOffset = 0x18;
inline constexpr std::size_t kAmmoPointCurveOffset = 0x48;
inline constexpr std::size_t kAmmoDamageTypeOffset = 0xA8;
inline constexpr std::size_t kAmmoPointerOffsets[3] = {kAmmoSpeedCurveOffset, kAmmoPointCurveOffset,
                                                       kAmmoDamageTypeOffset};

inline constexpr std::size_t kAmmoPointDamageOffset = 0x24;
inline constexpr std::size_t kAmmoPointDamageMinOffset = 0x40;
inline constexpr std::size_t kAmmoRadialDamageOffset = 0x54;
inline constexpr std::size_t kAmmoRadialMinimumDamageOffset = 0x74;
inline constexpr std::size_t kAmmoDamageFloats = 5;

inline constexpr std::size_t kMaxAmmoPath = 240;

inline constexpr std::size_t kMaxAmmoName = 96;

inline constexpr std::size_t kAmmoSpawnCapacity = 128;

struct AmmoCopyDamage {
    bool  sawPointDamage = false;
    bool  sawRadialDamage = false;
    float pointBefore[kAmmoDamageFloats] = {};
    float pointMinBefore = 0.0f;
    float radialBefore[kAmmoDamageFloats] = {};
    float radialMinBefore[kAmmoDamageFloats] = {};
};

[[nodiscard]] AmmoCopyDamage clear_copy_damage(unsigned char* params, std::size_t size) noexcept;

struct AmmoSpawnSample {
    std::uintptr_t actorClass = 0;
    std::uintptr_t actor = 0;

    unsigned char  params[kAmmoParamsBytes] = {};
    std::uintptr_t speedCurve = 0;
    std::uintptr_t pointDamageCurve = 0;
    std::uintptr_t damageType = 0;
    std::uint64_t  ms = 0;
};

void set_ammo_params_function(std::uintptr_t function);
[[nodiscard]] std::uintptr_t ammo_params_function();

void set_ammo_capture_enabled(bool enabled);
[[nodiscard]] bool ammo_capture_enabled();

void note_ammo_spawn(const AmmoSpawnSample& sample);

[[nodiscard]] bool take_ammo_spawn(AmmoSpawnSample& out);

struct AmmoSpawnStats {
    std::uint64_t noted = 0;
    std::uint64_t dropped = 0;
    std::uint64_t drained = 0;
    std::uint64_t refused = 0;

    std::uint64_t foreign = 0;
};
[[nodiscard]] AmmoSpawnStats ammo_spawn_stats();

void note_ammo_refused();

void note_ammo_foreign();

void request_process_event_trace(std::uint64_t untilMs);
[[nodiscard]] std::uint64_t process_event_trace_until_ms();

inline constexpr std::uint8_t kAmmoWireFormat = 3;

inline constexpr std::uint8_t kAmmoTargetNone     = 0;
inline constexpr std::uint8_t kAmmoTargetOwnDoll  = 1;
inline constexpr std::uint8_t kAmmoTargetEnemy    = 2;
inline constexpr std::uint8_t kAmmoTargetRefused  = 3;

[[nodiscard]] std::vector<std::uint8_t> encode_ammo_spawn(const dsh::reflect::AmmoSpawnRequest& request,
                                                          std::string& note);

[[nodiscard]] bool decode_ammo_spawn(std::span<const std::uint8_t> bytes,
                                      dsh::reflect::AmmoSpawnRequest& out, std::string& note);

}
