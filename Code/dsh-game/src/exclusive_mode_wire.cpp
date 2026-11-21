#include "dsh/game/exclusive_mode.h"

#include "dsh/game/doll_stance.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace dsh::game {
namespace {

constexpr char kHexDigits[] = "0123456789ABCDEF";

std::string hex_pointer(std::uintptr_t value) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(value));
    return buf;
}

}

std::string exclusive_types_text(std::uint8_t types) {
    std::string out;
    if ((types & kExclusiveAim) != 0) out += "Aim";
    if ((types & kExclusiveFirstPerson) != 0) {
        if (!out.empty()) out += "|";
        out += "FirstPerson";
    }
    const std::uint8_t unknown = static_cast<std::uint8_t>(types & ~(kExclusiveAim | kExclusiveFirstPerson));
    if (unknown != 0) {
        if (!out.empty()) out += "|";
        out += "unknown(0x";
        out += kHexDigits[(unknown >> 4) & 0xF];
        out += kHexDigits[unknown & 0xF];
        out += ")";
    }
    if (out.empty()) out = "None";
    return out;
}

ExclusiveState exclusive_state_read(std::uintptr_t actor, std::uintptr_t daCharacterClass,
                                    ExclusiveByteReader read) {
    ExclusiveState state;
    if (actor < 0x10000) {
        state.note = "there is no actor to read an exclusive state off (the pointer is implausible)";
        return state;
    }
    if (daCharacterClass == 0) {
        state.note = "the DACharacter class could not be resolved, so the offsets cannot be checked "
                     "against the class chain and NOTHING is read";
        return state;
    }
    if (read == nullptr) {
        state.note = "there is no reader, so nothing is read";
        return state;
    }
    if (!actor_class_chain_reaches(actor, daCharacterClass, read)) {
        state.note = "the actor's class chain does not reach DACharacter, so/ are not its "
                     "ExclusiveOwner/ExclusiveTypes and nothing is read";
        return state;
    }

    std::uint64_t owner = 0;
    if (!read(actor + kCharacterExclusiveOwnerOffset, &owner, sizeof(owner))) {
        state.note = "ExclusiveOwner at could not be read";
        return state;
    }
    std::uint8_t types = 0;
    if (!read(actor + kCharacterExclusiveTypesOffset, &types, sizeof(types))) {
        state.note = "ExclusiveTypes at could not be read";
        return state;
    }

    if (owner != 0 && owner > 0x00007FFFFFFFFFFFull) {
        state.note = "ExclusiveOwner at does not look like a pointer (0x" +
                     std::to_string(static_cast<unsigned long long>(owner)) + " without the prefix is not an "
                     "address this process can hold)";
        return state;
    }
    state.owner = static_cast<std::uintptr_t>(owner);
    state.types = types;
    state.read = true;
    return state;
}

std::string exclusive_state_line(const ExclusiveState& state) {
    if (!state.read) {
        return "exclusive mode: NOT read (" + state.note + ") -- a state this side did not see must not be "
               "printed as `None`";
    }
    std::string out = "exclusive mode: ";
    if (state.types == 0) {
        out += "NONE (types 0; owner ";
        out += state.owner == 0 ? std::string("is NULL)") : (hex_pointer(state.owner) + ")");
        return out;
    }
    out += "ACTIVE types=";
    out += exclusive_types_text(state.types);
    out += "(";
    out += std::to_string(static_cast<unsigned>(state.types));
    out += ") owner ";
    out += state.owner == 0 ? std::string("is NULL -- an active mode with no owner") : hex_pointer(state.owner);
    return out;
}

const char* exclusive_owner_kind_text(ExclusiveOwnerKind kind) {
    switch (kind) {
        case ExclusiveOwnerKind::none:            return "none";
        case ExclusiveOwnerKind::leftRearWeapon:  return "left rear weapon";
        case ExclusiveOwnerKind::rightRearWeapon: return "right rear weapon";
        case ExclusiveOwnerKind::frontWeaponSlot0: return "front weapon slot 0";
        case ExclusiveOwnerKind::frontWeaponSlot1: return "front weapon slot 1";
        case ExclusiveOwnerKind::other:           return "other (not a weapon slot)";
    }
    return "?";
}

std::uintptr_t exclusive_owner_slot_offset(ExclusiveOwnerKind kind) {
    switch (kind) {
        case ExclusiveOwnerKind::leftRearWeapon:   return kMachineLeftRearWeaponOffset;
        case ExclusiveOwnerKind::rightRearWeapon:  return kMachineRightRearWeaponOffset;
        case ExclusiveOwnerKind::frontWeaponSlot0: return 0x1620;
        case ExclusiveOwnerKind::frontWeaponSlot1: return 0x1628;
        case ExclusiveOwnerKind::none:
        case ExclusiveOwnerKind::other:            return 0;
    }
    return 0;
}

ExclusiveOwnerKindRead exclusive_owner_kind(std::uintptr_t owner, std::uintptr_t machine,
                                            ExclusiveByteReader read) {
    ExclusiveOwnerKindRead out;
    out.owner = owner;
    if (owner == 0) {
        out.read = true;
        out.kind = ExclusiveOwnerKind::none;
        return out;
    }
    if (machine < 0x10000 || read == nullptr) {
        out.note = "the owner is set but there is no machine pointer (or no reader) to compare it against";
        out.kind = ExclusiveOwnerKind::other;
        return out;
    }
    struct Slot {
        std::uintptr_t     offset;
        ExclusiveOwnerKind kind;
    };
    constexpr Slot kSlots[4] = {
        {kMachineLeftRearWeaponOffset, ExclusiveOwnerKind::leftRearWeapon},
        {kMachineRightRearWeaponOffset, ExclusiveOwnerKind::rightRearWeapon},
        {0x1620, ExclusiveOwnerKind::frontWeaponSlot0},
        {0x1628, ExclusiveOwnerKind::frontWeaponSlot1},
    };
    for (const Slot& slot : kSlots) {
        std::uint64_t value = 0;
        if (!read(machine + slot.offset, &value, sizeof(value))) {
            out.note = "one of the four weapon-slot pointers could not be read";
            out.kind = ExclusiveOwnerKind::other;
            return out;
        }
        if (value != 0 && static_cast<std::uintptr_t>(value) == owner) {
            out.read = true;
            out.kind = slot.kind;
            return out;
        }
    }
    out.read = true;
    out.kind = ExclusiveOwnerKind::other;
    return out;
}

}
