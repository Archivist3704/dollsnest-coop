#include "dsh/game/front_weapon.h"

#include "dsh/game/doll_stance.h"

namespace dsh::game {

std::uint8_t front_weapon_index(const FrontWeaponState& state) noexcept {

    if (state.current != 0) {
        if (state.current == state.inventory[0]) return kFrontWeaponSlot0;
        if (state.current == state.inventory[1]) return kFrontWeaponSlot1;
    }
    return kFrontWeaponNone;
}

const char* front_weapon_wire_name(std::uint8_t index) noexcept {
    switch (index) {
        case kFrontWeaponSlot0:
            return "slot 0 of 2 (FrontWeaponInventory[0],)";
        case kFrontWeaponSlot1:
            return "slot 1 of 2 (FrontWeaponInventory[1],)";
        case kFrontWeaponNone:
            return "no weapon in hand -- CurrentFrontWeapon matched neither of the two inventory "
                   "slots, which is the game's own nothing-drawn state and not a failure";
        case kFrontWeaponNotRead:
            return "NOT read -- the side that sent this byte could not read its own three weapon pointers "
                   "(,)";
        default:
            return "?";
    }
}

std::string front_weapon_line(std::uint8_t index, bool haveValue, const std::string& refusal) {

    if (!haveValue) {
        return "front weapon: NOT read or received" +
               (refusal.empty() ? std::string() : (" (" + refusal + ")"));
    }
    return "front weapon: " + std::string(front_weapon_wire_name(index));
}

bool read_front_weapon(std::uintptr_t machine, std::uintptr_t daMachineClass, FrontWeaponState& out,
                       std::string& note, MovementInputByteReader read) {
    out = FrontWeaponState{};
    note.clear();

    if (machine < 0x10000) {
        note = "there is no machine to read the front weapon from (the pointer is implausible)";
        return false;
    }
    if (read == nullptr) {
        note = "no memory reader was supplied";
        return false;
    }
    if (daMachineClass == 0) {

        note = "the " + std::string(kFrontWeaponApiClass) +
               " class could not be resolved, so the actor's class chain cannot be checked and NEITHER "
               "the inventory nor CurrentFrontWeapon is read";
        return false;
    }
    if (!actor_class_chain_reaches(machine, daMachineClass, read)) {
        note = "the actor's class chain does not reach " + std::string(kFrontWeaponApiClass) +
               ", so is not FrontWeaponInventory and is not CurrentFrontWeapon on it -- "
               "NOTHING is read from this actor";
        return false;
    }

    std::uintptr_t triple[kMachineFrontWeaponSlots + 1] = {0, 0, 0};
    static_assert(sizeof(triple) == kMachineFrontWeaponSpanBytes,
                  "the three pointers are the whole span this reader asks for");
    if (!read(machine + kMachineFrontWeaponInventoryOffset, triple, sizeof(triple))) {
        note = "FrontWeaponInventory at (two pointers) and CurrentFrontWeapon at could "
               "not be read as one 0x18-byte span";
        return false;
    }
    out.inventory[0] = triple[0];
    out.inventory[1] = triple[1];
    out.current = triple[2];
    return true;
}

}
