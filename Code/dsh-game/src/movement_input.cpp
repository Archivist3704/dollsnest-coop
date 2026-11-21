#include "dsh/game/movement_input.h"

#include "dsh/game/doll_stance.h"
#include "dsh/reflect/spawn.h"

#include <cstring>

namespace dsh::game {
namespace {

constexpr std::size_t kFlagGroupBytes =
    static_cast<std::size_t>(kMovementInputFlagGroupEnd - kCharacterIgnoreFrictionOffset);
constexpr std::size_t kFlagIndexInGroup   = 0;
constexpr std::size_t kHoverIndexInGroup  = kCharacterHoverOffset - kCharacterIgnoreFrictionOffset;
constexpr std::size_t kSprintIndexInGroup = kCharacterSprintOffset - kCharacterIgnoreFrictionOffset;
constexpr std::size_t kBoostJumpIndexInGroup =
    kCharacterBoostJumpOffset - kCharacterIgnoreFrictionOffset;
constexpr std::size_t kBoostDashIndexInGroup =
    kCharacterBoostDashOffset - kCharacterIgnoreFrictionOffset;

void flags_into_group(std::uint8_t* group, const MovementInputs& in) noexcept {
    group[kFlagIndexInGroup] = in.ignoreFriction;
    group[kHoverIndexInGroup] = in.hover;
    group[kSprintIndexInGroup] = in.sprint;
    group[kBoostJumpIndexInGroup] = in.boostJump;
    group[kBoostDashIndexInGroup] = in.boostDash;
}

bool group_flags_match(const std::uint8_t* group, const MovementInputs& in) noexcept {
    return group[kFlagIndexInGroup] == in.ignoreFriction && group[kHoverIndexInGroup] == in.hover &&
           group[kSprintIndexInGroup] == in.sprint &&
           group[kBoostJumpIndexInGroup] == in.boostJump &&
           group[kBoostDashIndexInGroup] == in.boostDash;
}

void group_into_flags(const std::uint8_t* group, MovementInputs& out) noexcept {
    out.ignoreFriction = group[kFlagIndexInGroup];
    out.hover = group[kHoverIndexInGroup];
    out.sprint = group[kSprintIndexInGroup];
    out.boostJump = group[kBoostJumpIndexInGroup];
    out.boostDash = group[kBoostDashIndexInGroup];
}

}

MovementInputApplyResult puppet_movement_input_apply(std::uintptr_t actor,
                                                     std::uintptr_t daCharacterClass,
                                                     const MovementInputs& incoming) {
    MovementInputApplyResult result;

    if (actor < 0x10000) {
        result.note = "no actor to apply the movement input to";
        return result;
    }
    if (daCharacterClass == 0) {
        result.note = "the " + std::string(kMovementInputApiClass) +
                      " class could not be resolved, so the class chain cannot be checked and NOTHING "
                      "is written at or";
        return result;
    }

    std::string readNote;
    if (!read_movement_inputs(actor, daCharacterClass, result.before, readNote,
                              stance_read_process_bytes)) {
        result.note = "the movement input could not be read, so there is nothing to compare against "
                      "and NOTHING is written: " + readNote;
        return result;
    }

    result.written = result.before;

    result.flagsChanged = !result.before.flagsSame(incoming);
    if (result.flagsChanged) {

        std::uint8_t group[kFlagGroupBytes];
        if (!dsh::reflect::read_process_bytes(actor + kCharacterIgnoreFrictionOffset, group,
                                              sizeof(group))) {
            result.note = "the flag group at.. could not be read, so it cannot be "
                          "written without destroying its pad bytes -- NOTHING is written";
            return result;
        }
        flags_into_group(group, incoming);
        if (!dsh::reflect::write_process_bytes(actor + kCharacterIgnoreFrictionOffset, group,
                                               sizeof(group))) {
            result.note = "write_process_bytes refused the flag group at (uncommitted, "
                          "read-only, or a range that straddles two regions) -- the flags were NOT "
                          "applied";
            return result;
        }

        std::uint8_t back[kFlagGroupBytes];
        if (!dsh::reflect::read_process_bytes(actor + kCharacterIgnoreFrictionOffset, back,
                                              sizeof(back))) {
            result.note = "the flag group was written but.. does not read back";
            return result;
        }
        group_into_flags(back, result.written);
        if (!group_flags_match(back, incoming)) {
            result.note = "the flag bytes were accepted but the actor does not hold them: they read "
                          "back as different values, so something else is writing this field";
            return result;
        }
        result.flagsApplied = true;
    }

    result.vectorChanged = !result.before.vectorSame(incoming);
    if (result.vectorChanged) {
        if (!dsh::reflect::write_process_bytes(actor + kCharacterRecentInputOffset, incoming.recentInput,
                                               sizeof(incoming.recentInput))) {
            result.note = "write_process_bytes refused RecentObviousControlInputVector at "
                          "(uncommitted, read-only, or a range that straddles two regions) -- the "
                          "vector was NOT applied";
            return result;
        }
        float back[3] = {0.0f, 0.0f, 0.0f};
        if (!dsh::reflect::read_process_bytes(actor + kCharacterRecentInputOffset, back,
                                              sizeof(back))) {
            result.note = "the vector was written but does not read back";
            return result;
        }
        result.written.recentInput[0] = back[0];
        result.written.recentInput[1] = back[1];
        result.written.recentInput[2] = back[2];

        if (back[0] != incoming.recentInput[0] || back[1] != incoming.recentInput[1] ||
            back[2] != incoming.recentInput[2]) {
            result.note = "the twelve bytes were accepted but the actor does not hold them: "
                          "reads back as a different vector, so something else is writing this field";
            return result;
        }
        result.vectorApplied = true;
    }

    return result;
}

EnergyTimerApplyResult puppet_energy_timers_apply(std::uintptr_t actor,
                                                  std::uintptr_t daCharacterClass,
                                                  const EnergyTimers& incoming) {
    EnergyTimerApplyResult result;

    if (actor < 0x10000) {
        result.note = "no actor to apply the energy timers to";
        return result;
    }
    if (daCharacterClass == 0) {
        result.note = "the " + std::string(kMovementInputApiClass) +
                      " class could not be resolved, so the class chain cannot be checked and NOTHING "
                      "is written at";
        return result;
    }

    std::string readNote;
    if (!read_energy_timers(actor, daCharacterClass, result.before, readNote,
                            stance_read_process_bytes)) {
        result.note = "the two energy timers could not be read, so there is nothing to compare against "
                      "and NOTHING is written: " + readNote;
        return result;
    }
    result.beforeRead = true;

    result.written = result.before;

    result.changed = result.before != incoming;
    if (!result.changed) {

        return result;
    }

    float pair[2] = {incoming.intermissionTime, incoming.fastChargeStartTime};
    if (!dsh::reflect::write_process_bytes(actor + kCharacterIntermissionTimeOffset, pair,
                                           sizeof(pair))) {
        result.note = "write_process_bytes refused the energy timers at (uncommitted, read-only, "
                      "or a range that straddles two regions) -- they were NOT applied";
        return result;
    }

    EnergyTimers back;
    std::string backNote;
    if (!read_energy_timers(actor, daCharacterClass, back, backNote, stance_read_process_bytes)) {
        result.note = "the energy timers were written but does not read back";
        return result;
    }
    result.written = back;
    result.writtenRead = true;

    if (back != incoming) {
        result.note = "the eight bytes were accepted but the actor does not hold them: reads back "
                      "as a different pair, so something else is writing these fields";
        return result;
    }
    result.applied = true;
    return result;
}

}
