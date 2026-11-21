#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dsh::game {

inline constexpr std::uintptr_t kCharacterExclusiveOwnerOffset = 0x0FE0;
inline constexpr std::uintptr_t kCharacterExclusiveTypesOffset = 0x0FE8;

inline constexpr std::uint8_t kExclusiveNone        = 0;
inline constexpr std::uint8_t kExclusiveAim         = 1;
inline constexpr std::uint8_t kExclusiveFirstPerson = 2;

[[nodiscard]] std::string exclusive_types_text(std::uint8_t types);

struct ExclusiveState {
    bool           read = false;
    std::uintptr_t owner = 0;
    std::uint8_t   types = 0;
    std::string    note;

    [[nodiscard]] bool active() const noexcept { return read && types != 0; }
};

using ExclusiveByteReader = bool (*)(std::uintptr_t address, void* dst, std::size_t bytes);

[[nodiscard]] ExclusiveState exclusive_state_read(std::uintptr_t actor,
                                                  std::uintptr_t daCharacterClass,
                                                  ExclusiveByteReader read);

[[nodiscard]] std::string exclusive_state_line(const ExclusiveState& state);

inline constexpr std::uintptr_t kMachineLeftRearWeaponOffset  = 0x1610;
inline constexpr std::uintptr_t kMachineRightRearWeaponOffset = 0x1618;

enum class ExclusiveOwnerKind : std::uint8_t {
    none = 0,
    leftRearWeapon = 1,
    rightRearWeapon = 2,
    frontWeaponSlot0 = 3,
    frontWeaponSlot1 = 4,
    other = 255,
};

[[nodiscard]] const char* exclusive_owner_kind_text(ExclusiveOwnerKind kind);

[[nodiscard]] std::uintptr_t exclusive_owner_slot_offset(ExclusiveOwnerKind kind);

struct ExclusiveOwnerKindRead {
    ExclusiveOwnerKind kind = ExclusiveOwnerKind::none;
    std::uintptr_t     owner = 0;
    bool               read = false;
    std::string        note;
};

[[nodiscard]] ExclusiveOwnerKindRead exclusive_owner_kind(std::uintptr_t owner, std::uintptr_t machine,
                                                          ExclusiveByteReader read);

}
