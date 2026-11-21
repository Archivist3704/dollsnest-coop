#include "dsh/game/movement_state.h"

#include "dsh/game/doll_stance.h"
#include "dsh/reflect/spawn.h"

#include <cmath>
#include <cstring>

namespace dsh::game {
namespace {

bool same_velocity(const float a[3], const float b[3]) noexcept {

    for (int i = 0; i < 3; ++i) {
        if (!(std::fabs(a[i] - b[i]) < 1.0f)) return false;
    }
    return true;
}

}

VelocityApplyResult puppet_velocity_apply(std::uintptr_t actor, std::uintptr_t daCharacterClass,
                                          const float velocity[3], std::uint8_t requestedMode,
                                          const float acceleration[3]) {
    VelocityApplyResult result;
    result.modeAsked = requestedMode;

    if (actor < 0x10000) {
        result.note = "no actor to apply the velocity to";
        return result;
    }
    if (daCharacterClass == 0) {
        result.note = "the " + std::string(kMovementApiClass) +
                      " class could not be resolved, so the class chain cannot be checked and "
                      "NOTHING is written at component + 0x00C4";
        return result;
    }

    if (!actor_class_chain_reaches(actor, daCharacterClass, stance_read_process_bytes)) {
        result.note = "the puppet's class chain does not reach " + std::string(kMovementApiClass) +
                      ", so/ are not movement components on it and nothing is written";
        return result;
    }

    std::uintptr_t componentOffset = 0;
    std::uintptr_t other = 0;
    const std::uintptr_t component =
        movement_component_address(actor, componentOffset, other, stance_read_process_bytes);
    if (component == 0) {
        result.note = "neither movement component pointer is set ( reads 0x0 and reads "
                      "0x0), so there is no Velocity to write on this actor";
        return result;
    }

    std::uint8_t mode = 0;
    if (!dsh::reflect::read_process_bytes(component + kMovementModeOffset, &mode, sizeof(mode))) {
        result.note = "MovementMode at component + 0x0168 could not be read, so which branch this "
                      "write lands in is unknown and NOTHING is written";
        return result;
    }
    result.movementMode = mode;

    float current[3] = {0.0f, 0.0f, 0.0f};
    if (!dsh::reflect::read_process_bytes(component + kMovementVelocityOffset, current,
                                          sizeof(current))) {
        result.note = "UMovementComponent::Velocity at component + 0x00C4 could not be read, so there "
                      "is no way to say whether the write changes anything and nothing is written";
        return result;
    }
    result.changed = !same_velocity(current, velocity);

    if (!dsh::reflect::write_process_bytes(component + kMovementVelocityOffset, velocity,
                                           sizeof(float) * 3)) {
        result.note = "write_process_bytes refused component + 0x00C4 (uncommitted, read-only, or a "
                      "range that straddles two regions) -- nothing was applied";
        return result;
    }

    float readBack[3] = {0.0f, 0.0f, 0.0f};
    if (!dsh::reflect::read_process_bytes(component + kMovementVelocityOffset, readBack,
                                          sizeof(readBack))) {
        result.note = "the velocity was written but component + 0x00C4 does not read back -- this "
                      "component is not holding a velocity this call can report";
        return result;
    }
    result.written[0] = readBack[0];
    result.written[1] = readBack[1];
    result.written[2] = readBack[2];
    if (!same_velocity(readBack, velocity)) {
        result.note = "the twelve bytes were accepted but the component does not hold them: it reads "
                      "back as a different velocity, so something else is writing this field";
        return result;
    }
    result.applied = true;

    if (acceleration != nullptr) {
        result.accelerationRequested = true;
        float currentAcceleration[3] = {0.0f, 0.0f, 0.0f};
        if (!dsh::reflect::read_process_bytes(component + kAccelerationOffset, currentAcceleration,
                                              sizeof(currentAcceleration))) {
            result.accelerationNote = "UCharacterMovementComponent::Acceleration at component + 0x022C "
                                      "could not be read, so nothing is written there";
        } else {

            result.accelerationChanged = !same_velocity(currentAcceleration, acceleration);
            if (!dsh::reflect::write_process_bytes(component + kAccelerationOffset, acceleration,
                                                   sizeof(float) * 3)) {
                result.accelerationNote =
                    "write_process_bytes refused component + 0x022C (uncommitted, read-only, or a range "
                    "that straddles two regions) -- nothing was applied";
            } else {
                float readBackAcceleration[3] = {0.0f, 0.0f, 0.0f};
                if (!dsh::reflect::read_process_bytes(component + kAccelerationOffset,
                                                      readBackAcceleration,
                                                      sizeof(readBackAcceleration))) {
                    result.accelerationNote = "the acceleration was written but component + 0x022C does "
                                              "not read back -- this component is not holding an "
                                              "acceleration this call can report";
                } else {
                    result.accelerationWritten[0] = readBackAcceleration[0];
                    result.accelerationWritten[1] = readBackAcceleration[1];
                    result.accelerationWritten[2] = readBackAcceleration[2];
                    if (!same_velocity(readBackAcceleration, acceleration)) {
                        result.accelerationNote = "the twelve bytes were accepted but the component does "
                                                  "not hold them: it reads back as a different "
                                                  "acceleration, so something else is writing this field";
                    } else {
                        result.accelerationApplied = true;
                    }
                }
            }
        }
    }

    switch (mode_write_decision(requestedMode)) {
        case ModeWriteDecision::outsideEnum:

            result.modeNote = "the host's MovementMode byte is " + std::to_string(requestedMode) +
                              ", which is OUTSIDE the EMovementMode the game defines (0..6; 7 is "
                              "MOVE_MAX, the enum's count and not a mode) -- so the mode byte at "
                              "component + 0x0168 is NOT written and the puppet keeps the mode it has";
            result.movementModeAfter = mode;
            result.modeReadBack = true;
            return result;
        case ModeWriteDecision::alreadyNone:

            result.modeNote = "the host's MovementMode is MOVE_None, which is the mode the puppet's own "
                              "component already holds -- there is nothing to write";
            result.movementModeAfter = mode;
            result.modeReadBack = true;
            return result;
        case ModeWriteDecision::writeOk:
            break;
    }
    result.modeRequested = true;
    if (mode != requestedMode) {

        if (!dsh::reflect::write_process_bytes(component + kMovementModeOffset, &requestedMode,
                                               sizeof(requestedMode))) {
            result.modeNote = "write_process_bytes refused component + 0x0168 (uncommitted, read-only, "
                              "or a range that straddles two regions) -- the mode was NOT written and "
                              "the puppet keeps " +
                              std::string(movement_mode_name(mode));
            result.movementModeAfter = mode;
            result.modeReadBack = true;
            return result;
        }
        result.modeWritten = true;
    }

    std::uint8_t after = 0;
    if (!dsh::reflect::read_process_bytes(component + kMovementModeOffset, &after, sizeof(after))) {
        result.modeNote = "the mode was written but component + 0x0168 does not read back -- whether the "
                          "component holds it cannot be reported from here";
        return result;
    }
    result.movementModeAfter = after;
    result.modeReadBack = true;
    result.modeApplied = after == requestedMode;
    if (!result.modeApplied) {
        result.modeNote = std::string("the mode byte was written as ") +
                          movement_mode_name(requestedMode) +
                          " and reads back as " + movement_mode_name(after) +
                          " -- something in the same dispatch overwrote it, so the mode is NOT held";
    }

    float location[3] = {0.0f, 0.0f, 0.0f};
    if (dsh::reflect::read_process_bytes(component + kLastUpdateLocationOffset, location,
                                         sizeof(location))) {
        result.location[0] = location[0];
        result.location[1] = location[1];
        result.location[2] = location[2];
        result.locationRead = true;
    }
    return result;
}

}
