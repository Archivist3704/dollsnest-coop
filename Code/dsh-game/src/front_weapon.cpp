#include "dsh/game/front_weapon.h"

#include "dsh/game/doll_stance.h"
#include "dsh/reflect/spawn.h"

#include <cstdio>

namespace dsh::game {
namespace {

std::string hex_pointer(std::uintptr_t value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(value));
    return buf;
}

}

FrontWeaponApplyResult puppet_front_weapon_apply(std::uintptr_t machine, std::uintptr_t daMachineClass,
                                                 std::uint8_t requested) {
    FrontWeaponApplyResult result;
    result.requested = requested;

    if (machine < 0x10000) {
        result.note = "no puppet to hand a front weapon to (the pointer is implausible)";
        return result;
    }
    if (daMachineClass == 0) {
        result.note = "the " + std::string(kFrontWeaponApiClass) +
                      " class could not be resolved, so the puppet's class chain cannot be checked and "
                      "NOTHING is read from or";
        return result;
    }

    std::string readNote;
    if (!read_front_weapon(machine, daMachineClass, result.before, readNote,
                           stance_read_process_bytes)) {
        result.note = "the puppet's three weapon pointers could not be read, so there is nothing to "
                      "compare against and NOTHING is called: " + readNote;
        return result;
    }
    result.beforeRead = true;

    result.after = result.before;

    if (requested == kFrontWeaponNotRead) {

        return result;
    }
    if (requested != kFrontWeaponSlot0 && requested != kFrontWeaponSlot1 &&
        requested != kFrontWeaponNone) {
        result.note = "the wire carried " + std::to_string(requested) +
                      ", which is not one of the four values this build writes (0 and 1 are the two "
                      "inventory slots, 2 is nothing in hand, 3 is not read) -- NOTHING is called";
        return result;
    }
    result.stated = true;

    if (requested == kFrontWeaponNone) {

        result.note = "the host is holding no front weapon in hand, and this build does not force the "
                      "puppet to put one down -- NOTHING is called";
        return result;
    }

    result.wanted = result.before.inventory[requested];
    if (result.wanted < 0x10000) {
        result.changed = true;
        result.note = "the puppet's own FrontWeaponInventory[" + std::to_string(requested) +
                      "] (/) is EMPTY, so there is no module to hand to EquipFrontWeapon -- "
                      "NOTHING is called, and a pointer invented on this side would be a weapon the game "
                      "never built";
        return result;
    }

    result.changed = result.before.current != result.wanted;
    if (!result.changed) {
        result.holding = true;
        return result;
    }

    std::string callNote;
    if (!dsh::reflect::machine_call_equip_front_weapon(machine, result.wanted, callNote)) {
        result.note = "the game's own EquipFrontWeapon was NOT called: " + callNote;
        return result;
    }
    result.called = true;

    FrontWeaponState after;
    std::string afterNote;
    if (!read_front_weapon(machine, daMachineClass, after, afterNote, stance_read_process_bytes)) {
        result.note = "EquipFrontWeapon was called with " + hex_pointer(result.wanted) +
                      " but the puppet's three weapon pointers do not read back: " + afterNote;
        return result;
    }
    result.after = after;
    result.afterRead = true;
    if (after.current != result.wanted) {
        result.note = "the game's own EquipFrontWeapon was called with " + hex_pointer(result.wanted) +
                      " and CurrentFrontWeapon reads back as " + hex_pointer(after.current) +
                      " -- the call did not move the field that decides which weapon the doll holds";
        return result;
    }
    result.applied = true;
    return result;
}

}
