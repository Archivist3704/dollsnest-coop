#include "dsh/game/movement_input.h"

#include "dsh/game/doll_stance.h"

#include <cstdio>

namespace dsh::game {
namespace {

bool read_bytes(MovementInputByteReader read, std::uintptr_t address, void* dst, std::size_t bytes) {
    return read != nullptr && read(address, dst, bytes);
}

}

std::string energy_timer_line(const EnergyTimers& in, bool readable, bool havePrevious,
                              float previousIntermissionTime, const std::string& refusal) {
    if (!readable) {
        return "energy timers: NOT read" +
               (refusal.empty() ? std::string() : (" (" + refusal + ")"));
    }
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "energy timers: IntermissionTime=%.3f EnergyFastChargeStartTime=%.3f (step since the "
                  "previous readable IntermissionTime: %s)",
                  static_cast<double>(in.intermissionTime),
                  static_cast<double>(in.fastChargeStartTime),
                  havePrevious ? std::to_string(static_cast<double>(in.intermissionTime -
                                                                    previousIntermissionTime))
                                     .c_str()
                               : "none yet -- this is the first readable pair");
    return buf;
}

std::string movement_input_line(const MovementInputs& in) {

    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "ignoreFriction=%u hover=%u sprint=%u boostJump=%u boostDash=%u recentInput=(%.3f, "
                  "%.3f, %.3f)",
                  static_cast<unsigned>(in.ignoreFriction), static_cast<unsigned>(in.hover),
                  static_cast<unsigned>(in.sprint), static_cast<unsigned>(in.boostJump),
                  static_cast<unsigned>(in.boostDash), static_cast<double>(in.recentInput[0]),
                  static_cast<double>(in.recentInput[1]), static_cast<double>(in.recentInput[2]));
    return buf;
}

bool read_movement_inputs(std::uintptr_t actor, std::uintptr_t daCharacterClass, MovementInputs& out,
                          std::string& note, MovementInputByteReader read) {
    out = MovementInputs{};
    note.clear();

    if (actor < 0x10000) {
        note = "there is no actor to read the movement input from (the pointer is implausible)";
        return false;
    }
    if (read == nullptr) {
        note = "no memory reader was supplied";
        return false;
    }
    if (daCharacterClass == 0) {

        note = "the " + std::string(kMovementInputApiClass) +
               " class could not be resolved, so the actor's class chain cannot be checked and NONE of "
               "the movement-input fields is read";
        return false;
    }
    if (!actor_class_chain_reaches(actor, daCharacterClass, read)) {
        note = "the actor's class chain does not reach " + std::string(kMovementInputApiClass) +
               ", so is not bIgnoreFriction and is not "
               "RecentObviousControlInputVector on it -- NOTHING is read from this actor";
        return false;
    }

    if (!read_bytes(read, actor + kCharacterIgnoreFrictionOffset, &out.ignoreFriction,
                    sizeof(out.ignoreFriction))) {
        note = "bIgnoreFriction at could not be read";
        return false;
    }
    if (!read_bytes(read, actor + kCharacterHoverOffset, &out.hover, sizeof(out.hover))) {
        note = "bHover at could not be read";
        return false;
    }
    if (!read_bytes(read, actor + kCharacterSprintOffset, &out.sprint, sizeof(out.sprint))) {
        note = "bSprint at could not be read";
        return false;
    }
    if (!read_bytes(read, actor + kCharacterBoostJumpOffset, &out.boostJump, sizeof(out.boostJump))) {
        note = "bBoostJump at could not be read";
        return false;
    }
    if (!read_bytes(read, actor + kCharacterBoostDashOffset, &out.boostDash, sizeof(out.boostDash))) {
        note = "bBoostDash at could not be read";
        return false;
    }
    if (!read_bytes(read, actor + kCharacterRecentInputOffset, out.recentInput,
                    sizeof(out.recentInput))) {
        note = "RecentObviousControlInputVector at could not be read as three float32";
        return false;
    }
    return true;
}

bool read_character_ability_system(std::uintptr_t actor, std::uintptr_t daCharacterClass,
                                   AbilitySystemState& out, std::string& note,
                                   MovementInputByteReader read) {
    out = AbilitySystemState{};
    note.clear();

    if (actor < 0x10000) {
        note = "there is no actor to read the ability system pointer from (the pointer is implausible)";
        return false;
    }
    if (read == nullptr) {
        note = "no memory reader was supplied";
        return false;
    }
    if (daCharacterClass == 0) {
        note = "the " + std::string(kMovementInputApiClass) +
               " class could not be resolved, so the actor's class chain cannot be checked and "
               "is NOT read as AbilitySystem";
        return false;
    }
    if (!actor_class_chain_reaches(actor, daCharacterClass, read)) {
        note = "the actor's class chain does not reach " + std::string(kMovementInputApiClass) +
               ", so is not AbilitySystem on it -- NOTHING is read from this actor";
        return false;
    }

    std::uintptr_t pointer = 0;
    if (!read_bytes(read, actor + kCharacterAbilitySystemOffset, &pointer, sizeof(pointer))) {
        note = "AbilitySystem at could not be read";
        return false;
    }
    out.abilitySystem = pointer;
    return true;
}

bool read_energy_timers(std::uintptr_t actor, std::uintptr_t daCharacterClass, EnergyTimers& out,
                        std::string& note, MovementInputByteReader read) {
    out = EnergyTimers{};
    note.clear();

    if (actor < 0x10000) {
        note = "there is no actor to read the energy timers from (the pointer is implausible)";
        return false;
    }
    if (read == nullptr) {
        note = "no memory reader was supplied";
        return false;
    }
    if (daCharacterClass == 0) {
        note = "the " + std::string(kMovementInputApiClass) +
               " class could not be resolved, so the actor's class chain cannot be checked and NEITHER "
               "energy timer is read";
        return false;
    }
    if (!actor_class_chain_reaches(actor, daCharacterClass, read)) {
        note = "the actor's class chain does not reach " + std::string(kMovementInputApiClass) +
               ", so is not IntermissionTime and is not EnergyFastChargeStartTime on it "
               "-- NOTHING is read from this actor";
        return false;
    }

    float pair[2] = {0.0f, 0.0f};
    if (!read_bytes(read, actor + kCharacterIntermissionTimeOffset, pair, sizeof(pair))) {
        note = "IntermissionTime at and EnergyFastChargeStartTime at could not be read "
               "as two float32";
        return false;
    }
    out.intermissionTime = pair[0];
    out.fastChargeStartTime = pair[1];
    return true;
}

}
