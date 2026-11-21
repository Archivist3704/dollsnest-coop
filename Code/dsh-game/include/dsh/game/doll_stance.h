#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "dsh/game/front_weapon.h"
#include "dsh/game/movement_input.h"
#include "dsh/net/bytes.h"

namespace dsh::game {

inline constexpr std::uint8_t kStanceNeutral = 0;
inline constexpr std::uint8_t kStanceFight   = 1;
inline constexpr std::uint8_t kStanceBeware  = 2;

inline constexpr std::uint8_t kStanceHighestReal = kStanceBeware;

[[nodiscard]] const char* stance_name(std::uint8_t stance) noexcept;

inline constexpr std::uintptr_t kCharacterDefaultStanceOffset = 0x10F5;
inline constexpr std::uintptr_t kCharacterCurrentStanceOffset = 0x10F6;

inline constexpr std::uintptr_t kPawnRemoteViewPitchOffset = 0x0232;

inline constexpr std::uintptr_t kControllerControlRotationOffset = 0x0288;

[[nodiscard]] inline std::uint8_t remote_view_pitch_byte(float degrees) noexcept {
    float turn = degrees;
    while (turn < 0.0f) turn += 360.0f;
    while (turn >= 360.0f) turn -= 360.0f;
    return static_cast<std::uint8_t>(turn * 256.0f / 360.0f);
}

inline constexpr const char* kStanceApiClass = "DACharacter";

inline constexpr std::size_t kStatePayloadFloats = 5;

inline constexpr std::size_t kStatePayloadVelocityBytes = 3 * 4;
inline constexpr std::size_t kStatePayloadModeBytes     = 1;

inline constexpr std::size_t kStatePayloadInputFlagBytes = 5;
inline constexpr std::size_t kStatePayloadInputVectorBytes = 3 * 4;

inline constexpr std::size_t kStatePayloadEnergyTimerBytes = 2 * 4;

inline constexpr std::size_t kStatePayloadFrontWeaponBytes  = 1;

inline constexpr std::size_t kStatePayloadAccelerationBytes = 3 * 4;

inline constexpr std::size_t kStatePayloadExclusiveBytes = 2;
inline constexpr std::size_t kStatePayloadBytes = kStatePayloadFloats * 4 + 2 +
                                                 kStatePayloadVelocityBytes + kStatePayloadModeBytes +
                                                 kStatePayloadInputFlagBytes +
                                                 kStatePayloadInputVectorBytes +
                                                 kStatePayloadEnergyTimerBytes +
                                                 kStatePayloadFrontWeaponBytes +
                                                 kStatePayloadAccelerationBytes +
                                                 kStatePayloadExclusiveBytes;

static_assert(kStatePayloadBytes == 75,
              "the state payload is the five pose floats, the two stance bytes, the host's velocity, "
              "the host's MovementMode, the five movement-input flags, the control-input vector, the "
              "host's two energy timers, the host's front-weapon choice, the host's three acceleration "
              "float32 and the host's exclusive-mode pair -- 75 bytes, and both ends must agree on this "
              "number");
static_assert(kStatePayloadBytes == 60 + kStatePayloadFrontWeaponBytes +
                                        kStatePayloadAccelerationBytes + kStatePayloadExclusiveBytes,
              "the payload grew by exactly the front-weapon byte, then exactly three acceleration float32, "
              "then exactly the exclusive-mode pair -- written as a sum so a later edit that quietly widened "
              "another field fails here");

struct DollStance {
    std::uint8_t defaultStance = kStanceNeutral;
    std::uint8_t currentStance = kStanceNeutral;

    [[nodiscard]] bool operator==(const DollStance& other) const noexcept {
        return defaultStance == other.defaultStance && currentStance == other.currentStance;
    }
    [[nodiscard]] bool operator!=(const DollStance& other) const noexcept {
        return !(*this == other);
    }
};

struct StanceWireState {
    float posM[3] = {0.0f, 0.0f, 0.0f};
    float yawRad = 0.0f;
    float pitchRad = 0.0f;
    DollStance stance;

    float        velocity[3] = {0.0f, 0.0f, 0.0f};
    std::uint8_t movementMode = 0;

    MovementInputs movementInput;

    EnergyTimers energyTimers;

    std::uint8_t frontWeaponIndex = kFrontWeaponNotRead;

    float acceleration[3] = {0.0f, 0.0f, 0.0f};

    std::uint8_t exclusiveTypes = 0;
    std::uint8_t exclusiveOwnerKind = 0;
};

[[nodiscard]] std::size_t encode_stance_state(dsh::net::ByteWriter& w, const StanceWireState& state);

[[nodiscard]] bool decode_stance_state(std::span<const std::uint8_t> bytes, StanceWireState& out,
                                       std::string& note);

using StanceByteReader = bool (*)(std::uintptr_t address, void* dst, std::size_t bytes);

[[nodiscard]] bool stance_read_process_bytes(std::uintptr_t address, void* dst, std::size_t bytes);

[[nodiscard]] bool actor_class_chain_reaches(std::uintptr_t actor, std::uintptr_t wantedClass,
                                             StanceByteReader read) noexcept;

[[nodiscard]] bool read_doll_stance(std::uintptr_t actor, std::uintptr_t characterClass, DollStance& out,
                                    std::string& note, StanceByteReader read);

struct StanceApplyResult {
    bool         applied = false;
    bool         changed = false;
    std::uint8_t written = 0;
    std::string  note;
};

[[nodiscard]] StanceApplyResult doll_stance_apply(std::uintptr_t actor, std::uintptr_t characterClass,
                                                  const DollStance& incoming);

}
