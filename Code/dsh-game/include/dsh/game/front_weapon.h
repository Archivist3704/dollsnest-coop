#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "dsh/game/movement_input.h"

namespace dsh::game {

inline constexpr std::uintptr_t kMachineFrontWeaponInventoryOffset = 0x1620;
inline constexpr std::uintptr_t kMachineCurrentFrontWeaponOffset   = 0x1630;
inline constexpr std::size_t    kMachineFrontWeaponSlots           = 2;

inline constexpr std::size_t kMachineFrontWeaponSpanBytes =
    static_cast<std::size_t>(kMachineCurrentFrontWeaponOffset - kMachineFrontWeaponInventoryOffset) +
    sizeof(std::uintptr_t);
static_assert(kMachineFrontWeaponSpanBytes == 0x18,
              "FrontWeaponInventory[2] at and CurrentFrontWeapon at are one 0x18-byte span");

inline constexpr const char* kFrontWeaponApiClass = "DAMachine";

inline constexpr std::uint8_t kFrontWeaponSlot0   = 0;
inline constexpr std::uint8_t kFrontWeaponSlot1   = 1;
inline constexpr std::uint8_t kFrontWeaponNone    = 2;
inline constexpr std::uint8_t kFrontWeaponNotRead = 3;

struct FrontWeaponState {
    std::uintptr_t inventory[kMachineFrontWeaponSlots] = {0, 0};
    std::uintptr_t current = 0;

    [[nodiscard]] bool operator==(const FrontWeaponState& other) const noexcept {
        return inventory[0] == other.inventory[0] && inventory[1] == other.inventory[1] &&
               current == other.current;
    }
    [[nodiscard]] bool operator!=(const FrontWeaponState& other) const noexcept {
        return !(*this == other);
    }
};

[[nodiscard]] std::uint8_t front_weapon_index(const FrontWeaponState& state) noexcept;

[[nodiscard]] bool read_front_weapon(std::uintptr_t machine, std::uintptr_t daMachineClass,
                                     FrontWeaponState& out, std::string& note,
                                     MovementInputByteReader read);

[[nodiscard]] const char* front_weapon_wire_name(std::uint8_t index) noexcept;

[[nodiscard]] std::string front_weapon_line(std::uint8_t index, bool haveValue,
                                            const std::string& refusal);

struct FrontWeaponApplyResult {
    bool           applied = false;
    bool           changed = false;
    bool           called = false;
    bool           holding = false;
    bool           stated = false;
    bool           beforeRead = false;
    bool           afterRead = false;
    std::uint8_t   requested = kFrontWeaponNotRead;
    std::uintptr_t wanted = 0;
    FrontWeaponState before;
    FrontWeaponState after;
    std::string    note;
};

[[nodiscard]] FrontWeaponApplyResult puppet_front_weapon_apply(std::uintptr_t machine,
                                                              std::uintptr_t daMachineClass,
                                                              std::uint8_t requested);

}
