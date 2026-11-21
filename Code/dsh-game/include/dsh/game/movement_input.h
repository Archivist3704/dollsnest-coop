#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dsh::game {

inline constexpr std::uintptr_t kCharacterIgnoreFrictionOffset = 0x06E0;
inline constexpr std::uintptr_t kCharacterHoverOffset          = 0x06E1;
inline constexpr std::uintptr_t kCharacterSprintOffset         = 0x06E2;
inline constexpr std::uintptr_t kCharacterBoostJumpOffset      = 0x06F0;
inline constexpr std::uintptr_t kCharacterBoostDashOffset      = 0x06F1;
inline constexpr std::uintptr_t kCharacterRecentInputOffset    = 0x06F4;

inline constexpr std::uintptr_t kCharacterIntermissionTimeOffset    = 0x0700;
inline constexpr std::uintptr_t kCharacterFastChargeStartOffset     = 0x0704;
inline constexpr std::size_t    kEnergyTimerBytes                   = 8;

inline constexpr std::uintptr_t kCharacterAbilitySystemOffset    = 0x0860;

inline constexpr std::uintptr_t kMovementInputFlagGroupEnd  = 0x06F0 + 12;
inline constexpr std::uintptr_t kMovementInputVectorEnd     = kCharacterRecentInputOffset + 3 * 4;

inline constexpr const char* kMovementInputApiClass = "DACharacter";

struct MovementInputs {
    std::uint8_t ignoreFriction = 0;
    std::uint8_t hover = 0;
    std::uint8_t sprint = 0;
    std::uint8_t boostJump = 0;
    std::uint8_t boostDash = 0;
    float        recentInput[3] = {0.0f, 0.0f, 0.0f};

    [[nodiscard]] bool operator==(const MovementInputs& other) const noexcept {
        return ignoreFriction == other.ignoreFriction && hover == other.hover &&
               sprint == other.sprint && boostJump == other.boostJump &&
               boostDash == other.boostDash && recentInput[0] == other.recentInput[0] &&
               recentInput[1] == other.recentInput[1] && recentInput[2] == other.recentInput[2];
    }
    [[nodiscard]] bool operator!=(const MovementInputs& other) const noexcept {
        return !(*this == other);
    }

    [[nodiscard]] bool flagsSame(const MovementInputs& other) const noexcept {
        return ignoreFriction == other.ignoreFriction && hover == other.hover &&
               sprint == other.sprint && boostJump == other.boostJump &&
               boostDash == other.boostDash;
    }
    [[nodiscard]] bool vectorSame(const MovementInputs& other) const noexcept {
        return recentInput[0] == other.recentInput[0] && recentInput[1] == other.recentInput[1] &&
               recentInput[2] == other.recentInput[2];
    }
};

using MovementInputByteReader = bool (*)(std::uintptr_t address, void* dst, std::size_t bytes);

[[nodiscard]] bool read_movement_inputs(std::uintptr_t actor, std::uintptr_t daCharacterClass,
                                        MovementInputs& out, std::string& note,
                                        MovementInputByteReader read);

struct AbilitySystemState {
    std::uintptr_t abilitySystem = 0;
    std::string    className;
};

[[nodiscard]] bool read_character_ability_system(std::uintptr_t actor, std::uintptr_t daCharacterClass,
                                                 AbilitySystemState& out, std::string& note,
                                                 MovementInputByteReader read);

struct EnergyTimers {
    float intermissionTime = 0.0f;
    float fastChargeStartTime = 0.0f;

    [[nodiscard]] bool operator==(const EnergyTimers& other) const noexcept {
        return intermissionTime == other.intermissionTime &&
               fastChargeStartTime == other.fastChargeStartTime;
    }
    [[nodiscard]] bool operator!=(const EnergyTimers& other) const noexcept {
        return !(*this == other);
    }
};

[[nodiscard]] bool read_energy_timers(std::uintptr_t actor, std::uintptr_t daCharacterClass,
                                      EnergyTimers& out, std::string& note,
                                      MovementInputByteReader read);

struct EnergyTimerApplyResult {
    bool         applied = false;
    bool         changed = false;
    EnergyTimers written;
    EnergyTimers before;
    bool         beforeRead = false;
    bool         writtenRead = false;
    std::string  note;
};

[[nodiscard]] EnergyTimerApplyResult puppet_energy_timers_apply(std::uintptr_t actor,
                                                               std::uintptr_t daCharacterClass,
                                                               const EnergyTimers& incoming);

struct MovementInputApplyResult {
    bool         flagsApplied = false;
    bool         vectorApplied = false;
    bool         flagsChanged = false;
    bool         vectorChanged = false;
    MovementInputs written;
    MovementInputs before;
    std::string  note;
};

[[nodiscard]] MovementInputApplyResult puppet_movement_input_apply(std::uintptr_t actor,
                                                                   std::uintptr_t daCharacterClass,
                                                                   const MovementInputs& incoming);

[[nodiscard]] std::string movement_input_line(const MovementInputs& in);

[[nodiscard]] std::string energy_timer_line(const EnergyTimers& in, bool readable, bool havePrevious,
                                            float previousIntermissionTime,
                                            const std::string& refusal);

}
