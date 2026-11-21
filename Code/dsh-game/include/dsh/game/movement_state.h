#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dsh::game {

inline constexpr std::uintptr_t kCharacterMovementOffset    = 0x0288;
inline constexpr std::uintptr_t kDACharacterMovementOffset  = 0x06D0;

inline constexpr std::uintptr_t kMovementVelocityOffset     = 0x00C4;

inline constexpr std::uintptr_t kMovementModeOffset          = 0x0168;
inline constexpr std::uintptr_t kCustomMovementModeOffset    = 0x0169;
inline constexpr std::uintptr_t kGravityScaleOffset          = 0x0150;
inline constexpr std::uintptr_t kMaxWalkSpeedOffset          = 0x018C;
inline constexpr std::uintptr_t kMaxAccelerationOffset       = 0x01A0;
inline constexpr std::uintptr_t kAccelerationOffset          = 0x022C;
inline constexpr std::uintptr_t kLastUpdateLocationOffset    = 0x0250;
inline constexpr std::uintptr_t kLastUpdateVelocityOffset    = 0x025C;
inline constexpr std::uintptr_t kJustTeleportedByteOffset    = 0x0387;
inline constexpr std::uint8_t   kJustTeleportedBitIndex      = 5;
inline constexpr std::uintptr_t kRequestedVelocityFlagOffset = 0x038A;
inline constexpr std::uint8_t   kRequestedVelocityBitIndex   = 1;
inline constexpr std::uintptr_t kRequestedVelocityOffset     = 0x03A0;

inline constexpr std::uintptr_t kNoControllerPhysicsByteOffset  = 0x01F1;
inline constexpr std::uint8_t   kNoControllerPhysicsBitIndex    = 2;
inline constexpr std::uintptr_t kDefaultLandMovementModeOffset   = 0x0384;

inline constexpr std::uintptr_t kPawnControllerOffset         = 0x0258;
inline constexpr std::uintptr_t kActorReplicatedMovementOffset = 0x0060;
inline constexpr std::uintptr_t kReplicatedLinearVelocityOffset = 0x0000;

inline constexpr std::uint8_t kMoveNone      = 0;
inline constexpr std::uint8_t kMoveWalking   = 1;
inline constexpr std::uint8_t kMoveNavWalking = 2;
inline constexpr std::uint8_t kMoveFalling   = 3;
inline constexpr std::uint8_t kMoveSwimming  = 4;
inline constexpr std::uint8_t kMoveFlying    = 5;
inline constexpr std::uint8_t kMoveCustom    = 6;
inline constexpr std::uint8_t kMoveMax       = 7;

[[nodiscard]] const char* movement_mode_name(std::uint8_t mode) noexcept;

enum class ModeWriteDecision : std::uint8_t {
    writeOk = 0,
    alreadyNone = 1,
    outsideEnum = 2,
};

[[nodiscard]] ModeWriteDecision mode_write_decision(std::uint8_t mode) noexcept;

struct MachineMovementState {
    std::uintptr_t actor = 0;
    std::uintptr_t controller = 0;
    std::uintptr_t component = 0;
    std::uintptr_t componentOffset = 0;
    std::uintptr_t otherComponent = 0;
    std::uintptr_t otherComponentOffset = 0;
    bool           useDACharacterMovement = false;

    float replicatedLinearVelocity[3] = {0.0f, 0.0f, 0.0f};

    float velocity[3] = {0.0f, 0.0f, 0.0f};
    float acceleration[3] = {0.0f, 0.0f, 0.0f};
    float requestedVelocity[3] = {0.0f, 0.0f, 0.0f};
    float lastUpdateLocation[3] = {0.0f, 0.0f, 0.0f};
    float lastUpdateVelocity[3] = {0.0f, 0.0f, 0.0f};
    float gravityScale = 0.0f;
    float maxWalkSpeed = 0.0f;
    float maxAcceleration = 0.0f;
    std::uint8_t movementMode = 0;
    std::uint8_t customMovementMode = 0;
    bool  justTeleported = false;
    bool  hasRequestedVelocity = false;

    bool  noControllerPhysicsRead = false;
    bool  runPhysicsWithNoController = false;
    bool  defaultLandMovementModeRead = false;
    std::uint8_t defaultLandMovementMode = 0;

    bool  lastUpdateLocationRead = false;

    bool         readable = false;
    std::string  note;

    [[nodiscard]] float speed() const noexcept;
};

inline constexpr const char* kMovementApiClass = "DACharacter";

using MovementByteReader = bool (*)(std::uintptr_t address, void* dst, std::size_t bytes);

[[nodiscard]] MachineMovementState movement_state_probe(std::uintptr_t actor,
                                                        std::uintptr_t daCharacterClass,
                                                        MovementByteReader read);

[[nodiscard]] std::uintptr_t movement_component_address(std::uintptr_t actor,
                                                        std::uintptr_t& offset,
                                                        std::uintptr_t& other,
                                                        MovementByteReader read) noexcept;

struct MachineVelocity {
    float          value[3] = {0.0f, 0.0f, 0.0f};
    std::uintptr_t component = 0;
    std::uintptr_t componentOffset = 0;
    std::string    note;
};

[[nodiscard]] bool read_machine_velocity(std::uintptr_t actor, std::uintptr_t daCharacterClass,
                                         MachineVelocity& out, MovementByteReader read);

struct VelocityApplyResult {
    bool         applied = false;
    bool         changed = false;
    std::uint8_t movementMode = 0;
    float        written[3] = {0.0f, 0.0f, 0.0f};
    std::string  note;

    bool         accelerationRequested = false;
    bool         accelerationApplied = false;
    bool         accelerationChanged = false;
    float        accelerationWritten[3] = {0.0f, 0.0f, 0.0f};
    std::string  accelerationNote;

    bool         modeRequested = false;
    std::uint8_t modeAsked = 0;
    bool         modeWritten = false;
    bool         modeApplied = false;
    std::uint8_t movementModeAfter = 0;
    bool         modeReadBack = false;
    std::string  modeNote;

    bool  locationRead = false;
    float location[3] = {0.0f, 0.0f, 0.0f};
};

[[nodiscard]] VelocityApplyResult puppet_velocity_apply(std::uintptr_t actor,
                                                        std::uintptr_t daCharacterClass,
                                                        const float velocity[3],
                                                        std::uint8_t requestedMode = kMoveNone,
                                                        const float acceleration[3] = nullptr);

[[nodiscard]] std::string movement_state_line(const char* label, const MachineMovementState& state,
                                              float windowMaxSpeed, const char* trailing = nullptr);

}
