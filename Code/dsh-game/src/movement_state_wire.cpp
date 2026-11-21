#include "dsh/game/movement_state.h"

#include "dsh/game/doll_stance.h"

#include <cmath>
#include <cstdio>

namespace dsh::game {
namespace {

bool finite3(const float v[3]) noexcept {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

bool read_bytes(MovementByteReader read, std::uintptr_t address, void* dst, std::size_t bytes) {
    return read != nullptr && read(address, dst, bytes);
}

bool read_vec(MovementByteReader read, std::uintptr_t address, float out[3]) {
    return read_bytes(read, address, out, sizeof(float) * 3);
}

bool read_f32(MovementByteReader read, std::uintptr_t address, float& out) {
    return read_bytes(read, address, &out, sizeof(out));
}

bool read_u8(MovementByteReader read, std::uintptr_t address, std::uint8_t& out) {
    return read_bytes(read, address, &out, sizeof(out));
}

bool read_uptr(MovementByteReader read, std::uintptr_t address, std::uintptr_t& out) {
    std::uint64_t value = 0;
    if (!read_bytes(read, address, &value, sizeof(value))) return false;
    out = static_cast<std::uintptr_t>(value);
    return true;
}

bool bit_set(std::uint8_t byte, std::uint8_t index) noexcept {
    return ((byte >> index) & 0x1u) != 0;
}

std::string f1(float value) {
    if (!std::isfinite(value)) return "n/a";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(value));
    return buf;
}

std::string vec3(const float v[3]) {
    if (!finite3(v)) return "n/a";
    char buf[80];
    std::snprintf(buf, sizeof(buf), "(%.1f, %.1f, %.1f)", static_cast<double>(v[0]),
                  static_cast<double>(v[1]), static_cast<double>(v[2]));
    return buf;
}

}

float MachineMovementState::speed() const noexcept {
    return std::sqrt((velocity[0] * velocity[0]) + (velocity[1] * velocity[1]) +
                     (velocity[2] * velocity[2]));
}

const char* movement_mode_name(std::uint8_t mode) noexcept {
    switch (mode) {
        case kMoveNone:       return "MOVE_None";
        case kMoveWalking:    return "MOVE_Walking";
        case kMoveNavWalking: return "MOVE_NavWalking";
        case kMoveFalling:    return "MOVE_Falling";
        case kMoveSwimming:   return "MOVE_Swimming";
        case kMoveFlying:     return "MOVE_Flying";
        case kMoveCustom:     return "MOVE_Custom";
        default:              return "?";
    }
}

ModeWriteDecision mode_write_decision(std::uint8_t mode) noexcept {

    if (mode > kMoveCustom) return ModeWriteDecision::outsideEnum;
    if (mode == kMoveNone) return ModeWriteDecision::alreadyNone;
    return ModeWriteDecision::writeOk;
}

std::uintptr_t movement_component_address(std::uintptr_t actor, std::uintptr_t& offset,
                                          std::uintptr_t& other, MovementByteReader read) noexcept {
    offset = 0;
    other = 0;
    if (actor < 0x10000 || read == nullptr) return 0;

    std::uintptr_t viaCharacter = 0;
    std::uintptr_t viaDACharacter = 0;

    if (!read_uptr(read, actor + kCharacterMovementOffset, viaCharacter)) return 0;
    if (!read_uptr(read, actor + kDACharacterMovementOffset, viaDACharacter)) return 0;

    if (viaCharacter >= 0x10000) {
        offset = kCharacterMovementOffset;
        other = viaDACharacter >= 0x10000 ? viaDACharacter : 0;
        return viaCharacter;
    }
    if (viaDACharacter >= 0x10000) {
        offset = kDACharacterMovementOffset;
        return viaDACharacter;
    }
    return 0;
}

bool read_machine_velocity(std::uintptr_t actor, std::uintptr_t daCharacterClass,
                           MachineVelocity& out, MovementByteReader read) {
    out = MachineVelocity{};

    if (actor < 0x10000) {
        out.note = "there is no actor to read a velocity from (the pointer is implausible)";
        return false;
    }
    if (read == nullptr) {
        out.note = "no memory reader was supplied";
        return false;
    }
    if (daCharacterClass == 0) {
        out.note = "the " + std::string(kMovementApiClass) +
                   " class could not be resolved, so the actor's class chain cannot be checked and "
                   "NOTHING is read from its movement component";
        return false;
    }

    if (!actor_class_chain_reaches(actor, daCharacterClass, read)) {
        out.note = "the actor's class chain does not reach " + std::string(kMovementApiClass) +
                   ", so and are not movement components on it and NOTHING is read "
                   "from them";
        return false;
    }

    std::uintptr_t other = 0;
    out.component = movement_component_address(actor, out.componentOffset, other, read);
    if (out.component == 0) {
        out.note = "neither movement component pointer is set ( reads 0x0 and reads 0x0) "
                   "-- there is no Velocity to read on this actor";
        return false;
    }

    if (!read_vec(read, out.component + kMovementVelocityOffset, out.value)) {
        out.note = "UMovementComponent::Velocity at component + 0x00C4 could not be read";
        return false;
    }
    out.note.clear();
    return true;
}

MachineMovementState movement_state_probe(std::uintptr_t actor, std::uintptr_t daCharacterClass,
                                          MovementByteReader read) {
    MachineMovementState s;
    s.actor = actor;

    if (actor < 0x10000) {
        s.note = "there is no actor to read (the pointer is implausible)";
        return s;
    }
    if (read == nullptr) {
        s.note = "no memory reader was supplied";
        return s;
    }
    if (daCharacterClass == 0) {

        s.note = "the ADACharacter class could not be resolved, so the actor's class chain cannot be "
                 "checked and NOTHING is read from its movement component";
        return s;
    }
    if (!actor_class_chain_reaches(actor, daCharacterClass, read)) {
        s.note = "the actor's class chain does not reach ADACharacter, so and are not "
                 "movement components on it and NOTHING is read from them";
        return s;
    }

    if (!read_uptr(read, actor + kPawnControllerOffset, s.controller)) {
        s.note = "the actor's Controller at could not be read";
        return s;
    }

    if (!read_vec(read, actor + kActorReplicatedMovementOffset + kReplicatedLinearVelocityOffset,
                  s.replicatedLinearVelocity)) {
        s.note = "AActor::ReplicatedMovement.LinearVelocity at actor + 0x0060 could not be read";
        return s;
    }

    std::uintptr_t viaCharacter = 0;
    std::uintptr_t viaDACharacter = 0;
    if (!read_uptr(read, actor + kCharacterMovementOffset, viaCharacter)) {
        s.note = "ACharacter::CharacterMovement at could not be read";
        return s;
    }
    if (!read_uptr(read, actor + kDACharacterMovementOffset, viaDACharacter)) {
        s.note = "ADACharacter::DACharacterMovement at could not be read";
        return s;
    }

    std::uintptr_t otherPointer = 0;
    s.component = movement_component_address(actor, s.componentOffset, otherPointer, read);
    if (s.component == 0) {

        s.note = "neither movement component pointer is set ( reads 0x0 and reads 0x0) -- "
                 "there is no MovementMode to report on this actor";
        return s;
    }
    s.useDACharacterMovement = s.componentOffset == kDACharacterMovementOffset;

    s.otherComponentOffset = s.useDACharacterMovement ? kCharacterMovementOffset
                                                      : kDACharacterMovementOffset;
    if (s.useDACharacterMovement) {
        s.otherComponent = 0;
    } else {
        s.otherComponent = viaDACharacter >= 0x10000 ? viaDACharacter : 0;
    }

    const std::uintptr_t c = s.component;

    if (!read_u8(read, c + kMovementModeOffset, s.movementMode)) {
        s.note = "MovementMode at component + 0x0168 could not be read";
        return s;
    }
    if (!read_u8(read, c + kCustomMovementModeOffset, s.customMovementMode)) {
        s.note = "CustomMovementMode at component + 0x0169 could not be read";
        return s;
    }

    if (!read_vec(read, c + kMovementVelocityOffset, s.velocity)) {
        s.note = "UMovementComponent::Velocity at component + 0x00C4 could not be read";
        return s;
    }
    if (!read_vec(read, c + kAccelerationOffset, s.acceleration)) {
        s.note = "Acceleration at component + 0x022C could not be read";
        return s;
    }
    if (!read_f32(read, c + kGravityScaleOffset, s.gravityScale)) {
        s.note = "GravityScale at component + 0x0150 could not be read";
        return s;
    }
    if (!read_f32(read, c + kMaxWalkSpeedOffset, s.maxWalkSpeed)) {
        s.note = "MaxWalkSpeed at component + 0x018C could not be read";
        return s;
    }
    if (!read_f32(read, c + kMaxAccelerationOffset, s.maxAcceleration)) {
        s.note = "MaxAcceleration at component + 0x01A0 could not be read";
        return s;
    }
    if (!read_vec(read, c + kLastUpdateLocationOffset, s.lastUpdateLocation)) {
        s.note = "LastUpdateLocation at component + 0x0250 could not be read";
        return s;
    }
    s.lastUpdateLocationRead = true;
    if (!read_vec(read, c + kLastUpdateVelocityOffset, s.lastUpdateVelocity)) {
        s.note = "LastUpdateVelocity at component + 0x025C could not be read";
        return s;
    }
    if (!read_vec(read, c + kRequestedVelocityOffset, s.requestedVelocity)) {
        s.note = "RequestedVelocity at component + 0x03A0 could not be read";
        return s;
    }

    std::uint8_t teleportByte = 0;
    if (!read_u8(read, c + kJustTeleportedByteOffset, teleportByte)) {
        s.note = "the bitfield byte at component + 0x0387 could not be read";
        return s;
    }
    s.justTeleported = bit_set(teleportByte, kJustTeleportedBitIndex);

    std::uint8_t requestedByte = 0;
    if (!read_u8(read, c + kRequestedVelocityFlagOffset, requestedByte)) {
        s.note = "the bitfield byte at component + 0x038A could not be read";
        return s;
    }
    s.hasRequestedVelocity = bit_set(requestedByte, kRequestedVelocityBitIndex);

    std::uint8_t noControllerByte = 0;
    if (!read_u8(read, c + kNoControllerPhysicsByteOffset, noControllerByte)) {
        s.note = "the bitfield byte at component + 0x01F1 could not be read";
        return s;
    }
    s.noControllerPhysicsRead = true;
    s.runPhysicsWithNoController = bit_set(noControllerByte, kNoControllerPhysicsBitIndex);

    if (!read_u8(read, c + kDefaultLandMovementModeOffset, s.defaultLandMovementMode)) {
        s.note = "DefaultLandMovementMode at component + 0x0384 could not be read";
        return s;
    }
    s.defaultLandMovementModeRead = true;

    s.readable = true;
    s.note.clear();
    return s;
}

std::string movement_state_line(const char* label, const MachineMovementState& state,
                                float windowMaxSpeed, const char* trailing) {
    const std::string who = (label != nullptr && label[0] != '\0') ? std::string(label) : std::string("?");

    if (!state.readable) {

        std::string out = who + ": NOT readable -- " + state.note;
        out += "; controller=";
        out += state.controller >= 0x10000 ? "set" : "NONE";
        if (state.actor >= 0x10000) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), " actor=0x%llX;",
                          static_cast<unsigned long long>(state.actor));
            out += buf;
        }
        return out;
    }

    char head[192];
    std::snprintf(head, sizeof(head),
                  "%s: controller=%s component=%llX(%s) MovementMode=%s(%u) CustomMovementMode=%u "
                  "GravityScale=%s current=%s max=%s",
                  who.c_str(), state.controller >= 0x10000 ? "set" : "NONE",
                  static_cast<unsigned long long>(state.componentOffset),
                  state.useDACharacterMovement ? "DACharacterMovement" : "CharacterMovement",
                  movement_mode_name(state.movementMode),
                  static_cast<unsigned>(state.movementMode),
                  static_cast<unsigned>(state.customMovementMode), f1(state.gravityScale).c_str(),
                  f1(state.speed()).c_str(), f1(windowMaxSpeed).c_str());

    std::string out = head;
    if (state.otherComponent >= 0x10000) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), " otherComponent=%llX(also set)",
                      static_cast<unsigned long long>(state.otherComponentOffset));
        out += buf;
    }
    out += " Velocity=" + vec3(state.velocity);
    out += " Acceleration=" + vec3(state.acceleration);
    out += " MaxWalkSpeed=" + f1(state.maxWalkSpeed);
    out += " MaxAcceleration=" + f1(state.maxAcceleration);
    out += std::string(" bJustTeleported=") + (state.justTeleported ? "1" : "0");
    out += std::string(" bHasRequestedVelocity=") + (state.hasRequestedVelocity ? "1" : "0");
    out += " RequestedVelocity=" + vec3(state.requestedVelocity);
    out += " LastUpdateVelocity=" + vec3(state.lastUpdateVelocity);
    out += " LastUpdateLocation=" + vec3(state.lastUpdateLocation);
    out += " ReplicatedMovement.LinearVelocity=" + vec3(state.replicatedLinearVelocity);

    out += " bRunPhysicsWithNoController=";
    if (!state.noControllerPhysicsRead) {
        out += "NOT read";
    } else {
        out += state.runPhysicsWithNoController ? "1" : "0";

        out += (state.runPhysicsWithNoController || state.controller >= 0x10000)
                   ? "(this actor can run physics, so a written Velocity CAN be integrated HERE)"
                   : "(no controller and this bit clear: on this actor the component does not run "
                     "physics, so a written Velocity is inert HERE whatever the mode says)";
    }
    out += " DefaultLandMovementMode=";
    out += state.defaultLandMovementModeRead ? movement_mode_name(state.defaultLandMovementMode) : "?";
    if (state.defaultLandMovementModeRead) {
        out += std::string("(") + std::to_string(static_cast<unsigned>(state.defaultLandMovementMode)) +
               ")";
    }

    if (state.lastUpdateLocationRead) {
        bool nonZero = false;
        for (float v : state.lastUpdateLocation) {
            if (std::fabs(v) > 0.5f) nonZero = true;
        }
        out += nonZero ? " [LastUpdateLocation is NON-ZERO, so this component HAS performed a movement "
                         "update]"
                       : " [LastUpdateLocation is all zero: no movement update has moved this "
                         "component's record]";
    }

    const float threshold = 1.0f;
    const bool puppetMoving = state.speed() > threshold;
    bool replicatedMoving = false;
    for (float v : state.replicatedLinearVelocity) {
        if (std::fabs(v) > threshold) replicatedMoving = true;
    }

    out += " | ";

    const bool canIntegrate = state.controller >= 0x10000 || state.runPhysicsWithNoController;

    if (puppetMoving) {

        out += "the component's Velocity is non-zero: this actor's own component holds a velocity "
               "something else put there (a write this build made, or the game's own)";
    } else if (replicatedMoving) {
        out += "the component's Velocity reads zero while ReplicatedMovement.LinearVelocity does NOT -- "
               "the actor's replicated movement state is being carried by the engine and the component "
               "has not been told about it";
    } else if (state.movementMode == kMoveNone) {
        out += "MovementMode is MOVE_None, so PerformMovement is never called and a written Velocity is "
               "inert and would only be READ by the animation graph -- that is the safe branch";
    } else if (!state.noControllerPhysicsRead) {
        out += "MovementMode integrates, and whether THIS component runs physics could not be read "
               "(bRunPhysicsWithNoController is `NOT read` above), so whether a written Velocity would "
               "be integrated here is UNDECIDED -- the two facts do not disagree, one of them is missing";
    } else if (state.movementMode <= kMoveCustom && !canIntegrate) {
        out += "MovementMode integrates, but this actor has no controller and bRunPhysicsWithNoController "
               "is 0 -- a written Velocity is inert HERE anyway, and the mode is what the animation graph "
               "reads. A puppet advancing between two teleports would show up as LastUpdateLocation "
               "changing; it is printed above";
    } else if (state.movementMode <= kMoveCustom) {
        out += "MovementMode integrates AND this actor can run physics (see bRunPhysicsWithNoController "
               "above), so a written Velocity IS integrated and the ownership fight against the 20 Hz "
               "teleport this build applies is live -- the per-window distance on the caller's own line "
               "is the measurement of it";
    } else {
        out += "MovementMode is OUTSIDE the enum the game defines, so which branch applies is UNDECIDED "
               "-- this is a wrong offset or a torn read, not a mode";
    }

    if (trailing != nullptr && trailing[0] != '\0') {
        out += " | ";
        out += trailing;
    }
    return out;
}

}
